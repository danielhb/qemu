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

/*
 * For PMCs 1-4, IBM POWER chips has support for an implementation
 * dependent event, 0x1E, that enables cycle counting. The Linux kernel
 * makes extensive use of 0x1E, so let's also support it.
 *
 * Likewise, event 0x2 is an implementation-dependent event that IBM
 * POWER chips implement (at least since POWER8) that is equivalent to
 * PM_INST_CMPL. Let's support this event on PMCs 1-4 as well.
 */
static void define_enabled_events(CPUPPCState *env)
{
    uint8_t mmcr1_evt_extr[] = { MMCR1_PMC1EVT_EXTR, MMCR1_PMC2EVT_EXTR,
                                 MMCR1_PMC3EVT_EXTR, MMCR1_PMC4EVT_EXTR };
    int i;

    for (i = 0; i < 4; i++) {
        uint8_t pmcsel = extract64(env->spr[SPR_POWER_MMCR1],
                                   mmcr1_evt_extr[i],
                                   MMCR1_EVT_SIZE);
        PMUEvent *event = &env->pmu_events[i];

        switch (pmcsel) {
        case 0x2:
            event->type = PMU_EVENT_INSTRUCTIONS;
            break;
        case 0x1E:
            event->type = PMU_EVENT_CYCLES;
            break;
        case 0xF0:
            /*
             * PMC1SEL = 0xF0 is the architected PowerISA v3.1
             * event that counts cycles using PMC1.
             */
            if (event->sprn == SPR_POWER_PMC1) {
                event->type = PMU_EVENT_CYCLES;
            }
            break;
        case 0xFE:
            /*
             * PMC1SEL = 0xFE is the architected PowerISA v3.1
             * event to sample instructions using PMC1.
             */
            if (event->sprn == SPR_POWER_PMC1) {
                event->type = PMU_EVENT_INSTRUCTIONS;
            }
            break;
        default:
            event->type = PMU_EVENT_INVALID;
        }
    }
}

void helper_store_mmcr1(CPUPPCState *env, uint64_t value)
{
    env->spr[SPR_POWER_MMCR1] = value;

    define_enabled_events(env);
}

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

        event->cyc_overflow_timer =  timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                                  &cpu_ppc_pmu_timer_cb,
                                                  cpu);
    }
}

#endif /* defined(TARGET_PPC64) && !defined(CONFIG_USER_ONLY) */
