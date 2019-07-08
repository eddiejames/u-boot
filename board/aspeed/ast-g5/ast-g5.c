/*
 * Copyright 2016 IBM Corporation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <common.h>
#include <mmc.h>
#include <netdev.h>

#include <asm/arch/ast_scu.h>
#include <asm/arch/ast_sdhci.h>
#include <asm/arch/ast-sdmc.h>
#include <asm/io.h>

DECLARE_GLOBAL_DATA_PTR;

int board_init(void)
{
	gd->bd->bi_boot_params = CONFIG_SYS_SDRAM_BASE + 0x100;
	gd->flags = 0;

	return 0;
}

int dram_init(void)
{
	u32 vga = ast_scu_get_vga_memsize();
	u32 dram = ast_sdmc_get_mem_size();
	gd->ram_size = dram - vga;

	return 0;
}

#ifdef CONFIG_FTGMAC100
int board_eth_init(bd_t *bd)
{
        return ftgmac100_initialize(bd);
}
#endif

#ifdef CONFIG_ASPEEDNIC
int board_eth_init(bd_t *bd)
{
        return aspeednic_initialize(bd);
}
#endif

#ifdef CONFIG_MMC
int board_mmc_init(bd_t *bd)
{
	return ast_sdhci_init();
}
#endif

/* Called by macro WATCHDOG_RESET */
#if defined(CONFIG_HW_WATCHDOG)
void hw_watchdog_reset(void)
{
	/* Restart WD2 timer */
	writel(0x4755, AST_WDT2_BASE + 0x08);
}
#endif /* CONFIG_WATCHDOG */
