/*
 *  PowerPC ISAv3 Book3S PMU emulation helpers
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
#include "kvm_ppc.h"
#include "tcg/tcg.h"
#include "pmu-book3s-v3.h"


struct PMUState
{
    unsigned long PMC1;
    unsigned long PMC2;
    unsigned long PMC3;
    unsigned long PMC4;
    unsigned long PMC5;
    unsigned long PMC6;

    bool freeze_counters;
    bool freeze_pmc5_pmc6;
};

static struct PMUState pmuState;

void PMU_set_freeze_counters(bool fc)
{
    pmuState.freeze_counters = fc;

    if (fc)
        printf("----- MMCR0_FC is set = freeze counters \n");
    else
        printf("----- MMCR0_FC is zeroed = unfreeze counters \n");
}

void PMU_set_freeze_PMC5PMC6(bool fc56)
{
    pmuState.freeze_pmc5_pmc6 = fc56;
}

void init_book3s_PMU(void)
{
    pmuState.freeze_counters = true;
}

void PMU_instructions_completed(int num_insns)
{
    if (pmuState.freeze_counters) {
        return;
    }

    // PMC1 count insns, PMC2 cycles (quick hack pmu kernel selftests
    pmuState.PMC1 += num_insns;
    pmuState.PMC2 += num_insns * 4;

    if (!pmuState.freeze_pmc5_pmc6) {
        pmuState.PMC5 += num_insns;
        pmuState.PMC6 += num_insns * 4;
    }
}


unsigned long PMU_get_PMC(int spr_power_pmc)
{
    unsigned long val;

    switch (spr_power_pmc) {
    case SPR_POWER_PMC1:
        val = pmuState.PMC1;
        break;
    case SPR_POWER_PMC2:
        val = pmuState.PMC2;
        break;
    case SPR_POWER_PMC3:
        val = pmuState.PMC3;
        break;
    case SPR_POWER_PMC4:
        val = pmuState.PMC4;
        break;
    case SPR_POWER_PMC5:
        val = pmuState.PMC5;
        break;
    case SPR_POWER_PMC6:
        val = pmuState.PMC6;
        break;
    default:
        val = 0;
    }

    return val;
}

void PMU_set_PMC(int spr_power_pmc, unsigned long val)
{
    switch (spr_power_pmc) {
    case SPR_POWER_PMC1:
        pmuState.PMC1 = val;
        break;
    case SPR_POWER_PMC2:
        pmuState.PMC2 = val;
        break;
    case SPR_POWER_PMC3:
        pmuState.PMC3 = val;
        break;
    case SPR_POWER_PMC4:
        pmuState.PMC4 = val;
        break;
    case SPR_POWER_PMC5:
        pmuState.PMC5 = val;
        break;
    case SPR_POWER_PMC6:
        pmuState.PMC6 = val;
        break;
    default:
        break;
    }
}
