// SPDX-License-Identifier: GPL-2.0
/*
 * 
 */

#include <common.h>
#include <dm.h>
#include <fdtdec.h>
#include <log.h>
#include <i2c.h>
#include <tpm-v2.h>
#include <linux/errno.h>
#include <linux/compiler.h>
#include <linux/types.h>
#include <linux/unaligned/be_byteshift.h>
#include <asm-generic/gpio.h>

#include "tpm_tis.h"
#include "tpm_internal.h"

DECLARE_GLOBAL_DATA_PTR;

#define TPM_ACCESS(l)		(0x0000 | ((l) << 12))
#define TPM_INT_ENABLE(l)	(0x0008 | ((l) << 12))
#define TPM_STS(l)			(0x0018 | ((l) << 12))
#define TPM_DATA_FIFO(l)	(0x0024 | ((l) << 12))
#define TPM_DID_VID(l)		(0x0F00 | ((l) << 12))
#define TPM_RID(l)			(0x0F04 | ((l) << 12))

/* Number of wait states to wait for */
#define TPM_WAIT_STATES 100

static int tpm_tis_i2c_read(struct udevice *dev, u16 addr, u8 *in, u16 len)
{

}

static int tpm_tis_i2c_read32(struct udevice *dev, u32 addr, u32 *result)
{
	__le32 result_le;
	int ret;

	ret = tpm_tis_i2c_read(dev, addr, (u8 *)&result_le, sizeof(u32));
	if (!ret)
		*result = le32_to_cpu(result_le);

	return ret;
}

static int tpm_tis_i2c_write(struct udevice *dev, u16 addr, const u8 *out,
							 u16 len)
{

}

static int tpm_tis_i2c_check_locality(struct udevice *dev, int loc)
{
	const u8 mask = TPM_ACCESS_ACTIVE_LOCALITY | TPM_ACCESS_VALID;
	struct tpm_chip *chip = dev_get_priv(dev);
	u8 buf;
	int ret;

	ret = tpm_tis_i2c_read(dev, TPM_ACCESS(loc), &buf, 1);
	if (ret)
		return ret;

	if ((buf & mask) == mask) {
		chip->locality = loc;
		return 0;
	}

	return -ENOENT;
}

static void tpm_tis_i2c_release_locality(struct udevice *dev, int loc,
										 bool force)
{
	const u8 mask = TPM_ACCESS_REQUEST_PENDING | TPM_ACCESS_VALID;
	u8 buf;

	if (tpm_tis_i2c_read(dev, TPM_ACCESS(loc), &buf, 1) < 0)
		return;

	if (force || (buf & mask) == mask) {
		buf = TPM_ACCESS_ACTIVE_LOCALITY;
		tpm_tis_i2c_write(dev, TPM_ACCESS(loc), &buf, 1);
	}
}

static int tpm_tis_i2c_request_locality(struct udevice *dev, int loc)
{
	struct tpm_chip *chip = dev_get_priv(dev);
    u8 buf = TPM_ACCESS_REQUEST_USE;
	unsigned long start, stop;
	int ret;

	ret = tpm_tis_i2c_check_locality(dev, loc);
	if (!ret)
		return 0;

	if (ret != -ENOENT) {
		log(LOGC_NONE, LOGL_ERR, "%s: Failed to get locality: %d\n",
		    __func__, ret);
		return ret;
	}

	ret = tpm_tis_i2c_write(dev, TPM_ACCESS(loc), &buf, 1);
	if (ret) {
		log(LOGC_NONE, LOGL_ERR, "%s: Failed to write to TPM: %d\n",
		    __func__, ret);
		return ret;
	}

	start = get_timer(0);
	stop = chip->timeout_a;
	do {
		ret = tpm_tis_i2c_check_locality(dev, loc);
		if (!ret)
			return 0;

		if (ret != -ENOENT) {
			log(LOGC_NONE, LOGL_ERR,
			    "%s: Failed to get locality: %d\n", __func__, ret);
			return ret;
		}

		mdelay(TPM_TIMEOUT_MS);
	} while (get_timer(start) < stop);

	log(LOGC_NONE, LOGL_ERR, "%s: Timeout getting locality: %d\n", __func__,
	    ret);

	return ret;
}

static u8 tpm_tis_i2c_status(struct udevice *dev, u8 *status)
{
	struct tpm_chip *chip = dev_get_priv(dev);

	return tpm_tis_i2c_read(dev, TPM_STS(chip->locality), status, 1);
}

static int tpm_tis_i2c_wait_for_stat(struct udevice *dev, u8 mask,
				    				 unsigned long timeout, u8 *status)
{
	unsigned long start = get_timer(0);
	unsigned long stop = timeout;
	int ret;

	do {
		mdelay(TPM_TIMEOUT_MS);
		ret = tpm_tis_i2c_status(dev, status);
		if (ret)
			return ret;

		if ((*status & mask) == mask)
			return 0;
	} while (get_timer(start) < stop);

	return -ETIMEDOUT;
}

static int tpm_tis_i2c_get_burstcount(struct udevice *dev)
{
	struct tpm_chip *chip = dev_get_priv(dev);
	unsigned long start, stop;
	u32 burstcount, ret;

	/* wait for burstcount */
	start = get_timer(0);
	stop = chip->timeout_d;
	do {
		ret = tpm_tis_i2c_read32(dev, TPM_STS(chip->locality),
					 &burstcount);
		if (ret)
			return -EBUSY;

		burstcount = (burstcount >> 8) & 0xFFFF;
		if (burstcount)
			return burstcount;

		mdelay(TPM_TIMEOUT_MS);
	} while (get_timer(start) < stop);

	return -EBUSY;
}

static int tpm_tis_i2c_cancel(struct udevice *dev)
{
	struct tpm_chip *chip = dev_get_priv(dev);
	u8 data = TPM_STS_COMMAND_READY;

	return tpm_tis_i2c_write(dev, TPM_STS(chip->locality), &data, 1);
}

static int tpm_tis_i2c_recv_data(struct udevice *dev, u8 *buf, size_t count)
{
	struct tpm_chip *chip = dev_get_priv(dev);
	int size = 0, burstcnt, len, ret;
	u8 status;

	while (size < count &&
	       tpm_tis_i2c_wait_for_stat(dev, TPM_STS_DATA_AVAIL | TPM_STS_VALID,
									 chip->timeout_c, &status) == 0) {
		burstcnt = tpm_tis_i2c_get_burstcount(dev);
		if (burstcnt < 0)
			return burstcnt;

		len = min_t(int, burstcnt, count - size);
		ret = tpm_tis_i2c_read(dev, TPM_DATA_FIFO(chip->locality), buf + size,
							   len);
		if (ret < 0)
			return ret;

		size += len;
	}

	return size;
}

static int tpm_tis_i2c_recv(struct udevice *dev, u8 *buf, size_t count)
{
	struct tpm_chip *chip = dev_get_priv(dev);
	int size, expected;

	if (!chip)
		return -ENODEV;

	if (count < TPM_HEADER_SIZE) {
		size = -EIO;
		goto out;
	}

	size = tpm_tis_i2c_recv_data(dev, buf, TPM_HEADER_SIZE);
	if (size < TPM_HEADER_SIZE) {
		log(LOGC_NONE, LOGL_ERR, "TPM error, unable to read header\n");
		goto out;
	}

	expected = get_unaligned_be32(buf + 2);
	if (expected > count) {
		size = -EIO;
		goto out;
	}

	size += tpm_tis_i2c_recv_data(dev, &buf[TPM_HEADER_SIZE],
								  expected - TPM_HEADER_SIZE);
	if (size < expected) {
		log(LOGC_NONE, LOGL_ERR,
		    "TPM error, unable to read remaining bytes of result\n");
		size = -EIO;
		goto out;
	}

out:
	tpm_tis_i2c_cancel(dev);
	tpm_tis_i2c_release_locality(dev, chip->locality, false);

	return size;
}

static int tpm_tis_i2c_send(struct udevice *dev, const u8 *buf, size_t len)
{
	struct tpm_chip *chip = dev_get_priv(dev);
	u32 i, size;
	u8 status;
	int burstcnt, ret;
	u8 data;

	if (!chip)
		return -ENODEV;

	if (len > TPM_DEV_BUFSIZE)
		return -E2BIG;  /* Command is too long for our tpm, sorry */

	ret = tpm_tis_i2c_request_locality(dev, 0);
	if (ret < 0)
		return -EBUSY;

	/*
	 * Check if the TPM is ready. If not, if not, cancel the pending command
	 * and poll on the status to be finally ready.
	 */
	ret = tpm_tis_i2c_status(dev, &status);
	if (ret)
		return ret;

	if (!(status & TPM_STS_COMMAND_READY)) {
		/* Force the transition, usually this will be done at startup */
		ret = tpm_tis_i2c_cancel(dev);
		if (ret) {
			log(LOGC_NONE, LOGL_ERR,
			    "%s: Could not cancel previous operation\n",
			    __func__);
			goto out_err;
		}

		ret = tpm_tis_i2c_wait_for_stat(dev, TPM_STS_COMMAND_READY,
										chip->timeout_b, &status);
		if (ret < 0 || !(status & TPM_STS_COMMAND_READY)) {
			log(LOGC_NONE, LOGL_ERR,
			    "status %d after wait for stat returned %d\n",
			    status, ret);
			goto out_err;
		}
	}

	for (i = 0; i < len - 1;) {
		burstcnt = tpm_tis_i2c_get_burstcount(dev);
		if (burstcnt < 0)
			return burstcnt;

		size = min_t(int, len - i - 1, burstcnt);
		ret = tpm_tis_i2c_write(dev, TPM_DATA_FIFO(chip->locality),
								buf + i, size);
		if (ret < 0)
			goto out_err;

		i += size;
	}

	ret = tpm_tis_i2c_status(dev, &status);
	if (ret)
		goto out_err;

	if ((status & TPM_STS_DATA_EXPECT) == 0) {
		ret = -EIO;
		goto out_err;
	}

	ret = tpm_tis_i2c_write(dev, TPM_DATA_FIFO(chip->locality),
							buf + len - 1, 1);
	if (ret)
		goto out_err;

	ret = tpm_tis_i2c_status(dev, &status);
	if (ret)
		goto out_err;

	if ((status & TPM_STS_DATA_EXPECT) != 0) {
		ret = -EIO;
		goto out_err;
	}

	data = TPM_STS_GO;
	ret = tpm_tis_i2c_write(dev, TPM_STS(chip->locality), &data, 1);
	if (ret)
		goto out_err;

	return len;

out_err:
	tpm_tis_i2c_cancel(dev);
	tpm_tis_i2c_release_locality(dev, chip->locality, false);

	return ret;
}

static int tpm_tis_i2c_cleanup(struct udevice *dev)
{
	struct tpm_chip *chip = dev_get_priv(dev);

	tpm_tis_i2c_cancel(dev);
	/*
	 * The TPM needs some time to clean up here,
	 * so we sleep rather than keeping the bus busy
	 */
	mdelay(2);
	tpm_tis_i2c_release_locality(dev, chip->locality, false);

	return 0;
}

static int tpm_tis_i2c_open(struct udevice *dev)
{
	struct tpm_chip *chip = dev_get_priv(dev);
	struct tpm_chip_priv *priv = dev_get_uclass_priv(dev);

	if (chip->is_open)
		return -EBUSY;

	chip->is_open = 1;

	return 0;
}

static int tpm_tis_i2c_close(struct udevice *dev)
{
	struct tpm_chip *chip = dev_get_priv(dev);

	if (chip->is_open) {
		tpm_tis_i2c_release_locality(dev, chip->locality, true);
		chip->is_open = 0;
	}

	return 0;
}

static int tpm_tis_i2c_get_desc(struct udevice *dev, char *buf, int size)
{
	struct tpm_chip *chip = dev_get_priv(dev);

	if (size < 80)
		return -ENOSPC;

	return snprintf(buf, size,
			"%s v2.0: VendorID 0x%04x, DeviceID 0x%04x, RevisionID 0x%02x [%s]",
			dev->name, chip->vend_dev & 0xFFFF,
			chip->vend_dev >> 16, chip->rid,
			(chip->is_open ? "open" : "closed"));
}

static int tpm_tis_wait_init(struct udevice *dev, int loc)
{
	struct tpm_chip *chip = dev_get_priv(dev);
	unsigned long start, stop;
	u8 status;
	int ret;

	start = get_timer(0);
	stop = chip->timeout_b;
	do {
		mdelay(TPM_TIMEOUT_MS);

		ret = tpm_tis_i2c_read(dev, TPM_ACCESS(loc), &status, 1);
		if (ret)
			break;

		if (status & TPM_ACCESS_VALID)
			return 0;
	} while (get_timer(start) < stop);

	return -EIO;
}

static int tpm_tis_i2c_probe(struct udevice *dev)
{
	struct tpm_chip_priv *priv = dev_get_uclass_priv(dev);
	struct tpm_chip *chip = dev_get_priv(dev);
	int ret;

	/* Use the TPM v2 stack */
	priv->version = TPM_V2;

	/* Ensure a minimum amount of time elapsed since reset of the TPM */
	mdelay(30);

	chip->locality = 0;
	chip->timeout_a = TIS_SHORT_TIMEOUT_MS;
	chip->timeout_b = TIS_LONG_TIMEOUT_MS;
	chip->timeout_c = TIS_SHORT_TIMEOUT_MS;
	chip->timeout_d = TIS_SHORT_TIMEOUT_MS;
	priv->pcr_count = 24;
	priv->pcr_select_min = 3;

	ret = tpm_tis_wait_init(dev, chip->locality);
	if (ret) {
		log(LOGC_DM, LOGL_ERR, "%s: no device found\n", __func__);
		return ret;
	}

	ret = tpm_tis_i2c_request_locality(dev, chip->locality);
	if (ret) {
		log(LOGC_NONE, LOGL_ERR, "%s: could not request locality %d\n",
		    __func__, chip->locality);
		return ret;
	}

	ret = tpm_tis_i2c_read32(dev, TPM_DID_VID(chip->locality),
				 			 &chip->vend_dev);
	if (ret) {
		log(LOGC_NONE, LOGL_ERR,
		    "%s: could not retrieve VendorID/DeviceID\n", __func__);
		return ret;
	}

	ret = tpm_tis_i2c_read(dev, TPM_RID(chip->locality), &chip->rid, 1);
	if (ret) {
		log(LOGC_NONE, LOGL_ERR, "%s: could not retrieve RevisionID\n",
		    __func__);
		return ret;
	}

	log(LOGC_NONE, LOGL_ERR,
	    "i2c TPMv2.0 found (vid:%04x, did:%04x, rid:%02x)\n",
	    chip->vend_dev & 0xFFFF, chip->vend_dev >> 16, chip->rid);

	return 0;
}

static int tpm_tis_i2c_remove(struct udevice *dev)
{
	struct tpm_chip *chip = dev_get_priv(dev);

	tpm_tis_i2c_release_locality(dev, chip->locality, true);

	return 0;
}

static const struct tpm_ops tpm_tis_i2c_ops = {
	.open		= tpm_tis_i2c_open,
	.close		= tpm_tis_i2c_close,
	.get_desc	= tpm_tis_i2c_get_desc,
	.send		= tpm_tis_i2c_send,
	.recv		= tpm_tis_i2c_recv,
	.cleanup	= tpm_tis_i2c_cleanup,
};

static const struct udevice_id tpm_tis_i2c_ids[] = {
	{
		.compatible = "tis,tpm2-i2c",
	},
	{ }
};

U_BOOT_DRIVER(tpm_tis_i2c) = {
	.name   = "tpm_tis_i2c",
	.id     = UCLASS_TPM,
	.of_match = tpm_tis_i2c_ids,
	.ops    = &tpm_tis_i2c_ops,
	.probe	= tpm_tis_i2c_probe,
	.remove	= tpm_tis_i2c_remove,
	.priv_auto_alloc_size = sizeof(struct tpm_chip),
};
