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
#include "cpu.h"
#include "exec/exec-all.h"
#include "exec/helper-proto.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"

static bool pmu_global_freeze(CPUPPCState *env)
{
    return env->spr[SPR_POWER_MMCR0] & MMCR0_FC;
}

static uint64_t insns_get_count(CPUPPCState *env)
{
    return (uint64_t)icount_get_raw();
}

static uint64_t cycles_get_count(uint64_t insns)
{
    /* Placeholder value */
    return insns * 4;
}

static void update_PMC_PM_INST_CMPL(CPUPPCState *env, int sprn,
                                    uint64_t curr_icount)
{
    int pmc_idx;

    pmc_idx = sprn - SPR_POWER_PMC1;
    env->spr[sprn] += curr_icount - env->pmc_base_icount[pmc_idx];
}

static void update_PMC_PM_CYC(CPUPPCState *env, int sprn,
                              uint64_t curr_icount)
{
    uint64_t insns;
    int pmc_idx;

    pmc_idx = sprn - SPR_POWER_PMC1;
    insns = curr_icount - env->pmc_base_icount[pmc_idx];
    env->spr[sprn] += cycles_get_count(insns);
}

static void update_programmable_PMC_reg(CPUPPCState *env, int sprn,
                                        uint64_t curr_icount)
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

    // printf("---- mmcr1 = %lx , event = %x \n", env->spr[SPR_POWER_MMCR1], event);

    switch (event) {
        case 0x2:
            update_PMC_PM_INST_CMPL(env, sprn, curr_icount);
            break;
        case 0x1E:
            update_PMC_PM_CYC(env, sprn, curr_icount);
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
static void update_PMCs_on_freeze(CPUPPCState *env, uint32_t insns)
{
    uint64_t curr_icount = insns_get_count(env);
    int sprn;

    for (sprn = SPR_POWER_PMC1; sprn < SPR_POWER_PMC5; sprn++) {
        update_programmable_PMC_reg(env, sprn, curr_icount);
    }

    // update_PMC_PM_INST_CMPL(env, SPR_POWER_PMC5, curr_icount);
    printf("--- SPR_POWER_PMC5 = %lx , base_icount = %lx, insns = %x",
            env->spr[SPR_POWER_PMC5], env->pmc_base_icount[4], insns );
    env->spr[SPR_POWER_PMC5] += env->pmc_base_icount[4] + insns;
    env->pmc_base_icount[4] = 0;


    update_PMC_PM_CYC(env, SPR_POWER_PMC6, curr_icount);
}

/*
 * Update a PMC register value (specified by sprn) based on its
 * current set event.
 *
 * This is a no-op if the PMU is frozen (MMCR0_FC set).
 */
static void update_PMC_reg(CPUPPCState *env, int sprn)
{
    uint64_t curr_icount;
    int pmc_idx;

    if (pmu_global_freeze(env)) {
        return;
    }

    g_assert(sprn >= SPR_POWER_PMC1 && sprn < (SPR_POWER_PMC6 + 1));

    curr_icount = insns_get_count(env);
    pmc_idx = sprn - SPR_POWER_PMC1;

    switch(sprn) {
        case SPR_POWER_PMC1:
        case SPR_POWER_PMC2:
        case SPR_POWER_PMC3:
        case SPR_POWER_PMC4:
            update_programmable_PMC_reg(env, sprn, curr_icount);
            break;

        case SPR_POWER_PMC5:
            //env->spr[sprn] += env->pmc_base_icount[pmc_idx];
            // env->pmc_base_icount[pmc_idx] = 0;
            return;
            // update_PMC_PM_INST_CMPL(env, SPR_POWER_PMC5, curr_icount);
            // break;
        case SPR_POWER_PMC6:
            update_PMC_PM_CYC(env, SPR_POWER_PMC6, curr_icount);
            break;
        default:
            break;
    }

    env->pmc_base_icount[pmc_idx] = curr_icount;
}

static void update_PMC_base_icount(CPUPPCState *env, int sprn)
{
    uint64_t curr_icount;
    int pmc_idx;

    g_assert(sprn >= SPR_POWER_PMC1 && sprn < (SPR_POWER_PMC6 + 1));

    /*
     * The base_icounts will be updated when the PMU starts
     * spinning again.
     */
    if (pmu_global_freeze(env)) {
        return;
    }

    curr_icount = insns_get_count(env);
    pmc_idx = sprn - SPR_POWER_PMC1;
    env->pmc_base_icount[pmc_idx] = curr_icount;
    if (pmc_idx == 4)
        env->pmc_base_icount[pmc_idx] = 0;
}

void helper_store_PMC_value(CPUPPCState *env, uint32_t sprn,
                            target_ulong value)
{
    env->spr[sprn] = value;
    update_PMC_base_icount(env, sprn);
}

uint64_t helper_get_PMC_value(CPUPPCState *env, uint32_t sprn)
{
    update_PMC_reg(env, sprn);
    return env->spr[sprn];
}

void helper_store_mmcr0(CPUPPCState *env, target_ulong value, uint32_t insns)
{
    bool curr_FC = env->spr[SPR_POWER_MMCR0] & MMCR0_FC;
    bool new_FC = value & MMCR0_FC;
    int i;

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
            printf("---- setting FC to 1 (froze PMCs) \n");
            update_PMCs_on_freeze(env, insns);
        } else {
            uint64_t curr_icount = insns_get_count(env);

            printf("---- setting FC to 0 (PMCs now running) \n");

            for (i = 0; i < 6; i++) {
                env->pmc_base_icount[i] = curr_icount;
            }
            env->pmc_base_icount[4] = 0;
        }
    }

    env->spr[SPR_POWER_MMCR0] = value;
}

void helper_store_insns_completed(CPUPPCState *env, uint32_t insns)
{
    if (pmu_global_freeze(env)) {
        return;
    }

    env->spr[SPR_POWER_PMC5] += insns;
}
