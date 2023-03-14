#include "goodix_ts_core.h"
#include <linux/spi/spi.h>

static atomic_t ato_ver = ATOMIC_INIT(0);

#define SPI_NUM 4
#define MAX_NAME_LEN_20  20
char gtp_vendor_name[MAX_NAME_LEN_20] = { 0 };

static int tpd_init_tpinfo(struct ztp_device *cdev)
{
	int firmware;
	struct goodix_ts_core *core_data = (struct goodix_ts_core *)cdev->private;
	struct goodix_ts_hw_ops *hw_ops = core_data->hw_ops;
	struct goodix_fw_version chip_ver;
	int r = 0;
	char *cfg_buf;

	if (atomic_read(&core_data->suspended)) {
		ts_err("%s: error, tp in suspend!", __func__);
		return -EIO;
	}

	if (atomic_cmpxchg(&ato_ver, 0, 1)) {
		ts_err("busy, wait!");
		return -EIO;
	}

	cfg_buf = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!cfg_buf)
		return -ENOMEM;

	ts_info("%s: enter!", __func__);

	if (hw_ops->read_version) {
		r = hw_ops->read_version(core_data, &chip_ver);
		if (!r) {
			snprintf(cdev->ic_tpinfo.tp_name, MAX_VENDOR_NAME_LEN, "goodix:GT%s",
				chip_ver.patch_pid);

			firmware = (unsigned int)chip_ver.patch_vid[3] +
					((unsigned int)chip_ver.patch_vid[2] << 8) +
					((unsigned int)chip_ver.patch_vid[1] << 16) +
					((unsigned int)chip_ver.patch_vid[0] << 24);
			cdev->ic_tpinfo.firmware_ver = firmware;

			cdev->ic_tpinfo.chip_model_id = TS_CHIP_GOODIX;

			cdev->ic_tpinfo.module_id = chip_ver.sensor_id;

			cdev->ic_tpinfo.spi_num = SPI_NUM;

			strlcpy(cdev->ic_tpinfo.vendor_name, gtp_vendor_name, sizeof(cdev->ic_tpinfo.vendor_name));
		}
	} else {
		ts_err("%s: read_version failed!", __func__);
		goto exit;
	}

	if (hw_ops->read_config) {
		r = hw_ops->read_config(core_data, cfg_buf, PAGE_SIZE);
		if (r <= 0)
			goto exit;

		cdev->ic_tpinfo.config_ver = cfg_buf[34];
	}

	ts_info("%s: end!", __func__);

exit:
	kfree(cfg_buf);
	atomic_cmpxchg(&ato_ver, 1, 0);

	return r;
}

void goodix_tpd_register_fw_class(struct goodix_ts_core *core_data)
{
	ts_info("%s: entry", __func__);

	tpd_cdev->private = (void *)core_data;
	tpd_cdev->get_tpinfo = tpd_init_tpinfo;

	ts_info("%s: end", __func__);
}
