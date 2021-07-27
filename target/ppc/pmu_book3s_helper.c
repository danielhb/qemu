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

/* taken from target/arm/helper.c */
static uint64_t insns_get_count(CPUPPCState *env)
{
    return (uint64_t)icount_get_raw();
}

static uint64_t cycles_get_count(uint64_t insns)
{
    /* Placeholder value */
    return insns * 4;
}

/*
 * Set all PMCs values after a PMU freeze via MMCR0_FC.
 *
 * There is no need to update the base icount of each PMC since
 * the PMU is not running.
 */
static void update_PMCs_on_freeze(CPUPPCState *env)
{
    uint64_t curr_icount = insns_get_count(env);

    env->spr[SPR_POWER_PMC5] += curr_icount - env->pmc5_base_icount;
    env->spr[SPR_POWER_PMC6] += cycles_get_count(curr_icount -
                                                 env->pmc6_base_icount);
}

/*
 * Update a PMC register value (specified by sprn) based on its
 * current set event.
 *
 * This is a no-op if the PMU is frozen (MMCR0_FC set).
 */
static void update_PMC_reg(CPUPPCState *env, int sprn)
{
    uint64_t insns;

    if (pmu_global_freeze(env)) {
        return;
    }

    switch(sprn) {
        case SPR_POWER_PMC1:
        case SPR_POWER_PMC2:
        case SPR_POWER_PMC3:
        case SPR_POWER_PMC4:
            break;

        case SPR_POWER_PMC5:
            insns = insns_get_count(env) - env->pmc5_base_icount;
            env->spr[sprn] += insns;
            env->pmc5_base_icount += insns;
            break;
        case SPR_POWER_PMC6:
            insns = insns_get_count(env) - env->pmc6_base_icount;
            env->spr[sprn] += cycles_get_count(insns);
            env->pmc6_base_icount += insns;
            break;
        default:
            break;
    }
}

static void update_PMC_base_icount(CPUPPCState *env, int sprn)
{
    uint64_t curr_icount;

    /*
     * The base_icounts will be updated when the PMU starts
     * spinning again.
     */
    if (pmu_global_freeze(env)) {
        return;
    }

    curr_icount = insns_get_count(env);

    switch(sprn) {
        case SPR_POWER_PMC1:
        case SPR_POWER_PMC2:
        case SPR_POWER_PMC3:
        case SPR_POWER_PMC4:
            break;

        case SPR_POWER_PMC5:
            env->pmc5_base_icount = curr_icount;
            break;
        case SPR_POWER_PMC6:
            env->pmc6_base_icount = curr_icount;
            break;
        default:
            break;
    }
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

void helper_store_mmcr0(CPUPPCState *env, target_ulong value)
{
    bool curr_FC = env->spr[SPR_POWER_MMCR0] & MMCR0_FC;
    bool new_FC = value & MMCR0_FC;

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
            update_PMCs_on_freeze(env);
        } else {
            uint64_t curr_icount = insns_get_count(env);

            printf("---- setting FC to 0 (PMCs now running) \n");
            env->pmc5_base_icount = curr_icount;
            env->pmc6_base_icount = curr_icount;
        }
    }

    env->spr[SPR_POWER_MMCR0] = value;
}
