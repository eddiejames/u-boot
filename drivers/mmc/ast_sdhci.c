/*
 * Copyright 2019 IBM Corp.
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <common.h>
#include <malloc.h>
#include <sdhci.h>

#include <asm/arch/ast_sdhci.h>
#include <asm/arch/platform.h>

#define AST_SDHCI_MAX_CLK	1000000
#define AST_SDHCI_NAME_SIZE	8

#if defined(AST_SOC_G4) || defined(AST_SOC_G5)
#define AST_SDHCI_NUM_SLOTS	2
#endif

static const u32 ast_sdhci_slot_addrs[] = {
	AST_SDHC_BASE + 0x100,
	AST_SDHC_BASE + 0x200,
};

struct ast_sdhci_host {
	char name[AST_SDHCI_NAME_SIZE];
	struct sdhci_host host;
};

int ast_sdhci_init(void)
{
	int rc;
	struct ast_sdhci_host *hosts;

	hosts = (struct ast_sdhci_host *)calloc(AST_SDHCI_NUM_SLOTS,
						sizeof(struct ast_sdhci_host));
	if (!hosts)
		return -ENOMEM;

	for (unsigned int i = 0; i < AST_SDHCI_NUM_SLOTS; ++i) {
		snprintf(hosts[i].name, AST_SDHCI_NAME_SIZE, "sdhci%d", i);
		hosts[i].host.name = hosts[i].name;
		hosts[i].host.ioaddr = (void *)ast_sdhci_slot_addrs[i];
		hosts[i].host.version = SDHCI_SPEC_200;

		rc = add_sdhci(&hosts[i].host, AST_SDHCI_MAX_CLK, 0);
		if (rc) {
			free(hosts);
			return rc;
		}
	}

	return 0;
}
