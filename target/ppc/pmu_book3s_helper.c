/*
 * PowerPC Book3s PMU emulation helpers for QEMU TCG
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

#include "pmu_book3s_helper.h"
#include "cpu.h"
#include "exec/exec-all.h"
#include "exec/helper-proto.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "hw/ppc/ppc.h"
 #include "helper_regs.h"

/*
 * Set arbitrarily based on clock-frequency values used in PNV
 * and SPAPR code.
 */
#define PPC_CPU_FREQ 1000000000
#define COUNTER_NEGATIVE_VAL 0x80000000

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

        /*
         * Event 0xFA for PMC4SEL is described as follows in
         * PowerISA v3.1:
         *
         * "The thread has completed an instruction when the RUN bit of
         * the thread’s CTRL register contained 1"
         *
         * Our closest equivalent for this event at this moment is plain
         * INST_CMPL (event 0x2)
         */
        if (event == 0xFA) {
            event = 0x2;
        }
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

static void update_PMC_PM_INST_CMPL(CPUPPCState *env, int sprn)
{
    return;
    // env->spr[sprn] += env->pmu_insns_count;
}

static void update_PMC_PM_CYC(CPUPPCState *env, int sprn,
                              uint64_t time_delta)
{
    env->spr[sprn] += get_cycles(time_delta);
}

static int get_stall_ratio(uint8_t stall_event)
{
    int stall_ratio = 0;

    switch (stall_event) {
    case 0xA:
        stall_ratio = 25;
        break;
    case 0x6:
    case 0x16:
    case 0x1C:
        stall_ratio = 5;
        break;
    default:
        break;
    }

    return stall_ratio;
}

static void update_PMC_PM_STALL(CPUPPCState *env, int sprn,
                                uint64_t icount_delta,
                                uint8_t stall_event)
{
    int stall_ratio = get_stall_ratio(stall_event);
    uint64_t cycles = muldiv64(get_cycles(icount_delta), stall_ratio, 100);

    env->spr[sprn] += cycles;
}

static void update_programmable_PMC_reg(CPUPPCState *env, int sprn,
                                        uint64_t icount_delta)
{
    uint8_t event = get_PMC_event(env, sprn);

    switch (event) {
    case 0x2:
        update_PMC_PM_INST_CMPL(env, sprn);
        break;
    case 0x1E:
        update_PMC_PM_CYC(env, sprn, icount_delta);
        break;
    case 0xA:
    case 0x6:
    case 0x16:
    case 0x1C:
        update_PMC_PM_STALL(env, sprn, icount_delta, event);
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
    bool PMC56_running = !(env->spr[SPR_POWER_MMCR0] & MMCR0_FC56);
    int sprn;

    if (PMC14_running) {
        for (sprn = SPR_POWER_PMC1; sprn < SPR_POWER_PMC5; sprn++) {
            update_programmable_PMC_reg(env, sprn, time_delta);
        }
    }

    if (PMC56_running) {
        update_PMC_PM_INST_CMPL(env, SPR_POWER_PMC5);
        update_PMC_PM_CYC(env, SPR_POWER_PMC6, time_delta);
    }

    // env->pmu_insns_count = 0;
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

static int64_t get_stall_timeout(CPUPPCState *env, int sprn,
                                 uint8_t stall_event)
{
    uint64_t remaining_cyc;
    int stall_multiplier;

    if (env->spr[sprn] >= COUNTER_NEGATIVE_VAL) {
        return 0;
    }

    remaining_cyc = COUNTER_NEGATIVE_VAL - env->spr[sprn];

    /*
     * Consider that for this stall event we'll advance the counter
     * in a lower rate, thus requiring more cycles to overflow.
     * E.g. for PM_CMPLU_STALL (0xA), ratio 25, it'll require
     * 100/25 = 4 times the same amount of cycles to overflow.
     */
    stall_multiplier = 100 / get_stall_ratio(stall_event);
    remaining_cyc *= stall_multiplier;

    return muldiv64(remaining_cyc, NANOSECONDS_PER_SECOND, PPC_CPU_FREQ);
}

static bool pmc_counter_negative_enabled(CPUPPCState *env, int sprn)
{
    bool PMC14_running = !(env->spr[SPR_POWER_MMCR0] & MMCR0_FC14);
    bool PMC56_running = !(env->spr[SPR_POWER_MMCR0] & MMCR0_FC56);

    switch (sprn) {
    case SPR_POWER_PMC1:
        return env->spr[SPR_POWER_MMCR0] & MMCR0_PMC1CE && PMC14_running;

    case SPR_POWER_PMC2:
    case SPR_POWER_PMC3:
    case SPR_POWER_PMC4:
        return env->spr[SPR_POWER_MMCR0] & MMCR0_PMCjCE && PMC14_running;

    case SPR_POWER_PMC5:
    case SPR_POWER_PMC6:
        return env->spr[SPR_POWER_MMCR0] & MMCR0_PMCjCE && PMC56_running;

    default:
        break;
    }

    return false;
}

static int64_t get_counter_neg_timeout(CPUPPCState *env, int sprn)
{
    int64_t timeout = -1;
    uint8_t event;

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
        event = get_PMC_event(env, sprn);

        switch (event) {
        case 0x1E:
            timeout = get_CYC_timeout(env, sprn);
            break;
        case 0xA:
        case 0x6:
        case 0x16:
        case 0x1c:
            timeout = get_stall_timeout(env, sprn, event);
            break;
        default:
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
     * Scroll through all PMCs and check which one is closer to a
     * counter negative timeout.
     */
    for (i = SPR_POWER_PMC1; i <= SPR_POWER_PMC6; i++) {
        int64_t curr_timeout = get_counter_neg_timeout(env, i);

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
    uint64_t time_delta = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - env->pmu_base_time;

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
        hreg_compute_hflags(env);

        if (!curr_FC) {
            uint64_t time_delta = (curr_time - env->pmu_base_time);

            /*
             * Update the counter with the instructions run
             * until the freeze.
             */
            update_PMCs(env, time_delta);

            /* delete pending timer */
            timer_del(env->pmu_intr_timer);
        } else {
            env->pmu_base_time = curr_time;
            // env->pmu_insns_count = 0;

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

static bool pmc_is_running(CPUPPCState *env, int sprn)
{
    if (sprn < SPR_POWER_PMC5) {
        return !(env->spr[SPR_POWER_MMCR0] & MMCR0_FC14);
    }

    return !(env->spr[SPR_POWER_MMCR0] & MMCR0_FC56);
}

static bool pmc_counting_insns(CPUPPCState *env, int sprn)
{
    bool ret = false;
    uint8_t event;

    if (!pmc_is_running(env, sprn)) {
        return false;
    }

    if (sprn == SPR_POWER_PMC5) {
        return true;
    }

    event = get_PMC_event(env, sprn);

    switch (sprn) {
    case SPR_POWER_PMC1:
        return event == 0x2 || event == 0xF2 || event == 0xFE;
    case SPR_POWER_PMC2:
    case SPR_POWER_PMC3:
        return event == 0x2;
    case SPR_POWER_PMC4:
        return event == 0x2 || event == 0xFA;
    default:
        break;
    }

    return ret;
}

void helper_insns_inc(CPUPPCState *env, uint32_t num_insns)
{
    bool counter_neg_triggered = false;
    PowerPCCPU *cpu;
    int sprn;

    for (sprn = SPR_POWER_PMC1; sprn <= SPR_POWER_PMC5; sprn++) {
        if (pmc_counting_insns(env, sprn)) {
            env->spr[sprn] += num_insns;

            if (env->spr[sprn] >= COUNTER_NEGATIVE_VAL &&
                pmc_counter_negative_enabled(env, sprn)) {
                counter_neg_triggered = true;
                env->spr[sprn] = COUNTER_NEGATIVE_VAL;
            }
        }
    }

    if (counter_neg_triggered) {
        /* delete pending timer */
        timer_del(env->pmu_intr_timer);

        cpu = env_archcpu(env);
        cpu_ppc_pmu_timer_cb(cpu);
    }
}

void helper_insns_dec(CPUPPCState *env, uint32_t num_insns)
{
    env->pmu_insns_count -= num_insns;
}
