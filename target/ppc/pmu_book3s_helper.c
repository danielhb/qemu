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
    int event;

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
            break;
        default:
            return;
    }

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

#if 0
static void cpu_ppc_pmu_set_timer(CPUPPCState *env, uint64_t time_ns)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    timer_mod(env->pmu_intr_timer, now + time_ns);
}
#endif

static int64_t get_val_for_cn(CPUPPCState *env, uint64_t val)
{
    return 0x80000000 - val;
}

static void set_PMU_excp_timer(CPUPPCState *env)
{
    uint64_t timeout, now, limit;

    if (!(env->spr[SPR_POWER_MMCR0] & MMCR0_PMC1CE)) {
        return;
    }

    limit = get_val_for_cn(env, env->spr[SPR_POWER_PMC1]);

    switch (get_PMC_event(env, SPR_POWER_PMC1)) {
    case 0x2:
        timeout = icount_to_ns(limit);
        break;
    case 0x1e:
        timeout = muldiv64(limit, NANOSECONDS_PER_SECOND,
                           PPC_CPU_FREQ);
        break;
    default:
        return;
    }

    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    timer_mod(env->pmu_intr_timer, now + timeout);

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
            if ((value & MMCR0_PMAE) && (value & MMCR0_FCECE)) {
                set_PMU_excp_timer(env);
            }
        }
    }
}

static void cpu_ppc_pmu_timer_cb(void *opaque)
{
    PowerPCCPU *cpu = opaque;
    CPUPPCState *env = &cpu->env;
    uint64_t curr_insns = (uint64_t)icount_get_raw() - env->pmu_base_icount;
    uint64_t mmcr0, curr_val = 0;
    int64_t remaining_val;

    /*
     * Get current PMC1 val, see if we're early for the counter
     * negative condition.
     */
    switch (get_PMC_event(env, SPR_POWER_PMC1)) {
    case 0x2:
        curr_val = curr_insns + env->spr[SPR_POWER_PMC1];
        break;
    case 0x1e:
        curr_val = get_cycles(curr_insns) + env->spr[SPR_POWER_PMC1];
        break;
    default:
        return;
    }

    remaining_val = get_val_for_cn(env, curr_val);

    if (remaining_val > 0 ) {
        uint64_t timeout, now;
        printf("==== cpu_ppc_pmu_timer_cb too early: curr_val = %lu \n",
               curr_val);

        timeout = icount_to_ns(remaining_val);
        now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

        timer_mod(env->pmu_intr_timer, now + timeout);
        return;
    }

    printf("==== cpu_ppc_pmu_timer_cb reached, curr_val = %lu \n", curr_val);

    mmcr0 = env->spr[SPR_POWER_MMCR0];
    if (env->spr[SPR_POWER_MMCR0] & MMCR0_EBE) {
        update_PMCs(env, curr_insns);

        mmcr0 &= ~MMCR0_FCECE;
        mmcr0 &= ~MMCR0_PMAE;
        mmcr0 |= MMCR0_PMAO;
        mmcr0 |= MMCR0_FC;

        env->spr[SPR_POWER_MMCR0] = mmcr0;

        /* Fire the PMC hardware exception */
        ppc_set_irq(cpu, PPC_INTERRUPT_PMC, 1);
    }
}

void cpu_ppc_pmu_timer_init(CPUPPCState *env)
{
    PowerPCCPU *cpu = env_archcpu(env);
    QEMUTimer *timer;

    timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, &cpu_ppc_pmu_timer_cb, cpu);
    env->pmu_intr_timer = timer;
}
