// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2024 - 2025 Dang Huynh <dang.huynh@mainlining.org>
 */

#include <display.h>
#include <dm.h>
#include <dw_hdmi.h>
#include <edid.h>
#include <asm/arch-rockchip/hardware.h>
#include <asm/arch-rockchip/grf_rk3568.h>
#include "rk_hdmi.h"

#define RK3568_IO_DDC_IN_MSK             ((3 << 14) | (3 << (14 + 16)))

static int rk3568_hdmi_enable(struct udevice *dev, int panel_bpp,
			      const struct display_timing *edid)
{
	struct rk_hdmi_priv *priv = dev_get_priv(dev);

	return dw_hdmi_enable(&priv->hdmi, edid);
}

static int rk3568_hdmi_of_to_plat(struct udevice *dev)
{
	struct rk_hdmi_priv *priv = dev_get_priv(dev);
	struct dw_hdmi *hdmi = &priv->hdmi;

	hdmi->i2c_clk_high = 0x6c;
	hdmi->i2c_clk_low = 0x7d;

	return rk_hdmi_of_to_plat(dev);
}

static const char * const rk3568_hdmi_reg_names[] = {
	"avdd-0v9",
	"avdd-1v8"
};

static int rk3568_hdmi_probe(struct udevice *dev)
{
	struct rk_hdmi_priv *priv = dev_get_priv(dev);
	struct rk3568_grf *grf = priv->grf;

	rk_hdmi_probe_regulators(dev, rk3568_hdmi_reg_names,
				 ARRAY_SIZE(rk3568_hdmi_reg_names));

	writel(RK3568_IO_DDC_IN_MSK, &grf->vo_con1);

	return rk_hdmi_probe(dev);
}

static const struct dm_display_ops rk3568_hdmi_ops = {
	.read_edid = rk_hdmi_read_edid,
	.enable = rk3568_hdmi_enable,
};

static const struct udevice_id rk3568_hdmi_ids[] = {
	{ .compatible = "rockchip,rk3568-dw-hdmi" },
	{ }
};

U_BOOT_DRIVER(rk3568_hdmi_rockchip) = {
	.name = "rk3568_hdmi_rockchip",
	.id = UCLASS_DISPLAY,
	.of_match = rk3568_hdmi_ids,
	.ops = &rk3568_hdmi_ops,
	.of_to_plat = rk3568_hdmi_of_to_plat,
	.probe = rk3568_hdmi_probe,
	.priv_auto = sizeof(struct rk_hdmi_priv),
};
