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

#include "power8_pmu.h"
#include "cpu.h"
#include "helper_regs.h"
#include "exec/exec-all.h"
#include "exec/helper-proto.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "hw/ppc/ppc.h"

#define COUNTER_NEGATIVE_VAL 0x80000000

/*
 * The pseries and pvn clock runs at 1Ghz, meaning that 1 nanosec
 * equals 1 cycle.
 */
static uint64_t get_cycles(uint64_t time_delta)
{
    return time_delta;
}

static uint8_t get_PMC_event(CPUPPCState *env, int sprn)
{
    int event = 0x0;

    switch (sprn) {
    case SPR_POWER_PMC1:
        event = MMCR1_PMC1SEL & env->spr[SPR_POWER_MMCR1];
        event = event >> MMCR1_PMC1SEL_SHIFT;
        break;
    case SPR_POWER_PMC2:
        event = MMCR1_PMC2SEL & env->spr[SPR_POWER_MMCR1];
        event = event >> MMCR1_PMC2SEL_SHIFT;
        break;
    case SPR_POWER_PMC3:
        event = MMCR1_PMC3SEL & env->spr[SPR_POWER_MMCR1];
        event = event >> MMCR1_PMC3SEL_SHIFT;
        break;
    case SPR_POWER_PMC4:
        event = MMCR1_PMC4SEL & env->spr[SPR_POWER_MMCR1];
        break;
    case SPR_POWER_PMC5:
        event = 0x2;
        break;
    case SPR_POWER_PMC6:
        event = 0x1E;
        break;
    default:
        break;
    }

    return event;
}

static bool pmc_is_running(CPUPPCState *env, int sprn)
{
    if (sprn < SPR_POWER_PMC5) {
        return !(env->spr[SPR_POWER_MMCR0] & MMCR0_FC14);
    }

    return !(env->spr[SPR_POWER_MMCR0] & MMCR0_FC56);
}

static void update_PMC_PM_CYC(CPUPPCState *env, int sprn,
                              uint64_t time_delta)
{
    env->spr[sprn] += get_cycles(time_delta);
}

static void update_programmable_PMC_reg(CPUPPCState *env, int sprn,
                                        uint64_t time_delta)
{
    uint8_t event = get_PMC_event(env, sprn);

    /*
     * MMCR0_PMC1SEL = 0xF0 is the architected PowerISA v3.1 event
     * that counts cycles using PMC1.
     *
     * IBM POWER chips also has support for an implementation dependent
     * event, 0x1E, that enables cycle counting on PMCs 1-4. The
     * Linux kernel makes extensive use of 0x1E, so let's also support
     * it.
     */
    switch (event) {
    case 0xF0:
        if (sprn == SPR_POWER_PMC1) {
            update_PMC_PM_CYC(env, sprn, time_delta);
        }
        break;
    case 0x1E:
        update_PMC_PM_CYC(env, sprn, time_delta);
        break;
    default:
        return;
    }
}

/*
 * Set all PMCs values after a PMU freeze via MMCR0_FC.
 *
 * There is no need to update the base icount of each PMC since
 * the PMU is not running.
 */
static void update_PMCs(CPUPPCState *env, uint64_t time_delta)
{
    bool PMC14_running = !(env->spr[SPR_POWER_MMCR0] & MMCR0_FC14);
    bool PMC6_running = !(env->spr[SPR_POWER_MMCR0] & MMCR0_FC56);
    int sprn;

    if (PMC14_running) {
        for (sprn = SPR_POWER_PMC1; sprn < SPR_POWER_PMC5; sprn++) {
            update_programmable_PMC_reg(env, sprn, time_delta);
        }
    }

    if (PMC6_running) {
        update_PMC_PM_CYC(env, SPR_POWER_PMC6, time_delta);
    }
}

static int64_t get_CYC_timeout(CPUPPCState *env, int sprn)
{
    int64_t remaining_cyc;

    if (env->spr[sprn] >= COUNTER_NEGATIVE_VAL) {
        return 0;
    }

    remaining_cyc = COUNTER_NEGATIVE_VAL - env->spr[sprn];
    return remaining_cyc;
}

static bool pmc_counter_negative_enabled(CPUPPCState *env, int sprn)
{
    switch (sprn) {
    case SPR_POWER_PMC1:
        return env->spr[SPR_POWER_MMCR0] & MMCR0_PMC1CE;

    case SPR_POWER_PMC2:
    case SPR_POWER_PMC3:
    case SPR_POWER_PMC4:
    case SPR_POWER_PMC5:
    case SPR_POWER_PMC6:
        return env->spr[SPR_POWER_MMCR0] & MMCR0_PMCjCE;

    default:
        break;
    }

    return false;
}

static int64_t get_counter_neg_timeout(CPUPPCState *env, int sprn)
{
    int64_t timeout = -1;

    if (!pmc_counter_negative_enabled(env, sprn)) {
        return -1;
    }

    if (env->spr[sprn] >= COUNTER_NEGATIVE_VAL) {
        return 0;
    }

    switch (sprn) {
    case SPR_POWER_PMC1:
    case SPR_POWER_PMC2:
    case SPR_POWER_PMC3:
    case SPR_POWER_PMC4:
        switch (get_PMC_event(env, sprn)) {
        case 0xF0:
            if (sprn == SPR_POWER_PMC1) {
                timeout = get_CYC_timeout(env, sprn);
            }
            break;
        case 0x1E:
            timeout = get_CYC_timeout(env, sprn);
            break;
        }

        break;
    case SPR_POWER_PMC6:
        timeout = get_CYC_timeout(env, sprn);
        break;
    default:
        break;
    }

    return timeout;
}

static void set_PMU_excp_timer(CPUPPCState *env)
{
    int64_t timeout = -1;
    uint64_t now;
    int i;

    /*
     * Scroll through all PMCs but PMC5 and check which one is
     * closer to a counter negative timeout with PM_CYC.
     */
    for (i = SPR_POWER_PMC1; i <= SPR_POWER_PMC6; i++) {
        int64_t curr_timeout;

        /* PMC5 doesn't count cycles under any conditions. */
        if (i == SPR_POWER_PMC5) {
            continue;
        }

        curr_timeout = get_counter_neg_timeout(env, i);

        if (curr_timeout == -1) {
            continue;
        }

        if (curr_timeout == 0) {
            timeout = 0;
            break;
        }

        if (timeout == -1 || timeout > curr_timeout) {
            timeout = curr_timeout;
        }
    }

    /*
     * This can happen if counter negative conditions were enabled
     * without any events to be sampled.
     */
    if (timeout == -1) {
        return;
    }

    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    timer_mod(env->pmu_intr_timer, now + timeout);
}

static void cpu_ppc_pmu_timer_cb(void *opaque)
{
    PowerPCCPU *cpu = opaque;
    CPUPPCState *env = &cpu->env;
    uint64_t time_delta = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) -
                          env->pmu_base_time;

    if (!(env->spr[SPR_POWER_MMCR0] & MMCR0_EBE)) {
        return;
    }

    update_PMCs(env, time_delta);

    if (env->spr[SPR_POWER_MMCR0] & MMCR0_FCECE) {
        env->spr[SPR_POWER_MMCR0] &= ~MMCR0_FCECE;
        env->spr[SPR_POWER_MMCR0] |= MMCR0_FC;

        hreg_compute_hflags(env);
    }

    if (env->spr[SPR_POWER_MMCR0] & MMCR0_PMAE) {
        env->spr[SPR_POWER_MMCR0] &= ~MMCR0_PMAE;
        env->spr[SPR_POWER_MMCR0] |= MMCR0_PMAO;
    }

    /* Fire the PMC hardware exception */
    ppc_set_irq(cpu, PPC_INTERRUPT_PMC, 1);
}

void cpu_ppc_pmu_timer_init(CPUPPCState *env)
{
    PowerPCCPU *cpu = env_archcpu(env);
    QEMUTimer *timer;

    timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, &cpu_ppc_pmu_timer_cb, cpu);
    env->pmu_intr_timer = timer;
}

static bool counter_negative_cond_enabled(uint64_t mmcr0)
{
    return mmcr0 & (MMCR0_PMC1CE | MMCR0_PMCjCE);
}

void helper_store_mmcr0(CPUPPCState *env, target_ulong value)
{
    uint64_t curr_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    bool curr_FC = env->spr[SPR_POWER_MMCR0] & MMCR0_FC;
    bool new_FC = value & MMCR0_FC;

    env->spr[SPR_POWER_MMCR0] = value;

    /* MMCR0 writes can change HFLAGS_PMCCCLEAR */
    hreg_compute_hflags(env);

    /*
     * In an frozen count (FC) bit change:
     *
     * - if PMCs were running (curr_FC = false) and we're freezing
     * them (new_FC = true), save the PMCs values in the registers.
     *
     * - if PMCs were frozen (curr_FC = true) and we're activating
     * them (new_FC = false), calculate the current icount for each
     * register to allow for subsequent reads to calculate the insns
     * passed.
     */
    if (curr_FC != new_FC) {
        if (!curr_FC) {
            uint64_t time_delta = (curr_time - env->pmu_base_time);
            update_PMCs(env, time_delta);
        } else {
            env->pmu_base_time = curr_time;

            /*
             * Start performance monitor alert timer for counter negative
             * events, if needed.
             */
            if (counter_negative_cond_enabled(env->spr[SPR_POWER_MMCR0])) {
                set_PMU_excp_timer(env);
            }
        }
    }
}

static bool pmc_counting_insns(CPUPPCState *env, int sprn,
                               uint8_t event)
{
    bool ret = false;

    if (!pmc_is_running(env, sprn)) {
        return false;
    }

    if (sprn == SPR_POWER_PMC5) {
        return true;
    }

    switch (sprn) {
    case SPR_POWER_PMC1:
        return event == 0x2 || event == 0xF2 || event == 0xFE;
    case SPR_POWER_PMC2:
    case SPR_POWER_PMC3:
    case SPR_POWER_PMC4:
        /*
         * Event 0xFA is the "instructions completed with run latch
         * set" event. Consider it as instruction counting event.
         * The caller is responsible for handling it separately
         * from PM_INST_CMPL.
         */
        return event == 0x2 || event == 0xFA;
    default:
        break;
    }

    return ret;
}

void helper_insns_inc(CPUPPCState *env, uint32_t num_insns)
{
    int sprn;

    for (sprn = SPR_POWER_PMC1; sprn <= SPR_POWER_PMC5; sprn++) {
        uint8_t event = get_PMC_event(env, sprn);

        if (pmc_counting_insns(env, sprn, event)) {
            if (sprn == SPR_POWER_PMC4 && event == 0xFA) {
                if (env->spr[SPR_CTRL] & CTRL_RUN) {
                    env->spr[SPR_POWER_PMC4] += num_insns;
                }
            } else {
                env->spr[sprn] += num_insns;
            }
        }
    }
}

void helper_store_pmc(CPUPPCState *env, uint32_t sprn, uint64_t value)
{
    bool pmu_frozen = env->spr[SPR_POWER_MMCR0] & MMCR0_FC;
    uint64_t curr_time, time_delta;

    if (pmu_frozen) {
        env->spr[sprn] = value;
        return;
    }

    curr_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    time_delta = curr_time - env->pmu_base_time;

    /* Update the counter with the events counted so far */
    update_PMCs(env, time_delta);

    /* Set the counter to the desirable value after update_PMCs() */
    env->spr[sprn] = value;

    /*
     * Delete the current timer and restart a new one with the
     * updated values.
     */
    timer_del(env->pmu_intr_timer);

    env->pmu_base_time = curr_time;

    if (counter_negative_cond_enabled(env->spr[SPR_POWER_MMCR0])) {
        set_PMU_excp_timer(env);
    }
}
