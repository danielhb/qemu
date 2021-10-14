/*
 * PMU emulation helpers for TCG IBM POWER chips
 *
 *  Copyright IBM Corp. 2021
 *
 * Authors:
 *  Daniel Henrique Barboza      <danielhb413@gmail.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"

#include "power8-pmu.h"
#include "cpu.h"
#include "helper_regs.h"
#include "exec/exec-all.h"
#include "exec/helper-proto.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "hw/ppc/ppc.h"

#if defined(TARGET_PPC64) && !defined(CONFIG_USER_ONLY)

#define COUNTER_NEGATIVE_VAL 0x80000000

/*
 * For PMCs 1-4, IBM POWER chips has support for an implementation
 * dependent event, 0x1E, that enables cycle counting. The Linux kernel
 * makes extensive use of 0x1E, so let's also support it.
 *
 * Likewise, event 0x2 is an implementation-dependent event that IBM
 * POWER chips implement (at least since POWER8) that is equivalent to
 * PM_INST_CMPL. Let's support this event on PMCs 1-4 as well.
 */
static void define_enabled_events(CPUPPCState *env)
{
    uint8_t mmcr1_evt_extr[] = { MMCR1_PMC1EVT_EXTR, MMCR1_PMC2EVT_EXTR,
                                 MMCR1_PMC3EVT_EXTR, MMCR1_PMC4EVT_EXTR };
    int i;

    for (i = 0; i < 4; i++) {
        uint8_t pmcsel = extract64(env->spr[SPR_POWER_MMCR1],
                                   mmcr1_evt_extr[i],
                                   MMCR1_EVT_SIZE);
        PMUEvent *event = &env->pmu_events[i];

        switch (pmcsel) {
        case 0x2:
            event->type = PMU_EVENT_INSTRUCTIONS;
            break;
        case 0x1E:
            event->type = PMU_EVENT_CYCLES;
            break;
        case 0xF0:
            /* PMC1SEL = 0xF0 is the architected PowerISA v3.1
             * event that counts cycles using PMC1. */
            if (event->sprn == SPR_POWER_PMC1) {
                event->type = PMU_EVENT_CYCLES;
            }
            break;
        case 0xFE:
            /* PMC1SEL = 0xFE is the architected PowerISA v3.1
             * event to sample instructions using PMC1. */
            if (event->sprn == SPR_POWER_PMC1) {
                event->type = PMU_EVENT_INSTRUCTIONS;
            }
            break;
        default:
            event->type = PMU_EVENT_INVALID;
        }
    }
}

void helper_store_mmcr1(CPUPPCState *env, uint64_t value)
{
    env->spr[SPR_POWER_MMCR1] = value;

    define_enabled_events(env);
}

static bool pmu_event_is_active(CPUPPCState *env, PMUEvent *event)
{
    if (event->sprn < SPR_POWER_PMC5) {
        return !(env->spr[SPR_POWER_MMCR0] & MMCR0_FC14);
    }

    return !(env->spr[SPR_POWER_MMCR0] & MMCR0_FC56);
}

static bool pmu_event_has_overflow_enabled(CPUPPCState *env, PMUEvent *event)
{
    if (event->sprn == SPR_POWER_PMC1) {
        return env->spr[SPR_POWER_MMCR0] & MMCR0_PMC1CE;
    }

    return env->spr[SPR_POWER_MMCR0] & MMCR0_PMCjCE;
}

static bool pmu_events_increment_insns(CPUPPCState *env, uint32_t num_insns)
{
    bool overflow_triggered = false;
    int i;

    /* PMC6 never counts instructions. */
    for (i = 0; i < PMU_EVENTS_NUM - 1; i++) {
        PMUEvent *event = &env->pmu_events[i];

        if (!pmu_event_is_active(env, event) ||
            event->type != PMU_EVENT_INSTRUCTIONS) {
            continue;
        }

        env->spr[event->sprn] += num_insns;

        if (env->spr[event->sprn] >= COUNTER_NEGATIVE_VAL &&
            pmu_event_has_overflow_enabled(env, event)) {

            overflow_triggered = true;
            env->spr[event->sprn] = COUNTER_NEGATIVE_VAL;
        }
    }

    return overflow_triggered;
}

static void pmu_events_update_cycles(CPUPPCState *env)
{
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t time_delta = now - env->pmu_base_time;
    int i;

    for (i = 0; i < PMU_EVENTS_NUM; i++) {
        PMUEvent *event = &env->pmu_events[i];

        if (!pmu_event_is_active(env, event) ||
            event->type != PMU_EVENT_CYCLES) {
            continue;
        }

        /*
         * The pseries and powernv clock runs at 1Ghz, meaning
         * that 1 nanosec equals 1 cycle.
         */
        env->spr[event->sprn] += time_delta;
    }

    /*
     * Update base_time for future calculations if we updated
     * the PMCs while the PMU was running.
     */
    if (!(env->spr[SPR_POWER_MMCR0] & MMCR0_FC)) {
        env->pmu_base_time = now;
    }
}

static void pmu_delete_timers(CPUPPCState *env)
{
    int i;

    for (i = 0; i < PMU_EVENTS_NUM; i++) {
        PMUEvent *event = &env->pmu_events[i];

        if (event->sprn == SPR_POWER_PMC5) {
            continue;
        }

        timer_del(event->cycle_timer);
    }
}

static void pmu_events_start_overflow_timers(CPUPPCState *env)
{
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t timeout;
    int i;

    env->pmu_base_time = now;

    /*
     * Scroll through all PMCs ad start counter overflow timers for
     * PM_CYC events, if needed.
     */
    for (i = 0; i < PMU_EVENTS_NUM; i++) {
        PMUEvent *event = &env->pmu_events[i];

        if (!pmu_event_is_active(env, event) ||
            !(event->type == PMU_EVENT_CYCLES) ||
            !pmu_event_has_overflow_enabled(env, event)) {
            continue;
        }

        if (env->spr[event->sprn] >= COUNTER_NEGATIVE_VAL) {
            timeout =  0;
        } else {
            timeout  = COUNTER_NEGATIVE_VAL - env->spr[event->sprn];
        }

        timer_mod(event->cycle_timer, now + timeout);
    }
}

/*
 * A cycle count session consists of the basic operations we
 * need to do to support PM_CYC events: redefine a new base_time
 * to be used to calculate PMC values and start overflow timers.
 */
static void start_cycle_count_session(CPUPPCState *env)
{
    bool overflow_enabled = env->spr[SPR_POWER_MMCR0] &
                            (MMCR0_PMC1CE | MMCR0_PMCjCE);

    /*
     * Always delete existing overflow timers when starting a
     * new cycle counting session.
     */
    pmu_delete_timers(env);

    if (!overflow_enabled) {
        /* Define pmu_base_time and leave */
        env->pmu_base_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        return;
    }

    pmu_events_start_overflow_timers(env);
}

void helper_store_mmcr0(CPUPPCState *env, target_ulong value)
{
    target_ulong curr_value = env->spr[SPR_POWER_MMCR0];
    bool curr_FC = curr_value & MMCR0_FC;
    bool new_FC = value & MMCR0_FC;

    env->spr[SPR_POWER_MMCR0] = value;

    /* MMCR0 writes can change HFLAGS_PMCCCLEAR and HFLAGS_MMCR0FC */
    if (((curr_value & MMCR0_PMCC) != (value & MMCR0_PMCC)) ||
        (curr_FC != new_FC)) {
        hreg_compute_hflags(env);
    }

    /*
     * In an frozen count (FC) bit change:
     *
     * - if PMCs were running (curr_FC = false) and we're freezing
     * them (new_FC = true), save the PMCs values in the registers.
     *
     * - if PMCs were frozen (curr_FC = true) and we're activating
     * them (new_FC = false), set the new base_time for future cycle
     * calculations.
     */
    if (curr_FC != new_FC) {
        if (!curr_FC) {
            pmu_events_update_cycles(env);
        } else {
            start_cycle_count_session(env);
        }
    }
}

static void fire_PMC_interrupt(PowerPCCPU *cpu)
{
    CPUPPCState *env = &cpu->env;

    if (!(env->spr[SPR_POWER_MMCR0] & MMCR0_EBE)) {
        return;
    }

    /* PMC interrupt not implemented yet */
    return;
}

/* This helper assumes that the PMC is running. */
void helper_insns_inc(CPUPPCState *env, uint32_t num_insns)
{
    bool overflow_triggered;
    PowerPCCPU *cpu;

    overflow_triggered = pmu_events_increment_insns(env, num_insns);

    if (overflow_triggered) {
        cpu = env_archcpu(env);
        fire_PMC_interrupt(cpu);
    }
}

static void cpu_ppc_pmu_timer_cb(void *opaque)
{
    PowerPCCPU *cpu = opaque;

    fire_PMC_interrupt(cpu);
}

void cpu_ppc_pmu_init(CPUPPCState *env)
{
    PowerPCCPU *cpu = env_archcpu(env);
    int i;

    /*
     * PMC1 event first, PMC2 second and so on. PMC5 and PMC6
     * PMUEvent are always the same regardless of MMCR1.
     */
    for (i = 0; i < PMU_EVENTS_NUM; i++) {
        PMUEvent *event = &env->pmu_events[i];

        event->sprn = SPR_POWER_PMC1 + i;
        event->type = PMU_EVENT_INVALID;

        if (event->sprn == SPR_POWER_PMC5) {
            event->type = PMU_EVENT_INSTRUCTIONS;
            continue;
        }

        if (event->sprn == SPR_POWER_PMC6) {
            event->type = PMU_EVENT_CYCLES;
        }

        event->cycle_timer =  timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                           &cpu_ppc_pmu_timer_cb,
                                           cpu);
    }
}

#endif /* defined(TARGET_PPC64) && !defined(CONFIG_USER_ONLY) */
