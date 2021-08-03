/*
 * PMU register read/write functions for TCG IBM POWER chips
 *
 * Copyright IBM Corp. 2021
 *
 * Authors:
 *  Daniel Henrique Barboza      <danielhb413@gmail.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#if defined(TARGET_PPC64) && !defined(CONFIG_USER_ONLY)

/*
 * Checks whether the Group A SPR (MMCR0, MMCR2, MMCRA, and the
 * PMCs) has problem state read access.
 *
 * Read acccess is granted for all PMCC values but 0b01, where a
 * Facility Unavailable Interrupt will occur.
 */
static bool spr_groupA_read_allowed(DisasContext *ctx)
{
    if (!ctx->mmcr0_pmcc0 && ctx->mmcr0_pmcc1) {
        gen_hvpriv_exception(ctx, POWERPC_EXCP_FU);
        return false;
    }

    return true;
}

/*
 * Checks whether the Group A SPR (MMCR0, MMCR2, MMCRA, and the
 * PMCs) has problem state write access.
 *
 * Write acccess is granted for PMCC values 0b10 and 0b11. Userspace
 * writing with PMCC 0b00 will generate a Hypervisor Emulation
 * Assistance Interrupt. Userspace writing with PMCC 0b01 will
 * generate a Facility Unavailable Interrupt.
 */
static bool spr_groupA_write_allowed(DisasContext *ctx)
{
    if (ctx->mmcr0_pmcc0) {
        return true;
    }

    if (ctx->mmcr0_pmcc1) {
        /* PMCC = 0b01 */
        gen_hvpriv_exception(ctx, POWERPC_EXCP_FU);
    } else {
        /* PMCC = 0b00 */
        gen_hvpriv_exception(ctx, POWERPC_EXCP_INVAL_SPR);
    }

    return false;
}

void spr_read_MMCR0_ureg(DisasContext *ctx, int gprn, int sprn)
{
    TCGv t0;

    if (!spr_groupA_read_allowed(ctx)) {
        return;
    }

    t0 = tcg_temp_new();

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

static void write_MMCR0_common(DisasContext *ctx, TCGv val)
{
    /*
     * helper_store_mmcr0 will make clock based operations that
     * will cause 'bad icount read' errors if we do not execute
     * gen_icount_io_start() beforehand.
     */
    gen_icount_io_start(ctx);
    gen_helper_store_mmcr0(cpu_env, val);

    /*
     * End the translation block because MMCR0 writes can change
     * ctx->pmu_frozen.
     */
    ctx->base.is_jmp = DISAS_EXIT_UPDATE;
}

void spr_write_MMCR0_ureg(DisasContext *ctx, int sprn, int gprn)
{
    TCGv t0, t1;

    if (!spr_groupA_write_allowed(ctx)) {
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

void spr_read_MMCR2_ureg(DisasContext *ctx, int gprn, int sprn)
{
    TCGv t0;

    if (!spr_groupA_read_allowed(ctx)) {
        return;
    }

    t0 = tcg_temp_new();

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

void spr_write_MMCR2_ureg(DisasContext *ctx, int sprn, int gprn)
{
    if (!spr_groupA_write_allowed(ctx)) {
        return;
    }

    spr_write_generic(ctx, SPR_POWER_MMCR2, gprn);
}

void spr_read_PMC14_ureg(DisasContext *ctx, int gprn, int sprn)
{
    if (!spr_groupA_read_allowed(ctx)) {
        return;
    }

    spr_read_ureg(ctx, gprn, sprn);
}

void spr_read_PMC56_ureg(DisasContext *ctx, int gprn, int sprn)
{
    /*
     * If PMCC = 0b11, PMC5 and PMC6 aren't included in the Performance
     * Monitor, and a read attempt results in a Facility Unavailable
     * Interrupt.
     */
    if (ctx->mmcr0_pmcc0 && ctx->mmcr0_pmcc1) {
        gen_hvpriv_exception(ctx, POWERPC_EXCP_FU);
        return;
    }

    /* The remaining steps are similar to PMCs 1-4 userspace read */
    spr_read_PMC14_ureg(ctx, gprn, sprn);
}

void spr_write_PMC(DisasContext *ctx, int sprn, int gprn)
{
    TCGv_i32 t_sprn = tcg_const_i32(sprn);

    gen_icount_io_start(ctx);
    gen_helper_store_pmc(cpu_env, t_sprn, cpu_gpr[gprn]);

    tcg_temp_free_i32(t_sprn);
}

void spr_write_PMC14_ureg(DisasContext *ctx, int sprn, int gprn)
{
    if (!spr_groupA_write_allowed(ctx)) {
        return;
    }

    spr_write_PMC(ctx, sprn + 0x10, gprn);
}

void spr_write_PMC56_ureg(DisasContext *ctx, int sprn, int gprn)
{
    /*
     * If PMCC = 0b11, PMC5 and PMC6 aren't included in the Performance
     * Monitor, and a write attempt results in a Facility Unavailable
     * Interrupt.
     */
    if (ctx->mmcr0_pmcc0 && ctx->mmcr0_pmcc1) {
        gen_hvpriv_exception(ctx, POWERPC_EXCP_FU);
        return;
    }

    /* The remaining steps are similar to PMCs 1-4 userspace write */
    spr_write_PMC14_ureg(ctx, sprn, gprn);
}
#else
void spr_read_MMCR0_ureg(DisasContext *ctx, int gprn, int sprn)
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

void spr_read_MMCR2_ureg(DisasContext *ctx, int gprn, int sprn)
{
    spr_read_ureg(ctx, gprn, sprn);
}

void spr_write_MMCR2_ureg(DisasContext *ctx, int sprn, int gprn)
{
    spr_noaccess(ctx, gprn, sprn);
}

void spr_read_PMC14_ureg(DisasContext *ctx, int gprn, int sprn)
{
    spr_read_ureg(ctx, gprn, sprn);
}

void spr_read_PMC56_ureg(DisasContext *ctx, int gprn, int sprn)
{
    spr_read_ureg(ctx, gprn, sprn);
}

void spr_write_PMC14_ureg(DisasContext *ctx, int sprn, int gprn)
{
    spr_noaccess(ctx, gprn, sprn);
}

void spr_write_PMC56_ureg(DisasContext *ctx, int sprn, int gprn)
{
    spr_noaccess(ctx, gprn, sprn);
}

void spr_write_PMC(DisasContext *ctx, int sprn, int gprn)
{
    spr_write_generic(ctx, sprn, gprn);
}
#endif /* defined(TARGET_PPC64) && !defined(CONFIG_USER_ONLY) */
