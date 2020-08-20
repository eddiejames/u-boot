/*
 * (C) Copyright ASPEED Technology Inc.
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */
#include <common.h>
#include <debug_uart.h>
#include <spl.h>
#include <dm.h>
#include <mmc.h>
#include <xyzModem.h>
#include <asm/io.h>
#include <asm/arch/aspeed_verify.h>

DECLARE_GLOBAL_DATA_PTR;

#define AST_BOOTMODE_SPI	0
#define AST_BOOTMODE_EMMC	1
#define AST_BOOTMODE_UART	2

u32 aspeed_bootmode(void);
void aspeed_mmc_init(void);

void board_init_f(ulong dummy)
{
#ifndef CONFIG_SPL_TINY
	spl_early_init();
	timer_init();
	preloader_console_init();
	dram_init();
	aspeed_mmc_init();
#endif
}

u32 spl_boot_device(void)
{
	switch (aspeed_bootmode()) {
	case AST_BOOTMODE_EMMC:
		return (IS_ENABLED(CONFIG_ASPEED_SECURE_BOOT)) ?
		       SECBOOT_DEVICE_MMC : BOOT_DEVICE_MMC1;
	case AST_BOOTMODE_SPI:
		return (IS_ENABLED(CONFIG_ASPEED_SECURE_BOOT)) ?
		       SECBOOT_DEVICE_RAM : BOOT_DEVICE_RAM;
	case AST_BOOTMODE_UART:
		return (IS_ENABLED(CONFIG_ASPEED_SECURE_BOOT)) ?
		       SECBOOT_DEVICE_UART : BOOT_DEVICE_UART;
	default:
		break;
	}

	return BOOT_DEVICE_NONE;
}

void board_boot_order(u32 *spl_boot_list)
{
	spl_boot_list[0] = spl_boot_device();

	switch (spl_boot_list[0]) {
	case BOOT_DEVICE_MMC1:
		spl_boot_list[1] = BOOT_DEVICE_RAM;
		break;
	case BOOT_DEVICE_NONE:
		spl_boot_list[0] = BOOT_DEVICE_RAM;
	case BOOT_DEVICE_RAM:
		spl_boot_list[1] = BOOT_DEVICE_MMC1;
		break;
	case BOOT_DEVICE_UART:
		return;
	}

	spl_boot_list[2] = BOOT_DEVICE_UART;
}

#ifdef CONFIG_SPL_OS_BOOT
int spl_start_uboot(void)
{
	/* boot linux */
	return 0;
}
#endif

int board_fit_config_name_match(const char *name)
{
	/* we always use the default configuration */
	debug("%s: %s\n", __func__, name);
	return 0;
}

struct image_header *spl_get_load_buffer(ssize_t offset, size_t size)
{
	return (struct image_header *)(CONFIG_SYS_LOAD_ADDR);
}

static ulong dummy_read(struct spl_load_info *load, ulong sector,
			ulong count, void *buf)
{
	return count;
}

static int spl_secboot_ram_load_image(struct spl_image_info *spl_image,
				      struct spl_boot_device *bootdev)
{
	struct aspeed_secboot_header sb_hdr;
	struct image_header *img_hdr;
	struct spl_load_info load;
	void *load_buf;
	uint32_t sb_size;

	sb_size = sizeof(sb_hdr);
	memcpy(&sb_hdr, (void *)CONFIG_SPL_LOAD_FIT_ADDRESS, sb_size);

	if (strcmp((char *)sb_hdr.sbh_magic, ASPEED_SECBOOT_MAGIC_STR)) {
		debug("spl: cannot find secure boot header\n");
		return -EPERM;
	}

	load_buf = spl_get_load_buffer(0, 0);
	memcpy(load_buf, (void *)CONFIG_SPL_LOAD_FIT_ADDRESS + sb_size, sb_hdr.sbh_img_size - sb_size + 512);

	if (aspeed_bl2_verify(&sb_hdr, load_buf, CONFIG_SPL_TEXT_BASE) != 0)
		hang();

	img_hdr = (struct image_header *)load_buf;

	if (!IS_ENABLED(CONFIG_SPL_LOAD_FIT) || image_get_magic(img_hdr) != FDT_MAGIC) {
		debug("spl: cannot find FIT image\n");
		return -ENOENT;
	}

	load.bl_len = 1;
	load.read = dummy_read;

	return spl_load_simple_fit(spl_image, &load, 0, load_buf);
}

SPL_LOAD_IMAGE_METHOD("Secure Boot RAM", 0, SECBOOT_DEVICE_RAM, spl_secboot_ram_load_image);

u32 spl_boot_mode(const u32 boot_device)
{
	return MMCSD_MODE_RAW;
}

static int spl_secboot_mmc_load_image(struct spl_image_info *spl_image,
				      struct spl_boot_device *bootdev)
{
	int err;
	u32 sb_size;
	u32 count;
	u32 load_sectors;

	struct mmc *mmc = NULL;
	struct udevice *dev;
	struct blk_desc *bd;

	struct aspeed_secboot_header sb_hdr;
	u32 sb_hdr_sector;

	struct image_header *img_hdr;
	u32 img_hdr_sector;

	struct spl_load_info load;
	void *load_buf;

	err = mmc_initialize(NULL);
	if (err) {
		debug("spl: could not initialize mmc. error: %d\n", err);
		return err;
	}

	err = uclass_get_device(UCLASS_MMC, 0, &dev);
	if (err) {
		debug("spl: mmc get device through DM with error: %d\n", err);
		return err;
	}

	mmc = mmc_get_mmc_dev(dev);
	if (mmc == NULL) {
		debug("spl: could not find mmc device\n");
		return -ENODEV;
	}

	err = mmc_init(mmc);
	if (err) {
		debug("spl: mmc init failed with error: %d\n", err);
		return err;
	}

	/* currently only RAW sector mode is supported */
	bd = mmc_get_blk_desc(mmc);

	load_buf = spl_get_load_buffer(0, 0);
	sb_hdr_sector = CONFIG_SYS_MMCSD_RAW_MODE_U_BOOT_SECTOR;

	count = blk_dread(bd, sb_hdr_sector, 1, &sb_hdr);
	sb_size = sizeof(sb_hdr);
	if (count == 0) {
		debug("spl: mmc raw sector read failed\n");
		return -EIO;
	}

	if (strcmp((char *)sb_hdr.sbh_magic, ASPEED_SECBOOT_MAGIC_STR)) {
		debug("spl: cannot find secure boot header\n");
		return -EPERM;
	}

	img_hdr_sector = sb_hdr_sector + (sizeof(sb_hdr) / mmc->read_bl_len);

	load_sectors = (sb_hdr.sbh_img_size - sb_size + 512 + mmc->read_bl_len - 1) / mmc->read_bl_len;
	count = blk_dread(bd, img_hdr_sector, load_sectors, load_buf);
	if (count == 0) {
		debug("spl: mmc raw sector read failed\n");
		return -EIO;
	}

	if (aspeed_bl2_verify(&sb_hdr, load_buf, CONFIG_SPL_TEXT_BASE) != 0)
		hang();

	img_hdr = (struct image_header *)load_buf;


	if (!IS_ENABLED(CONFIG_SPL_LOAD_FIT) || image_get_magic(img_hdr) != FDT_MAGIC) {
		debug("spl: cannot find FIT image\n");
		return -ENOENT;
	}

	load.dev = mmc;
	load.priv = NULL;
	load.filename = NULL;
	load.bl_len = mmc->read_bl_len;
	load.read = dummy_read;

	return spl_load_simple_fit(spl_image, &load, CONFIG_SYS_MMCSD_RAW_MODE_U_BOOT_SECTOR, load_buf);
}

SPL_LOAD_IMAGE_METHOD("Secure Boot MMC", 0, SECBOOT_DEVICE_MMC, spl_secboot_mmc_load_image);

#define BUF_SIZE	1024

struct ymodem_fit_info {
	int image_read;
	char *buf;
};

static int getcymodem(void)
{
	if (tstc())
		return (getc());
	return -1;
}

static ulong spl_ymodem_read(struct ymodem_fit_info *info, ulong offset,
			     ulong size, void *addr)
{
	int res, err;
	char *buf = info->buf;

	while (info->image_read < offset) {
		res = xyzModem_stream_read(buf, BUF_SIZE, &err);
		if (res <= 0)
			return res;
		info->image_read += res;
	}

	if (info->image_read > offset) {
		res = info->image_read - offset;
		memcpy(addr, &buf[BUF_SIZE - res], res);
		addr = addr + res;
	}

	while (info->image_read < offset + size) {
		res = xyzModem_stream_read(buf, BUF_SIZE, &err);
		if (res <= 0)
			return res;

		memcpy(addr, buf, res);
		info->image_read += res;
		addr += res;
	}

	return size;
}

static int spl_secboot_ymodem_load_image(struct spl_image_info *spl_image,
		struct spl_boot_device *bootdev)
{
	struct aspeed_secboot_header sb_hdr;
	struct image_header *img_hdr;
	struct spl_load_info load;
	struct ymodem_fit_info ymodem_info;
	connection_info_t conn_info;
	char buf[BUF_SIZE];
	uint32_t sb_size;
	void *load_buf;
	ulong size = 0;
	int err;
	int res;
	int ret = 0;

	sb_size = sizeof(sb_hdr);

	conn_info.mode = xyzModem_ymodem;
	ret = xyzModem_stream_open(&conn_info, &err);
	if (ret) {
		debug("spl: ymodem err - %s\n", xyzModem_error(err));
		return ret;
	}

	res = xyzModem_stream_read(buf, BUF_SIZE, &err);
	if (res <= 0) {
		ret = -EIO;
		goto end_stream;
	}

	sb_hdr = *(struct aspeed_secboot_header *)buf;
	if (strcmp((char *)sb_hdr.sbh_magic, ASPEED_SECBOOT_MAGIC_STR)) {
		debug("spl: cannot find secure boot header\n");
		ret = -EPERM;
		goto end_stream;
	}

	ymodem_info.buf = buf;
	ymodem_info.image_read = BUF_SIZE;
	load_buf = spl_get_load_buffer(0, 0);
	spl_ymodem_read(&ymodem_info, sb_size, sb_hdr.sbh_img_size - sb_size + 512, load_buf);

	img_hdr = (struct image_header *)load_buf;

	if (!IS_ENABLED(CONFIG_SPL_LOAD_FIT) || image_get_magic(img_hdr) != FDT_MAGIC) {
		debug("spl: cannot find FIT image\n");
		ret = -ENOENT;
		goto end_stream;
	}

	if (aspeed_bl2_verify(&sb_hdr, load_buf, CONFIG_SPL_TEXT_BASE) != 0)
		hang();

	load.bl_len = 1;
	load.read = dummy_read;
	ret = spl_load_simple_fit(spl_image, &load, 0, load_buf);
	if (ret) {
		debug("spl: load FIT image failed with error: %d\n", ret);
		goto end_stream;
	}

	size = ymodem_info.image_read;
	while ((res = xyzModem_stream_read(buf, BUF_SIZE, &err)) > 0)
		size += res;

end_stream:
	xyzModem_stream_close(&err);
	xyzModem_stream_terminate(false, &getcymodem);

	debug("Loaded %lu bytes\n", size);

	return ret;
}

SPL_LOAD_IMAGE_METHOD("Secure Boot UART", 0, SECBOOT_DEVICE_UART, spl_secboot_ymodem_load_image);
