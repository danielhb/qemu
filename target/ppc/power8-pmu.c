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

static void fire_PMC_interrupt(PowerPCCPU *cpu)
{
    CPUPPCState *env = &cpu->env;

    if (!(env->spr[SPR_POWER_MMCR0] & MMCR0_EBE)) {
        return;
    }

    /* PMC interrupt not implemented yet */
    return;
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
