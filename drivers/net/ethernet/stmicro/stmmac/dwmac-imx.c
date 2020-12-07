// SPDX-License-Identifier: GPL-2.0
/*
 * dwmac-imx.c - DWMAC Specific Glue layer for NXP imx8
 *
 * Copyright 2020 NXP
 *
 */

#include <linux/clk.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_net.h>
#include <linux/phy.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeirq.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/stmmac.h>

#include "stmmac_platform.h"

#define GPR_ENET_QOS_INTF_MODE_MASK	GENMASK(21, 16)
#define GPR_ENET_QOS_INTF_SEL_MII	(0x0 << 16)
#define GPR_ENET_QOS_INTF_SEL_RMII	(0x4 << 16)
#define GPR_ENET_QOS_INTF_SEL_RGMII	(0x1 << 16)
#define GPR_ENET_QOS_CLK_GEN_EN		(0x1 << 19)
#define GPR_ENET_QOS_RGMII_EN		(0x1 << 21)

struct imx_dwmac_ops {
	u32 addr_width;
	bool mac_txclk_auto_adj;

	int (*set_intf_mode)(struct plat_stmmacenet_data *plat_dat);
	int (*set_stop_mode)(struct plat_stmmacenet_data *plat_dat, bool is_en);
};

struct imx_priv_data {
	struct device *dev;
	struct clk *clk_tx;
	u32 intf_reg_off;
	struct regmap *intf_regmap;

	const struct imx_dwmac_ops *ops;
	struct plat_stmmacenet_data *plat_dat;
};

static int imx8mp_set_intf_mode(struct plat_stmmacenet_data *plat_dat)
{
	struct imx_priv_data *dwmac = plat_dat->bsp_priv;
	int val;

	switch (plat_dat->interface) {
	case PHY_INTERFACE_MODE_MII:
		val = GPR_ENET_QOS_INTF_SEL_MII;
		break;
	case PHY_INTERFACE_MODE_RMII:
		val = GPR_ENET_QOS_INTF_SEL_RMII;
		break;
	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_RGMII_ID:
	case PHY_INTERFACE_MODE_RGMII_RXID:
	case PHY_INTERFACE_MODE_RGMII_TXID:
		val = GPR_ENET_QOS_INTF_SEL_RGMII |
		      GPR_ENET_QOS_CLK_GEN_EN |
		      GPR_ENET_QOS_RGMII_EN;
		break;
	default:
		pr_debug("imx dwmac doesn't support %d interface\n",
			 plat_dat->interface);
		return -EINVAL;
	}

	return regmap_update_bits(dwmac->intf_regmap, dwmac->intf_reg_off,
				  GPR_ENET_QOS_INTF_MODE_MASK, val);
};

static int
imx8dxl_set_intf_mode(struct plat_stmmacenet_data *plat_dat)
{
	/* TBD */
	return 0;
}

static int
imx8mp_set_stop_mode(struct plat_stmmacenet_data *plat_dat, bool is_en)
{
	/* TBD */
	return 0;
};

static int
imx8dxl_set_stop_mode(struct plat_stmmacenet_data *plat_dat, bool is_en)
{
	/* TBD */
	return 0;
};

static int imx_dwmac_init(struct platform_device *pdev, void *priv)
{
	struct imx_priv_data *dwmac = priv;
	struct plat_stmmacenet_data *plat_dat = dwmac->plat_dat;
	int ret;

	ret = clk_prepare_enable(dwmac->clk_tx);
	if (ret) {
		dev_err(&pdev->dev, "tx clock enable failed\n");
		return ret;
	}

	if (dwmac->ops->set_stop_mode) {
		ret = dwmac->ops->set_stop_mode(plat_dat, false);
		if (ret)
			goto stop_mode_failed;
	}

	if (dwmac->ops->set_intf_mode) {
		ret = dwmac->ops->set_intf_mode(plat_dat);
		if (ret)
			goto intf_mode_failed;
	}

	return 0;

intf_mode_failed:
stop_mode_failed:
	clk_disable_unprepare(dwmac->clk_tx);
	return ret;
}

static void imx_dwmac_exit(struct platform_device *pdev, void *priv)
{
	struct imx_priv_data *dwmac = priv;
	struct plat_stmmacenet_data *plat_dat = dwmac->plat_dat;
	int ret;

	if (dwmac->ops->set_stop_mode) {
		ret = dwmac->ops->set_stop_mode(plat_dat, true);
		if (ret) {
			dev_err(dwmac->dev, "enter stop mode failed %d\n", ret);
			return;
		}
	}

	if (dwmac->clk_tx)
		clk_disable_unprepare(dwmac->clk_tx);
}

static void imx_dwmac_fix_speed(void *priv, unsigned int speed)
{
	struct imx_priv_data *dwmac = priv;
	unsigned long rate;
	int err;

	if (dwmac->ops->mac_txclk_auto_adj)
		return;

	switch (speed) {
	case SPEED_1000:
		rate = 125000000;
		break;
	case SPEED_100:
		rate = 25000000;
		break;
	case SPEED_10:
		rate = 2500000;
		break;
	default:
		dev_err(dwmac->dev, "invalid speed %u\n", speed);
		return;
	}

	err = clk_set_rate(dwmac->clk_tx, rate);
	if (err < 0)
		dev_err(dwmac->dev, "failed to set tx rate %lu\n", rate);
}

static int
imx_dwmac_parse_dt(struct imx_priv_data *dwmac, struct device *dev)
{
	struct device_node *np = dev->of_node;
	int err;

	dwmac->clk_tx = devm_clk_get(dev, "tx");
	if (IS_ERR(dwmac->clk_tx)) {
		dev_err(dev, "failed to get tx clock\n");
		return PTR_ERR(dwmac->clk_tx);
	}

	dwmac->intf_regmap = syscon_regmap_lookup_by_phandle(np, "intf_mode");
	if (IS_ERR(dwmac->intf_regmap))
		return PTR_ERR(dwmac->intf_regmap);

	err = of_property_read_u32_index(np, "intf_mode", 1, &dwmac->intf_reg_off);
	if (err)
		dev_err(dev, "Can't get intf mode reg offset (%d)\n", err);


	return err;
}

static int imx_dwmac_probe(struct platform_device *pdev)
{
	struct plat_stmmacenet_data *plat_dat;
	struct stmmac_resources stmmac_res;
	struct imx_priv_data *dwmac;
	const struct imx_dwmac_ops *data;
	int ret;

	ret = stmmac_get_platform_resources(pdev, &stmmac_res);
	if (ret)
		return ret;

	dwmac = devm_kzalloc(&pdev->dev, sizeof(*dwmac), GFP_KERNEL);
	if (!dwmac)
		return PTR_ERR(dwmac);

	plat_dat = stmmac_probe_config_dt(pdev, &stmmac_res.mac);
	if (IS_ERR(plat_dat))
		return PTR_ERR(plat_dat);

	data = of_device_get_match_data(&pdev->dev);
	if (!data) {
		dev_err(&pdev->dev, "failed to get match data\n");
		ret = -EINVAL;
		goto err_match_data;
	}

	dwmac->ops = data;
	dwmac->dev = &pdev->dev;

	ret = imx_dwmac_parse_dt(dwmac, &pdev->dev);
	if (ret) {
		dev_err(&pdev->dev, "failed to parse OF data\n");
		goto err_parse_dt;
	}

	ret = dma_set_mask_and_coherent(&pdev->dev,
					DMA_BIT_MASK(dwmac->ops->addr_width));
	if (ret) {
		dev_err(&pdev->dev, "DMA mask set failed\n");
		goto err_dma_mask;
	}

	plat_dat->init = imx_dwmac_init;
	plat_dat->exit = imx_dwmac_exit;
	plat_dat->fix_mac_speed = imx_dwmac_fix_speed;
	plat_dat->bsp_priv = dwmac;
	dwmac->plat_dat = plat_dat;

	ret = imx_dwmac_init(pdev, dwmac);
	if (ret)
		goto err_dwmac_init;

	ret = stmmac_dvr_probe(&pdev->dev, plat_dat, &stmmac_res);
	if (ret)
		goto err_drv_probe;

	return 0;

err_dwmac_init:
err_drv_probe:
	imx_dwmac_exit(pdev, plat_dat->bsp_priv);
err_dma_mask:
err_parse_dt:
err_match_data:
	stmmac_remove_config_dt(pdev, plat_dat);
	return ret;
}

static struct imx_dwmac_ops imx8mp_dwmac_data = {
	.addr_width = 34,
	.mac_txclk_auto_adj = false,
	.set_intf_mode = imx8mp_set_intf_mode,
	.set_stop_mode = imx8mp_set_stop_mode,
};

static struct imx_dwmac_ops imx8dxl_dwmac_data = {
	.addr_width = 32, /* for bringup */
	.mac_txclk_auto_adj = true,
	.set_intf_mode = imx8dxl_set_intf_mode,
	.set_stop_mode = imx8dxl_set_stop_mode,
};

static const struct of_device_id imx_dwmac_match[] = {
	{ .compatible = "nxp,imx8mp-dwmac-eqos", .data = &imx8mp_dwmac_data },
	{ .compatible = "nxp,imx8dxl-dwmac-eqos", .data = &imx8dxl_dwmac_data },
	{ }
};
MODULE_DEVICE_TABLE(of, imx_dwmac_match);

static struct platform_driver imx_dwmac_driver = {
	.probe  = imx_dwmac_probe,
	.remove = stmmac_pltfr_remove,
	.driver = {
		.name           = "imx-dwmac",
		.pm		= &stmmac_pltfr_pm_ops,
		.of_match_table = imx_dwmac_match,
	},
};
module_platform_driver(imx_dwmac_driver);

MODULE_AUTHOR("NXP");
MODULE_DESCRIPTION("NXP imx8 DWMAC Specific Glue layer");
MODULE_LICENSE("GPL v2");
