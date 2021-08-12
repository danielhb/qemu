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
#include "cpu.h"
#include "helper_regs.h"
#include "exec/exec-all.h"
#include "exec/helper-proto.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"

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
    default:
        break;
    }

    return event;
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
    int sprn;

    for (sprn = SPR_POWER_PMC1; sprn < SPR_POWER_PMC5; sprn++) {
        update_programmable_PMC_reg(env, sprn, time_delta);
    }

    update_PMC_PM_CYC(env, SPR_POWER_PMC6, time_delta);
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
        }
    }
}

static bool pmc_counting_insns(CPUPPCState *env, int sprn)
{
    bool ret = false;
    uint8_t event;

    if (env->spr[SPR_POWER_MMCR0] & MMCR0_FC) {
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
    case SPR_POWER_PMC4:
        return event == 0x2;
    default:
        break;
    }

    return ret;
}

void helper_insns_inc(CPUPPCState *env, uint32_t num_insns)
{
    int sprn;

    for (sprn = SPR_POWER_PMC1; sprn <= SPR_POWER_PMC5; sprn++) {
        if (pmc_counting_insns(env, sprn)) {
            env->spr[sprn] += num_insns;
        }
    }
}
