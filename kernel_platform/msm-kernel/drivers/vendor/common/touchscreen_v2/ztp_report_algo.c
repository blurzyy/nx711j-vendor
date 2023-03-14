
/***********************
 * file : ztp_report_algo.c
 **********************/
#include <linux/module.h>
#include <linux/err.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/vmalloc.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include "ztp_common.h"

#define MAX_POINTS_SUPPORT 10
#define LONG_PRESS_MIN_COUNT 50
static void edge_point_report(int id);
static void edge_long_press_up(struct input_dev *input, u16 id);
static void point_report_reset(int id);
static bool is_have_other_point_down(int id);
static bool is_have_inside_point_down(void);
void tpd_touch_release(struct input_dev *input, u16 id);

typedef struct point_info {
	int x;
	int y;
	u8 touch_major;
	u8 pressure;
} tpd_point_info_t;

typedef struct point_fifo {
	tpd_point_info_t point_data[2];
	tpd_point_info_t first_report_point_data;
	tpd_point_info_t last_point;
	tpd_point_info_t mistake_touch_check_point;
	bool is_report_point;
	bool save_first_down_point;
	bool is_moving_in_limit_area;
	bool finger_down;
	bool edge_finger_down;
	bool limit_area_log_print;
	bool is_inside_finger_down;
	bool jitter_check;
	bool mistake_touch_check;
	bool cancel_clean_edge_area_ponit;
	unsigned long touch_down_timer;
	unsigned long edge_down_timer;
	unsigned long long_press_timer;
	struct input_dev *input;
} tpd_point_fifo_t;

tpd_point_fifo_t point_report_info[MAX_POINTS_SUPPORT];

#define tpd_idn_report_work(id)\
static void tpd_id##id##_report_work(struct work_struct *work)\
{\
	tpd_point_fifo_t *point = &point_report_info[id];\
	edge_long_press_up(point->input, id);\
}

tpd_idn_report_work(0)
tpd_idn_report_work(1)
tpd_idn_report_work(2)
tpd_idn_report_work(3)
tpd_idn_report_work(4)
tpd_idn_report_work(5)
tpd_idn_report_work(6)
tpd_idn_report_work(7)
tpd_idn_report_work(8)
tpd_idn_report_work(9)

static bool point_in_report_judge_area(u16 x, u16 y)
{
	struct ztp_device *cdev = tpd_cdev;

	if (cdev->display_rotation == mRotatin_0) {
		if ((x < cdev->long_pess_suppression[0] * 3  / 2) || (x > cdev->max_x - cdev->long_pess_suppression[1] * 3 / 2))
			return true;
	}
	return false;
}

static bool point_is_in_limit_area(u16 x, u16 y)
{
	struct ztp_device *cdev = tpd_cdev;

	if (cdev->display_rotation == mRotatin_90 || cdev->display_rotation == mRotatin_270) {
		if ((x < cdev->edge_report_limit[0]) || (x > cdev->max_x - cdev->edge_report_limit[1]) ||
			(y < cdev->edge_report_limit[2]) || (y > cdev->max_y - cdev->edge_report_limit[3]))
			return true;
	} else {
		if ((x  < cdev->edge_report_limit[0]) || (x > cdev->max_x - cdev->edge_report_limit[1]))
			return true;
		if (point_in_report_judge_area(x, y) && is_have_inside_point_down()) {
			TPD_DMESG("have other point down and tpd Press in judge area: x = %d, y = %d\n", x, y);
			return true;
		}
		if (cdev->edge_limit_pixel_level > 0) {
			if ((y > cdev->user_edge_limit[1]) &&
				(((x < cdev->user_edge_limit[0]) || (x > cdev->max_x - cdev->user_edge_limit[0]))))
				return true;
		}
	}
	return false;
}

static bool point_in_long_pess_suppression_area(u16 x, u16 y)
{
	struct ztp_device *cdev = tpd_cdev;

	if (cdev->edge_long_press_check == false)
		return false;
	if (cdev->display_rotation == mRotatin_90 || cdev->display_rotation == mRotatin_270) {
		if ((x < cdev->long_pess_suppression[0]) || (x > cdev->max_x - cdev->long_pess_suppression[1]) ||
			(y < cdev->long_pess_suppression[2]) || (y > cdev->max_y - cdev->long_pess_suppression[3]))
			return true;
		if (cdev->edge_limit_pixel_level > 0) {
			if ((y > cdev->user_edge_limit[1]) &&
				(((x < cdev->user_edge_limit[0]) || (x > cdev->max_x - cdev->user_edge_limit[0]))))
				return true;
		}
	} else {
		if ((x  < cdev->long_pess_suppression[0]) || (x > cdev->max_x - cdev->long_pess_suppression[1]))
			return true;
	}
	return false;
}

static bool is_need_clean_long_pess_area_down(tpd_point_fifo_t *point, u16 x, u16 y)
{
	struct ztp_device *cdev = tpd_cdev;

	if (cdev->edge_long_press_check == false || point->cancel_clean_edge_area_ponit) {
		return false;
	}
	if (cdev->display_rotation == mRotatin_90 || cdev->display_rotation == mRotatin_270) {
		return false;
	} else {
		if (x > cdev->max_x / 4 &&  x < cdev->max_x  * 3 / 4)
			point->cancel_clean_edge_area_ponit = true;
		if (point->first_report_point_data.x < cdev->max_x / 4 ||  point->first_report_point_data.x > cdev->max_x  * 3 / 4)
			return true;
	}
	return false;
}

static bool is_report_point(u16 x, u16 y, u16 id, u8 touch_major, u8  pressure)
{
	tpd_point_fifo_t *point = &point_report_info[id];
	u16 limit_pixel = 100;

	if (point->is_report_point) {
		if (is_need_clean_long_pess_area_down(point, x, y) == false)
			return true;
		if (point_in_long_pess_suppression_area(x, y)) {
			if (!point->mistake_touch_check) {
				point->mistake_touch_check_point.x = x;
				point->mistake_touch_check_point.y = y;
				point->mistake_touch_check = true;
				point->edge_down_timer = jiffies;
			}
			if(jiffies_to_msecs(jiffies - point->edge_down_timer) > 800) {
				if ((abs(point->mistake_touch_check_point.x  - x) > 20
					|| abs(point->mistake_touch_check_point.y - y) > 20)) {
					point->mistake_touch_check_point.x = x;
					point->mistake_touch_check_point.y = y;
					point->edge_down_timer = jiffies;
					return true;
				}
				tpd_touch_release(point->input,  id);
				point->mistake_touch_check = false;
				return false;
			} else {
				return true;
			}
		}
		point->mistake_touch_check = false;
		return true;
	}
	if (point_is_in_limit_area(x, y) || point_in_long_pess_suppression_area(x, y)) {
		if ((!point_is_in_limit_area(x, y) && point_in_long_pess_suppression_area(x, y))) {
			if (point->limit_area_log_print == false) {
				point->limit_area_log_print = true;
				point->long_press_timer = jiffies;
				TPD_DMESG("tpd Press in long pess suppression area: id = %d, x = %d, y = %d\n", id, x, y);
			}
			if (is_have_inside_point_down()) {
				point->is_inside_finger_down = true;
			}
		} else {
			if (point->limit_area_log_print == false) {
				point->limit_area_log_print = true;
				TPD_DMESG("tpd Press in limit area: id = %d, x = %d, y = %d\n", id, x, y);
			}
		}
		if (point->save_first_down_point == false) {
			point->point_data[0].x = x;
			point->point_data[0].y = y;
			point->point_data[0].touch_major = touch_major;
			point->point_data[0].pressure = pressure;
			point->save_first_down_point = true;
			return false;
		}
		if (abs(point->point_data[0].x - x) > limit_pixel
			|| abs(point->point_data[0].y - y) > limit_pixel) {
			goto save_last_ponit;

		} else {
			return false;
		}
	}
save_last_ponit:
	if (point->save_first_down_point == false) {
		point->is_moving_in_limit_area = false;
		return true;
	}
	point->point_data[1].x = x;
	point->point_data[1].y = y;
	point->point_data[1].touch_major = touch_major;
	point->point_data[1].pressure = pressure;
	point->is_moving_in_limit_area = true;
	return true;
}

static void tpd_touch_report(struct input_dev *input, u16 x, u16 y, u16 id, u8 touch_major, u8  pressure)
{
	struct ztp_device *cdev = tpd_cdev;

	mutex_lock(&cdev->report_mutex);
	input_mt_slot(input, id);
	input_mt_report_slot_state(input, MT_TOOL_FINGER, true);
	input_report_key(input, BTN_TOUCH, 1);
	input_report_abs(input, ABS_MT_POSITION_X, x);
	input_report_abs(input, ABS_MT_POSITION_Y, y);
	if (pressure)
		input_report_abs(input, ABS_MT_PRESSURE, pressure);
	if (touch_major)
		input_report_abs(input, ABS_MT_TOUCH_MAJOR, touch_major);
	mutex_unlock(&cdev->report_mutex);
}

static void tpd_touch_report_nolock(struct input_dev *input, u16 x, u16 y, u16 id, u8 touch_major, u8  pressure)
{
	input_mt_slot(input, id);
	input_mt_report_slot_state(input, MT_TOOL_FINGER, true);
	input_report_key(input, BTN_TOUCH, 1);
	input_report_abs(input, ABS_MT_POSITION_X, x);
	input_report_abs(input, ABS_MT_POSITION_Y, y);
	if (pressure)
		input_report_abs(input, ABS_MT_PRESSURE, pressure);
	if (touch_major)
		input_report_abs(input, ABS_MT_TOUCH_MAJOR, touch_major);
}

static bool is_have_other_point_down(int id)
{
	int i = 0;

	for (i = 0; i < MAX_POINTS_SUPPORT; i++) {
		if (i == id)
			continue;
		if ((point_report_info[i].finger_down || point_report_info[i].edge_finger_down))
			return true;
	}
	return false;
}

static bool is_have_inside_point_down(void)
{
	int i = 0;

	for (i = 0; i < MAX_POINTS_SUPPORT; i++) {
		if (point_report_info[i].finger_down)
			return true;
	}
	return false;
}

static void edge_long_press_up(struct input_dev *input, u16 id)
{
	tpd_point_fifo_t *point = &point_report_info[id];
	struct ztp_device *cdev = tpd_cdev;


	if (point->edge_finger_down == false) {
		return;
	}
	mutex_lock(&cdev->report_mutex);
	input_mt_slot(input, id);
	input_mt_report_slot_state(input, MT_TOOL_FINGER, false);
	if (!is_have_other_point_down(id)) {
		input_report_key(input, BTN_TOUCH, 0);
	}
	input_sync(input);
	mutex_unlock(&cdev->report_mutex);
	point->edge_finger_down = false;
	TPD_DMESG("%s:touch_up id: %d, coord [%d:%d]\n",
		__func__, id, point->point_data[0].x, point->point_data[0].y);

}

void tpd_touch_press(struct input_dev *input, u16 x, u16 y, u16 id, u8 touch_major, u8  pressure)
{
	int i = 0;
	tpd_point_fifo_t *point = NULL;
	struct ztp_device *cdev = tpd_cdev;

	if (cdev->display_rotation) {
		if (x == 0)
			x = x + 1;
		if (y == 0)
			y = y + 1;
	}

	if (input == NULL || id >= MAX_POINTS_SUPPORT) {
		TPD_DMESG("%s:input is NULL? id = %d", __func__, id);
		return;
	}
	point = &point_report_info[id];
	point->input = input;
	if (is_report_point(x, y, id, touch_major, pressure) == false) {
		return;
	}

	point->is_report_point = true;
	if (point->is_moving_in_limit_area) {
		for (i = 0; i < 2; i++) {
			if (point->finger_down == false) {
				point->finger_down = true;
				point->first_report_point_data.x = point->point_data[0].x;
				point->first_report_point_data.y = point->point_data[0].y;
				point_report_reset(id);
				pr_info("ufp_touch_point: touch_down id: %d, coord [%d:%d]\n",
					id, point->point_data[i].x, point->point_data[i].y);
			}
			tpd_touch_report(input, point->point_data[i].x, point->point_data[i].y,
				id, touch_major, pressure);
			if (i == 0) {
				input_sync(input);
				usleep_range(1000, 1500);
			}
		}
	} else {
		if (cdev->tp_jitter_check) {
			if (point->finger_down == false) {
				point->finger_down = true;
				point_report_reset(id);
				point->first_report_point_data.x = x;
				point->first_report_point_data.y = y;
				point->touch_down_timer = jiffies;
				point->jitter_check = true;
				pr_info("ufp_touch_point:touch_down id: %d, coord [%d:%d]\n",
					id, x, y);
				tpd_touch_report(input, x, y, id, touch_major, pressure);
			} else {
				if (point->jitter_check) {
					if (jiffies_to_msecs(jiffies - point->touch_down_timer) > 100) {
						if (abs(point->first_report_point_data.x - x) > cdev->tp_jitter_check
							|| abs(point->first_report_point_data.y - y) > cdev->tp_jitter_check) {
							tpd_touch_report(input, x, y, id, touch_major, pressure);
							point->jitter_check = false;
						}
					} else if (abs(point->first_report_point_data.x - x) > cdev->tp_jitter_check * 3
							|| abs(point->first_report_point_data.y - y) > cdev->tp_jitter_check * 3) {
						tpd_touch_report(input, x, y, id, touch_major, pressure);
						point->jitter_check = false;
					}
				} else {
					tpd_touch_report(input, x, y, id, touch_major, pressure);
				}
			}
		}else {

			if (point->finger_down == false) {
				point->finger_down = true;
				point->first_report_point_data.x = x;
				point->first_report_point_data.y = y;
				point_report_reset(id);
				TPD_DMESG("tpd touch down id: %d, coord [%d:%d]\n", id, x, y);
			}
			tpd_touch_report(input, x, y, id, touch_major, pressure);
		}
	}
	point->last_point.x = x;
	point->last_point.y = y;
	point->is_moving_in_limit_area = false;
}
EXPORT_SYMBOL_GPL(tpd_touch_press);

void tpd_touch_release(struct input_dev *input, u16 id)
{
	tpd_point_fifo_t *point = &point_report_info[id];
	struct ztp_device *cdev = tpd_cdev;

	if (input == NULL || id > MAX_POINTS_SUPPORT) {
		TPD_DMESG("%s:input is NULL? id = %d", __func__, id);
		return;
	}
	if (point->finger_down) {
		mutex_lock(&cdev->report_mutex);
		input_mt_slot(input, id);
		input_mt_report_slot_state(input, MT_TOOL_FINGER, false);
		pr_info("ufp_touch_point: touch_up id: %d, coord [%d:%d]\n", id, point->last_point.x, point->last_point.y);
		mutex_unlock(&cdev->report_mutex);
	}
	if (cdev->edge_long_press_check && !point->is_inside_finger_down && (point->long_press_timer != 0)
		&& (jiffies_to_msecs(jiffies - point->long_press_timer) < cdev->edge_long_press_timer)) {
		edge_point_report(id);
	}
	point->long_press_timer = 0;
	point->finger_down = false;
	point->is_report_point = false;
	point->save_first_down_point = false;
	point->limit_area_log_print = false;
	point->is_inside_finger_down = false;
	point->jitter_check = false;
	point->mistake_touch_check = false;
	point->cancel_clean_edge_area_ponit = false;
}
EXPORT_SYMBOL_GPL(tpd_touch_release);

void tpd_clean_all_event(void)
{
	int i = 0;

	for (i = 0; i < MAX_POINTS_SUPPORT; i++) {
		point_report_info[i].finger_down = false;
		point_report_info[i].edge_finger_down = false;
		point_report_info[i].is_report_point = false;
		point_report_info[i].save_first_down_point = false;
		point_report_info[i].is_moving_in_limit_area = false;
		point_report_info[i].limit_area_log_print = false;
		point_report_info[i].is_inside_finger_down = false;
		point_report_info[i].jitter_check = false;
		point_report_info[i].mistake_touch_check = false;
		point_report_info[i].cancel_clean_edge_area_ponit = false;
	}
}
EXPORT_SYMBOL_GPL(tpd_clean_all_event);

static void edge_point_report(int id)
{
	struct ztp_device *cdev = tpd_cdev;
	tpd_point_fifo_t *point = &point_report_info[id];

	TPD_DMESG("%s:tpd id:%d", __func__, id);
	if (!cdev->tpd_report_wq) {
		TPD_DMESG("%s:tpd_report_wq is null", __func__);
		return;
	}
	if (is_have_inside_point_down()) {
		TPD_DMESG("%s:have inside point down", __func__);
		return;
	}
	mutex_lock(&cdev->report_mutex);
	tpd_touch_report_nolock(point->input, point->point_data[0].x, point->point_data[0].y,
				id, point->point_data[0].touch_major, point->point_data[0].pressure);
	input_sync(point->input);
	point->edge_finger_down = true;
	TPD_DMESG("%s:tpd touch down id: %d, coord [%d:%d]\n",
		__func__, id, point->point_data[0].x, point->point_data[0].y);
	mutex_unlock(&cdev->report_mutex);

	switch (id) {
	case 0:
		queue_delayed_work(cdev->tpd_report_wq, &cdev->tpd_report_work0, msecs_to_jiffies(50));
		break;
	case 1:
		queue_delayed_work(cdev->tpd_report_wq, &cdev->tpd_report_work1, msecs_to_jiffies(50));
		break;
	case 2:
		queue_delayed_work(cdev->tpd_report_wq, &cdev->tpd_report_work2, msecs_to_jiffies(50));
		break;
	case 3:
		queue_delayed_work(cdev->tpd_report_wq, &cdev->tpd_report_work3, msecs_to_jiffies(50));
		break;
	case 4:
		queue_delayed_work(cdev->tpd_report_wq, &cdev->tpd_report_work4, msecs_to_jiffies(50));
		break;
	case 5:
		queue_delayed_work(cdev->tpd_report_wq, &cdev->tpd_report_work5, msecs_to_jiffies(50));
		break;
	case 6:
		queue_delayed_work(cdev->tpd_report_wq, &cdev->tpd_report_work6, msecs_to_jiffies(50));
		break;
	case 7:
		queue_delayed_work(cdev->tpd_report_wq, &cdev->tpd_report_work7, msecs_to_jiffies(50));
		break;
	case 8:
		queue_delayed_work(cdev->tpd_report_wq, &cdev->tpd_report_work8, msecs_to_jiffies(50));
		break;
	case 9:
		queue_delayed_work(cdev->tpd_report_wq, &cdev->tpd_report_work9, msecs_to_jiffies(50));
		break;
	default:
		TPD_DMESG("%s:error id %d", __func__, id);
	}
}

static void point_report_reset(int id)
{
	tpd_point_fifo_t *point = &point_report_info[id];
	struct ztp_device *cdev = tpd_cdev;

	if (point->edge_finger_down) {
		TPD_DMESG("%s:tpd touch up id: %d\n",  __func__, id);
		point->edge_finger_down = false;
		mutex_lock(&cdev->report_mutex);
		input_mt_slot(point->input, id);
		input_mt_report_slot_state(point->input, MT_TOOL_FINGER, false);
		input_sync(point->input);
		mutex_unlock(&cdev->report_mutex);
		usleep_range(1000, 1100);
	}
}

#ifdef CONFIG_TOUCHSCREEN_POINT_REPORT_CHECK
static void ts_point_report_check(struct work_struct *work)
{
	int id = 0;
	struct input_dev *input= point_report_info[0].input;
	struct ztp_device *cdev = tpd_cdev;

	if (input) {
		TPD_DMESG("Release all touch");
		mutex_lock(&cdev->report_mutex);
		for (id = MAX_POINTS_SUPPORT - 1; id >= 0; id--) {
			input_mt_slot(input, id);
			input_mt_report_slot_state(input, MT_TOOL_FINGER, false);
		}
		input_report_key(input, BTN_TOUCH, 0);
		input_sync(input);
		mutex_unlock(&cdev->report_mutex);
		tpd_clean_all_event();
	}
}
#endif

#ifdef CONFIG_VENDOR_ZTE_DEV_MONITOR_SYSTEM
static void tpd_zlog_check(struct work_struct *work)
{
	struct ztp_device *cdev = tpd_cdev;

	TPD_ZLOG("%s enter, tpd_zlog_flag: %d", __func__, cdev->tpd_zlog_flag);
	if (cdev->tpd_zlog_flag == 0) {
		queue_delayed_work(cdev->tpd_report_wq, &cdev->tpd_zlog_check_work, msecs_to_jiffies(300000));
		return;
	}
	if (is_i2c_read_error(cdev->tpd_zlog_flag)) {
		TPD_ZLOG("tpd i2c read err,count:%d.\n", cdev->tpd_zlog_error_count->i2c_read_error_count);
		zlog_client_record(cdev->zlog_client, "tpd i2c read err,count:%d.\n", cdev->tpd_zlog_error_count->i2c_read_error_count);
		zlog_client_notify(cdev->zlog_client,  ZLOG_TP_I2C_R_ERROR_NO);
	}
	if (is_i2c_write_error(cdev->tpd_zlog_flag)) {
		TPD_ZLOG("tpd i2c write err, count:%d.\n", cdev->tpd_zlog_error_count->i2c_write_error_count);
		zlog_client_record(cdev->zlog_client, "tpd i2c write err, count:%d.\n", cdev->tpd_zlog_error_count->i2c_write_error_count);
		zlog_client_notify(cdev->zlog_client,  ZLOG_TP_I2C_W_ERROR_NO);
	}
	if (is_crc_error(cdev->tpd_zlog_flag)) {
		TPD_ZLOG("tpd crc check err, count:%d.\n", cdev->tpd_zlog_error_count->crc_error_count);
		zlog_client_record(cdev->zlog_client, "tpd crc check err, count:%d.\n", cdev->tpd_zlog_error_count->crc_error_count);
		zlog_client_notify(cdev->zlog_client,  ZLOG_TP_CRC_ERROR_NO);
	}
	if (is_firmware_upgrade_error(cdev->tpd_zlog_flag)) {
		TPD_ZLOG("tpd crc check err, count:%d.\n", cdev->tpd_zlog_error_count->crc_error_count);
		zlog_client_record(cdev->zlog_client, "tpd  firmware upgrade err, count:%d.\n", cdev->tpd_zlog_error_count->firmware_upgrade_error_count);
		zlog_client_notify(cdev->zlog_client,  ZLOG_TP_FW_UPGRADE_ERROR_NO);
	}
	if (is_esd_check_error(cdev->tpd_zlog_flag)) {
		TPD_ZLOG("tpd esd check err, count:%d.\n", cdev->tpd_zlog_error_count->esd_check_error_count);
		zlog_client_record(cdev->zlog_client, "tpd esd check err, count:%d.\n", cdev->tpd_zlog_error_count->esd_check_error_count);
		zlog_client_notify(cdev->zlog_client,  ZLOG_TP_ESD_CHECK_ERROR_NO);
	}
	cdev->tpd_zlog_flag = 0;
	queue_delayed_work(cdev->tpd_report_wq, &cdev->tpd_zlog_check_work, msecs_to_jiffies(300000));
}
#endif

int tpd_report_work_init(void)
{
	struct ztp_device *cdev = tpd_cdev;

	TPD_DMESG("%s enter", __func__);
	cdev->tpd_report_wq = create_singlethread_workqueue("tpd_report_wq");

	if (!cdev->tpd_report_wq) {
		goto err_create_tpd_report_wq_failed;
	}
	INIT_DELAYED_WORK(&cdev->tpd_report_work0, tpd_id0_report_work);
	INIT_DELAYED_WORK(&cdev->tpd_report_work1, tpd_id1_report_work);
	INIT_DELAYED_WORK(&cdev->tpd_report_work2, tpd_id2_report_work);
	INIT_DELAYED_WORK(&cdev->tpd_report_work3, tpd_id3_report_work);
	INIT_DELAYED_WORK(&cdev->tpd_report_work4, tpd_id4_report_work);
	INIT_DELAYED_WORK(&cdev->tpd_report_work5, tpd_id5_report_work);
	INIT_DELAYED_WORK(&cdev->tpd_report_work6, tpd_id6_report_work);
	INIT_DELAYED_WORK(&cdev->tpd_report_work7, tpd_id7_report_work);
	INIT_DELAYED_WORK(&cdev->tpd_report_work8, tpd_id8_report_work);
	INIT_DELAYED_WORK(&cdev->tpd_report_work9, tpd_id9_report_work);
#ifdef CONFIG_TOUCHSCREEN_POINT_REPORT_CHECK
	INIT_DELAYED_WORK(&cdev->point_report_check_work, ts_point_report_check);
#endif
#ifdef CONFIG_VENDOR_ZTE_DEV_MONITOR_SYSTEM
	INIT_DELAYED_WORK(&cdev->tpd_zlog_check_work, tpd_zlog_check);
#endif
	return 0;
err_create_tpd_report_wq_failed:
	TPD_DMESG("%s: create tpd report workqueue failed\n", __func__);
	return -ENOMEM;

}

void tpd_report_work_deinit(void)
{
	struct ztp_device *cdev = tpd_cdev;

	TPD_DMESG("%s enter", __func__);
	cancel_delayed_work_sync(&cdev->tpd_report_work0);
	cancel_delayed_work_sync(&cdev->tpd_report_work1);
	cancel_delayed_work_sync(&cdev->tpd_report_work2);
	cancel_delayed_work_sync(&cdev->tpd_report_work3);
	cancel_delayed_work_sync(&cdev->tpd_report_work4);
	cancel_delayed_work_sync(&cdev->tpd_report_work5);
	cancel_delayed_work_sync(&cdev->tpd_report_work6);
	cancel_delayed_work_sync(&cdev->tpd_report_work7);
	cancel_delayed_work_sync(&cdev->tpd_report_work8);
	cancel_delayed_work_sync(&cdev->tpd_report_work9);
#ifdef CONFIG_TOUCHSCREEN_POINT_REPORT_CHECK
	cancel_delayed_work_sync(&cdev->point_report_check_work);
#endif
}

