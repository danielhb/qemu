/*
 * PMU register read/write functions TCG IBM POWER chips
 *
 * Copyright IBM Corp. 2021
 *
 * Authors:
 *  Daniel Henrique Barboza      <danielhb413@gmail.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#define MMCR2_UREG_MASK 0x4020100804020000UL

#if defined(TARGET_PPC64) && !defined(CONFIG_USER_ONLY)
void spr_read_MMCR0_ureg(DisasContext *ctx, int gprn, int sprn)
{
    TCGv t0 = tcg_temp_new();

    /*
     * Filter out all bits but FC, PMAO, and PMAE, according
     * to ISA v3.1, in 10.4.4 Monitor Mode Control Register 0,
     * fourth paragraph.
     */
    gen_load_spr(t0, SPR_POWER_MMCR0);
    tcg_gen_andi_tl(t0, t0, MMCR0_UREG_MASK);
    tcg_gen_mov_tl(cpu_gpr[gprn], t0);

    tcg_temp_free(t0);
}

void spr_read_MMCR2_ureg(DisasContext *ctx, int gprn, int sprn)
{
    TCGv t0 = tcg_temp_new();

    /*
     * On read, filter out all bits that are not FCnP0 bits.
     * When MMCR0[PMCC] is set to 0b10 or 0b11, providing
     * problem state programs read/write access to MMCR2,
     * only the FCnP0 bits can be accessed. All other bits are
     * not changed when mtspr is executed in problem state, and
     * all other bits return 0s when mfspr is executed in problem
     * state, according to ISA v3.1, section 10.4.6 Monitor Mode
     * Control Register 2, p. 1316, third paragraph.
     */
    gen_load_spr(t0, SPR_POWER_MMCR2);
    tcg_gen_andi_tl(t0, t0, MMCR2_UREG_MASK);
    tcg_gen_mov_tl(cpu_gpr[gprn], t0);

    tcg_temp_free(t0);
}

static void write_MMCR0_common(DisasContext *ctx, TCGv val)
{
    /*
     * helper_store_mmcr0 will make clock based operations that
     * will cause 'bad icount read' errors if we do not execute
     * gen_icount_io_start() beforehand.
     */
    gen_icount_io_start(ctx);
    gen_helper_store_mmcr0(cpu_env, val);
}

void spr_write_MMCR0_ureg(DisasContext *ctx, int sprn, int gprn)
{
    TCGv t0, t1;

    /*
     * For group A PMU sprs, if PMCC = 0b00, PowerISA v3.1
     * dictates that:
     *
     * "If an attempt is made to write to an SPR in group A in
     * problem state, a Hypervisor Emulation Assistance
     * interrupt will occur."
     *
     * MMCR0 is a Group A SPR and can't be written by userspace
     * if PMCC = 0b00.
     */
    if (ctx->pmcc_clear) {
        gen_hvpriv_exception(ctx, POWERPC_EXCP_INVAL_SPR);
        return;
    }

    t0 = tcg_temp_new();
    t1 = tcg_temp_new();

    /*
     * Filter out all bits but FC, PMAO, and PMAE, according
     * to ISA v3.1, in 10.4.4 Monitor Mode Control Register 0,
     * fourth paragraph.
     */
    tcg_gen_andi_tl(t0, cpu_gpr[gprn], MMCR0_UREG_MASK);
    gen_load_spr(t1, SPR_POWER_MMCR0);
    tcg_gen_andi_tl(t1, t1, ~(MMCR0_UREG_MASK));
    /* Keep all other bits intact */
    tcg_gen_or_tl(t1, t1, t0);
    write_MMCR0_common(ctx, t1);

    tcg_temp_free(t0);
    tcg_temp_free(t1);
}

void spr_write_MMCR0(DisasContext *ctx, int sprn, int gprn)
{
    write_MMCR0_common(ctx, cpu_gpr[gprn]);
}
#else
void spr_read_MMCR0_ureg(DisasContext *ctx, int gprn, int sprn)
{
    spr_read_ureg(ctx, gprn, sprn);
}

void spr_read_MMCR2_ureg(DisasContext *ctx, int gprn, int sprn)
{
    spr_read_ureg(ctx, gprn, sprn);
}

void spr_write_MMCR0_ureg(DisasContext *ctx, int sprn, int gprn)
{
    spr_noaccess(ctx, gprn, sprn);
}

void spr_write_MMCR0(DisasContext *ctx, int sprn, int gprn)
{
    spr_write_generic(ctx, sprn, gprn);
}
#endif /* defined(TARGET_PPC64) && !defined(CONFIG_USER_ONLY) */
