// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2019 IBM Corp.
 * Eddie James <eajames@linux.ibm.com>
 */

#include <common.h>
#include <clk.h>
#include <dm.h>
#include <malloc.h>
#include <sdhci.h>

struct aspeed_sdhci_plat {
	struct mmc_config cfg;
	struct mmc mmc;
};

static int aspeed_sdhci_probe(struct udevice *dev)
{
	struct mmc_uclass_priv *upriv = dev_get_uclass_priv(dev);
	struct aspeed_sdhci_plat *plat = dev_get_platdata(dev);
	struct sdhci_host *host = dev_get_priv(dev);
	u32 max_clk;
	struct clk clk;
	int ret;

	ret = clk_get_by_index(dev, 0, &clk);
	if (ret)
		return ret;

	ret = clk_enable(&clk);
	if (ret)
		return ret;

	host->name = dev->name;
	host->ioaddr = (void *)devfdt_get_addr(dev);

	max_clk = clk_get_rate(&clk);
	if (!max_clk)
		return -EINVAL;

	host->max_clk = max_clk;
	host->mmc = &plat->mmc;
	host->mmc->dev = dev;
	host->mmc->priv = host;
	upriv->mmc = host->mmc;

	ret = sdhci_setup_cfg(&plat->cfg, host, 0, 0);
	if (ret)
		return ret;

	return sdhci_probe(dev);
}

static int aspeed_sdhci_bind(struct udevice *dev)
{
	struct aspeed_sdhci_plat *plat = dev_get_platdata(dev);

	return sdhci_bind(dev, &plat->mmc, &plat->cfg);
}

static const struct udevice_id aspeed_sdhci_ids[] = {
	{ .compatible = "aspeed,ast2400-sdhci" },
	{ .compatible = "aspeed,ast2500-sdhci" },
	{ .compatible = "aspeed,ast2600-sdhci" },
	{ }
};

U_BOOT_DRIVER(aspeed_sdhci_drv) = {
	.name		= "aspeed_sdhci",
	.id		= UCLASS_MMC,
	.of_match	= aspeed_sdhci_ids,
	.ops		= &sdhci_ops,
	.bind		= aspeed_sdhci_bind,
	.probe		= aspeed_sdhci_probe,
	.priv_auto_alloc_size = sizeof(struct sdhci_host),
	.platdata_auto_alloc_size = sizeof(struct aspeed_sdhci_plat),
};
