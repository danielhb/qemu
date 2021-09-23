/*
 * Power ISA decode for branch instructions
 *
 *  Copyright IBM Corp. 2021
 *
 * Authors:
 *  Daniel Henrique Barboza      <danielhb413@gmail.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#if defined(TARGET_PPC64) && !defined(CONFIG_USER_ONLY)
void spr_write_MMCR0(DisasContext *ctx, int sprn, int gprn)
{
    /*
     * helper_store_mmcr0 will make clock based operations that
     * will cause 'bad icount read' errors if we do not execute
     * gen_icount_io_start() beforehand.
     */
    gen_icount_io_start(ctx);
    gen_helper_store_mmcr0(cpu_env, cpu_gpr[gprn]);

    /*
     * End the translation block because MMCR0 writes can change
     * ctx->pmu_frozen.
     */
    ctx->base.is_jmp = DISAS_EXIT_UPDATE;
}

void spr_write_PMC(DisasContext *ctx, int sprn, int gprn)
{
    TCGv_i32 t_sprn = tcg_const_i32(sprn);

    gen_icount_io_start(ctx);
    gen_helper_store_pmc(cpu_env, t_sprn, cpu_gpr[gprn]);

    tcg_temp_free_i32(t_sprn);
}
#else
void spr_write_MMCR0(DisasContext *ctx, int sprn, int gprn)
{
    spr_write_generic(ctx, sprn, gprn);
}
void spr_write_PMC(DisasContext *ctx, int sprn, int gprn)
{
    spr_write_generic(ctx, sprn, gprn);
}
#endif

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
    tcg_gen_andi_tl(t0, t0, 0x4020100804020000UL);
    tcg_gen_mov_tl(cpu_gpr[gprn], t0);

    tcg_temp_free(t0);
}

void spr_write_CTRL(DisasContext *ctx, int sprn, int gprn)
{
    spr_write_generic(ctx, sprn, gprn);

    /*
     * Write in SPR_CTRL must force a new translation block,
     * allowing the PMU to calculate the run latch events with
     * more accuracy.
     */
    ctx->base.is_jmp = DISAS_EXIT_UPDATE;
}

#if defined(TARGET_PPC64) && !defined(CONFIG_USER_ONLY)
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

    /* Overwrite cpu_gpr[gprn] and use spr_write_MMCR0() */
    tcg_gen_mov_tl(cpu_gpr[gprn], t1);
    spr_write_MMCR0(ctx, sprn + 0x10, gprn);

    tcg_temp_free(t0);
    tcg_temp_free(t1);
}

void spr_write_PMC_ureg(DisasContext *ctx, int sprn, int gprn)
{
    /*
     * All PMCs belongs to Group A SPRs and can't be written by
     * userspace if PMCC = 0b00.
     */
    if (ctx->pmcc_clear) {
        gen_hvpriv_exception(ctx, POWERPC_EXCP_INVAL_SPR);
        return;
    }

    spr_write_PMC(ctx, sprn + 0x10, gprn);
}
#else
void spr_write_MMCR0_ureg(DisasContext *ctx, int sprn, int gprn)
{
    spr_noaccess(ctx, gprn, sprn);
}

void spr_write_PMC_ureg(DisasContext *ctx, int sprn, int gprn)
{
    spr_noaccess(ctx, gprn, sprn);
}
#endif /* defined(TARGET_PPC64) && !defined(CONFIG_USER_ONLY) */
