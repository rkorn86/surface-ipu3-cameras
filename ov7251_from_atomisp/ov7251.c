/*
 * Support for OmniVision OV7251 1080p HD camera sensor.
 *
 * Copyright (c) 2013 Intel Corporation. All Rights Reserved.
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
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/kmod.h>
#include <linux/input.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/gpio.h>
#include <linux/moduleparam.h>
#include <media/v4l2-device.h>

#include <linux/acpi.h>
#include <linux/io.h>

#include "ov7251.h"

/* i2c read/write stuff */
static int ov7251_read_reg(struct i2c_client *client,
			   u16 data_length, u16 reg, u16 *val)
{
	int err;
	struct i2c_msg msg[2];
	unsigned char data[6];

	if (!client->adapter) {
		dev_err(&client->dev, "%s error, no client->adapter\n",
			__func__);
		return -ENODEV;
	}

	if (data_length != OV7251_8BIT && data_length != OV7251_16BIT
					&& data_length != OV7251_32BIT) {
		dev_err(&client->dev, "%s error, invalid data length\n",
			__func__);
		return -EINVAL;
	}

	memset(msg, 0, sizeof(msg));

	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].len = I2C_MSG_LENGTH;
	msg[0].buf = data;

	/* high byte goes out first */
	data[0] = (u8)(reg >> 8);
	data[1] = (u8)(reg & 0xff);

	msg[1].addr = client->addr;
	msg[1].len = data_length;
	msg[1].flags = I2C_M_RD;
	msg[1].buf = data;

	err = i2c_transfer(client->adapter, msg, 2);
	if (err != 2) {
		if (err >= 0)
			err = -EIO;
		dev_err(&client->dev,
			"read from offset 0x%x error %d", reg, err);
		return err;
	}

	*val = 0;
	/* high byte comes first */
	if (data_length == OV7251_8BIT)
		*val = (u8)data[0];
	else if (data_length == OV7251_16BIT)
		*val = be16_to_cpu(*(u16 *)&data[0]);
	else
		*val = be32_to_cpu(*(u32 *)&data[0]);

	return 0;
}

static int ov7251_i2c_write(struct i2c_client *client, u16 len, u8 *data)
{
	struct i2c_msg msg;
	const int num_msg = 1;
	int ret;

	msg.addr = client->addr;
	msg.flags = 0;
	msg.len = len;
	msg.buf = data;
	ret = i2c_transfer(client->adapter, &msg, 1);

	return ret == num_msg ? 0 : -EIO;
}

static int ov7251_write_reg(struct i2c_client *client, u16 data_length,
							u16 reg, u16 val)
{
	int ret;
	unsigned char data[4] = {0};
	u16 *wreg = (u16 *)data;
	const u16 len = data_length + sizeof(u16); /* 16-bit address + data */

	if (data_length != OV7251_8BIT && data_length != OV7251_16BIT) {
		dev_err(&client->dev,
			"%s error, invalid data_length\n", __func__);
		return -EINVAL;
	}

	/* high byte goes out first */
	*wreg = cpu_to_be16(reg);

	if (data_length == OV7251_8BIT) {
		data[2] = (u8)(val);
	} else {
		/* OV7251_16BIT */
		u16 *wdata = (u16 *)&data[2];
		*wdata = cpu_to_be16(val);
	}

	ret = ov7251_i2c_write(client, len, data);
	if (ret)
		dev_err(&client->dev,
			"write error: wrote 0x%x to offset 0x%x error %d",
			val, reg, ret);

	return ret;
}

/*
 * ov7251_write_reg_array - Initializes a list of OV7251 registers
 * @client: i2c driver client structure
 * @reglist: list of registers to be written
 *
 * This function initializes a list of registers. When consecutive addresses
 * are found in a row on the list, this function creates a buffer and sends
 * consecutive data in a single i2c_transfer().
 *
 * __ov7251_flush_reg_array, __ov7251_buf_reg_array() and
 * __ov7251_write_reg_is_consecutive() are internal functions to
 * ov7251_write_reg_array_fast() and should be not used anywhere else.
 *
 */

static int __ov7251_flush_reg_array(struct i2c_client *client,
				    struct ov7251_write_ctrl *ctrl)
{
	u16 size;

	if (ctrl->index == 0)
		return 0;

	size = sizeof(u16) + ctrl->index; /* 16-bit address + data */
	ctrl->buffer.addr = cpu_to_be16(ctrl->buffer.addr);
	ctrl->index = 0;

	return ov7251_i2c_write(client, size, (u8 *)&ctrl->buffer);
}

static int __ov7251_buf_reg_array(struct i2c_client *client,
				  struct ov7251_write_ctrl *ctrl,
				  const struct ov7251_reg *next)
{
	int size;
	u16 *data16;

	switch (next->type) {
	case OV7251_8BIT:
		size = 1;
		ctrl->buffer.data[ctrl->index] = (u8)next->val;
		break;
	case OV7251_16BIT:
		size = 2;
		data16 = (u16 *)&ctrl->buffer.data[ctrl->index];
		*data16 = cpu_to_be16((u16)next->val);
		break;
	default:
		return -EINVAL;
	}

	/* When first item is added, we need to store its starting address */
	if (ctrl->index == 0)
		ctrl->buffer.addr = next->reg;

	ctrl->index += size;

	/*
	 * Buffer cannot guarantee free space for u32? Better flush it to avoid
	 * possible lack of memory for next item.
	 */
	if (ctrl->index + sizeof(u16) >= OV7251_MAX_WRITE_BUF_SIZE)
		return __ov7251_flush_reg_array(client, ctrl);

	return 0;
}

static int __ov7251_write_reg_is_consecutive(struct i2c_client *client,
					     struct ov7251_write_ctrl *ctrl,
					     const struct ov7251_reg *next)
{
	if (ctrl->index == 0)
		return 1;

	return ctrl->buffer.addr + ctrl->index == next->reg;
}

static int ov7251_write_reg_array(struct i2c_client *client,
				  const struct ov7251_reg *reglist)
{
	const struct ov7251_reg *next = reglist;
	struct ov7251_write_ctrl ctrl;
	int err;

	ctrl.index = 0;
	for (; next->type != OV7251_TOK_TERM; next++) {
		switch (next->type & OV7251_TOK_MASK) {
		case OV7251_TOK_DELAY:
			err = __ov7251_flush_reg_array(client, &ctrl);
			if (err)
				return err;
			msleep(next->val);
			break;
		default:
			/*
			 * If next address is not consecutive, data needs to be
			 * flushed before proceed.
			 */
			if (!__ov7251_write_reg_is_consecutive(client, &ctrl,
								next)) {
				err = __ov7251_flush_reg_array(client, &ctrl);
				if (err)
					return err;
			}
			err = __ov7251_buf_reg_array(client, &ctrl, next);
			if (err) {
				dev_err(&client->dev, "%s: write error, aborted\n",
					 __func__);
				return err;
			}
			break;
		}
	}

	return __ov7251_flush_reg_array(client, &ctrl);
}
static int ov7251_g_focal(struct v4l2_subdev *sd, s32 *val)
{
	*val = (OV7251_FOCAL_LENGTH_NUM << 16) | OV7251_FOCAL_LENGTH_DEM;
	return 0;
}

static int ov7251_g_fnumber(struct v4l2_subdev *sd, s32 *val)
{
	/*const f number for imx*/
	*val = (OV7251_F_NUMBER_DEFAULT_NUM << 16) | OV7251_F_NUMBER_DEM;
	return 0;
}

static int ov7251_g_fnumber_range(struct v4l2_subdev *sd, s32 *val)
{
	*val = (OV7251_F_NUMBER_DEFAULT_NUM << 24) |
		(OV7251_F_NUMBER_DEM << 16) |
		(OV7251_F_NUMBER_DEFAULT_NUM << 8) | OV7251_F_NUMBER_DEM;
	return 0;
}

typedef union {
	u32 exp32;
	uint8_t regs[4];
} OV7251RegsWrapper;

#define GROUP_HOLD_START_VAL 0x00
#define GROUP_HOLD_END_VAL 0x10
#define GROUP_HOLD_LUNCH_VAL 0xA0

static long __ov7251_set_exposure_gain(struct v4l2_subdev *sd, u32 exposure, u32 gain)

{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	OV7251RegsWrapper *exp_regs = (OV7251RegsWrapper *)&exposure;
	OV7251RegsWrapper *gain_regs = (OV7251RegsWrapper *)&gain;
	int ret;

	unsigned char group_start_transaction[3];
	unsigned char group_end_transaction[3];
	unsigned char group_lunch_transaction[3];
	unsigned char exp_3500_transaction[3]; /*{0x35,0x00,exp_regs.regs[0]}; */
	unsigned char exp_3501_transaction[3]; /*{0x35,0x01,exp_regs.regs[1]}; */
	unsigned char exp_3502_transaction[3]; /*{0x35,0x02,exp_regs.regs[2]}; */
	unsigned char exp_350A_transaction[3]; /*{0x35,0x0B,gain8}; */
	unsigned char exp_350B_transaction[3]; /*{0x35,0x0B,gain8}; */
	struct i2c_msg msgs[8];

	if (exp_regs->exp32 > 0xFFFF)
		return -EINVAL;

	/* only 7 bits is valid */
	if (gain_regs->exp32 > 0x7F)
		return -EINVAL;

	/* 4 first bits is fraction bits */
	exp_regs->exp32 <<= 4;

	/* 4 first bits is fraction bits */
	gain_regs->exp32 <<= 4;

	/* setup transactions buffers for the different registers */
	group_start_transaction[0] = 0x32;
	group_start_transaction[1] = 0x08;
	group_start_transaction[2] = GROUP_HOLD_START_VAL;

	group_end_transaction[0] = 0x32;
	group_end_transaction[1] = 0x08;
	group_end_transaction[2] = GROUP_HOLD_END_VAL;

	group_lunch_transaction[0] = 0x32;
	group_lunch_transaction[1] = 0x08;
	group_lunch_transaction[2] = GROUP_HOLD_LUNCH_VAL;

	exp_3500_transaction[0] = 0x35;
	exp_3500_transaction[1] = 0x00;
	exp_3500_transaction[2] = exp_regs->regs[0];

	exp_3501_transaction[0] = 0x35;
	exp_3501_transaction[1] = 0x01;
	exp_3501_transaction[2] = exp_regs->regs[1];

	exp_3502_transaction[0] = 0x35;
	exp_3502_transaction[1] = 0x02;
	exp_3502_transaction[2] = exp_regs->regs[2];

	exp_350A_transaction[0] = 0x35;
	exp_350A_transaction[1] = 0x0A;
	exp_350A_transaction[2] = gain_regs->regs[0];

	exp_350B_transaction[0] = 0x35;
	exp_350B_transaction[1] = 0x0B;
	exp_350B_transaction[2] = gain_regs->regs[1];

	/* setup messages for the different transactions */
	msgs[0].addr = client->addr;
	msgs[0].flags = 0;
	msgs[0].len   = 3;
	msgs[0].buf   = group_start_transaction;

	msgs[1].addr = client->addr;
	msgs[1].flags = 0;
	msgs[1].len   = 3;
	msgs[1].buf   = exp_3500_transaction;

	msgs[2].addr = client->addr;
	msgs[2].flags = 0;
	msgs[2].len   = 3;
	msgs[2].buf   = exp_3501_transaction;

	msgs[3].addr = client->addr;
	msgs[3].flags = 0;
	msgs[3].len   = 3;
	msgs[3].buf   = exp_3502_transaction;

	msgs[4].addr = client->addr;
	msgs[4].flags = 0;
	msgs[4].len   = 3;
	msgs[4].buf   = exp_350A_transaction;

	msgs[5].addr = client->addr;
	msgs[5].flags = 0;
	msgs[5].len   = 3;
	msgs[5].buf   = exp_350B_transaction;

	msgs[6].addr = client->addr;
	msgs[6].flags = 0;
	msgs[6].len   = 3;
	msgs[6].buf   = group_end_transaction;

	msgs[7].addr = client->addr;
	msgs[7].flags = 0;
	msgs[7].len   = 3;
	msgs[7].buf   = group_lunch_transaction;

	/*perform the transaction */
	ret = i2c_transfer(client->adapter, msgs, sizeof(msgs) / sizeof(struct i2c_msg));

	if (ret == (sizeof(msgs) / sizeof(struct i2c_msg))) {
		struct ov7251_device *dev = to_ov7251_sensor(sd);

		dev->gain = gain_regs->exp32 >> 4;
		dev->exposure = exp_regs->exp32 >> 4;

		return 0;
	} else {
		return -EIO;
	}
}

static long ov7251_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
{
	/* lets do not implement any ioctl specific to atomisp */
	return -EINVAL;
}

/* This returns the exposure time being used. This should only be used
 * for filling in EXIF data, not for actual image processing.
 */
static int ov7251_q_exposure(struct v4l2_subdev *sd, s32 *value)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	u16 reg_v;
	int ret;
	u32 v;

	ret = ov7251_read_reg(client, OV7251_8BIT,
					EC_EXPO_7_0_BITS_REG,
					&reg_v);
	if (ret)
		goto err;

	/* 4 first bits is fraction */
	v = reg_v >> 4;

	ret = ov7251_read_reg(client, OV7251_8BIT,
					EC_EXPO_15_8_BITS_REG,
					&reg_v);
	if (ret)
		goto err;

	v |= reg_v << 4;

	ret = ov7251_read_reg(client, OV7251_8BIT,
					EC_EXPO_19_16_BITS_REG,
					&reg_v);
	if (ret)
		goto err;

	v |= reg_v << 12;
	*value = v;
err:
	return ret;
}

static int ov7251_q_gain(struct v4l2_subdev *sd, s32 *value)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	u16 reg_v;
	int ret;
	u32 v;

	ret = ov7251_read_reg(client, OV7251_8BIT,
					AEC_AGC_ADJ_7_0_REG,
					&reg_v);
	if (ret)
		goto err;

	/* 4 first bits is fraction */
	v = reg_v >> 4;

	ret = ov7251_read_reg(client, OV7251_8BIT,
					AEC_AGC_ADJ_10_8_REG,
					&reg_v);
	if (ret)
		goto err;

	v |= reg_v << 4;
	*value = v;
err:
	return ret;
}

static int ov7251_s_exposure(struct v4l2_subdev *sd, s32 value)
{
	struct ov7251_device *dev = to_ov7251_sensor(sd);

	return __ov7251_set_exposure_gain(sd, value, dev->gain);
}

static int ov7251_s_gain(struct v4l2_subdev *sd, s32 value)
{
	struct ov7251_device *dev = to_ov7251_sensor(sd);

	return __ov7251_set_exposure_gain(sd, dev->exposure, value);
}

struct ov7251_control ov7251_controls[] = {
	{
		.qc = {
			.id = V4L2_CID_EXPOSURE_ABSOLUTE,
			.type = V4L2_CTRL_TYPE_U16,
			.name = "exposure",
			.minimum = 0x0,
			.maximum = 0xFFFF,
			.step = 0x01,
			.default_value = 0x00,
			.flags = 0,
		},
		.query = ov7251_q_exposure,
		.tweak = ov7251_s_exposure,
	},
	{
		.qc = {
			.id = V4L2_CID_GAIN,
			.type = V4L2_CTRL_TYPE_U8,
			.name = "gain",
			.minimum = 0x0,
			.maximum = 0x7F,
			.step = 0x01,
			.default_value = 0x00,
			.flags = 0,
		},
		.query = ov7251_q_gain,
		.tweak = ov7251_s_gain,
	},
	{
		.qc = {
			.id = V4L2_CID_FOCAL_ABSOLUTE,
			.type = V4L2_CTRL_TYPE_INTEGER,
			.name = "focal length",
			.minimum = OV7251_FOCAL_LENGTH_DEFAULT,
			.maximum = OV7251_FOCAL_LENGTH_DEFAULT,
			.step = 0x01,
			.default_value = OV7251_FOCAL_LENGTH_DEFAULT,
			.flags = 0,
		},
		.query = ov7251_g_focal,
	},
	{
		.qc = {
			.id = V4L2_CID_FNUMBER_ABSOLUTE,
			.type = V4L2_CTRL_TYPE_INTEGER,
			.name = "f-number",
			.minimum = OV7251_F_NUMBER_DEFAULT,
			.maximum = OV7251_F_NUMBER_DEFAULT,
			.step = 0x01,
			.default_value = OV7251_F_NUMBER_DEFAULT,
			.flags = 0,
		},
		.query = ov7251_g_fnumber,
	},
	{
		.qc = {
			.id = V4L2_CID_FNUMBER_RANGE,
			.type = V4L2_CTRL_TYPE_INTEGER,
			.name = "f-number range",
			.minimum = OV7251_F_NUMBER_RANGE,
			.maximum =  OV7251_F_NUMBER_RANGE,
			.step = 0x01,
			.default_value = OV7251_F_NUMBER_RANGE,
			.flags = 0,
		},
		.query = ov7251_g_fnumber_range,
	},
};
#define N_CONTROLS (ARRAY_SIZE(ov7251_controls))

static int ov7251_g_volatile_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ov7251_device *dev = container_of(ctrl->handler, struct ov7251_device,
			ctrl_handler);
	unsigned int val;

	switch (ctrl->id) {
	case V4L2_CID_LINK_FREQ:
		val = ov7251_res[dev->fmt_idx].mipi_freq;
		if (val == 0)
			return -EINVAL;

		ctrl->val = val * 1000;			/* To Hz */
		break;
	default:
		return -EINVAL;
	}

	return 0;
}


static struct ov7251_control *ov7251_find_control(u32 id)
{
	int i;

	for (i = 0; i < N_CONTROLS; i++)
		if (ov7251_controls[i].qc.id == id)
			return &ov7251_controls[i];
	return NULL;
}

static int ov7251_queryctrl(struct v4l2_subdev *sd, struct v4l2_queryctrl *qc)
{
	struct ov7251_control *ctrl = ov7251_find_control(qc->id);
	struct ov7251_device *dev = to_ov7251_sensor(sd);

	if (ctrl == NULL)
		return -EINVAL;

	mutex_lock(&dev->input_lock);
	*qc = ctrl->qc;
	mutex_unlock(&dev->input_lock);

	return 0;
}

/* imx control set/get */
static int ov7251_g_ctrl(struct v4l2_subdev *sd, struct v4l2_control *ctrl)
{
	struct ov7251_control *s_ctrl;
	struct ov7251_device *dev = to_ov7251_sensor(sd);
	int ret;

	if (!ctrl)
		return -EINVAL;

	s_ctrl = ov7251_find_control(ctrl->id);
	if ((s_ctrl == NULL) || (s_ctrl->query == NULL))
		return -EINVAL;

	mutex_lock(&dev->input_lock);
	ret = s_ctrl->query(sd, &ctrl->value);
	mutex_unlock(&dev->input_lock);

	return ret;
}

static int ov7251_s_ctrl(struct v4l2_subdev *sd, struct v4l2_control *ctrl)
{
	struct ov7251_control *octrl = ov7251_find_control(ctrl->id);
	struct ov7251_device *dev = to_ov7251_sensor(sd);
	int ret;

	if ((octrl == NULL) || (octrl->tweak == NULL))
		return -EINVAL;

	mutex_lock(&dev->input_lock);
	ret = octrl->tweak(sd, ctrl->value);
	mutex_unlock(&dev->input_lock);

	return ret;
}

static int ov7251_init(struct v4l2_subdev *sd)
{
	struct ov7251_device *dev = to_ov7251_sensor(sd);

	mutex_lock(&dev->input_lock);

	/* restore settings */
	ov7251_res = ov7251_res_preview;
	N_RES = N_RES_PREVIEW;

	mutex_unlock(&dev->input_lock);

	return 0;
}

static int gpio_ctrl(struct v4l2_subdev *sd, bool flag)
{
	/* To be added */

	return 0;
}

static int power_up(struct v4l2_subdev *sd)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret;

	/* according to DS, at least 5ms is needed between DOVDD and PWDN */
	usleep_range(5000, 6000);

	/* gpio ctrl */
	ret = gpio_ctrl(sd, 1);
	if (ret) {
		ret = gpio_ctrl(sd, 0);
		if (ret)
			goto fail_power;
	}

	/* according to DS, 20ms is needed between PWDN and i2c access */
	msleep(20);

	return 0;

fail_power:

	dev_err(&client->dev, "sensor power-up failed\n");

	return ret;
}

static int power_down(struct v4l2_subdev *sd)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret = 0;

	/* gpio ctrl */
	ret = gpio_ctrl(sd, 0);
	if (ret) {
		ret = gpio_ctrl(sd, 0);
		if (ret)
			dev_err(&client->dev, "gpio failed 2\n");
	}

	return ret;
}

static int ov7251_s_power(struct v4l2_subdev *sd, int on)
{
	int ret;

	if (on == 0)
		return power_down(sd);

	/* on != 0 */
	ret = power_up(sd);
	if (!ret)
		return ov7251_init(sd);

	return ret;
}

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
#define LARGEST_ALLOWED_RATIO_MISMATCH 800
static int distance(struct ov7251_resolution *res, u32 w, u32 h)
{
	unsigned int w_ratio = ((res->width << 13)/w);
	unsigned int h_ratio;
	int match;

	if (h == 0)
		return -1;
	h_ratio = ((res->height << 13) / h);
	if (h_ratio == 0)
		return -1;
	match   = abs(((w_ratio << 13) / h_ratio) - (8192));

	if ((w_ratio < 8192) || (h_ratio < 8192)  ||
		(match > LARGEST_ALLOWED_RATIO_MISMATCH))
		return -1;

	return w_ratio + h_ratio;
}

/* Return the nearest higher resolution index */
static int nearest_resolution_index(int w, int h)
{
	int i;
	int idx = -1;
	int dist;
	int min_dist = INT_MAX;
	struct ov7251_resolution *tmp_res = NULL;

	for (i = 0; i < N_RES; i++) {
		tmp_res = &ov7251_res[i];
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

static int get_resolution_index(int w, int h)
{
	int i;

	for (i = 0; i < N_RES; i++) {
		if (w != ov7251_res[i].width)
			continue;
		if (h != ov7251_res[i].height)
			continue;

		return i;
	}

	return -1;
}

static int ov7251_try_mbus_fmt(struct v4l2_subdev *sd,
			struct v4l2_mbus_framefmt *fmt)
{
	int idx;

	if (!fmt)
		return -EINVAL;
	idx = nearest_resolution_index(fmt->width,
					fmt->height);
	if (idx == -1) {
		/* return the largest resolution */
		fmt->width = ov7251_res[N_RES - 1].width;
		fmt->height = ov7251_res[N_RES - 1].height;
	} else {
		fmt->width = ov7251_res[idx].width;
		fmt->height = ov7251_res[idx].height;
	}
	fmt->code = MEDIA_BUS_FMT_SGRBG10_1X10;

	return 0;
}

/* TODO: remove it. */
static int startup(struct v4l2_subdev *sd)
{
	struct ov7251_device *dev = to_ov7251_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret = 0;

	ret = ov7251_write_reg(client, OV7251_8BIT,
					OV7251_SW_RESET, 0x01);
	if (ret) {
		dev_err(&client->dev, "ov7251 reset err.\n");
		return ret;
	}

	usleep_range(5000, 6000);

	ret = ov7251_write_reg_array(client, ov7251_res[dev->fmt_idx].regs);
	if (ret) {
		dev_err(&client->dev, "ov7251 write register err.\n");
		return ret;
	}

	return ret;
}

static int ov7251_s_mbus_fmt(struct v4l2_subdev *sd,
			     struct v4l2_mbus_framefmt *fmt)
{
	struct ov7251_device *dev = to_ov7251_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct camera_mipi_info *ov7251_info = NULL;
	int ret = 0;

	ov7251_info = v4l2_get_subdev_hostdata(sd);
	if (ov7251_info == NULL)
		return -EINVAL;

	mutex_lock(&dev->input_lock);
	ret = ov7251_try_mbus_fmt(sd, fmt);
	if (ret == -1) {
		dev_err(&client->dev, "try fmt fail\n");
		goto err;
	}

	dev->fmt_idx = get_resolution_index(fmt->width,
					    fmt->height);
	if (dev->fmt_idx == -1) {
		dev_err(&client->dev, "get resolution fail\n");
		mutex_unlock(&dev->input_lock);
		return -EINVAL;
	}

	dev->pixels_per_line = ov7251_res[dev->fmt_idx].pixels_per_line;
	dev->lines_per_frame = ov7251_res[dev->fmt_idx].lines_per_frame;

	ret = startup(sd);
	if (ret) {
		dev_err(&client->dev, "ov7251 startup err\n");
		goto err;
	}

err:
	mutex_unlock(&dev->input_lock);
	return ret;
}
static int ov7251_g_mbus_fmt(struct v4l2_subdev *sd,
			     struct v4l2_mbus_framefmt *fmt)
{
	struct ov7251_device *dev = to_ov7251_sensor(sd);

	if (!fmt)
		return -EINVAL;

	fmt->width = ov7251_res[dev->fmt_idx].width;
	fmt->height = ov7251_res[dev->fmt_idx].height;
	fmt->code = MEDIA_BUS_FMT_SBGGR10_1X10;

	return 0;
}

static int ov7251_detect(struct i2c_client *client)
{
	struct i2c_adapter *adapter = client->adapter;
	u16 high, low;
	int ret;
	u16 id;
	u8 revision;

	if (!i2c_check_functionality(adapter, I2C_FUNC_I2C))
		return -ENODEV;

	ret = ov7251_read_reg(client, OV7251_8BIT,
					OV7251_SC_CMMN_CHIP_ID_H, &high);
	if (ret) {
		dev_err(&client->dev, "sensor_id_high = 0x%x\n", high);
		return -ENODEV;
	}
	ret = ov7251_read_reg(client, OV7251_8BIT,
					OV7251_SC_CMMN_CHIP_ID_L, &low);
	id = ((((u16) high) << 8) | (u16) low);

	ret = ov7251_read_reg(client, OV7251_8BIT, SC_REG0C_REG, &high);
	revision = (u8) high & 0x0f;

	dev_info(&client->dev, "sensor_revision = 0x%x\n", revision);
	dev_info(&client->dev, "detect ov7251 success\n");
	return 0;
}

static int ov7251_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct ov7251_device *dev = to_ov7251_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret = 0;

	dev_info(&client->dev, "tal test stream called %d\n", enable);
	mutex_lock(&dev->input_lock);

	ret = ov7251_write_reg(client, OV7251_8BIT, OV7251_SW_STREAM,
				enable ? OV7251_START_STREAMING :
				OV7251_STOP_STREAMING);
	mutex_unlock(&dev->input_lock);
	return ret;
}

static int ov7251_enum_frame_interval(struct v4l2_subdev *sd,
				      struct v4l2_subdev_pad_config *cfg,
				      struct v4l2_subdev_frame_interval_enum *fie)
{
	unsigned int index = fie->index;

	if (index >= N_RES)
		return -EINVAL;

	/* TODO: can we really drop this? */
	// fie->type = V4L2_FRMIVAL_TYPE_DISCRETE;

	fie->width = ov7251_res[index].width;
	fie->height = ov7251_res[index].height;
	fie->interval.numerator = 1;
	fie->interval.denominator = ov7251_res[index].fps;

	return 0;
}

static int ov7251_s_config(struct v4l2_subdev *sd)
{
	struct ov7251_device *dev = to_ov7251_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret = 0;

	mutex_lock(&dev->input_lock);

	/* power off the module, then power on it in future
	 * as first power on by board may not fulfill the
	 * power on sequqence needed by the module
	 */
	ret = power_down(sd);
	if (ret) {
		dev_err(&client->dev, "ov7251 power-off err.\n");
		goto fail_power_off;
	}

	ret = power_up(sd);
	if (ret) {
		dev_err(&client->dev, "ov7251 power-up err.\n");
		goto fail_power_off;
	}

	/* config & detect sensor */
	ret = ov7251_detect(client);
	if (ret) {
		dev_err(&client->dev, "ov7251_detect err s_config.\n");
		goto fail_power_off;
	}

	/* turn off sensor, after probed */
	ret = power_down(sd);
	if (ret) {
		dev_err(&client->dev, "ov7251 power-off err.\n");
		goto fail_power_off;
	}
	mutex_unlock(&dev->input_lock);

	return 0;

fail_power_off:
	power_down(sd);
	dev_err(&client->dev, "sensor power-gating failed\n");
	mutex_unlock(&dev->input_lock);
	return ret;
}

static int ov7251_g_parm(struct v4l2_subdev *sd,
			struct v4l2_streamparm *param)
{
	struct ov7251_device *dev = to_ov7251_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);

	if (!param)
		return -EINVAL;

	if (param->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
		dev_err(&client->dev,  "unsupported buffer type.\n");
		return -EINVAL;
	}

	memset(param, 0, sizeof(*param));
	param->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	if (dev->fmt_idx >= 0 && dev->fmt_idx < N_RES) {
		param->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
		param->parm.capture.timeperframe.numerator = 1;
		param->parm.capture.capturemode = dev->run_mode;
		param->parm.capture.timeperframe.denominator =
			ov7251_res[dev->fmt_idx].fps;
	}
	return 0;
}

static int ov7251_s_parm(struct v4l2_subdev *sd,
			struct v4l2_streamparm *param)
{
	struct ov7251_device *dev = to_ov7251_sensor(sd);

	dev->run_mode = param->parm.capture.capturemode;

	mutex_lock(&dev->input_lock);
	switch (dev->run_mode) {
	case CI_MODE_VIDEO:
		ov7251_res = ov7251_res_video;
		N_RES = N_RES_VIDEO;
		break;
	case CI_MODE_STILL_CAPTURE:
		ov7251_res = ov7251_res_still;
		N_RES = N_RES_STILL;
		break;
	default:
		ov7251_res = ov7251_res_preview;
		N_RES = N_RES_PREVIEW;
	}
	mutex_unlock(&dev->input_lock);
	return 0;
}

static int ov7251_g_frame_interval(struct v4l2_subdev *sd,
				   struct v4l2_subdev_frame_interval *interval)
{
	struct ov7251_device *dev = to_ov7251_sensor(sd);

	interval->interval.numerator = 1;
	interval->interval.denominator = ov7251_res[dev->fmt_idx].fps;

	return 0;
}

static int ov7251_enum_mbus_code(struct v4l2_subdev *sd,
				struct v4l2_subdev_pad_config *cfg,
				struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index >= MAX_FMTS)
		return -EINVAL;

	code->code = MEDIA_BUS_FMT_SBGGR10_1X10;
	return 0;
}

static int ov7251_enum_frame_size(struct v4l2_subdev *sd,
				struct v4l2_subdev_pad_config *cfg,
				struct v4l2_subdev_frame_size_enum *fse)
{
	int index = fse->index;

	if (index >= N_RES)
		return -EINVAL;

	fse->min_width = ov7251_res[index].width;
	fse->min_height = ov7251_res[index].height;
	fse->max_width = ov7251_res[index].width;
	fse->max_height = ov7251_res[index].height;

	return 0;

}

static struct v4l2_mbus_framefmt *
__ov7251_get_pad_format(struct ov7251_device *sensor, struct v4l2_subdev *sd,
			struct v4l2_subdev_pad_config *cfg,
			unsigned int pad,
			enum v4l2_subdev_format_whence which)
{
	struct i2c_client *client = v4l2_get_subdevdata(&sensor->sd);

	if (pad != 0) {
		dev_err(&client->dev,
			"%s err. pad %x\n", __func__, pad);
		return NULL;
	}

	switch (which) {
	case V4L2_SUBDEV_FORMAT_TRY:
		return v4l2_subdev_get_try_format(sd, cfg, pad);
	case V4L2_SUBDEV_FORMAT_ACTIVE:
		return &sensor->format;
	default:
		return NULL;
	}
}

static int ov7251_get_pad_format(struct v4l2_subdev *sd,
				struct v4l2_subdev_pad_config *cfg,
				struct v4l2_subdev_format *fmt)
{
	struct ov7251_device *snr = to_ov7251_sensor(sd);
	struct v4l2_mbus_framefmt *format =
			__ov7251_get_pad_format(snr, sd, cfg, fmt->pad, fmt->which);
	if (!format)
		return -EINVAL;

	fmt->format = *format;
	return 0;
}

static int ov7251_set_pad_format(struct v4l2_subdev *sd,
				struct v4l2_subdev_pad_config *cfg,
				struct v4l2_subdev_format *fmt)
{
	struct ov7251_device *snr = to_ov7251_sensor(sd);

	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE)
		snr->format = fmt->format;

	return 0;
}

static int ov7251_g_skip_frames(struct v4l2_subdev *sd, u32 *frames)
{
	struct ov7251_device *dev = to_ov7251_sensor(sd);

	mutex_lock(&dev->input_lock);
	*frames = ov7251_res[dev->fmt_idx].skip_frames;
	mutex_unlock(&dev->input_lock);

	return 0;
}

static const struct v4l2_subdev_sensor_ops ov7251_sensor_ops = {
	.g_skip_frames	= ov7251_g_skip_frames,
};

static const struct v4l2_ctrl_ops ov7251_ctrl_ops = {
	.g_volatile_ctrl = ov7251_g_volatile_ctrl,
};

static const struct v4l2_ctrl_config v4l2_ctrl_link_freq = {
	.ops = &ov7251_ctrl_ops,
	.id = V4L2_CID_LINK_FREQ,
	.name = "Link Frequency",
	.type = V4L2_CTRL_TYPE_INTEGER,
	.min = 1,
	.max = 1500000 * 1000,
	.step = 1,
	.def = 1,
	.flags = V4L2_CTRL_FLAG_VOLATILE | V4L2_CTRL_FLAG_READ_ONLY,
};

static const struct v4l2_subdev_video_ops ov7251_video_ops = {
	.s_stream = ov7251_s_stream,
	.g_parm = ov7251_g_parm,
	.s_parm = ov7251_s_parm,
	.try_mbus_fmt = ov7251_try_mbus_fmt,
	.g_mbus_fmt = ov7251_g_mbus_fmt,
	.s_mbus_fmt = ov7251_s_mbus_fmt,
	.g_frame_interval = ov7251_g_frame_interval,
};

static const struct v4l2_subdev_core_ops ov7251_core_ops = {
	.s_power = ov7251_s_power,
	.queryctrl = ov7251_queryctrl,
	.g_ctrl = ov7251_g_ctrl,
	.s_ctrl = ov7251_s_ctrl,
	.ioctl = ov7251_ioctl,
};

static const struct v4l2_subdev_pad_ops ov7251_pad_ops = {
	.enum_mbus_code = ov7251_enum_mbus_code,
	.enum_frame_size = ov7251_enum_frame_size,
	.enum_frame_interval = ov7251_enum_frame_interval,
	.get_fmt = ov7251_get_pad_format,
	.set_fmt = ov7251_set_pad_format,
};

static const struct v4l2_subdev_ops ov7251_ops = {
	.core = &ov7251_core_ops,
	.video = &ov7251_video_ops,
	.pad = &ov7251_pad_ops,
	.sensor = &ov7251_sensor_ops,
};

static int ov7251_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov7251_device *dev = to_ov7251_sensor(sd);

	dev_dbg(&client->dev, "%s...\n", __func__);

	v4l2_ctrl_handler_free(&dev->ctrl_handler);
	v4l2_device_unregister_subdev(sd);
	media_entity_cleanup(&dev->sd.entity);
	kfree(dev);

	return 0;
}

static int __ov7251_init_ctrl_handler(struct ov7251_device *dev)
{
	struct v4l2_ctrl_handler *hdl;

	hdl = &dev->ctrl_handler;

	v4l2_ctrl_handler_init(&dev->ctrl_handler, 3);

	dev->link_freq = v4l2_ctrl_new_custom(&dev->ctrl_handler,
					      &v4l2_ctrl_link_freq,
					      NULL);

	if (dev->ctrl_handler.error || dev->link_freq == NULL)
		return dev->ctrl_handler.error;

	dev->sd.ctrl_handler = hdl;

	return 0;
}


static ssize_t value_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct input_dev *input = to_input_dev(dev);
	struct ov7251_device *ov = input_get_drvdata(input);
	u16 high, low;
	int ret;
	u16 id;
	u8 revision;

	ret = ov7251_read_reg(ov->ov_i2c_client, OV7251_8BIT,
					OV7251_SC_CMMN_CHIP_ID_H, &high);
	if (ret) {
		dev_err(dev, "i2c error");
		return 0;
	}
	ret = ov7251_read_reg(ov->ov_i2c_client, OV7251_8BIT,
					OV7251_SC_CMMN_CHIP_ID_L, &low);

	if (ret) {
		dev_err(dev, "i2c error");
		return 0;
	}

	id = ((((u16) high) << 8) | (u16) low);

	ret = ov7251_read_reg(ov->ov_i2c_client, OV7251_8BIT, SC_REG0C_REG, &high);
	revision = (u8) high & 0x0f;

	return sprintf(buf, "%hd %c\r\n", id, revision);
}



static ssize_t spower_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	int enable;
	struct input_dev *input = to_input_dev(dev);
	struct ov7251_device *ov = input_get_drvdata(input);
	int ret;

	ret = kstrtoint(buf, 0, &enable);
	if (ret)
		return ret;

	ov7251_s_power(&(ov->sd), enable);
	return count;
}

static u16 tmp_reg;


static ssize_t reg_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct input_dev *input = to_input_dev(dev);
	struct ov7251_device *ov = input_get_drvdata(input);
	u16 value;
	int ret;

	ret = ov7251_read_reg(ov->ov_i2c_client, OV7251_8BIT,
					tmp_reg, &value);
	if (ret) {
		dev_err(dev, "i2c error");
		return 0;
	}
	return sprintf(buf, "%d\r\n", (int)value);
}



static ssize_t reg_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	int ret;
	u16 reg, value;
	struct input_dev *input = to_input_dev(dev);
	struct ov7251_device *ov = input_get_drvdata(input);

	if (sscanf(buf, "%hd %hd", &reg, &value) != 2)
		return -EINVAL;

	if (value == 0xffff) {
		tmp_reg = reg;
	} else {

		/* group hold end */
		ret = ov7251_write_reg(ov->ov_i2c_client, OV7251_8BIT,
					reg, value);
	}
	return count;
}

static ssize_t init_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	int enable;
	struct input_dev *input = to_input_dev(dev);
	struct ov7251_device *ov = input_get_drvdata(input);
	int ret;

	ret = kstrtoint(buf, 0, &enable);
	if (ret)
		return ret;

	if (enable)
		startup(&(ov->sd));

	return count;
}

static DEVICE_ATTR_RO(value);

static DEVICE_ATTR_WO(spower);

static DEVICE_ATTR_RW(reg);

static DEVICE_ATTR_WO(init);

static struct attribute *ov_attributes[] = {
	&dev_attr_value.attr,
	&dev_attr_spower.attr,
	&dev_attr_reg.attr,
	&dev_attr_init.attr,
	NULL
};

static struct attribute_group ov_attribute_group = {
	.attrs = ov_attributes
};

#define SENSOR_NAME                 "ov7251"
#define ABSMIN                      -512
#define ABSMAX                      512



static int ov7251_probe(struct i2c_client *client)
{
	struct ov7251_device *dev;
	int ret;

	dev_info(&client->dev, "tal test probe called");
	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev) {
		dev_err(&client->dev, "out of memory\n");
		return -ENOMEM;
	}

	mutex_init(&dev->input_lock);

	dev->fmt_idx = 0;
	v4l2_i2c_subdev_init(&dev->sd, client, &ov7251_ops);

	ret = ov7251_s_config(&dev->sd);
	if (ret)
		goto out_free;

	ret = __ov7251_init_ctrl_handler(dev);
	if (ret)
		goto out_ctrl_handler_free;

	dev->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	dev->pad.flags = MEDIA_PAD_FL_SOURCE;
	dev->format.code = MEDIA_BUS_FMT_SBGGR10_1X10;
	dev->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	ret = media_entity_pads_init(&dev->sd.entity, 1, &dev->pad);
	if (ret)
		ov7251_remove(client);

#if 1
	{
	struct input_dev *idev;

	dev->ov_i2c_client = client;

	idev = input_allocate_device();
	if (!idev) {
		dev_err(&client->dev, "input allocation1 failed");
		return -ENOMEM;
	}

	idev->name = SENSOR_NAME;
	idev->id.bustype = BUS_I2C;
	input_set_capability(idev, EV_ABS, ABS_MISC);
	input_set_abs_params(idev, ABS_X, ABSMIN, ABSMAX, 0, 0);
	input_set_abs_params(idev, ABS_Y, ABSMIN, ABSMAX, 0, 0);
	input_set_abs_params(idev, ABS_Z, ABSMIN, ABSMAX, 0, 0);

	input_set_drvdata(idev, dev);
	ret = input_register_device(idev);
	if (ret < 0)
		dev_err(&client->dev, "input_register_device failed");

	ret = sysfs_create_group(&idev->dev.kobj, &ov_attribute_group);
	if (ret < 0)
		dev_err(&client->dev, "sysfs_create_group failed");

	}
#endif

	return ret;

out_ctrl_handler_free:
	v4l2_ctrl_handler_free(&dev->ctrl_handler);

out_free:
	v4l2_device_unregister_subdev(&dev->sd);
	kfree(dev);
	return ret;
}

static struct acpi_device_id ov7251_acpi_match[] = {
	{"INT347A"},
	{"INT35AA"},
	{},
};

MODULE_DEVICE_TABLE(acpi, ov7251_acpi_match);

static struct i2c_driver ov7251_driver = {
	.driver = {
		.name = OV7251_NAME,
		.acpi_match_table = ACPI_PTR(ov7251_acpi_match),
	},
	.probe_new = ov7251_probe,
	.remove = ov7251_remove,
};
module_i2c_driver(ov7251_driver);

MODULE_AUTHOR("Wei Liu <wei.liu@intel.com>");
MODULE_DESCRIPTION("A low-level driver for OmniVision 7251 sensors");
MODULE_LICENSE("GPL");
