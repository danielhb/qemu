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

#ifndef PPC_PMU_BOOK3S_V3_H
#define PPC_PMU_BOOK3S_V3_H

#include "cpu.h"

void PMU_set_freeze_counters(bool fc);
void PMU_set_freeze_PMC5PMC6(bool fc56);

void PMU_instructions_completed(int num_insns);
unsigned long PMU_get_PMC5(void);
unsigned long PMU_get_PMC6(void);
void init_book3s_PMU(void);

#endif /* PPC_MMU_BOOK3S_V3_H */
