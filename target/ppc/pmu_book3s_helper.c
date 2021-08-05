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

#include "pmu_book3s_helper.h"

#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/exec-all.h"
#include "exec/helper-proto.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "hw/ppc/ppc.h"

/*
 * Set arbitrarily based on clock-frequency values used in PNV
 * and SPAPR code.
 */
#define PPC_CPU_FREQ 1000000000
#define COUNTER_NEGATIVE_VAL 0x80000000

static uint64_t get_cycles(uint64_t insns)
{
    return muldiv64(icount_to_ns(insns), PPC_CPU_FREQ,
                    NANOSECONDS_PER_SECOND);
}

static uint8_t get_PMC_event(CPUPPCState *env, int sprn)
{
    int event= 0x0;

    switch(sprn) {
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

static void update_PMC_PM_INST_CMPL(CPUPPCState *env, int sprn, uint32_t insns)
{
    env->spr[sprn] += insns;
}

static void update_PMC_PM_CYC(CPUPPCState *env, int sprn, uint32_t insns)
{
    env->spr[sprn] += get_cycles(insns);
}

static void update_programmable_PMC_reg(CPUPPCState *env, int sprn, uint32_t insns)
{
    switch (get_PMC_event(env, sprn)) {
        case 0x2:
            update_PMC_PM_INST_CMPL(env, sprn, insns);
            break;
        case 0x1E:
            update_PMC_PM_CYC(env, sprn, insns);
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
static void update_PMCs(CPUPPCState *env, uint32_t insns)
{
    int sprn;

    for (sprn = SPR_POWER_PMC1; sprn < SPR_POWER_PMC5; sprn++) {
        update_programmable_PMC_reg(env, sprn, insns);
    }

    update_PMC_PM_INST_CMPL(env, SPR_POWER_PMC5, insns);
    update_PMC_PM_CYC(env, SPR_POWER_PMC6, insns);
}

static int64_t get_INST_CMPL_timeout(CPUPPCState *env, int sprn)
{
    int64_t remaining_insns;

    if (env->spr[sprn] == 0) {
        return icount_to_ns(COUNTER_NEGATIVE_VAL);
    }

    if (env->spr[sprn] >= COUNTER_NEGATIVE_VAL) {
        return 0;
    }

    remaining_insns = COUNTER_NEGATIVE_VAL - env->spr[sprn];
    return icount_to_ns(remaining_insns);
}

static int64_t get_CYC_timeout(CPUPPCState *env, int sprn)
{
    int64_t remaining_cyc;

    if (env->spr[sprn] == 0) {
        return icount_to_ns(COUNTER_NEGATIVE_VAL);
    }

    if (env->spr[sprn] >= COUNTER_NEGATIVE_VAL) {
        return 0;
    }

    remaining_cyc = COUNTER_NEGATIVE_VAL - env->spr[sprn];
    return muldiv64(remaining_cyc, NANOSECONDS_PER_SECOND, PPC_CPU_FREQ);
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
        case 0x2:
            timeout = get_INST_CMPL_timeout(env, sprn);
            break;
        case 0x1E:
            timeout = get_CYC_timeout(env, sprn);
            break;
        default:
            break;
        }

        break;
    case SPR_POWER_PMC5:
        timeout = get_INST_CMPL_timeout(env, sprn);
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
    uint64_t icount_delta = (uint64_t)icount_get_raw() - env->pmu_base_icount;

    if (!(env->spr[SPR_POWER_MMCR0] & MMCR0_EBE)) {
        return;
    }

    update_PMCs(env, icount_delta);

    if (env->spr[SPR_POWER_MMCR0] & MMCR0_FCECE) {
        env->spr[SPR_POWER_MMCR0] &= ~MMCR0_FCECE;
        env->spr[SPR_POWER_MMCR0] |= MMCR0_FC;
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
    uint64_t curr_icount = (uint64_t)icount_get_raw();
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
        if (!curr_FC) {
            uint64_t insns = (curr_icount - env->pmu_base_icount);

            /* exclude both mtsprs() that opened and closed the timer */
            insns -= 2;

            /* update the counter with the instructions run until the freeze */
            update_PMCs(env, insns);

            /* delete pending timer */
            timer_del(env->pmu_intr_timer);
        } else {
            env->pmu_base_icount = curr_icount;

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
    uint64_t curr_icount, icount_delta;

    if (pmu_frozen) {
        env->spr[sprn] = value;
        return;
    }

    curr_icount = (uint64_t)icount_get_raw();
    icount_delta = curr_icount - env->pmu_base_icount;

    /* Update the counter with the events counted so far */
    update_PMCs(env, icount_delta);

    /* Set the counter to the desirable value after update_PMCs() */
    env->spr[sprn] = value;

    /*
     * Delete the current timer and restart a new one with the
     * updated values.
     */
    timer_del(env->pmu_intr_timer);

    env->pmu_base_icount = curr_icount;

    if (counter_negative_cond_enabled(env->spr[SPR_POWER_MMCR0])) {
        set_PMU_excp_timer(env);
    }
}
