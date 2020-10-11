/*
 * Support for OmniVision ov8865 8MP camera sensor.
 *
 * Copyright (c) 2011 Intel Corporation. All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/kmod.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/gpio.h>
#include <linux/moduleparam.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/bitops.h>
#include <media/v4l2-device.h>
#include <asm/intel-mid.h>
#include <linux/firmware.h>

#include "ov8865.h"

#ifndef __KERNEL__
#define __KERNEL__
#endif

#include "ov8865_bld_otp.c"
#define OV8865_BIN_FACTOR_MAX	2
#define OV8865_OTP_INPUT_NAME "ov8865_otp.bin"

#define to_ov8865_sensor(sd) container_of(sd, struct ov8865_device, sd)

#define DEBUG_VERBOSE	(1<<0)
#define DEBUG_GAIN_EXP	(1<<1)
#define DEBUG_INTG_FACT	(1<<2)
#define DEBUG_OTP	(1<<4)
static unsigned int debug;
module_param(debug, int, 0644);

struct ov8865_device *global_dev;
static unsigned int ctrl_value;
static int bu64243_vcm_ctrl(const char *val, const struct kernel_param *kp);
static int bu64243_t_focus_abs(struct v4l2_subdev *sd, s32 value);

module_param_call(vcm_ctrl, bu64243_vcm_ctrl, param_get_uint,
				&ctrl_value, S_IRUGO | S_IWUSR);

static int ov8865_raw_size;
static int ov8865_otp_size;
static unsigned char ov8865_raw[DATA_BUF_SIZE];
static unsigned char ov8865_otp_data[DATA_BUF_SIZE];
static u16 exposure_time;

#define OV8865_DEFAULT_LOG_LEVEL 1

static unsigned int log_level = OV8865_DEFAULT_LOG_LEVEL;
module_param(log_level, int, 0644);

#define OV8865_LOG(level, a, ...) \
	do { \
		if (level < log_level) \
			printk(a, ## __VA_ARGS__); \
	} while (0)

static int
ov8865_read_reg(struct i2c_client *client, u16 len, u16 reg, u16 *val)
{
	struct i2c_msg msg[2];
	u16 data[OV8865_SHORT_MAX];
	int err, i;

	if (!client->adapter) {
		v4l2_err(client, "%s error, no client->adapter\n", __func__);
		return -ENODEV;
	}

	/* @len should be even when > 1 */
	if (len > OV8865_BYTE_MAX) {
		v4l2_err(client, "%s error, invalid data length\n", __func__);
		return -EINVAL;
	}

	memset(msg, 0, sizeof(msg));
	memset(data, 0, sizeof(data));

	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].len = I2C_MSG_LENGTH;
	msg[0].buf = (u8 *)data;
	/* high byte goes first */
	data[0] = cpu_to_be16(reg);

	msg[1].addr = client->addr;
	msg[1].len = len;
	msg[1].flags = I2C_M_RD;
	msg[1].buf = (u8 *)data;

	err = i2c_transfer(client->adapter, msg, 2);
	if (err < 0)
		goto error;

	/* high byte comes first */
	if (len == OV8865_8BIT) {
		*val = (u8)data[0];
	} else {
		/* 16-bit access is default when len > 1 */
		for (i = 0; i < (len >> 1); i++)
			val[i] = be16_to_cpu(data[i]);
	}

	return 0;

error:
	dev_err(&client->dev, "read from offset 0x%x error %d", reg, err);
	return err;
}

static int ov8865_i2c_write(struct i2c_client *client, u16 len, u8 *data)
{
	struct i2c_msg msg;
	const int num_msg = 1;
	int ret;
	int retry = 0;

again:
	msg.addr = client->addr;
	msg.flags = 0;
	msg.len = len;
	msg.buf = data;

	ret = i2c_transfer(client->adapter, &msg, 1);

	/*
	 * It is said that Rev 2 sensor needs some delay here otherwise
	 * registers do not seem to load correctly. But tests show that
	 * removing the delay would not cause any in-stablility issue and the
	 * delay will cause serious performance down, so, removed previous
	 * mdelay(1) here.
	 */

	if (ret == num_msg)
		return 0;

	if (retry <= I2C_RETRY_COUNT) {
		dev_err(&client->dev, "retrying i2c write transfer... %d",
			retry);
		retry++;
		msleep(20);
		goto again;
	}

	return ret;
}

static int
ov8865_write_reg(struct i2c_client *client, u16 data_length, u16 reg, u16 val)
{
	int ret;
	unsigned char data[4] = {0};
	u16 *wreg;
	const u16 len = data_length + sizeof(u16); /* 16-bit address + data */

	if (!client->adapter) {
		v4l2_err(client, "%s error, no client->adapter\n", __func__);
		return -ENODEV;
	}

	if (data_length != OV8865_8BIT && data_length != OV8865_16BIT) {
		v4l2_err(client, "%s error, invalid data_length\n", __func__);
		return -EINVAL;
	}

	/* high byte goes out first */
	wreg = (u16 *)data;
	*wreg = cpu_to_be16(reg);

	if (data_length == OV8865_8BIT) {
		data[2] = (u8)(val);
	} else {
		/* OV8865_16BIT */
		u16 *wdata = (u16 *)&data[2];
		*wdata = be16_to_cpu(val);
	}
	//OV8865_LOG(1, "ov8865, ov8865_write_reg ,before ov8865_i2c_write\n");
	ret = ov8865_i2c_write(client, len, data);
	if (ret) {
	  //  OV8865_LOG(1, "ov8865_i2c_write error\n");
		dev_err(&client->dev,
			"write error: wrote 0x%x to offset 0x%x error %d",
			val, reg, ret);
	}
	return ret;
}


/*
 * ov8865_write_reg_array - Initializes a list of MT9M114 registers
 * @client: i2c driver client structure
 * @reglist: list of registers to be written
 *
 * This function initializes a list of registers. When consecutive addresses
 * are found in a row on the list, this function creates a buffer and sends
 * consecutive data in a single i2c_transfer().
 *
 * __ov8865_flush_reg_array, __ov8865_buf_reg_array() and
 * __ov8865_write_reg_is_consecutive() are internal functions to
 * ov8865_write_reg_array_fast() and should be not used anywhere else.
 *
 */

static int __ov8865_flush_reg_array(struct i2c_client *client,
				    struct ov8865_write_ctrl *ctrl)
{
	u16 size;

	if (ctrl->index == 0)
		return 0;

	size = sizeof(u16) + ctrl->index; /* 16-bit address + data */
	ctrl->buffer.addr = cpu_to_be16(ctrl->buffer.addr);
	ctrl->index = 0;
	//OV8865_LOG(1, "ov8865, __ov8865_flush_reg_array, before ov8865_i2c_write\n");
	return ov8865_i2c_write(client, size, (u8 *)&ctrl->buffer);
}

static int __ov8865_buf_reg_array(struct i2c_client *client,
				  struct ov8865_write_ctrl *ctrl,
				  const struct ov8865_reg *next)
{
	int size;
	u16 *data16;

	switch (next->type) {
	case OV8865_8BIT:
		size = 1;
		ctrl->buffer.data[ctrl->index] = (u8)next->val;
		break;
	case OV8865_16BIT:
		size = 2;
		data16 = (u16 *)&ctrl->buffer.data[ctrl->index];
		*data16 = cpu_to_be16((u16)next->val);
		break;
	default:
		return -EINVAL;
	}

	/* When first item is added, we need to store its starting address */
	if (ctrl->index == 0)
		ctrl->buffer.addr = next->reg.sreg;

	ctrl->index += size;

	/*
	 * Buffer cannot guarantee free space for u32? Better flush it to avoid
	 * possible lack of memory for next item.
	 */
	if (ctrl->index + sizeof(u16) >= OV8865_MAX_WRITE_BUF_SIZE)
		__ov8865_flush_reg_array(client, ctrl);

	return 0;
}

static int
__ov8865_write_reg_is_consecutive(struct i2c_client *client,
				  struct ov8865_write_ctrl *ctrl,
				  const struct ov8865_reg *next)
{
	if (ctrl->index == 0)
		return 1;

	return ctrl->buffer.addr + ctrl->index == next->reg.sreg;
}

static int ov8865_write_reg_array(struct i2c_client *client,
				  const struct ov8865_reg *reglist)
{
	const struct ov8865_reg *next = reglist;
	struct ov8865_write_ctrl ctrl;
	int err;

	ctrl.index = 0;
	for (; next->type != OV8865_TOK_TERM; next++) {
		switch (next->type & OV8865_TOK_MASK) {
		case OV8865_TOK_DELAY:
			err = __ov8865_flush_reg_array(client, &ctrl);
			if (err)
				return err;
			msleep(next->val);
			break;

		default:
			/*
			 * If next address is not consecutive, data needs to be
			 * flushed before proceed.
			 */
			if (!__ov8865_write_reg_is_consecutive(client, &ctrl,
								next)) {
				err = __ov8865_flush_reg_array(client, &ctrl);
				if (err)
					return err;
			}
			err = __ov8865_buf_reg_array(client, &ctrl, next);
			if (err) {
				v4l2_err(client, "%s: write error, aborted\n",
					 __func__);
				//OV8865_LOG(1, "ov8865, ov8865_write_reg_array ,error");
				return err;
			}
			break;
		}
	}

	return __ov8865_flush_reg_array(client, &ctrl);
}

static int bu64243_write8(struct v4l2_subdev *sd, int reg, int val)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct bu64243_device *dev = to_bu64243_device(sd);
	struct i2c_msg msg;

	memset(&msg, 0, sizeof(msg));
	msg.addr = BU64243_I2C_ADDR;
	msg.len = 2;
	msg.buf = dev->buffer;
	msg.buf[0] = reg;
	msg.buf[1] = val;

	OV8865_LOG(1, "%s %d reg:0x%x val:0x%x\n", __func__, __LINE__, reg, val);
	return i2c_transfer(client->adapter, &msg, 1);
}

static int bu64243_read8(struct v4l2_subdev *sd, int reg, u8 *data)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct bu64243_device *dev = to_bu64243_device(sd);
	struct i2c_msg msg[2];
	int r;

	OV8865_LOG(1, "%s %d reg:0x%x\n", __func__, __LINE__, reg);
	memset(msg, 0, sizeof(msg));
	msg[0].addr = BU64243_I2C_ADDR;
	msg[0].flags = 0;
	msg[0].len = 1;
	msg[0].buf = dev->buffer;
	msg[0].buf[0] = reg;

	msg[1].addr = BU64243_I2C_ADDR;
	msg[1].flags = I2C_M_RD;
	msg[1].len = 2;
	msg[1].buf = data;

	r = i2c_transfer(client->adapter, msg, ARRAY_SIZE(msg));
	if (r != ARRAY_SIZE(msg))
		return -EIO;

	return 0;
}
static int bu64243_cmd(struct v4l2_subdev *sd, s32 reg, s32 val)
{
	int cmd = 0;
	struct bu64243_device *dev = to_bu64243_device(sd);

	OV8865_LOG(1, "BU64243 reg:%d\n", reg);
	OV8865_LOG(1, "BU64243_PS(dev->power_state):%x-->%x\n", dev->power_state, BU64243_PS(dev->power_state));
	OV8865_LOG(1, "BU64243_EN(dev->output_status):%x-->%x\n", dev->output_status, BU64243_EN(dev->output_status));
	OV8865_LOG(1, "BU64243_PS(dev->W0_W2):%x-->%x\n", reg,	BU64243_W0_W2(reg));
	OV8865_LOG(1, "BU64243_PS(dev->isrc):%x-->%x\n", dev->isrc_mode, BU64243_M(dev->isrc_mode));
	OV8865_LOG(1, "BU64243_PS(dev->D9D8):%x-->%x\n", val, BU64243_D_HI(val));

	cmd = BU64243_PS(dev->power_state) | BU64243_EN(dev->output_status) |
			BU64243_W0_W2(reg) | BU64243_M(dev->isrc_mode) |
			BU64243_D_HI(val);

	return cmd;
}


static int bu64243_init(struct v4l2_subdev *sd)
{
	/* shunyong for power on */
	OV8865_LOG(1, "Bu64243 needs no platform data, nothing need to do\n");
	return 0;
}

static int bu64243_power_up(struct v4l2_subdev *sd)
{
	struct bu64243_device *dev = to_bu64243_device(sd);
	struct ov8865_device *ov8865_dev = to_ov8865_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int r, value;

	OV8865_LOG(1, "%s %d\n", __func__, __LINE__);
	dev->power_state = 1;
	dev->output_status = BU64243_DEFAULT_OUTPUT_STATUS;	/* */
	dev->point_a = BU64243_DEFAULT_POINT_A;			/* 0 um	*/
	dev->point_b = BU64243_DEFAULT_POINT_B;			/*	lens float */
	dev->focus = BU64243_DEFAULT_POINT_C;			/* focus point*/
	dev->res_freq = BU64243_DEFAULT_VCM_FREQ;		/* resonant frequency*/
	dev->slew_rate = BU64243_DEFAULT_SLEW_RATE;		/* slew rate*/
	dev->step_res = BU64243_DEFAULT_RES_SETTING;		/* step resolution */
	dev->step_time = BU64243_DEFAULT_STEP_TIME_SETTING;		/* step time */
	dev->isrc_mode = BU64243_DEFAULT_ISRC_MODE;
	/* jiggle SCL pin to wake up device */
	if (ov8865_dev->otp_data != NULL) {
		/*reprogram VCM point A, B*/
		unsigned int focus_far = ov8865_dev->otp_data[8] | ov8865_dev->otp_data[7];

		if (focus_far < (VCM_ORIENTATION_OFFSET + INTEL_FOCUS_OFFSET + 1 + POINT_AB_OFFSET))
			focus_far = VCM_ORIENTATION_OFFSET + INTEL_FOCUS_OFFSET + 1 + POINT_AB_OFFSET;
		dev->point_b = focus_far - VCM_ORIENTATION_OFFSET - INTEL_FOCUS_OFFSET - 1;			/*	lens loat */
		dev->point_a = dev->point_b - POINT_AB_OFFSET;			/* 0 um	*/
		OV8865_LOG(1, "%s focus far in OTP:%d point a:%d b:%d\n", __func__, focus_far, dev->point_a, dev->point_b);
	}

	r = bu64243_write8(sd, bu64243_cmd(sd, BU64243_VCM_CURRENT, dev->focus), BU64243_D_LO(dev->focus));
	if (r < 0)
		return r;

	value = (BU64243_RFEQ(dev->res_freq)) | BU64243_SRATE((dev->slew_rate));
	r = bu64243_write8(sd, bu64243_cmd(sd, BU64243_PARAM_1, value), BU64243_D_LO(value));
	if (r < 0)
		return r;

	r = bu64243_write8(sd, bu64243_cmd(sd, BU64243_PARAM_2, dev->point_a), BU64243_D_LO(dev->point_a));
	if (r < 0)
		return r;

	r = bu64243_write8(sd, bu64243_cmd(sd, BU64243_PARAM_3, dev->point_b), BU64243_D_LO(dev->point_b));
	if (r < 0)
		return r;

	value = (BU64243_STIME(dev->step_time)) | (BU64243_SRES(dev->step_res));
	r = bu64243_write8(sd, bu64243_cmd(sd, BU64243_PARAM_4, value), BU64243_D_LO(value));
	if (r < 0)
		return r;

	dev->initialized = true;
	v4l2_info(client, "detected bu64243\n");

	return 0;

	/* shunyong for power on */
	OV8865_LOG(1, "power is controlled via sensor control\n");
	//OV8865_LOG(1, "power is controlled via sensor control\n");
	return r;
}

static int bu64243_power_down(struct v4l2_subdev *sd)
{
	struct bu64243_device *dev = to_bu64243_device(sd);
	unsigned int focus_far = 0;
	u8 data[10];
	struct ov8865_device *ov8865_dev = to_ov8865_sensor(sd);
	int r, i;
	s32 current_value, average, focus_value;

	if (ov8865_dev->otp_data != NULL) {
		/*reprogram VCM point A, B*/
		OV8865_LOG(1, "otp data valid !!!enter %s\n", __func__);
		focus_far = ov8865_dev->otp_data[8] | ov8865_dev->otp_data[7];
		if (focus_far < (VCM_ORIENTATION_OFFSET + INTEL_FOCUS_OFFSET + 1 + POINT_AB_OFFSET))
			focus_far = VCM_ORIENTATION_OFFSET + INTEL_FOCUS_OFFSET + 1 + POINT_AB_OFFSET;
		OV8865_LOG(1, "%s focus far in OTP:%d point a:%d b:%d\n", __func__, focus_far, dev->point_a, dev->point_b);
	}
	r = bu64243_write8(sd, bu64243_cmd(sd, BU64243_VCM_CURRENT, focus_far), BU64243_D_LO(focus_far));
	if (r < 0)
		return r;
	bu64243_read8(sd, bu64243_cmd(sd, BU64243_VCM_CURRENT, 0), data);
	current_value = ((data[0] & 0x03)<<8) + data[1];
	average = current_value/10;
	OV8865_LOG(1, "current_value = %d  , average = %d\n", current_value, average);
	for (i = 1; i < 11; i++) {
		focus_value = current_value - average * i;
		OV8865_LOG(1, "focus_value = %d\n", focus_value);
		r = bu64243_write8(sd, bu64243_cmd(sd, BU64243_VCM_CURRENT, focus_value), BU64243_D_LO(focus_value));
		if (r < 0)
			return r;
		mdelay(5);
		}
	/* shunyong for power on */
	OV8865_LOG(1, "power is controlled via sensor control\n");
	return 0;
}

static void bu64243_dump_regs(struct v4l2_subdev *sd)
{
	int i;
	u8 data[10];

	OV8865_LOG(2, "%s %d\n", __func__, __LINE__);

	for (i = 0; i < 5; i++) {
		bu64243_read8(sd, bu64243_cmd(sd, i, 0), data);
		OV8865_LOG(2, "%s %d reg:%d data[0]:0x%x data[1]:0x%x\n", __func__, __LINE__, i, data[0], data[1]);
	}
}

static int bu64243_t_focus_abs(struct v4l2_subdev *sd, s32 value)
{
	struct bu64243_device *dev = to_bu64243_device(sd);
	int r;

	OV8865_LOG(1, "%s %d\n", __func__, __LINE__);
	if (!dev->initialized)
		return -ENODEV;

	value = clamp(value, 0, BU64243_MAX_FOCUS_POS);
	OV8865_LOG(1, "%s %d value:%x cmd:%x low:%x\n", __func__, __LINE__, value, bu64243_cmd(sd, BU64243_VCM_CURRENT, value), BU64243_D_LO(value));
	r = bu64243_write8(sd, bu64243_cmd(sd, BU64243_VCM_CURRENT, value), BU64243_D_LO(value));
	if (r < 0)
		return r;

	ktime_get_real_ts64(&dev->focus_time);
	dev->focus = value;
	return 0;
}



static int bu64243_vcm_ctrl(const char *val, const struct kernel_param *kp)
{
	int rv = param_set_int(val, kp);

	OV8865_LOG(2, "%s %d\n", __func__, __LINE__);

	if (rv)
		return rv;

	/* Enable power */
	OV8865_LOG(2, "%s %d enable power\n", __func__, __LINE__);
	/* TODO: need to do something for VCM here? */
	// ret = global_dev->platform_data->power_ctrl(&(global_dev->sd), 1);
	mdelay(200);
	bu64243_power_up(&(global_dev->sd));
	mdelay(10);

	bu64243_t_focus_abs(&(global_dev->sd), ctrl_value);
	OV8865_LOG(2, "%s %d ctrl value:0x%x\n", __func__, __LINE__, ctrl_value);

	bu64243_dump_regs(&(global_dev->sd));
	return 0;
}

/* Start group hold for the following register writes */
static int ov8865_grouphold_start(struct v4l2_subdev *sd)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	const int group = 0;

	return ov8865_write_reg(client, OV8865_8BIT,
				OV8865_GROUP_ACCESS,
				group | OV8865_GROUP_ACCESS_HOLD_START);
}

/* End group hold and delay launch it */
static int ov8865_grouphold_launch(struct v4l2_subdev *sd)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	const int group = 0;
	int ret;

	/* End group */
	ret = ov8865_write_reg(client, OV8865_8BIT,
					OV8865_GROUP_ACCESS,
					group | OV8865_GROUP_ACCESS_HOLD_END);
	if (ret)
		return ret;

	/* Delay launch group (during next vertical blanking) */
	return ov8865_write_reg(client, OV8865_8BIT,
				OV8865_GROUP_ACCESS,
				group | OV8865_GROUP_ACCESS_DELAY_LAUNCH);
}


static int ov8865_g_priv_int_data(struct v4l2_subdev *sd,
				  struct v4l2_private_int_data *priv)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct ov8865_device *dev = to_ov8865_sensor(sd);
	u8 __user *to = priv->data;
	u32 read_size = priv->size;
	int ret;
	int i;
	u8 *pdata;

	/* No need to copy data if size is 0 */
	if (!read_size)
		goto out;

	if (dev->otp_data == NULL) {
		dev_err(&client->dev, "OTP data not available");
		return -1;
	}
	/* Correct read_size value only if bigger than maximum */
	if (read_size > DATA_BUF_SIZE)
		read_size = DATA_BUF_SIZE;

	pdata = (u8 *) dev->otp_data;

	for (i = 0; i < 10; i++)
		OV8865_LOG(2, "%d %x\n", i, pdata[i]);

	OV8865_LOG(2, "yangsy %s %d read:%d\n", __func__, __LINE__, read_size);
	ret = copy_to_user(to, dev->otp_data, read_size);
	if (ret) {
		dev_err(&client->dev, "%s: failed to copy OTP data to user\n",
			 __func__);
		return -EFAULT;
	}
out:

	/* Return correct size */
	priv->size = DATA_BUF_SIZE;
	OV8865_LOG(2, "%s %d qurry size:%d\n", __func__, __LINE__, priv->size);

	return 0;
}

static int __ov8865_get_max_fps_index(const struct ov8865_fps_setting *fps_settings)
{
	int i;

	for (i = 0; i < MAX_FPS_OPTIONS_SUPPORTED; i++) {
		if (fps_settings[i].fps == 0)
			break;
	}

	return i - 1;
}

static int __ov8865_update_frame_timing(struct v4l2_subdev *sd, int exposure,
					u16 *hts, u16 *vts)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret;

	/* Increase the VTS to match exposure + 14 */
	if (exposure > *vts - OV8865_INTEGRATION_TIME_MARGIN)
		*vts = (u16) exposure + OV8865_INTEGRATION_TIME_MARGIN;

	ret = ov8865_write_reg(client, OV8865_16BIT, OV8865_TIMING_HTS, *hts);
	if (ret)
		return ret;

	return ov8865_write_reg(client, OV8865_16BIT, OV8865_TIMING_VTS, *vts);
}

static int __ov8865_set_exposure(struct v4l2_subdev *sd, int exposure, int gain,
				 int dig_gain, u16 *hts, u16 *vts)
{
	/* shunyong: disabled exposure setting */

	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int exp_val, ret;

	/* Update frame timings. Expsure must be minimum <  vts-14 */
	ret = __ov8865_update_frame_timing(sd, exposure, hts, vts);
	if (ret)
		return ret;

	/* For OV8835, the low 4 bits are fraction bits and must be kept 0 */
	exp_val = exposure << 4;
	exposure_time = exposure;
	ret = ov8865_write_reg(client, OV8865_8BIT,
			       OV8865_LONG_EXPO+2, exp_val & 0xFF);
	if (ret)
		return ret;

	ret = ov8865_write_reg(client, OV8865_8BIT,
			       OV8865_LONG_EXPO+1, (exp_val >> 8) & 0xFF);
	if (ret)
		return ret;

	ret = ov8865_write_reg(client, OV8865_8BIT,
			       OV8865_LONG_EXPO, (exp_val >> 16) & 0x0F);
	if (ret)
		return ret;

	/* Digital gain : to all MWB channel gains */
	if (dig_gain) {
		ret = ov8865_write_reg(client, OV8865_8BIT,
				OV8865_DIGI_GAIN, ((dig_gain >> 6) & 0xFF));
		if (ret)
			return ret;
		ret = ov8865_write_reg(client, OV8865_8BIT,
				(OV8865_DIGI_GAIN + 1), dig_gain & 0x3F);
		if (ret)
			return ret;

	}

	return ov8865_write_reg(client, OV8865_16BIT, OV8865_AGC_ADJ, gain & 0x1FFF);
}

static int ov8865_set_exposure(struct v4l2_subdev *sd, int exposure,
			       int gain, int dig_gain)
{
	struct ov8865_device *dev = to_ov8865_sensor(sd);
	const struct ov8865_resolution *res;
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	u16 hts, vts;
	int ret;

	mutex_lock(&dev->input_lock);

	/* Validate exposure:  cannot exceed 16bit value */
	exposure = clamp_t(int, exposure, 0, OV8865_MAX_EXPOSURE_VALUE);

	/* Validate gain: must not exceed maximum 8bit value */
	gain = clamp_t(int, gain, 0, OV8865_MAX_GAIN_VALUE);

	/* Validate digital gain: must not exceed 12 bit value*/
	dig_gain = clamp_t(int, dig_gain, 0, OV8865_MWB_GAIN_MAX);

	/* Group hold is valid only if sensor is streaming. */
	if (dev->streaming) {
		ret = ov8865_grouphold_start(sd);
		if (ret)
			goto out;
	}

	res = &dev->curr_res_table[dev->fmt_idx];
	hts = res->fps_options[dev->fps_index].pixels_per_line;
	vts = res->fps_options[dev->fps_index].lines_per_frame;

	ret = __ov8865_set_exposure(sd, exposure, gain, dig_gain, &hts, &vts);
	if (ret)
		goto out;

	/* Updated the device variable. These are the current values. */
	dev->gain = gain;
	dev->exposure = exposure;
	dev->digital_gain = dig_gain;

out:
	/* Group hold launch - delayed launch */
	if (dev->streaming)
		ret = ov8865_grouphold_launch(sd);

	mutex_unlock(&dev->input_lock);

	if (debug & DEBUG_GAIN_EXP) {
		u16 val3500, val3501, val3502;
		u16 val3503, val3508, val3509, val350a, val350b, val501e;
		int val_exp, val_again;

		OV8865_LOG(2, "%s %d exposure:%d(0x%x) gain:%d(8x%x)\n", __func__,
			 __LINE__, exposure, exposure, gain, gain);
		ov8865_read_reg(client, OV8865_8BIT, 0x3500, &val3500);
		ov8865_read_reg(client, OV8865_8BIT, 0x3501, &val3501);
		ov8865_read_reg(client, OV8865_8BIT, 0x3502, &val3502);
		val_exp = (val3502 + (val3501 << 8) + (val3500 << 16)) >> 4;
		OV8865_LOG(2, "%s %d 0x3500:%x 0x3501:%x 0x3502:%x exposure(dec):%d\n", __func__,
			 __LINE__, val3500, val3501, val3502, val_exp);

		ov8865_read_reg(client, OV8865_8BIT, 0x3503, &val3503);
		ov8865_read_reg(client, OV8865_8BIT, 0x3508, &val3508);
		ov8865_read_reg(client, OV8865_8BIT, 0x3509, &val3509);
		ov8865_read_reg(client, OV8865_8BIT, 0x350a, &val350a);
		ov8865_read_reg(client, OV8865_8BIT, 0x350b, &val350b);
		ov8865_read_reg(client, OV8865_8BIT, 0x501e, &val501e);

		val_again = ((val3508 << 8) + val3509) >> 7;
		OV8865_LOG(2, "%s %d 0x3503:%x 0x3508:%x 0x3509:%x val_again:(dec):%d\n", __func__,
			__LINE__, val3503, val3508, val3509, val_again);

		OV8865_LOG(2, "%s %d 0x501e:%x 0x350a:%x 0x350b:%x\n", __func__, __LINE__,
			val501e, val350a, val350b);
	}

	return ret;
}

static long ov8865_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
{
	switch (cmd) {
	default:
		return -EINVAL;
	}
	return 0;
}

static int ov8865_init_registers(struct v4l2_subdev *sd)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct ov8865_device *dev = to_ov8865_sensor(sd);
	//OV8865_LOG(1, "ov8865, ov8865_init_registers\n");

	dev->basic_settings_list = ov8865_BasicSettings;

	return ov8865_write_reg_array(client, dev->basic_settings_list);
}

static int ov8865_init(struct v4l2_subdev *sd, u32 val)
{
	struct ov8865_device *dev = to_ov8865_sensor(sd);
	int ret;

	mutex_lock(&dev->input_lock);
	ret = ov8865_init_registers(sd);
	mutex_unlock(&dev->input_lock);

	return ret;
}

static void ov8865_uninit(struct v4l2_subdev *sd)
{
	struct ov8865_device *dev = to_ov8865_sensor(sd);

	dev->exposure = 0;
	dev->gain     = 0;
	dev->digital_gain = 0;
}

/* Get GPIOs defined in dep_dev _CRS */
static int gpio_crs_get(struct ov8865_device *sensor, struct device *dep_dev)
{
	sensor->xshutdn = devm_gpiod_get_index(dep_dev, NULL, 0, GPIOD_ASIS);
	if (IS_ERR(sensor->xshutdn)) {
		dev_err(dep_dev, "Couldn't get GPIO XSHUTDN\n");
		return -EINVAL;
	}

	sensor->pwdnb = devm_gpiod_get_index(dep_dev, NULL, 1, GPIOD_ASIS);
	if (IS_ERR(sensor->pwdnb)) {
		dev_err(dep_dev, "Couldn't get GPIO PWDNB\n");
		return -EINVAL;
	}

	sensor->led_gpio = devm_gpiod_get_index(dep_dev, NULL, 2, GPIOD_ASIS);
	if (IS_ERR(sensor->led_gpio))
		dev_info(dep_dev,
			 "Couldn't get GPIO LED. Maybe not exist, continue anyway.\n");

	return 0;
}

/* Put GPIOs defined in dep_dev _CRS */
static void gpio_crs_put(struct ov8865_device *sensor)
{
	gpiod_put(sensor->xshutdn);
	gpiod_put(sensor->pwdnb);
	if (!IS_ERR(sensor->led_gpio))
		gpiod_put(sensor->led_gpio);
}

/* Control GPIOs defined in dep_dev _CRS */
static int gpio_crs_ctrl(struct v4l2_subdev *sd, bool flag)
{
	struct ov8865_device *sensor = to_ov8865_sensor(sd);

	gpiod_set_value_cansleep(sensor->xshutdn, flag);
	gpiod_set_value_cansleep(sensor->pwdnb, flag);
	if (!IS_ERR(sensor->led_gpio))
		gpiod_set_value_cansleep(sensor->led_gpio, flag);

	return 0;
}

static int power_up(struct v4l2_subdev *sd)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret;

	ret = gpio_crs_ctrl(sd, true);
	if (ret)
		goto fail_power;

	/* Minumum delay is 8192 clock cycles before first i2c transaction,
	 * which is 1.37 ms at the lowest allowed clock rate 6 MHz
	 */
	usleep_range(2000, 2100);

	return 0;

fail_power:
	gpio_crs_ctrl(sd, false);
	dev_err(&client->dev, "sensor power-up failed\n");
	return ret;
}

static int power_down(struct v4l2_subdev *sd)
{
	int ret;

	ret = gpio_crs_ctrl(sd, false);

	return ret;
}

static int __ov8865_s_power(struct v4l2_subdev *sd, int on)
{
	struct ov8865_device *dev = to_ov8865_sensor(sd);
	int ret, r;

	if (on == 0) {
		ov8865_uninit(sd);
		/* shunyong: disable VCM for PO */
		r = bu64243_power_down(sd);
		ret = power_down(sd);

		if (ret == 0)
			ret = r;
		dev->power = 0;
	} else {
		//OV8865_LOG(1, "ov8865 start power-up");
		ret = power_up(sd);
		if (ret)
			return ret;
		/* shunyong: disable VCM for PO */
		ret = bu64243_power_up(sd);
		if (ret) {
			power_down(sd);
			return ret;
		}
		//OV8865_LOG(1, "ov8865 before ov8865_init_registers\n");
		ret = ov8865_init_registers(sd);
		// OV8865_LOG(1, "ov8865 after ov8865_init_registers\n");
		if (ret) {
		// OV8865_LOG(1, "ov8865 bu64243_power_down power_down ret = %d\n",ret);
			bu64243_power_down(sd);
			power_down(sd);
		}
		dev->power = 1;
	}

	return ret;
}

static int ov8865_s_power(struct v4l2_subdev *sd, int on)
{
	int ret;
	struct ov8865_device *dev = to_ov8865_sensor(sd);

	mutex_lock(&dev->input_lock);
	ret = __ov8865_s_power(sd, on);
	mutex_unlock(&dev->input_lock);

	return ret;
}

/* Return value of the specified register, first try getting it from
 * the register list and if not found, get from the sensor via i2c.
 */
static int ov8865_get_register(struct v4l2_subdev *sd, int reg,
			       const struct ov8865_reg *reglist)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	const struct ov8865_reg *next;
	u16 val;

	/* Try if the values is in the register list */
	for (next = reglist; next->type != OV8865_TOK_TERM; next++) {
		if (next->type != OV8865_8BIT) {
			v4l2_err(sd, "only 8-bit registers supported\n");
			return -ENXIO;
		}
		if (next->reg.sreg == reg)
			return next->val;
	}

	/* If not, read from sensor */
	if (ov8865_read_reg(client, OV8865_8BIT, reg, &val)) {
		v4l2_err(sd, "failed to read register 0x%04X\n", reg);
		return -EIO;
	}

	return val;
}

static int ov8865_get_register_16bit(struct v4l2_subdev *sd, int reg,
				     const struct ov8865_reg *reglist,
				     unsigned int *value)
{
	int high, low;

	high = ov8865_get_register(sd, reg, reglist);
	if (high < 0)
		return high;

	low = ov8865_get_register(sd, reg + 1, reglist);
	if (low < 0)
		return low;

	*value = ((u8) high << 8) | (u8) low;
	return 0;
}

#if 0
static int __ov8865_s_frame_interval(struct v4l2_subdev *sd,
				     struct v4l2_subdev_frame_interval *interval)
{
	/* shunyong disabel s_frame_interval for PO */
#if 0
	struct ov8865_device *dev = to_ov8865_sensor(sd);
	struct camera_mipi_info *info = v4l2_get_subdev_hostdata(sd);
	const struct ov8865_resolution *res =
		res = &dev->curr_res_table[dev->fmt_idx];
	int i;
	int ret;
	int fps;
	u16 hts;
	u16 vts;

	if (!interval->interval.numerator)
		interval->interval.numerator = 1;

	fps = interval->interval.denominator / interval->interval.numerator;

	/* Ignore if we are already using the required FPS. */
	if (fps == res->fps_options[dev->fps_index].fps)
		return 0;

	dev->fps_index = 0;

	/* Go through the supported FPS list */
	for (i = 0; i < MAX_FPS_OPTIONS_SUPPORTED; i++) {
		if (!res->fps_options[i].fps)
			break;
		if (abs(res->fps_options[i].fps - fps)
		    < abs(res->fps_options[dev->fps_index].fps - fps))
			dev->fps_index = i;
	}

	/* Get the new Frame timing values for new exposure */
	hts = res->fps_options[dev->fps_index].pixels_per_line;
	vts = res->fps_options[dev->fps_index].lines_per_frame;

	/* update frametiming. Conside the curren exposure/gain as well */
	ret = __ov8865_set_exposure(sd, dev->exposure, dev->gain,
					dev->digital_gain, &hts, &vts);
	if (ret)
		return ret;

	interval->interval.denominator = res->fps_options[dev->fps_index].fps;
	interval->interval.numerator = 1;
#endif
	return 0;
}
#endif
/*
 * distance - calculate the distance
 * @res: resolution
 * @w: width
 * @h: height
 *
 * Get the gap between resolution and w/h.
 * res->width/height smaller than w/h wouldn't be considered.
 * Returns the value of gap or -1 if fail.
 */
/* tune this value so that the DVS resolutions get selected properly,
 * but make sure 16:9 does not match 4:3.
 */
#define LARGEST_ALLOWED_RATIO_MISMATCH 500
static int distance(struct ov8865_resolution const *res, const u32 w,
		    const u32 h)
{
	unsigned int w_ratio = ((res->width<<13)/w);
	unsigned int h_ratio = ((res->height<<13)/h);
	int match   = abs(((w_ratio<<13)/h_ratio) - 8192);

	if ((w_ratio < 8192) || (h_ratio < 8192)
		|| (match > LARGEST_ALLOWED_RATIO_MISMATCH))
		return -1;

	return w_ratio + h_ratio;
}

/*
 * Returns the nearest higher resolution index.
 * @w: width
 * @h: height
 * matching is done based on enveloping resolution and
 * aspect ratio. If the aspect ratio cannot be matched
 * to any index, -1 is returned.
 */
static int nearest_resolution_index(struct v4l2_subdev *sd, int w, int h)
{
	int i;
	int idx = -1;
	int dist;
	int min_dist = INT_MAX;
	const struct ov8865_resolution *tmp_res = NULL;
	struct ov8865_device *dev = to_ov8865_sensor(sd);

	OV8865_LOG(1, "-%lx %lx  w:%d h:%d\r\n", (unsigned long)dev->curr_res_table, (unsigned long)ov8865_res_preview, w, h);
	if ((dev->curr_res_table == ov8865_res_preview) ||
		(dev->curr_res_table == ov8865_res_still)) {
		if ((((w == 1332) && (h == 1092))) ||
			(((w == 1320) && (h == 1080))) || (((w == 1280) && (h == 720))) ||
			(((w == 1292) && (h == 732))) ||
			(((w == 1024) && (h == 576))) || (((w == 1036) && (h == 588))) ||
			(((w == 720) && (h == 480))) || (((w == 732) && (h == 492))) ||
			(((w == 640) && (h == 360))) || (((w == 652) && (h == 372))) ||
			(((w == 320) && (h == 180))) || (((w == 332) && (h == 192)))) {
			w = 1632;
			h = 1224;
		}
		if ((((w == 1920) && (h == 1080))) || (((w == 1932) && (h == 1092)))) {
			w = 1936;
			h = 1096;
		}
	}
	if (dev->curr_res_table == ov8865_res_still) {
		if ((((w == 176) && (h == 144))) ||
			(((w == 188) && (h == 156)))) {
			w = 1632;
			h = 1224;
		}
	}
#if 0
	OV8865_LOG(1, "%lx %lx  w:%d h:%d\r\n", (unsigned long)dev->curr_res_table, (unsigned long)ov8865_res_preview, w, h);
	if ((dev->curr_res_table == ov8865_res_preview) ||
		(dev->curr_res_table == ov8865_res_still)) {
		if ((((w == 1332) && (h == 1092))) ||
			(((w == 1320) && (h == 1080)))) {
			w = 1632;
			h = 1224;
		}
	}
	if (dev->curr_res_table == ov8865_res_still) {
		if ((((w == 176) && (h == 144))) ||
			(((w == 188) && (h == 156)))) {
			w = 1632;
			h = 1224;
		}
	}
#endif
	for (i = 0; i < dev->entries_curr_table; i++) {
		tmp_res = &dev->curr_res_table[i];
		dist = distance(tmp_res, w, h);
		if (dist == -1)
			continue;
		if (dist < min_dist) {
			min_dist = dist;
			idx = i;
		}
	}
	return idx;
}

static int get_resolution_index(struct v4l2_subdev *sd, int w, int h)
{
	int i;
	struct ov8865_device *dev = to_ov8865_sensor(sd);

	for (i = 0; i < dev->entries_curr_table; i++) {
		if (w != dev->curr_res_table[i].width)
			continue;
		if (h != dev->curr_res_table[i].height)
			continue;
		/* Found it */
		return i;
	}
	return -1;
}

static int ov8865_detect(struct i2c_client *client, u16 *id, u8 *revision)
{
	struct i2c_adapter *adapter = client->adapter;
	int ret;

	OV8865_LOG(1, "%s %d\n", __func__, __LINE__);
	/* i2c check */
	if (!i2c_check_functionality(adapter, I2C_FUNC_I2C))
		return -ENODEV;

	OV8865_LOG(1, "%s %d\n", __func__, __LINE__);

	/* check sensor chip ID - are same for both 8865 and 8835 modules */
	ret = ov8865_read_reg(client, OV8865_16BIT, OV8865_CHIP_ID_HIGH, id);
	dev_info(&client->dev, "chip_id = 0x%4.4x\n", *id);

	if (ret)
		return ret;

	OV8865_LOG(1, "%s %d\n", __func__, __LINE__);
	/* This always reads as 0x8865, even on 8835. */
	dev_info(&client->dev, "chip_id = 0x%4.4x\n", *id);
	if (*id != OV8865_CHIP_ID)
		return -ENODEV;

	return 0;
}

/*
 * ov8865 stream on/off
 */
static int ov8865_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct ov8865_device *dev = to_ov8865_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret;

	mutex_lock(&dev->input_lock);

	/* power_on() here before streaming for regular PCs. */
	if (enable) {
		ret = power_up(sd);
		if (ret) {
			dev_err(&client->dev, "sensor power-up error\n");
			power_down(sd);
			goto out;
		}
	}

	//OV8865_LOG(1, "ov8865, ov8865_s_stream, before ov8865_write_reg\n");
	ret = ov8865_write_reg(client, OV8865_8BIT, 0x0100, enable ? 1 : 0);
	if (ret != 0) {
		mutex_unlock(&dev->input_lock);
		v4l2_err(client, "failed to set streaming\n");
	//	OV8865_LOG(1, "failed to set streaming\n");
		return ret;
	}

	dev->streaming = enable;

	/* power_off() here after streaming for regular PCs. */
	if (!enable)
		power_down(sd);

out:
	mutex_unlock(&dev->input_lock);

	return 0;
}

static int ov8865_s_config(struct v4l2_subdev *sd, int irq)
{
	struct ov8865_device *dev = to_ov8865_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	u8 sensor_revision = 0;
	u16 sensor_id;
	int ret;

	mutex_lock(&dev->input_lock);

	ret = __ov8865_s_power(sd, 1);
	if (ret) {
		mutex_unlock(&dev->input_lock);
		v4l2_err(client, "ov8865 power-up err.\n");
		return ret;
	}
	OV8865_LOG(1, "%s %d ov8865 s_power done\n", __func__, __LINE__);
	dev->otp_data = NULL;
	//ret = request_firmware(&fw, OV8865_OTP_INPUT_NAME, &client->dev);
	//if (ret) {
		OV8865_LOG(2, "ov8865 load from user-space failed, load from sensor\n");
		ov8865_write_reg(client, OV8865_8BIT, 0x5002, 0x00);
		ov8865_write_reg(client, OV8865_8BIT, 0x0100, 0x01);
		ret = ov8865_otp_read(client, ov8865_raw, &ov8865_raw_size);
		ov8865_write_reg(client, OV8865_8BIT, 0x5002, 0x08);
		ov8865_write_reg(client, OV8865_8BIT, 0x0100, 0x00);
		if (!ret) {
			//OV8865_LOG(1, "ov8865 otp read done\n");
			ret = ov8865_otp_trans(ov8865_raw, ov8865_raw_size, ov8865_otp_data, &ov8865_otp_size);
			if (!ret) {
			//	OV8865_LOG(1, "ov8865 otp trans done\n");
				dev->otp_data = ov8865_otp_data;
			} else
				OV8865_LOG(1, "ov8865 otp trans failed\n");
		}
	//} else {
	//	OV8865_LOG(2, "ov8865 load from user-space success size:0x%x\n", fw->size);
	//	memcpy(ov8865_raw, fw->data, fw->size);
	//	ov8865_raw_size = fw->size;
	//	ret = ov8865_otp_trans(ov8865_raw, ov8865_raw_size, ov8865_otp_data, &ov8865_otp_size);
	//	if (!ret) {
	//		OV8865_LOG(2, "ov8865 otp trans done\n");
	//		dev->otp_data = ov8865_otp_data;
	//	} else
	//		OV8865_LOG(2, "ov8865 otp trans failed\n");
	//}

	OV8865_LOG(1, "%s %d ov8865 csi_cfg done\n", __func__, __LINE__);
	/* config & detect sensor */
	ret = ov8865_detect(client, &sensor_id, &sensor_revision);
	if (ret) {
		v4l2_err(client, "ov8865_detect err s_config.\n");
		goto out;
	}
	OV8865_LOG(1, "%s %d ov8865 detect done\n", __func__, __LINE__);
	dev->sensor_id = sensor_id;
	dev->sensor_revision = sensor_revision;

	/* power off sensor */
	ret = __ov8865_s_power(sd, 0);
	mutex_unlock(&dev->input_lock);
	if (ret) {
		v4l2_err(client, "ov8865 power-down err.\n");
		return ret;
	}

	OV8865_LOG(1, "%s %d ov8865 s_config successfully\n", __func__, __LINE__);
	return 0;

out:
	__ov8865_s_power(sd, 0);
	mutex_unlock(&dev->input_lock);
	dev_err(&client->dev, "sensor power-gating failed\n");
	return ret;
}

static int
ov8865_enum_mbus_code(struct v4l2_subdev *sd, struct v4l2_subdev_pad_config *cfg,
		      struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index)
		return -EINVAL;
	code->code = MEDIA_BUS_FMT_SBGGR10_1X10;

	return 0;
}

static int
ov8865_enum_frame_size(struct v4l2_subdev *sd, struct v4l2_subdev_pad_config *cfg,
		       struct v4l2_subdev_frame_size_enum *fse)
{
	int index = fse->index;
	struct ov8865_device *dev = to_ov8865_sensor(sd);

	mutex_lock(&dev->input_lock);
	if (index >= dev->entries_curr_table) {
		mutex_unlock(&dev->input_lock);
		return -EINVAL;
	}

	fse->min_width = dev->curr_res_table[index].width;
	fse->min_height = dev->curr_res_table[index].height;
	fse->max_width = dev->curr_res_table[index].width;
	fse->max_height = dev->curr_res_table[index].height;
	mutex_unlock(&dev->input_lock);

	return 0;
}

static int ov8865_enum_frame_interval(struct v4l2_subdev *sd,
				      struct v4l2_subdev_pad_config *cfg,
				      struct v4l2_subdev_frame_interval_enum *fie)
{
	unsigned int index = fie->index;
	int fmt_index;
	struct ov8865_device *dev = to_ov8865_sensor(sd);
	const struct ov8865_resolution *res;

	mutex_lock(&dev->input_lock);

	/*
	 * since the isp will donwscale the resolution to the right size,
	 * find the nearest one that will allow the isp to do so important to
	 * ensure that the resolution requested is padded correctly by the
	 * requester, which is the atomisp driver in this case.
	 */
	fmt_index = nearest_resolution_index(sd, fie->width, fie->height);
	if (-1 == fmt_index)
		fmt_index = dev->entries_curr_table - 1;

	res = &dev->curr_res_table[fmt_index];

	/* Check if this index is supported */
	if (index > __ov8865_get_max_fps_index(res->fps_options)) {
		mutex_unlock(&dev->input_lock);
		return -EINVAL;
	}

	/* TODO: can we really drop this? */
	// fie->type = V4L2_FRMIVAL_TYPE_DISCRETE;

	fie->interval.numerator = 1;
	fie->interval.denominator = res->fps_options[index].fps;

	mutex_unlock(&dev->input_lock);

	return 0;
}

static struct v4l2_mbus_framefmt *
__ov8865_get_pad_format(struct ov8865_device *sensor,
			struct v4l2_subdev_pad_config *cfg, unsigned int pad,
			enum v4l2_subdev_format_whence which)
{
	if (which == V4L2_SUBDEV_FORMAT_TRY)
		return v4l2_subdev_get_try_format(&sensor->sd, cfg, pad);

	return &sensor->format;
}

static int
ov8865_get_pad_format(struct v4l2_subdev *sd, struct v4l2_subdev_pad_config *cfg,
		      struct v4l2_subdev_format *fmt)
{
	struct ov8865_device *dev = to_ov8865_sensor(sd);

	fmt->format = *__ov8865_get_pad_format(dev, cfg, fmt->pad, fmt->which);

	return 0;
}

static int
ov8865_set_pad_format(struct v4l2_subdev *sd, struct v4l2_subdev_pad_config *cfg,
		      struct v4l2_subdev_format *fmt)
{
	struct ov8865_device *dev = to_ov8865_sensor(sd);
	struct v4l2_mbus_framefmt *format =
			__ov8865_get_pad_format(dev, cfg, fmt->pad, fmt->which);

	*format = fmt->format;

	return 0;
}

static int ov8865_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ov8865_device *dev = container_of(
		ctrl->handler, struct ov8865_device, ctrl_handler);
	struct i2c_client *client = v4l2_get_subdevdata(&dev->sd);
	int ret;

	/* input_lock is taken by the control framework, so it
	 * doesn't need to be taken here.
	 */

	/* We only handle V4L2_CID_RUN_MODE for now. */
	switch (ctrl->id) {
	case V4L2_CID_RUN_MODE:
		switch (ctrl->val) {
		default:
			dev->curr_res_table = ov8865_res_preview;
			dev->entries_curr_table = ARRAY_SIZE(ov8865_res_preview);
		}

		dev->fmt_idx = 0;
		dev->fps_index = 0;
		return 0;
	case V4L2_CID_TEST_PATTERN:
		ret = power_up(&dev->sd);
		if (ret) {
			dev_err(&client->dev, "sensor power-up error\n");
			power_down(&dev->sd);
			return ret;
		}
		ret = ov8865_write_reg(client, OV8865_16BIT, 0x3070,
				       ctrl->val);
		power_down(&dev->sd);
		return ret;
	/* shunyong: disable focus when PO */
	case V4L2_CID_FOCUS_ABSOLUTE:
		return bu64243_t_focus_abs(&dev->sd, ctrl->val);
	}
	return -EINVAL; /* Should not happen. */
}

static int ov8865_g_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ov8865_device *dev = container_of(
		ctrl->handler, struct ov8865_device, ctrl_handler);

	switch (ctrl->id) {
	break;
	case V4L2_CID_EXPOSURE_ABSOLUTE:
		ctrl->val = exposure_time;
		return 0;
	case V4L2_CID_BIN_FACTOR_HORZ:
	case V4L2_CID_BIN_FACTOR_VERT: {
		ctrl->val = ctrl->id == V4L2_CID_BIN_FACTOR_HORZ ?
			dev->curr_res_table[dev->fmt_idx].bin_factor_x : dev->curr_res_table[dev->fmt_idx].bin_factor_y;

		OV8865_LOG(1, "bin-factor for ISP:%d\n",  ctrl->val);
		return 0;
	}
	}

	return 0;
}

static int
ov8865_g_frame_interval(struct v4l2_subdev *sd,
			struct v4l2_subdev_frame_interval *interval)
{
	struct ov8865_device *dev = to_ov8865_sensor(sd);
	const struct ov8865_resolution *res;

	mutex_lock(&dev->input_lock);

	/* Return the currently selected settings' maximum frame interval */
	res = &dev->curr_res_table[dev->fmt_idx];

	interval->interval.numerator = 1;
	interval->interval.denominator = res->fps_options[dev->fps_index].fps;

	mutex_unlock(&dev->input_lock);

	return 0;
}
#if 0
static int ov8865_s_frame_interval(struct v4l2_subdev *sd,
				   struct v4l2_subdev_frame_interval *interval)
{
	struct ov8865_device *dev = to_ov8865_sensor(sd);
	int ret;

	mutex_lock(&dev->input_lock);
	ret = __ov8865_s_frame_interval(sd, interval);
	mutex_unlock(&dev->input_lock);

	return ret;
}
#endif
static int ov8865_g_skip_frames(struct v4l2_subdev *sd, u32 *frames)
{
	struct ov8865_device *dev = to_ov8865_sensor(sd);

	mutex_lock(&dev->input_lock);
	*frames = dev->curr_res_table[dev->fmt_idx].skip_frames;
	mutex_unlock(&dev->input_lock);

	return 0;
}

static const struct v4l2_subdev_video_ops ov8865_video_ops = {
	.s_stream = ov8865_s_stream,
	.g_frame_interval = ov8865_g_frame_interval,
	//.s_frame_interval = ov8865_s_frame_interval,
};

static const struct v4l2_subdev_sensor_ops ov8865_sensor_ops = {
	.g_skip_frames	= ov8865_g_skip_frames,
};

static const struct v4l2_subdev_core_ops ov8865_core_ops = {
	.s_power = ov8865_s_power,
	.ioctl = ov8865_ioctl,
	.init = ov8865_init,
};

/* REVISIT: Do we need pad operations? */
static const struct v4l2_subdev_pad_ops ov8865_pad_ops = {
	.enum_mbus_code = ov8865_enum_mbus_code,
	.enum_frame_size = ov8865_enum_frame_size,
	.enum_frame_interval = ov8865_enum_frame_interval,
	.get_fmt = ov8865_get_pad_format,
	.set_fmt = ov8865_set_pad_format,
};

static const struct v4l2_subdev_ops ov8865_ops = {
	.core = &ov8865_core_ops,
	.video = &ov8865_video_ops,
	.pad = &ov8865_pad_ops,
	.sensor = &ov8865_sensor_ops,
};

static int ov8865_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov8865_device *dev = to_ov8865_sensor(sd);

	gpio_crs_put(dev);

	media_entity_cleanup(&dev->sd.entity);
	v4l2_ctrl_handler_free(&dev->ctrl_handler);
	v4l2_async_unregister_subdev(sd);
	kfree(dev);

	return 0;
}

static const struct v4l2_ctrl_ops ctrl_ops = {
	.s_ctrl = ov8865_s_ctrl,
	.g_volatile_ctrl = ov8865_g_ctrl,
};

static const char * const ctrl_run_mode_menu[] = {
	NULL,
	"Video",
	"Still capture",
	"Continuous capture",
	"Preview",
};

static const struct v4l2_ctrl_config ctrl_run_mode = {
	.ops = &ctrl_ops,
	.id = V4L2_CID_RUN_MODE,
	.name = "run mode",
	.type = V4L2_CTRL_TYPE_MENU,
	.min = 1,
	.def = 4,
	.max = 4,
	.qmenu = ctrl_run_mode_menu,
};

static const struct v4l2_ctrl_config ctrls[] = {
	{
		.ops = &ctrl_ops,
		.id = V4L2_CID_EXPOSURE_ABSOLUTE,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "exposure",
		.max = 0xffff,
		.step = 0x01,
		.flags = V4L2_CTRL_FLAG_READ_ONLY | V4L2_CTRL_FLAG_VOLATILE,

	}, {
		.ops = &ctrl_ops,
		.id = V4L2_CID_TEST_PATTERN,
		.name = "Test pattern",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.step = 1,
		.max = 0xffff,
	}, {
		.ops = &ctrl_ops,
		.id = V4L2_CID_FOCUS_ABSOLUTE,
		.name = "Focus absolute",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.step = 1,
		.max = BU64243_MAX_FOCUS_POS,
	}, {
		/* This one is junk: see the spec for proper use of this CID. */
		.ops = &ctrl_ops,
		.id = V4L2_CID_FOCUS_STATUS,
		.name = "Focus status",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.step = 1,
		.max = 100,
		.flags = V4L2_CTRL_FLAG_READ_ONLY | V4L2_CTRL_FLAG_VOLATILE,
	}, {
		/* This is crap. For compatibility use only. */
		.ops = &ctrl_ops,
		.id = V4L2_CID_FOCAL_ABSOLUTE,
		.name = "Focal lenght",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min = (OV8865_FOCAL_LENGTH_NUM << 16) | OV8865_FOCAL_LENGTH_DEM,
		.max = (OV8865_FOCAL_LENGTH_NUM << 16) | OV8865_FOCAL_LENGTH_DEM,
		.step = 1,
		.def = (OV8865_FOCAL_LENGTH_NUM << 16) | OV8865_FOCAL_LENGTH_DEM,
		.flags = V4L2_CTRL_FLAG_READ_ONLY,
	}, {
		/* This one is crap, too. For compatibility use only. */
		.ops = &ctrl_ops,
		.id = V4L2_CID_FNUMBER_ABSOLUTE,
		.name = "F-number",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min = (OV8865_F_NUMBER_DEFAULT_NUM << 16) | OV8865_F_NUMBER_DEM,
		.max = (OV8865_F_NUMBER_DEFAULT_NUM << 16) | OV8865_F_NUMBER_DEM,
		.step = 1,
		.def = (OV8865_F_NUMBER_DEFAULT_NUM << 16) | OV8865_F_NUMBER_DEM,
		.flags = V4L2_CTRL_FLAG_READ_ONLY,
	}, {
		/*
		 * The most utter crap. _Never_ use this, even for
		 * compatibility reasons!
		 */
		.ops = &ctrl_ops,
		.id = V4L2_CID_FNUMBER_RANGE,
		.name = "F-number range",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min = (OV8865_F_NUMBER_DEFAULT_NUM << 24) | (OV8865_F_NUMBER_DEM << 16) | (OV8865_F_NUMBER_DEFAULT_NUM << 8) | OV8865_F_NUMBER_DEM,
		.max = (OV8865_F_NUMBER_DEFAULT_NUM << 24) | (OV8865_F_NUMBER_DEM << 16) | (OV8865_F_NUMBER_DEFAULT_NUM << 8) | OV8865_F_NUMBER_DEM,
		.step = 1,
		.def = (OV8865_F_NUMBER_DEFAULT_NUM << 24) | (OV8865_F_NUMBER_DEM << 16) | (OV8865_F_NUMBER_DEFAULT_NUM << 8) | OV8865_F_NUMBER_DEM,
		.flags = V4L2_CTRL_FLAG_READ_ONLY,
	}, {
		.ops = &ctrl_ops,
		.id = V4L2_CID_BIN_FACTOR_HORZ,
		.name = "Horizontal binning factor",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.max = OV8865_BIN_FACTOR_MAX,
		.step = 1,
		.flags = V4L2_CTRL_FLAG_READ_ONLY | V4L2_CTRL_FLAG_VOLATILE,
	}, {
		.ops = &ctrl_ops,
		.id = V4L2_CID_BIN_FACTOR_VERT,
		.name = "Vertical binning factor",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.max = OV8865_BIN_FACTOR_MAX,
		.step = 1,
		.flags = V4L2_CTRL_FLAG_READ_ONLY | V4L2_CTRL_FLAG_VOLATILE,
	}
};

static int ov8865_init_controls(struct ov8865_device *ov8865)
{
	struct i2c_client *client = v4l2_get_subdevdata(&ov8865->sd);
	struct v4l2_ctrl *ctrl;
	unsigned int i;
	int ret;

	ret = v4l2_ctrl_handler_init(&ov8865->ctrl_handler, ARRAY_SIZE(ctrls) + 1);
	if (ret) {
		ov8865_remove(client);
		return ret;
	}

	OV8865_LOG(1, "%s %d handle init done\n", __func__, __LINE__);
				//OV8865_LOG(1, "ov8865 handle init done\n");
	ov8865->run_mode = v4l2_ctrl_new_custom(&ov8865->ctrl_handler,
					     &ctrl_run_mode, NULL);

	for (i = 0; i < ARRAY_SIZE(ctrls); i++)
		v4l2_ctrl_new_custom(&ov8865->ctrl_handler, &ctrls[i], NULL);

	/* link freq */
	ctrl = v4l2_ctrl_new_int_menu(&ov8865->ctrl_handler, NULL,
				      V4L2_CID_LINK_FREQ,
				      0, 0, link_freq_menu_items);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	/* pixel rate */
	v4l2_ctrl_new_std(&ov8865->ctrl_handler, NULL, V4L2_CID_PIXEL_RATE,
			  0, OV8865_PIXEL_RATE, 1, OV8865_PIXEL_RATE);

	if (ov8865->ctrl_handler.error) {
		ov8865_remove(client);
		return ov8865->ctrl_handler.error;
	}

	/* Use same lock for controls as for everything else. */
	ov8865->ctrl_handler.lock = &ov8865->input_lock;
	ov8865->sd.ctrl_handler = &ov8865->ctrl_handler;
	v4l2_ctrl_handler_setup(&ov8865->ctrl_handler);

	return 0;
}

/* Get dependent INT3472 device */
static struct device *get_dep_dev(struct device *dev)
{
	struct acpi_device *sensor_adev;
	struct acpi_handle *dev_handle;
	struct acpi_handle_list dep_devices;
	struct acpi_device *dep_adev;
	struct acpi_device_physical_node *dep_phys;
	int ret;
	int i;

	sensor_adev = acpi_dev_get_first_match_dev(OV8865_HID, NULL, -1);
	if (!sensor_adev) {
		dev_err(dev, "Couldn't get sensor ACPI device\n");
		return ERR_PTR(-ENODEV);
	}
	dev_handle = sensor_adev->handle;
	acpi_dev_put(sensor_adev);

	if (!acpi_has_method(dev_handle, "_DEP")) {
		dev_err(dev, "No dependent devices\n");
		return ERR_PTR(-ENODEV);
	}

	ret = acpi_evaluate_reference(dev_handle, "_DEP", NULL, &dep_devices);
	if (ACPI_FAILURE(ret)) {
		dev_err(dev, "Failed to evaluate _DEP.\n");
		return ERR_PTR(-ENODEV);
	}

	for (i = 0; i < dep_devices.count; i++) {
		struct acpi_device_info *info;

		ret = acpi_get_object_info(dep_devices.handles[i], &info);
		if (ACPI_FAILURE(ret)) {
			dev_err(dev, "Error reading _DEP device info\n");
			return ERR_PTR(-ENODEV);
		}

		if (info->valid & ACPI_VALID_HID &&
		    !strcmp(info->hardware_id.string, "INT3472")) {
			if (acpi_bus_get_device(dep_devices.handles[i], &dep_adev)) {
				dev_err(dev, "Error getting adev of dep_dev\n");
				return ERR_PTR(-ENODEV);
			}

			/* found adev of dep_dev */
			break;
		}
	}

	if (!dep_adev) {
		dev_err(dev, "adev of dep_dev not found\n");
		return ERR_PTR(-ENODEV);
	}

	/*
	 * HACK: We know that the PMIC is a "discrete" PMIC, an ACPI device
	 * that just serves as a container to list system GPIOs.
	 *
	 * The ACPI device has no fwnode, nor does it have a platform device.
	 * This prevents fetching GPIOs. It however seems to be backed by the
	 * PCI root complex (pci0000:00/0000:00:00.0) as its physical device,
	 * and that device has its fwnode set to \_SB.PCI0.DSC1. Whether this
	 * is correct or not is unknown, let's just get the physical device and
	 * move on for now.
	 */
	dep_phys = list_first_entry_or_null(&dep_adev->physical_node_list,
					    struct acpi_device_physical_node, node);
	if (!dep_phys) {
		dev_info(dev, "Error getting physical node of dep_adev\n");
		return ERR_PTR(-ENODEV);
	}

	dev_info(dev, "Dependent device found: %s\n", dev_name(dep_phys->dev));

	return dep_phys->dev;
}

static int ov8865_probe(struct i2c_client *client)
{
	struct ov8865_device *dev;
	struct device *dep_dev;
	int ret;

	OV8865_LOG(2, "%s %d start\n", __func__, __LINE__);
				//OV8865_LOG(1, "ov8865 start\n");
	/* allocate sensor device & init sub device */
	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev) {
		v4l2_err(client, "%s: out of memory\n", __func__);
				//OV8865_LOG(1, "ov8865 out of memory\n");
		return -ENOMEM;
	}

	/* shunyong, disable focus when PO */

	mutex_init(&dev->input_lock);

	dev->fmt_idx = 0;
	v4l2_i2c_subdev_init(&(dev->sd), client, &ov8865_ops);

	dev->dep_dev = get_dep_dev(&client->dev);
	if (IS_ERR(dev->dep_dev)) {
		ret = PTR_ERR(dev->dep_dev);
		dev_err(&client->dev, "cannot get dep_dev: ret %d\n", ret);
		return ret;
	}
	dep_dev = dev->dep_dev;

	ret = gpio_crs_get(dev, dep_dev);
	if (ret) {
		dev_err(dep_dev, "Failed to get _CRS GPIOs\n");
		return ret;
	}

	ret = bu64243_init(&dev->sd);
	if (ret < 0)
		goto out_free;

	OV8865_LOG(1, "%s %d vcm done\n", __func__, __LINE__);
				//OV8865_LOG(1, "ov8865 vcm done\n");

	ret = ov8865_s_config(&dev->sd, client->irq);
			//OV8865_LOG(1, "ov8865 end ov8865_s_config\n");
	if (ret) {
		//OV8865_LOG(1, "ov8865 out_free\n");
		goto out_free;
	}

	OV8865_LOG(1, "%s %d s_config done\n", __func__, __LINE__);
				//OV8865_LOG(1, "ov8865 s_config done\n");
	dev->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	dev->pad.flags = MEDIA_PAD_FL_SOURCE;
	dev->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	dev->format.code = MEDIA_BUS_FMT_SBGGR10_1X10;

	ret = ov8865_init_controls(dev);
	if (ret)
		ov8865_remove(client);

	OV8865_LOG(1, "%s %d ctrl added\n", __func__, __LINE__);
				//OV8865_LOG(1, "ov8865 ctrl added\n");
	ret = media_entity_pads_init(&dev->sd.entity, 1, &dev->pad);
	if (ret) {
		ov8865_remove(client);
		return ret;
	}

	ret = v4l2_async_register_subdev_sensor_common(&dev->sd);
	if (ret) {
		dev_err(&client->dev, "failed to register V4L2 subdev: %d", ret);
		goto media_entity_cleanup;
	}

	global_dev = dev;
	OV8865_LOG(2, "%s %d done\n", __func__, __LINE__);
				//OV8865_LOG(1, "ov8865 done\n");

	return 0;

media_entity_cleanup:
	media_entity_cleanup(&dev->sd.entity);
out_free:

	OV8865_LOG(1, "%s %d fail, free\n", __func__, __LINE__);
				//OV8865_LOG(1, "ov8865 fail, free\n");
	v4l2_async_unregister_subdev(&dev->sd);
	kfree(dev);
	return ret;
}

static const struct acpi_device_id ov8865_acpi_ids[] = {
	{OV8865_HID},
	{},
};
MODULE_DEVICE_TABLE(acpi, ov8865_acpi_ids);

static struct i2c_driver ov8865_driver = {
	.driver = {
		.name = OV8865_NAME,
		.acpi_match_table = ov8865_acpi_ids,
	},
	.probe_new = ov8865_probe,
	.remove = ov8865_remove,
};
module_i2c_driver(ov8865_driver);

MODULE_DESCRIPTION("A low-level driver for Omnivision OV8865 sensors");
MODULE_LICENSE("GPL");
