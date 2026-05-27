// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025 Dang Huynh <dang.huynh@mainlining.org>
 *
 * Based on rk3568_vop.c and the barebox RK3588 VOP2 driver.
 */

#include <clk.h>
#include <display.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <regmap.h>
#include <syscon.h>
#include <video.h>
#include <asm/arch-rockchip/clock.h>
#include <asm/arch-rockchip/hardware.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <linux/bitfield.h>
#include "rk_vop2.h"

DECLARE_GLOBAL_DATA_PTR;

/* RK3588 DSP_IF_EN register bits */
#define RK3588_SYS_DSP_INFACE_EN_EDP_HDMI0_MUX		GENMASK(17, 16)
#define RK3588_SYS_DSP_INFACE_EN_EDP_HDMI1_MUX		GENMASK(19, 18)
#define RK3588_SYS_DSP_INFACE_EN_MIPI0_MUX		BIT(20)
#define RK3588_SYS_DSP_INFACE_EN_MIPI1_MUX		GENMASK(22, 21)
#define RK3588_SYS_DSP_INFACE_EN_DP0_MUX		GENMASK(13, 12)
#define RK3588_SYS_DSP_INFACE_EN_DP1_MUX		GENMASK(15, 14)

#define RK3588_SYS_DSP_INFACE_EN_DP0			BIT(0)
#define RK3588_SYS_DSP_INFACE_EN_DP1			BIT(1)
#define RK3588_SYS_DSP_INFACE_EN_EDP0			BIT(2)
#define RK3588_SYS_DSP_INFACE_EN_HDMI0			BIT(3)
#define RK3588_SYS_DSP_INFACE_EN_EDP1			BIT(4)
#define RK3588_SYS_DSP_INFACE_EN_HDMI1			BIT(5)
#define RK3588_SYS_DSP_INFACE_EN_MIPI0			BIT(6)
#define RK3588_SYS_DSP_INFACE_EN_MIPI1			BIT(7)

/* RK3588 DSP_IF_CTRL register bits (clock dividers) */
#define RK3588_DSP_IF_EDP_HDMI0_DCLK_DIV		GENMASK(17, 16)
#define RK3588_DSP_IF_EDP_HDMI0_PCLK_DIV		BIT(18)
#define RK3588_DSP_IF_EDP_HDMI1_DCLK_DIV		GENMASK(21, 20)
#define RK3588_DSP_IF_EDP_HDMI1_PCLK_DIV		BIT(22)
#define RK3588_DSP_IF_MIPI0_PCLK_DIV			GENMASK(25, 24)
#define RK3588_DSP_IF_MIPI1_PCLK_DIV			GENMASK(27, 26)

/* RK3588 DSP_IF_POL register bits */
#define RK3588_DSP_IF_POL_DP0_PIN_POL			GENMASK(10, 8)
#define RK3588_DSP_IF_POL_DP1_PIN_POL			GENMASK(14, 12)
#define RK3588_DSP_IF_POL_CFG_DONE_IMD			BIT(28)

/* RK3588 VP_CLK_CTRL register bits */
#define RK3588_VP_CLK_CTRL_DCLK_CORE_DIV		GENMASK(1, 0)
#define RK3588_VP_CLK_CTRL_DCLK_OUT_DIV		GENMASK(3, 2)

/* RK3588 power domain control */
#define RK3588_SYS_PD_CTRL				0x034
#define VOP2_PD_CLUSTER0				BIT(0)
#define VOP2_PD_CLUSTER1				BIT(1)
#define VOP2_PD_CLUSTER2				BIT(2)
#define VOP2_PD_CLUSTER3				BIT(3)
#define VOP2_PD_ESMART					BIT(7)

/* RK3588 VOP GRF registers */
#define RK3588_GRF_VOP_CON2				0x08
#define RK3588_GRF_VO1_CON0				0x00

/* Register offsets within VOP2 sys ctrl block */
#define RK3588_DSP_IF_EN				0x028
#define RK3588_DSP_IF_CTRL				0x02c
#define RK3588_DSP_IF_POL				0x030
#define RK3588_VP_CLK_CTRL_OFFSET			0x0c

/* HIWORD_UPDATE: set bits [h:l] to v, with write mask in upper 16 bits */
#define HIWORD_UPDATE(v, h, l)	((GENMASK(h, l) << 16) | ((v) << (l)))

static void rk3588_power_domain_enable_all(struct rk_vop2_priv *priv)
{
	u32 pd;

	pd = readl(priv->regs + RK3588_SYS_PD_CTRL);
	pd &= ~(VOP2_PD_CLUSTER0 | VOP2_PD_CLUSTER1 | VOP2_PD_CLUSTER2 |
		 VOP2_PD_CLUSTER3 | VOP2_PD_ESMART);
	writel(pd, priv->regs + RK3588_SYS_PD_CTRL);
}

static unsigned long rk3588_calc_dclk(unsigned long child_clk,
				      unsigned long max_dclk)
{
	if (child_clk * 4 <= max_dclk)
		return child_clk * 4;
	else if (child_clk * 2 <= max_dclk)
		return child_clk * 2;
	else if (child_clk <= max_dclk)
		return child_clk;
	else
		return 0;
}

static int ilog2_simple(unsigned int v)
{
	int r = 0;

	while (v >>= 1)
		r++;
	return r;
}

/*
 * Calculate the RK3588 clock configuration for a given output interface.
 * Returns the dclk rate to request from the CRU, or 0 on error.
 */
static unsigned long rk3588_calc_cru_cfg(enum vop_modes mode, u32 pixclk,
					 int *dclk_core_div, int *dclk_out_div,
					 int *if_pixclk_div, int *if_dclk_div)
{
	unsigned long dclk_core_rate = pixclk >> 2;
	unsigned long dclk_rate = pixclk;
	unsigned long dclk_out_rate;
	unsigned long if_pixclk_rate;
	int K = 1;

	*dclk_out_div = 0;
	*if_pixclk_div = 0;
	*if_dclk_div = 0;

	switch (mode) {
	case VOP_MODE_HDMI:
		*if_pixclk_div = 2;
		*if_dclk_div = 4;
		break;
	case VOP_MODE_EDP:
		if_pixclk_rate = pixclk / K;
		dclk_rate = if_pixclk_rate * K;
		*if_pixclk_div = K;
		*if_dclk_div = K;
		break;
	case VOP_MODE_DP:
		dclk_out_rate = pixclk >> 2;
		dclk_rate = rk3588_calc_dclk(dclk_out_rate, 600000000UL);
		if (!dclk_rate)
			return 0;
		*dclk_out_div = dclk_rate / dclk_out_rate;
		break;
	case VOP_MODE_MIPI:
		dclk_out_rate = dclk_core_rate / K;
		dclk_rate = rk3588_calc_dclk(dclk_out_rate, 600000000UL);
		if (!dclk_rate)
			return 0;
		*dclk_out_div = dclk_rate / dclk_out_rate;
		*if_pixclk_div = 1;
		break;
	default:
		dclk_rate = pixclk;
		break;
	}

	*dclk_core_div = dclk_rate / dclk_core_rate;
	*if_pixclk_div = ilog2_simple(*if_pixclk_div);
	*if_dclk_div = ilog2_simple(*if_dclk_div);
	*dclk_core_div = ilog2_simple(*dclk_core_div);
	*dclk_out_div = ilog2_simple(*dclk_out_div);

	return dclk_rate;
}

static void rk3588_enable_output(struct udevice *dev,
				 enum vop_modes mode, u32 port)
{
	struct rk_vop2_priv *priv = dev_get_priv(dev);
	void *regs = priv->regs;
	u32 die, dip, div, vp_clk_div;
	int dclk_core_div = 0, dclk_out_div = 0;
	int if_pixclk_div = 0, if_dclk_div = 0;
	unsigned long dclk_rate;
	int ep = priv->output_ep;

	dclk_rate = rk3588_calc_cru_cfg(mode, priv->pixclock,
					&dclk_core_div, &dclk_out_div,
					&if_pixclk_div, &if_dclk_div);
	if (!dclk_rate) {
		debug("%s: failed to calculate clock config\n", __func__);
		return;
	}

	vp_clk_div = FIELD_PREP(RK3588_VP_CLK_CTRL_DCLK_CORE_DIV, dclk_core_div);
	vp_clk_div |= FIELD_PREP(RK3588_VP_CLK_CTRL_DCLK_OUT_DIV, dclk_out_div);

	die = readl(regs + RK3588_DSP_IF_EN);
	dip = readl(regs + RK3588_DSP_IF_POL);
	div = readl(regs + RK3588_DSP_IF_CTRL);

	switch (mode) {
	case VOP_MODE_EDP:
		if (ep == 9) {
			/* eDP1: shares clock path with HDMI1 */
			div &= ~RK3588_DSP_IF_EDP_HDMI1_DCLK_DIV;
			div &= ~RK3588_DSP_IF_EDP_HDMI1_PCLK_DIV;
			div |= FIELD_PREP(RK3588_DSP_IF_EDP_HDMI1_DCLK_DIV, if_dclk_div);
			div |= FIELD_PREP(RK3588_DSP_IF_EDP_HDMI1_PCLK_DIV, if_pixclk_div);
			die &= ~RK3588_SYS_DSP_INFACE_EN_EDP_HDMI1_MUX;
			die |= RK3588_SYS_DSP_INFACE_EN_EDP1 |
			       FIELD_PREP(RK3588_SYS_DSP_INFACE_EN_EDP_HDMI1_MUX, port);
			if (priv->vop_grf)
				writel(HIWORD_UPDATE(1, 3, 3),
				       priv->vop_grf + RK3588_GRF_VOP_CON2);
		} else {
			/* eDP0: shares clock path with HDMI0 */
			div &= ~RK3588_DSP_IF_EDP_HDMI0_DCLK_DIV;
			div &= ~RK3588_DSP_IF_EDP_HDMI0_PCLK_DIV;
			div |= FIELD_PREP(RK3588_DSP_IF_EDP_HDMI0_DCLK_DIV, if_dclk_div);
			div |= FIELD_PREP(RK3588_DSP_IF_EDP_HDMI0_PCLK_DIV, if_pixclk_div);
			die &= ~RK3588_SYS_DSP_INFACE_EN_EDP_HDMI0_MUX;
			die |= RK3588_SYS_DSP_INFACE_EN_EDP0 |
			       FIELD_PREP(RK3588_SYS_DSP_INFACE_EN_EDP_HDMI0_MUX, port);
			if (priv->vop_grf)
				writel(HIWORD_UPDATE(1, 0, 0),
				       priv->vop_grf + RK3588_GRF_VOP_CON2);
		}
		break;

	case VOP_MODE_HDMI:
		if (ep == 8) {
			/* HDMI1: shares clock path with eDP1 */
			div &= ~RK3588_DSP_IF_EDP_HDMI1_DCLK_DIV;
			div &= ~RK3588_DSP_IF_EDP_HDMI1_PCLK_DIV;
			div |= FIELD_PREP(RK3588_DSP_IF_EDP_HDMI1_DCLK_DIV, if_dclk_div);
			div |= FIELD_PREP(RK3588_DSP_IF_EDP_HDMI1_PCLK_DIV, if_pixclk_div);
			die &= ~RK3588_SYS_DSP_INFACE_EN_EDP_HDMI1_MUX;
			die |= RK3588_SYS_DSP_INFACE_EN_HDMI1 |
			       FIELD_PREP(RK3588_SYS_DSP_INFACE_EN_EDP_HDMI1_MUX, port);
			if (priv->vop_grf)
				writel(HIWORD_UPDATE(1, 4, 4),
				       priv->vop_grf + RK3588_GRF_VOP_CON2);
		} else {
			/* HDMI0: shares clock path with eDP0 */
			div &= ~RK3588_DSP_IF_EDP_HDMI0_DCLK_DIV;
			div &= ~RK3588_DSP_IF_EDP_HDMI0_PCLK_DIV;
			div |= FIELD_PREP(RK3588_DSP_IF_EDP_HDMI0_DCLK_DIV, if_dclk_div);
			div |= FIELD_PREP(RK3588_DSP_IF_EDP_HDMI0_PCLK_DIV, if_pixclk_div);
			die &= ~RK3588_SYS_DSP_INFACE_EN_EDP_HDMI0_MUX;
			die |= RK3588_SYS_DSP_INFACE_EN_HDMI0 |
			       FIELD_PREP(RK3588_SYS_DSP_INFACE_EN_EDP_HDMI0_MUX, port);
			if (priv->vop_grf)
				writel(HIWORD_UPDATE(1, 1, 1),
				       priv->vop_grf + RK3588_GRF_VOP_CON2);
		}
		break;

	case VOP_MODE_MIPI:
		if (ep == 6) {
			/* MIPI1 */
			div &= ~RK3588_DSP_IF_MIPI1_PCLK_DIV;
			div |= FIELD_PREP(RK3588_DSP_IF_MIPI1_PCLK_DIV, if_pixclk_div);
			die &= ~RK3588_SYS_DSP_INFACE_EN_MIPI1_MUX;
			die |= RK3588_SYS_DSP_INFACE_EN_MIPI1;
		} else {
			/* MIPI0 */
			div &= ~RK3588_DSP_IF_MIPI0_PCLK_DIV;
			div |= FIELD_PREP(RK3588_DSP_IF_MIPI0_PCLK_DIV, if_pixclk_div);
			die &= ~RK3588_SYS_DSP_INFACE_EN_MIPI0_MUX;
			die |= RK3588_SYS_DSP_INFACE_EN_MIPI0;
		}
		break;

	case VOP_MODE_DP:
		if (ep == 11) {
			/* DP1 */
			die &= ~RK3588_SYS_DSP_INFACE_EN_DP1_MUX;
			die |= RK3588_SYS_DSP_INFACE_EN_DP1 |
			       FIELD_PREP(RK3588_SYS_DSP_INFACE_EN_DP1_MUX, port);
			dip &= ~RK3588_DSP_IF_POL_DP1_PIN_POL;
		} else {
			/* DP0 */
			die &= ~RK3588_SYS_DSP_INFACE_EN_DP0_MUX;
			die |= RK3588_SYS_DSP_INFACE_EN_DP0 |
			       FIELD_PREP(RK3588_SYS_DSP_INFACE_EN_DP0_MUX, port);
			dip &= ~RK3588_DSP_IF_POL_DP0_PIN_POL;
		}
		break;

	default:
		debug("%s: unsupported output mode %x\n", __func__, mode);
		return;
	}

	dip |= RK3588_DSP_IF_POL_CFG_DONE_IMD;

	/* Write VP clock divider to the video port's CLK_CTRL register */
	writel(vp_clk_div, regs + VOP2_POST_OFFSET(port) + RK3588_VP_CLK_CTRL_OFFSET);
	writel(die, regs + RK3588_DSP_IF_EN);
	writel(div, regs + RK3588_DSP_IF_CTRL);
	writel(dip, regs + RK3588_DSP_IF_POL);
}

static void rk3588_set_pin_polarity(struct udevice *dev,
				    enum vop_modes mode, u32 polarity)
{
	/*
	 * On RK3588, pin polarity for eDP/HDMI is handled via GRF registers
	 * rather than the VOP DSP_IF_POL register. For eDP the polarity is
	 * set by the eDP PHY/controller itself.
	 */
}

static int rk3588_vop_initialize(struct udevice *dev)
{
	struct rk_vop2_priv *priv = dev_get_priv(dev);
	struct rk3568_vop_sysctrl *sysctrl = priv->regs + VOP2_SYSREG_OFFSET;

	/* Enable all power domains */
	rk3588_power_domain_enable_all(priv);

	writel(M_GLOBAL_REGDONE, &sysctrl->reg_cfg_done);

	/* Disable auto gating */
	clrsetbits_le32(&sysctrl->autogating_ctrl, M_AUTO_GATING, V_AUTO_GATING(0));

	return 0;
}

static int rk3588_vop_remove(struct udevice *dev)
{
	if (CONFIG_IS_ENABLED(VIDEO_REMOVE)) {
		struct rk_vop2_priv *priv = dev_get_priv(dev);
		struct rk3568_vop_sysctrl *sysctrl = priv->regs + VOP2_SYSREG_OFFSET;
		struct rk3568_vop_esmart *esmart = priv->regs + VOP2_ESMART_OFFSET(priv->layer - 4);

		debug("Removing RK3588 VOP2 driver (vp=%d, layer=%d)\n", priv->vp, priv->layer);

		writel(0, &esmart->esmart_region0_mst_ctl);

		writel(M_GLOBAL_REGDONE | M_LOAD_GLOBAL(priv->vp) | M_LOAD_GLOBAL(priv->vp) << 16,
		       &sysctrl->reg_cfg_done);
	}

	return 0;
}

static int rk3588_vop_probe(struct udevice *dev)
{
	struct rk_vop2_priv *priv = dev_get_priv(dev);
	int ret;

	/* Before relocation we don't need to do anything */
	if (!(gd->flags & GD_FLG_RELOC))
		return 0;

	priv->vop_grf = syscon_get_first_range(ROCKCHIP_SYSCON_VOP_GRF);
	if (IS_ERR(priv->vop_grf)) {
		debug("%s: vop_grf syscon not found\n", __func__);
		priv->vop_grf = NULL;
	}

	ret = rk3588_vop_initialize(dev);
	if (ret)
		return ret;

	return rk_vop2_probe(dev);
}

struct rkvop2_platdata rk3588_platdata = {
	.delay = 26,
	.bg_dly = {60, 58, 58, 56},
	/* Use Esmart2 for VP0, Esmart3 for VP1, Esmart0 for VP2, Esmart1 for VP3 */
	.vp_lyr = {6, 7, 4, 5},
	.layers = {ROCKCHIP_VOP2_CLUSTER0, ROCKCHIP_VOP2_CLUSTER1,
		   ROCKCHIP_VOP2_CLUSTER2, ROCKCHIP_VOP2_CLUSTER3,
		   ROCKCHIP_VOP2_ESMART0, ROCKCHIP_VOP2_ESMART1,
		   ROCKCHIP_VOP2_ESMART2, ROCKCHIP_VOP2_ESMART3},
};

struct rkvop2_driverdata rk3588_driverdata = {
	.features = VOP_FEATURE_OUTPUT_10BIT,
	.set_pin_polarity = rk3588_set_pin_polarity,
	.enable_output = rk3588_enable_output,
	.platdata = &rk3588_platdata,
};

static const struct udevice_id rk3588_vop_ids[] = {
	{ .compatible = "rockchip,rk3588-vop",
	  .data = (ulong)&rk3588_driverdata },
	{ }
};

static const struct video_ops rk3588_vop_ops = {
};

U_BOOT_DRIVER(rk3588_vop) = {
	.name	= "rk3588_vop",
	.id	= UCLASS_VIDEO,
	.of_match = rk3588_vop_ids,
	.ops	= &rk3588_vop_ops,
	.bind	= rk_vop2_bind,
	.probe	= rk3588_vop_probe,
	.remove = rk3588_vop_remove,
	.priv_auto = sizeof(struct rk_vop2_priv),
#if CONFIG_IS_ENABLED(VIDEO_REMOVE)
	.flags = DM_FLAG_PRE_RELOC | DM_FLAG_OS_PREPARE,
#else
	.flags = DM_FLAG_PRE_RELOC,
#endif
};
