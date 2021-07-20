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

    if (!pmuState.freeze_pmc5_pmc6) {
        pmuState.PMC5 += num_insns;
        pmuState.PMC6 += num_insns * 4;
    }
}

unsigned long PMU_get_PMC5(void)
{
    return pmuState.PMC5;

}

unsigned long PMU_get_PMC6(void)
{
    return pmuState.PMC6;
}
