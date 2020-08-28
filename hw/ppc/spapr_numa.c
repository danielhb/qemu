/*
 * QEMU PowerPC pSeries Logical Partition NUMA associativity handling
 *
 * Copyright IBM Corp. 2020
 *
 * Authors:
 *  Daniel Henrique Barboza      <danielhb413@gmail.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu-common.h"
#include "hw/ppc/spapr_numa.h"
#include "hw/ppc/fdt.h"

/* Moved from hw/ppc/spapr_pci_nvlink2.c */
#define SPAPR_GPU_NUMA_ID           (cpu_to_be32(1))

void spapr_numa_associativity_init(MachineState *machine)
{
    SpaprMachineClass *smc = SPAPR_MACHINE_GET_CLASS(machine);
    int nb_numa_nodes = machine->numa_state->num_nodes;
    int i;

    /*
     * For all associativity arrays: first position is the size,
     * position MAX_DISTANCE_REF_POINTS is always the numa_id,
     * represented by the index 'i'.
     *
     * This will break on sparse NUMA setups, when/if QEMU starts
     * to support it, because there will be no more guarantee that
     * 'i' will be a valid node_id set by the user.
     */
    for (i = 0; i < nb_numa_nodes; i++) {
        smc->numa_assoc_array[i][0] = cpu_to_be32(MAX_DISTANCE_REF_POINTS);
        smc->numa_assoc_array[i][MAX_DISTANCE_REF_POINTS] = cpu_to_be32(i);
    }
}

void spapr_numa_write_associativity_dt(SpaprMachineState *spapr, void *fdt,
                                       int offset, int nodeid)
{
    SpaprMachineClass *smc = SPAPR_MACHINE_GET_CLASS(spapr);

    _FDT((fdt_setprop(fdt, offset, "ibm,associativity",
                      smc->numa_assoc_array[nodeid],
                      sizeof(smc->numa_assoc_array[nodeid]))));
}

int spapr_numa_fixup_cpu_dt(SpaprMachineState *spapr, void *fdt,
                            int offset, PowerPCCPU *cpu)
{
    SpaprMachineClass *smc = SPAPR_MACHINE_GET_CLASS(spapr);
    uint vcpu_assoc_size = NUMA_ASSOC_SIZE + 1;
    uint32_t vcpu_assoc[vcpu_assoc_size];
    int index = spapr_get_vcpu_id(cpu);
    int i;

    /*
     * VCPUs have an extra 'cpu_id' value in ibm,associativity
     * compared to other resources. Increment the size at index
     * 0, copy all associativity domains already set, then put
     * cpu_id last.
     */
    vcpu_assoc[0] = cpu_to_be32(MAX_DISTANCE_REF_POINTS + 1);

    for (i = 1; i <= MAX_DISTANCE_REF_POINTS; i++) {
        vcpu_assoc[i] = smc->numa_assoc_array[cpu->node_id][i];
    }

    vcpu_assoc[vcpu_assoc_size - 1] = cpu_to_be32(index);

    /* Advertise NUMA via ibm,associativity */
    return fdt_setprop(fdt, offset, "ibm,associativity",
                       vcpu_assoc, sizeof(vcpu_assoc));
}


int spapr_numa_write_assoc_lookup_arrays(SpaprMachineState *spapr, void *fdt,
                                         int offset)
{
    MachineState *machine = MACHINE(spapr);
    SpaprMachineClass *smc = SPAPR_MACHINE_GET_CLASS(spapr);
    int nb_numa_nodes = machine->numa_state->num_nodes;
    int nr_nodes = nb_numa_nodes ? nb_numa_nodes : 1;
    uint32_t *int_buf, *cur_index, buf_len;
    int ret, i, j;

    /* ibm,associativity-lookup-arrays */
    buf_len = (nr_nodes * MAX_DISTANCE_REF_POINTS + 2) * sizeof(uint32_t);
    cur_index = int_buf = g_malloc0(buf_len);
    int_buf[0] = cpu_to_be32(nr_nodes);
     /* Number of entries per associativity list */
    int_buf[1] = cpu_to_be32(MAX_DISTANCE_REF_POINTS);
    cur_index += 2;
    for (i = 0; i < nr_nodes; i++) {
        /*
         * For the lookup-array we use the ibm,associativity array,
         * from numa_assoc_array. without the first element (size).
         */
        uint32_t associativity[MAX_DISTANCE_REF_POINTS];

        for (j = 0; j < MAX_DISTANCE_REF_POINTS; j++) {
            associativity[j] = smc->numa_assoc_array[i][j + 1];
        }

        memcpy(cur_index, associativity, sizeof(associativity));
        cur_index += 4;
    }
    ret = fdt_setprop(fdt, offset, "ibm,associativity-lookup-arrays", int_buf,
                      (cur_index - int_buf) * sizeof(uint32_t));
    g_free(int_buf);

    return ret;
}

void spapr_numa_write_assoc_nvlink2(void *fdt, int offset, int numa_id,
                                    SpaprPhbState *sphb)
{
    uint32_t associativity[NUMA_ASSOC_SIZE];
    int i;

    associativity[0] = cpu_to_be32(MAX_DISTANCE_REF_POINTS);
    for (i = 1; i < NUMA_ASSOC_SIZE; i++) {
        associativity[i] = cpu_to_be32(numa_id);
    };

    if (sphb->pre_5_1_assoc) {
        associativity[1] = SPAPR_GPU_NUMA_ID;
        associativity[2] = SPAPR_GPU_NUMA_ID;
        associativity[3] = SPAPR_GPU_NUMA_ID;
    }

    _FDT((fdt_setprop(fdt, offset, "ibm,associativity", associativity,
                      sizeof(associativity))));
}

/*
 * Helper that writes ibm,associativity-reference-points and
 * max-associativity-domains in the RTAS pointed by @rtas
 * in the DT @fdt.
 */
void spapr_numa_write_rtas_dt(SpaprMachineState *spapr, void *fdt, int rtas)
{
    SpaprMachineClass *smc = SPAPR_MACHINE_GET_CLASS(spapr);
    uint32_t refpoints[] = {
        cpu_to_be32(0x4),
        cpu_to_be32(0x4),
        cpu_to_be32(0x2),
    };
    uint32_t nr_refpoints = ARRAY_SIZE(refpoints);
    uint32_t maxdomain = cpu_to_be32(spapr->gpu_numa_id > 1 ? 1 : 0);
    uint32_t maxdomains[] = {
        cpu_to_be32(4),
        maxdomain,
        maxdomain,
        maxdomain,
        cpu_to_be32(spapr->gpu_numa_id),
    };

    if (smc->pre_5_1_assoc_refpoints) {
        nr_refpoints = 2;
    }

    _FDT(fdt_setprop(fdt, rtas, "ibm,associativity-reference-points",
                     refpoints, nr_refpoints * sizeof(refpoints[0])));

    _FDT(fdt_setprop(fdt, rtas, "ibm,max-associativity-domains",
                     maxdomains, sizeof(maxdomains)));
}
