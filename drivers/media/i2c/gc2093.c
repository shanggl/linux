// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2020 MediaTek Inc.
#include <asm/unaligned.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/units.h>
#include <media/media-entity.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#define GC2093_ID					0x2093
#define GC2093_ID_MASK					GENMASK(15, 0)

#define GC2093_REG_CHIP_ID				0x03F0

/* Bit[1] vertical upside down */
/* Bit[0] horizontal mirror */
#define REG_MIRROR_FLIP_CONTROL				0x0017

/* Orientation */
#define REG_MIRROR_FLIP_ENABLE				0x03

#define REG_SC_CTRL_MODE					0x003E
#define SC_CTRL_MODE_STANDBY				0x11
#define SC_CTRL_MODE_STREAMING				0x91

/* Exposure control */
#define GC2093_EXP_SHIFT					8
#define GC2093_REG_EXPOSURE_H				0x0003
#define GC2093_REG_EXPOSURE_L				0x0004
#define	GC2093_EXPOSURE_MIN					4
#define GC2093_EXPOSURE_MAX_MARGIN			4
#define	GC2093_EXPOSURE_STEP				1

/* Vblanking control */
#define GC2093_VTS_SHIFT				8
#define GC2093_REG_VTS_H				0x0041
#define GC2093_REG_VTS_L				0x0042
#define GC2093_VTS_MAX					0x209f
#define GC2093_BASE_LINES				1080

/* Analog gain control */
#define GC2093_GAIN_MIN					0x40
#define GC2093_GAIN_MAX					0x2000
#define GC2093_GAIN_STEP				0x01
#define GC2093_GAIN_DEFAULT				0x40

/* Test pattern control */
#define GC2093_REG_TEST_PATTERN			0xb6

#define GC2093_LINK_FREQ_390MHZ			(390 * HZ_PER_MHZ)
#define GC2093_ECLK_FREQ				(24 * HZ_PER_MHZ)

/* Number of lanes supported by this driver */
#define GC2093_DATA_LANES				2

/* Bits per sample of sensor output */
#define GC2093_BITS_PER_SAMPLE			10

static const char * const gc2093_supply_names[] = {
	"dovdd",	/* Digital I/O power */
	"avdd",		/* Analog power */
	"dvdd",		/* Digital core power */
};

struct gc2093_reg {
	u16 addr;
	u8 val;
};

struct gc2093_reg_list {
	u32 num_of_regs;
	const struct gc2093_reg *regs;
};

struct gc2093_mode {
	u32 width;
	u32 height;
	u32 exp_def;//exposure default
	u32 hts_def;
	u32 vts_def;//vertcal time line default
	const struct gc2093_reg_list reg_list;
};

struct gc2093 {
	u32 eclk_freq;

	struct clk *eclk;
	//struct gpio_desc *pd_gpio;
	struct gpio_desc *rst_gpio;
	struct regulator_bulk_data supplies[ARRAY_SIZE(gc2093_supply_names)];

	bool streaming;
	bool upside_down;

	/*
	 * Serialize control access, get/set format, get selection
	 * and start streaming.
	 */
	struct mutex mutex;
	struct v4l2_subdev subdev;
	struct media_pad pad;
	struct v4l2_mbus_framefmt fmt;
	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *exposure;

	const struct gc2093_mode *cur_mode;
};

/*
struct again_lut {
	unsigned int index;
	unsigned char regb0;
	unsigned char regb1;
	unsigned char regb2;
	unsigned char regb3;
	unsigned char regb4;
	unsigned char regb5;
	unsigned char regb6;
	unsigned int gain;
};

struct again_lut gc2093_again_lut[] = {
{0x0,0x0, 0x1, 0x0, 0x8, 0x10, 0x8, 0xa, 0},             	//	1.000000
{0x1,0x10, 0x1, 0xc, 0x8, 0x10, 0x8, 0xa, 16247},           //  	1.187500
{0x2,0x20, 0x1, 0x1b, 0x8, 0x11, 0x8, 0xc, 33277},          //  	1.421875
{0x3,0x30, 0x1, 0x2c, 0x8, 0x12, 0x8, 0xe, 48592},          //  	1.671875
{0x4,0x40, 0x1, 0x3f, 0x8, 0x14, 0x8, 0x12, 63293},         //  	1.953125
{0x5,0x50, 0x2, 0x16, 0x8, 0x15, 0x8, 0x14, 78621},         //  	2.296875
{0x6,0x60, 0x2, 0x35, 0x8, 0x17, 0x8, 0x18, 96180},         //  	2.765625
{0x7,0x70, 0x3, 0x16, 0x8, 0x18, 0x8, 0x1a, 112793},        //  	3.296875
{0x8,0x80, 0x4, 0x2, 0x8, 0x1a, 0x8, 0x1e, 128070},         //   	3.875000
{0x9,0x90, 0x4, 0x31, 0x8, 0x1b, 0x8, 0x20, 145116},        //      4.640625
{0xA,0xa0, 0x5, 0x32, 0x8, 0x1d, 0x8, 0x24, 162249},        //      5.562500
{0XB,0xb0, 0x6, 0x35, 0x8, 0x1e, 0x8, 0x26, 178999},        //      6.640625
{0XC,0xc0, 0x8, 0x4, 0x8, 0x20, 0x8, 0x2a, 195118},         //      7.875000
{0XD,0x5a, 0x9, 0x19, 0x8, 0x1e, 0x8, 0x2a, 211445},        //      9.359375
{0XE,0x83, 0xb, 0xf, 0x8, 0x1f, 0x8, 0x2a, 227385},         //      11.078125
{0XF,0x93, 0xd, 0x12, 0x8, 0x21, 0x8, 0x2e, 242964},        //      13.062500
{0X10,0x84, 0x10, 0x0, 0xb, 0x22, 0x8, 0x30, 257797},        //      15.281250
{0X11,0x94, 0x12, 0x3a, 0xb, 0x24, 0x8, 0x34, 273361},       //      18.015625
{0X12,0x5d, 0x1a, 0x2, 0xb, 0x26, 0x8, 0x34, 307075},        //      25.734375
{0X13,0x9b, 0x1b, 0x20, 0xb, 0x26, 0x8, 0x34, 307305},       //      25.796875
{0X14,0x8c, 0x20, 0xf, 0xb, 0x26, 0x8, 0x34, 322312},        //      30.234375
{0X15,0x9c, 0x26, 0x7, 0x12, 0x26, 0x8, 0x34, 338321},       //      35.812500
{0X16,0xb6, 0x36, 0x21, 0x12, 0x26, 0x8, 0x34, 371019},      //      50.609375
{0X17,0xad, 0x37, 0x3a, 0x12, 0x26, 0x8, 0x34, 389998},      //      61.859375
{0X18,0xbd, 0x3d, 0x2, 0x12, 0x26, 0x8, 0x34, 405937},       //      73.218750

};
*/
static inline struct gc2093 *to_gc2093(struct v4l2_subdev *sd)
{
	return container_of(sd, struct gc2093, subdev);
}


	static const struct gc2093_reg gc2093_init_regs_1920_1080_30fps_mipi_linear[] = {

	/* copy from rv1109*/
	/*
 * window size=1920*1080 mipi@2lane
 * mclk=27M mipi_clk=594Mbps
 * pixel_line_total=2200 line_frame_total=1125
 * row_time=29.62us frame_rate=30fps
 */
	/* System */
	{0x03fe, 0x80},
	{0x03fe, 0x80},
	{0x03fe, 0x80},
	{0x03fe, 0x00},
	{0x03f2, 0x00},
	{0x03f3, 0x00},
	{0x03f4, 0x36},
	{0x03f5, 0xc0},
	{0x03f6, 0x0a},
	{0x03f7, 0x01},
	{0x03f8, 0x2c},
	{0x03f9, 0x10},
	{0x03fc, 0x8e},
	/* Cisctl & Analog */
	{0x0087, 0x18},
	{0x00ee, 0x30},
	{0x00d0, 0xb7},
	{0x01a0, 0x00},
	{0x01a4, 0x40},
	{0x01a5, 0x40},
	{0x01a6, 0x40},
	{0x01af, 0x09},
	{0x0001, 0x00},
	{0x0002, 0x02},
	{0x0003, 0x00},
	{0x0004, 0x02},
	{0x0005, 0x04},
	{0x0006, 0x4c},
	{0x0007, 0x00},
	{0x0008, 0x11},
	{0x0009, 0x00},
	{0x000a, 0x02},
	{0x000b, 0x00},
	{0x000c, 0x04},
	{0x000d, 0x04},
	{0x000e, 0x40},
	{0x000f, 0x07},
	{0x0010, 0x8c},
	{0x0013, 0x15},
	{0x0019, 0x0c},
	{0x0041, 0x04},
	{0x0042, 0x65},
	{0x0053, 0x60},
	{0x008d, 0x92},
	{0x0090, 0x00},
	{0x00c7, 0xe1},
	{0x001b, 0x73},
	{0x0028, 0x0d},
	{0x0029, 0x24},
	{0x002b, 0x04},
	{0x002e, 0x23},
	{0x0037, 0x03},
	{0x0043, 0x04},
	{0x0044, 0x38},
	{0x004a, 0x01},
	{0x004b, 0x28},
	{0x0055, 0x38},
	{0x006b, 0x44},
	{0x0077, 0x00},
	{0x0078, 0x20},
	{0x007c, 0xa1},
	{0x00d3, 0xd4},
	{0x00e6, 0x50},
	/* Gain */
	{0x00b6, 0xc0},
	{0x00b0, 0x60},
	/* Isp */
	{0x0102, 0x89},
	{0x0104, 0x01},
	{0x010f, 0x00},
	{0x0158, 0x00},
	{0x0123, 0x08},
	{0x0123, 0x00},
	{0x0120, 0x01},
	{0x0121, 0x00},
	{0x0122, 0x10},
	{0x0124, 0x03},
	{0x0125, 0xff},
	{0x0126, 0x3c},
	{0x001a, 0x8c},
	{0x00c6, 0xe0},
	/* Blk */
	{0x0026, 0x30},
	{0x0142, 0x00},
	{0x0149, 0x1e},
	{0x014a, 0x07},
	{0x014b, 0x80},
	{0x0155, 0x00},
	{0x0414, 0x78},
	{0x0415, 0x78},
	{0x0416, 0x78},
	{0x0417, 0x78},
	/* Window */
	{0x0192, 0x02},
	{0x0194, 0x03},
	{0x0195, 0x04},
	{0x0196, 0x38},
	{0x0197, 0x07},
	{0x0198, 0x80},
	/* MIPI */
	{0x019a, 0x06},
	{0x007b, 0x2a},
	{0x0023, 0x2d},
	{0x0201, 0x27},
	{0x0202, 0x56},
	{0x0203, 0xce},
	{0x0212, 0x80},
	{0x0213, 0x07},
	{0x003e, 0x91},
};

#if 0

/*
 * eclk 24Mhz
 * pclk 39Mhz
 * linelength 934(0x3a6)
 * framelength 1390(0x56E)
 * grabwindow_width 1600
 * grabwindow_height 1200
 * max_framerate 30fps
 * mipi_datarate per lane 780Mbps
 */
	/****system****/
	{0x03fe,0xf0},
	{0x03fe,0xf0},
	{0x03fe,0xf0},
	{0x03fe,0x00},
	{0x03f2,0x00},
	{0x03f3,0x00},
	{0x03f4,0x36},
	{0x03f5,0xc0},
	{0x03f6,0x0b},
	{0x03f7,0x11},
	{0x03f8,0x30},
	{0x03f9,0x42},
	{0x03fc,0x8e},
	/****CISCTL & ANALOG****/
	{0x0087,0x18},
	{0x00ee,0x30},
	{0x00d0,0xbf},
	{0x01a0,0x00},
	{0x01a4,0x40},
	{0x01a5,0x40},
	{0x01a6,0x40},
	{0x01af,0x09},

	{0x0003,0x01},
	{0x0004,0x38},/*1 step 10ms*/
	{0x0005,0x05},/*hts*/
	{0x0006,0x8e},
	{0x0007,0x00},
	{0x0008,0x11},
	{0x0009,0x00},
	{0x000a,0x02},
	{0x000b,0x00},
	{0x000c,0x04},
	{0x000d,0x04},
	{0x000e,0x40},
	{0x000f,0x07},
	{0x0010,0x8c},
	{0x0013,0x15},
	{0x0019,0x0c},
	{0x0041,0x04},/*vts*/
	{0x0042,0x65},
	{0x0053,0x60},
	{0x008d,0x92},
	{0x0090,0x00},
	{0x00c7,0xe1},
	{0x001b,0x73},
	{0x0028,0x0d},
	{0x0029,0x40},
	{0x002b,0x04},
	{0x002e,0x23},
	{0x0037,0x03},
	{0x0043,0x04},
	{0x0044,0x30},
	{0x004a,0x01},
	{0x004b,0x28},
	{0x0055,0x30},
	{0x0066,0x3f},
	{0x0068,0x3f},
	{0x006b,0x44},
	{0x0077,0x00},
	{0x0078,0x20},
	{0x007c,0xa1},
	{0x00ce,0x7c},
	{0x00d3,0xd4},
	{0x00e6,0x50},
	/*gain*/
	{0x00b6,0xc0},
	{0x00b0,0x68},
	{0x00b3,0x00},
	{0x00b8,0x01},
	{0x00b9,0x00},
	{0x00b1,0x01},
	{0x00b2,0x00},
	/*isp*/
	{0x0101,0x0c},
	{0x0102,0x89},
	{0x0104,0x01},
	{0x0107,0xa6},
	{0x0108,0xa9},
	{0x0109,0xa8},
	{0x010a,0xa7},
	{0x010b,0xff},
	{0x010c,0xff},
	{0x010f,0x00},
	{0x0158,0x00},
	{0x0428,0x86},
	{0x0429,0x86},
	{0x042a,0x86},
	{0x042b,0x68},
	{0x042c,0x68},
	{0x042d,0x68},
	{0x042e,0x68},
	{0x042f,0x68},
	{0x0430,0x4f},
	{0x0431,0x68},
	{0x0432,0x67},
	{0x0433,0x66},
	{0x0434,0x66},
	{0x0435,0x66},
	{0x0436,0x66},
	{0x0437,0x66},
	{0x0438,0x62},
	{0x0439,0x62},
	{0x043a,0x62},
	{0x043b,0x62},
	{0x043c,0x62},
	{0x043d,0x62},
	{0x043e,0x62},
	{0x043f,0x62},
	/*dark sun*/
	{0x0123,0x08},
	{0x0123,0x00},
	{0x0120,0x01},
	{0x0121,0x04},
	{0x0122,0x65},
	{0x0124,0x03},
	{0x0125,0xff},
	{0x001a,0x8c},
	{0x00c6,0xe0},
	/*blk*/
	{0x0026,0x30},
	{0x0142,0x00},
	{0x0149,0x1e},
	{0x014a,0x0f},
	{0x014b,0x00},
	{0x0155,0x07},
	{0x0414,0x78},
	{0x0415,0x78},
	{0x0416,0x78},
	{0x0417,0x78},
	{0x04e0,0x18},
	/*window*/
	{0x0192,0x02},
	{0x0194,0x03},
	{0x0195,0x04},
	{0x0196,0x38},
	{0x0197,0x07},
	{0x0198,0x80},
	/****DVP & MIPI****/
	{0x019a,0x06},
	{0x007b,0x2a},
	{0x0023,0x2d},
	{0x0201,0x27},
	{0x0202,0x56},
	{0x0203,0xb6},
	{0x0212,0x80},
	{0x0213,0x07},
	{0x0215,0x10},
	{0x003e,0x91},
};
#endif 
#if 0
static const struct gc2093_reg gc2093_1600x1200_regs[] = {
	{0xfd, 0x01},
	{0xac, 0x00},
	{0xfd, 0x00},
	{0x2f, 0x29},
	{0x34, 0x00},
	{0x35, 0x21},
	{0x30, 0x15},
	{0x33, 0x01},
	{0xfd, 0x01},
	{0x44, 0x00},
	{0x2a, 0x4c},
	{0x2b, 0x1e},
	{0x2c, 0x60},
	{0x25, 0x11},
	{0x03, 0x01},
	{0x04, 0xae},
	{0x09, 0x00},
	{0x0a, 0x02},
	{0x06, 0xa6},
	{0x31, 0x00},
	{0x24, 0x40},
	{0x01, 0x01},
	{0xfb, 0x73},
	{0xfd, 0x01},
	{0x16, 0x04},
	{0x1c, 0x09},
	{0x21, 0x42},
	{0x12, 0x04},
	{0x13, 0x10},
	{0x11, 0x40},
	{0x33, 0x81},
	{0xd0, 0x00},
	{0xd1, 0x01},
	{0xd2, 0x00},
	{0x50, 0x10},
	{0x51, 0x23},
	{0x52, 0x20},
	{0x53, 0x10},
	{0x54, 0x02},
	{0x55, 0x20},
	{0x56, 0x02},
	{0x58, 0x48},
	{0x5d, 0x15},
	{0x5e, 0x05},
	{0x66, 0x66},
	{0x68, 0x68},
	{0x6b, 0x00},
	{0x6c, 0x00},
	{0x6f, 0x40},
	{0x70, 0x40},
	{0x71, 0x0a},
	{0x72, 0xf0},
	{0x73, 0x10},
	{0x75, 0x80},
	{0x76, 0x10},
	{0x84, 0x00},
	{0x85, 0x10},
	{0x86, 0x10},
	{0x87, 0x00},
	{0x8a, 0x22},
	{0x8b, 0x22},
	{0x19, 0xf1},
	{0x29, 0x01},
	{0xfd, 0x01},
	{0x9d, 0x16},
	{0xa0, 0x29},
	{0xa1, 0x04},
	{0xad, 0x62},
	{0xae, 0x00},
	{0xaf, 0x85},
	{0xb1, 0x01},
	{0x8e, 0x06},
	{0x8f, 0x40},
	{0x90, 0x04},
	{0x91, 0xb0},
	{0x45, 0x01},
	{0x46, 0x00},
	{0x47, 0x6c},
	{0x48, 0x03},
	{0x49, 0x8b},
	{0x4a, 0x00},
	{0x4b, 0x07},
	{0x4c, 0x04},
	{0x4d, 0xb7},
	{0xf0, 0x40},
	{0xf1, 0x40},
	{0xf2, 0x40},
	{0xf3, 0x40},
	{0x3f, 0x00},
	{0xfd, 0x01},
	{0x05, 0x00},
	{0x06, 0xa6},
	{0xfd, 0x01},
};
#endif
static const char * const gc2093_test_pattern_menu[] = {
	"Disabled",
	"Eight Vertical Colour Bars",
};

static const s64 link_freq_menu_items[] = {
	GC2093_LINK_FREQ_390MHZ,
};

static u64 to_pixel_rate(u32 f_index)
{
	u64 pixel_rate = link_freq_menu_items[f_index] * 2 * GC2093_DATA_LANES ;

	do_div(pixel_rate, GC2093_BITS_PER_SAMPLE);

	return pixel_rate;
}

static const struct gc2093_mode supported_modes[] = {
	{
		.width = 1920,
		.height = 1080,
		.exp_def = 0x0465,
		.hts_def = 0x0b1c,
		.vts_def = 0x0465,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(gc2093_init_regs_1920_1080_30fps_mipi_linear),
			.regs = gc2093_init_regs_1920_1080_30fps_mipi_linear,
		},
	},
};

static int gc2093_read_reg(struct gc2093 *gc2093, u16 reg, u32 len, u32 *val)
{
	struct i2c_client *client = v4l2_get_subdevdata(&gc2093->subdev);
	struct i2c_msg msgs[2] = {0};
	u8 addr_buf[2] = {0};
	u8 data_buf[4] = {0};
	int ret;

	if (WARN_ON(len > 4))
		return -EINVAL;

	put_unaligned_be16(reg, addr_buf);

	/* Write register address */
	msgs[0].addr = client->addr;
	msgs[0].flags = 0;
	msgs[0].len = ARRAY_SIZE(addr_buf);
	msgs[0].buf = addr_buf;

	/* Read data from register */
	msgs[1].addr = client->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = len;
	msgs[1].buf = data_buf;

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret != ARRAY_SIZE(msgs))
		return -EIO;

	*val = get_unaligned_be16(data_buf);//返回的是 0x2093 但是转换成了9320

	return 0;
}

static int gc2093_write_reg(struct gc2093 *gc2093, u16 reg, u32 len, u32 val)
{
	struct i2c_client *client = v4l2_get_subdevdata(&gc2093->subdev);
	u8 buf[6] = {0};

	if (WARN_ON(len > 4))
		return -EINVAL;

	put_unaligned_be16(reg, buf);
	put_unaligned_le32(val, buf + 2);
	if (i2c_master_send(client, buf, len + 2) != len + 2)
		return -EIO;

	return 0;
}

static int gc2093_write_array(struct gc2093 *gc2093,
			       const struct gc2093_reg_list *r_list)
{
	unsigned int i;
	int ret;

	for (i = 0; i < r_list->num_of_regs; i++) {
		ret = gc2093_write_reg(gc2093, r_list->regs[i].addr, 1, r_list->regs[i].val);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static void gc2093_fill_fmt(const struct gc2093_mode *mode,
			     struct v4l2_mbus_framefmt *fmt)
{
	fmt->width = mode->width;
	fmt->height = mode->height;
	fmt->field = V4L2_FIELD_NONE;
}

static int gc2093_set_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *sd_state,
			   struct v4l2_subdev_format *fmt)
{
	struct gc2093 *gc2093 = to_gc2093(sd);
	struct v4l2_mbus_framefmt *mbus_fmt = &fmt->format;
	struct v4l2_mbus_framefmt *frame_fmt;
	int ret = 0;

	mutex_lock(&gc2093->mutex);

	if (gc2093->streaming && fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE) {
		ret = -EBUSY;
		goto out_unlock;
	}

	/* Only one sensor mode supported */
	mbus_fmt->code = gc2093->fmt.code;
	gc2093_fill_fmt(gc2093->cur_mode, mbus_fmt);

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY)
		frame_fmt = v4l2_subdev_get_try_format(sd, sd_state, 0);
	else
		frame_fmt = &gc2093->fmt;

	*frame_fmt = *mbus_fmt;

out_unlock:
	mutex_unlock(&gc2093->mutex);
	return ret;
}

static int gc2093_get_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *sd_state,
			   struct v4l2_subdev_format *fmt)
{
	struct gc2093 *gc2093 = to_gc2093(sd);
	struct v4l2_mbus_framefmt *mbus_fmt = &fmt->format;

	mutex_lock(&gc2093->mutex);

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		fmt->format = *v4l2_subdev_get_try_format(sd, sd_state,
							  fmt->pad);
	} else {
		fmt->format = gc2093->fmt;
		mbus_fmt->code = gc2093->fmt.code;
		gc2093_fill_fmt(gc2093->cur_mode, mbus_fmt);
	}

	mutex_unlock(&gc2093->mutex);

	return 0;
}

static int gc2093_enum_mbus_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_mbus_code_enum *code)
{
	struct gc2093 *gc2093 = to_gc2093(sd);

	if (code->index != 0)
		return -EINVAL;

	code->code = gc2093->fmt.code;

	return 0;
}

static int gc2093_enum_frame_sizes(struct v4l2_subdev *sd,
				    struct v4l2_subdev_state *sd_state,
				    struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index >= ARRAY_SIZE(supported_modes))
		return -EINVAL;

	fse->min_width  = supported_modes[fse->index].width;
	fse->max_width  = supported_modes[fse->index].width;
	fse->max_height = supported_modes[fse->index].height;
	fse->min_height = supported_modes[fse->index].height;

	return 0;
}

static int gc2093_check_sensor_id(struct gc2093 *gc2093)
{
	struct i2c_client *client = v4l2_get_subdevdata(&gc2093->subdev);
	u32 chip_id;
	int ret;

	/* Validate the chip ID */
	ret = gc2093_read_reg(gc2093, GC2093_REG_CHIP_ID, 2, &chip_id);
	
	dev_info(&client->dev, "get sensor id(0x%04x) ret[%d]\n", chip_id,ret);

	if (ret < 0)
		return ret;


	if ((chip_id & GC2093_ID_MASK) != GC2093_ID) {
		dev_err(&client->dev, "unexpected sensor id(0x%04x)\n", chip_id);
		return -EINVAL;
	}

	return 0;
}

static int gc2093_power_on(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct gc2093 *gc2093 = to_gc2093(sd);
	int ret;

	gpiod_set_value_cansleep(gc2093->rst_gpio, 0);
	//gpiod_set_value_cansleep(gc2093->pd_gpio, 1);

	dev_info(dev, "Power on gc2093\n");

	ret = clk_prepare_enable(gc2093->eclk);
	if (ret < 0) {
		dev_err(dev, "failed to enable eclk\n");
		return ret;
	}

	ret = regulator_bulk_enable(ARRAY_SIZE(gc2093_supply_names),
				    gc2093->supplies);
	if (ret < 0) {
		dev_err(dev, "failed to enable regulators\n");
		goto disable_clk;
	}
	usleep_range(5000, 6000);

	//gpiod_set_value_cansleep(gc2093->pd_gpio, 0);
	//usleep_range(5000, 6000);

	gpiod_set_value_cansleep(gc2093->rst_gpio, 1);
	usleep_range(5000, 6000);

	ret = gc2093_check_sensor_id(gc2093);
	if (ret)
		goto disable_regulator;

	return 0;

disable_regulator:
	regulator_bulk_disable(ARRAY_SIZE(gc2093_supply_names),
			       gc2093->supplies);
disable_clk:
	clk_disable_unprepare(gc2093->eclk);

	return ret;
}

static int gc2093_power_off(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct gc2093 *gc2093 = to_gc2093(sd);

	gpiod_set_value_cansleep(gc2093->rst_gpio, 0);
	clk_disable_unprepare(gc2093->eclk);
	//gpiod_set_value_cansleep(gc2093->pd_gpio, 1);
	regulator_bulk_disable(ARRAY_SIZE(gc2093_supply_names),
			       gc2093->supplies);

	return 0;
}

static int __gc2093_start_stream(struct gc2093 *gc2093)
{
	const struct gc2093_reg_list *reg_list;
	int ret;

	struct i2c_client *client = v4l2_get_subdevdata(&gc2093->subdev);

	/* Apply default values of current mode */
	reg_list = &gc2093->cur_mode->reg_list;
	ret = gc2093_write_array(gc2093, reg_list);
	dev_info(&client->dev,"[GC2093] write_array ret [%d]\n",ret);
	if (ret)
		return ret;

	/* Apply customized values from user */
	ret = __v4l2_ctrl_handler_setup(gc2093->subdev.ctrl_handler);
	dev_info(&client->dev,"[GC2093] ctrl handler ret [%d]\n",ret);
	if (ret)
		return ret;

	/* Set stream on register */
	ret = gc2093_write_reg(gc2093, REG_SC_CTRL_MODE, 1,
					 SC_CTRL_MODE_STREAMING);
	dev_info(&client->dev,"[GC2093] set mode stream ret [%d]\n",ret);

	return ret;
}

static int __gc2093_stop_stream(struct gc2093 *gc2093)
{
	return gc2093_write_reg(gc2093, REG_SC_CTRL_MODE, 1,
					 SC_CTRL_MODE_STANDBY);
}

static int gc2093_entity_init_cfg(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *sd_state)
{
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.format = {
			.width = 1920,
			.height = 1080,
		}
	};

	gc2093_set_fmt(sd, sd_state, &fmt);

	return 0;
}

static int gc2093_s_stream(struct v4l2_subdev *sd, int on)
{
	
	struct gc2093 *gc2093 = to_gc2093(sd);
	struct i2c_client *client = v4l2_get_subdevdata(&gc2093->subdev);
	int ret;
	dev_info(&client->dev, "[GC2093] begin start stream\n");

	mutex_lock(&gc2093->mutex);

	if (gc2093->streaming == on) {
		ret = 0;
		goto unlock_and_return;
	}

	if (on) {
		ret = pm_runtime_resume_and_get(&client->dev);
		
		dev_info(&client->dev, "[GC2093]  start stream pm resume ret[%d]\n",ret);

		if (ret < 0)
			goto unlock_and_return;

		ret = __gc2093_start_stream(gc2093);

		dev_info(&client->dev, "[GC2093] __gc2093_start_stream [%d]\n",ret);

		if (ret) {
			__gc2093_stop_stream(gc2093);
			gc2093->streaming = !on;
			goto err_rpm_put;
		}
	} else {

		dev_info(&client->dev, "[GC2093] __gc2093_stop_stream\n");
		__gc2093_stop_stream(gc2093);
		pm_runtime_put(&client->dev);
	}

	gc2093->streaming = on;
	mutex_unlock(&gc2093->mutex);

	return 0;

err_rpm_put:
	pm_runtime_put(&client->dev);
unlock_and_return:
	mutex_unlock(&gc2093->mutex);

	return ret;
}

static const struct dev_pm_ops gc2093_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
	SET_RUNTIME_PM_OPS(gc2093_power_off, gc2093_power_on, NULL)
};

struct gain_reg_config {
	u32 value;
	u16 analog_gain;
	u16 col_gain;
	u16 analog_sw;
	u16 ram_width;
};

static const struct gain_reg_config gain_reg_configs[] = {
	{  64, 0x0000, 0x0100, 0x6807, 0x00f8},
	{  75, 0x0010, 0x010c, 0x6807, 0x00f8},
	{  90, 0x0020, 0x011b, 0x6c08, 0x00f9},
	{ 105, 0x0030, 0x012c, 0x6c0a, 0x00fa},
	{ 122, 0x0040, 0x013f, 0x7c0b, 0x00fb},
	{ 142, 0x0050, 0x0216, 0x7c0d, 0x00fe},
	{ 167, 0x0060, 0x0235, 0x7c0e, 0x00ff},
	{ 193, 0x0070, 0x0316, 0x7c10, 0x0801},
	{ 223, 0x0080, 0x0402, 0x7c12, 0x0802},
	{ 257, 0x0090, 0x0431, 0x7c13, 0x0803},
	{ 299, 0x00a0, 0x0532, 0x7c15, 0x0805},
	{ 346, 0x00b0, 0x0635, 0x7c17, 0x0807},
	{ 397, 0x00c0, 0x0804, 0x7c18, 0x0808},
	{ 444, 0x005a, 0x0919, 0x7c17, 0x0807},
	{ 523, 0x0083, 0x0b0f, 0x7c17, 0x0807},
	{ 607, 0x0093, 0x0d12, 0x7c19, 0x0809},
	{ 700, 0x0084, 0x1000, 0x7c1b, 0x080c},
	{ 817, 0x0094, 0x123a, 0x7c1e, 0x080f},
	{1131, 0x005d, 0x1a02, 0x7c23, 0x0814},
	{1142, 0x009b, 0x1b20, 0x7c25, 0x0816},
	{1334, 0x008c, 0x200f, 0x7c27, 0x0818},
	{1568, 0x009c, 0x2607, 0x7c2a, 0x081b},
	{2195, 0x00b6, 0x3621, 0x7c32, 0x0823},
	{2637, 0x00ad, 0x373a, 0x7c36, 0x0827},
	{3121, 0x00bd, 0x3d02, 0x7c3a, 0x082b},
};

static int gc2093_set_gain(struct gc2093 *gc2093, u32 gain)
{
	int ret, i = 0;
	u16 pre_gain = 0;

	for (i = 0; i < ARRAY_SIZE(gain_reg_configs) - 1; i++)
		if ((gain_reg_configs[i].value <= gain) && (gain < gain_reg_configs[i+1].value))
			break;

	ret = gc2093_write_reg(gc2093, 0x00b4, 1, gain_reg_configs[i].analog_gain >> 8);
	ret |= gc2093_write_reg(gc2093, 0x00b3, 1, gain_reg_configs[i].analog_gain & 0xff);
	ret |= gc2093_write_reg(gc2093, 0x00b8, 1, gain_reg_configs[i].col_gain >> 8);
	ret |= gc2093_write_reg(gc2093, 0x00b9, 1, gain_reg_configs[i].col_gain & 0xff);
	ret |= gc2093_write_reg(gc2093, 0x00ce, 1, gain_reg_configs[i].analog_sw >> 8);
	ret |= gc2093_write_reg(gc2093, 0x00c2, 1, gain_reg_configs[i].analog_sw & 0xff);
	ret |= gc2093_write_reg(gc2093, 0x00cf, 1, gain_reg_configs[i].ram_width >> 8);
	ret |= gc2093_write_reg(gc2093, 0x00d9, 1, gain_reg_configs[i].ram_width & 0xff);

	pre_gain = 64 * gain / gain_reg_configs[i].value;

	ret |= gc2093_write_reg(gc2093, 0x00b1, 1, (pre_gain >> 6));
	ret |= gc2093_write_reg(gc2093, 0x00b2, 1, ((pre_gain & 0x3f) << 2));

	return ret;
}

#if 0
static int gc2093_set_test_pattern(struct gc2093 *gc2093, int pattern)
{
	struct i2c_client *client = v4l2_get_subdevdata(&gc2093->subdev);
	int ret;

	ret = i2c_smbus_write_byte_data(client, REG_PAGE_SWITCH, REG_ENABLE);
	if (ret < 0)
		return ret;

	ret = i2c_smbus_write_byte_data(client, GC2093_REG_TEST_PATTERN,
					pattern);
	if (ret < 0)
		return ret;

	ret = i2c_smbus_write_byte_data(client, REG_GLOBAL_EFFECTIVE,
					REG_ENABLE);
	if (ret < 0)
		return ret;

	return i2c_smbus_write_byte_data(client, REG_SC_CTRL_MODE,
					 SC_CTRL_MODE_STREAMING);
}
#endif

static int gc2093_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct gc2093 *gc2093 = container_of(ctrl->handler,
					       struct gc2093, ctrl_handler);
	struct i2c_client *client = v4l2_get_subdevdata(&gc2093->subdev);
	s64 max_expo;
	u32 vts = 0;
	int ret = 0;

	/* Propagate change of current control to all related controls */
	if (ctrl->id == V4L2_CID_VBLANK) {
		/* Update max exposure while meeting expected vblanking */
		max_expo = gc2093->cur_mode->height + ctrl->val -
			   GC2093_EXPOSURE_MAX_MARGIN;
		__v4l2_ctrl_modify_range(gc2093->exposure,
					 gc2093->exposure->minimum, max_expo,
					 gc2093->exposure->step,
					 gc2093->exposure->default_value);
	}

	dev_info(&client->dev, "set ctrl begin\n");

	/* V4L2 controls values will be applied only when power is already up */
	if (!pm_runtime_get_if_in_use(&client->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		ret = gc2093_write_reg(gc2093, GC2093_REG_EXPOSURE_H, 1,
				       (ctrl->val >> 8) & 0x3f);
		ret |= gc2093_write_reg(gc2093, GC2093_REG_EXPOSURE_L, 1,
					ctrl->val & 0xff);
		dev_info(&client->dev, "set ctrl exposure ret [%d]\n",ret);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		ret = gc2093_set_gain(gc2093, ctrl->val);
		dev_info(&client->dev, "set ctrl gain ret [%d]\n",ret);
		break;
	case V4L2_CID_VBLANK:
		vts = gc2093->cur_mode->height + ctrl->val;
		ret = gc2093_write_reg(gc2093, GC2093_REG_VTS_H, 1,
				       (vts >> 8) & 0x3f);
		ret |= gc2093_write_reg(gc2093, GC2093_REG_VTS_L, 1,
					vts & 0xff);
		dev_info(&client->dev, "set ctrl vblank ret [%d]\n",ret);
		break;
	case V4L2_CID_TEST_PATTERN:
	//	ret = gc2093_set_test_pattern(gc2093, ctrl->val);
		dev_info(&client->dev, "set ctrl test patten \n");
		break;
	case V4L2_CID_PIXEL_RATE:
	case V4L2_CID_HBLANK:
		/* Read-only, but we adjust it based on mode. */
		break;
	default:
		dev_info(&client->dev, "set ctrl not support [%d] \n",ctrl->id);
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(&client->dev);

	return ret;
}

static const struct v4l2_subdev_video_ops gc2093_video_ops = {
	.s_stream = gc2093_s_stream,
};

static const struct v4l2_subdev_pad_ops gc2093_pad_ops = {
	.init_cfg = gc2093_entity_init_cfg,
	.enum_mbus_code = gc2093_enum_mbus_code,
	.enum_frame_size = gc2093_enum_frame_sizes,
	.get_fmt = gc2093_get_fmt,
	.set_fmt = gc2093_set_fmt,
};

static const struct v4l2_subdev_ops gc2093_subdev_ops = {
	.video	= &gc2093_video_ops,
	.pad	= &gc2093_pad_ops,
};

static const struct media_entity_operations gc2093_subdev_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static const struct v4l2_ctrl_ops gc2093_ctrl_ops = {
	.s_ctrl = gc2093_set_ctrl,
};

static int gc2093_initialize_controls(struct gc2093 *gc2093)
{
	struct i2c_client *client = v4l2_get_subdevdata(&gc2093->subdev);
	const struct gc2093_mode *mode;
	struct v4l2_ctrl_handler *handler;
	struct v4l2_ctrl *ctrl;
	s64 exposure_max;
	s64 exposure_def;
	s64 vblank_def;
	s64 pixel_rate;
	s64 h_blank;
	int ret;

	handler = &gc2093->ctrl_handler;
	mode = gc2093->cur_mode;
	ret = v4l2_ctrl_handler_init(handler, 7);
	if (ret)
	{
		dev_info(&client->dev,"end handler init 0 ret=[%d] \n",handler->error);
		return ret;
	}
		

	handler->lock = &gc2093->mutex;
	

	ctrl = v4l2_ctrl_new_int_menu(handler, &gc2093_ctrl_ops, V4L2_CID_LINK_FREQ, 0, 0,
				      link_freq_menu_items);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

    dev_info(&client->dev,"end link frequency ret=[%d] \n",handler->error);

	pixel_rate = to_pixel_rate(0);
	v4l2_ctrl_new_std(handler, NULL, V4L2_CID_PIXEL_RATE, 0, pixel_rate, 1, pixel_rate);
	//v4l2_ctrl_new_std(handler, &gc2093_ctrl_ops, V4L2_CID_PIXEL_RATE, pixel_rate, pixel_rate, 1, pixel_rate);
    dev_info(&client->dev,"end pixel rate ret=[%d] \n",handler->error);

	h_blank = mode->hts_def - mode->width;
	v4l2_ctrl_new_std(handler, &gc2093_ctrl_ops, V4L2_CID_HBLANK, h_blank, h_blank, 1,
			  h_blank);
	vblank_def = mode->vts_def - mode->height;
	v4l2_ctrl_new_std(handler, &gc2093_ctrl_ops, V4L2_CID_VBLANK,
			  vblank_def, GC2093_VTS_MAX - mode->height, 1,
			  vblank_def);
    dev_info(&client->dev,"end hblank vblank ret=[%d] \n",handler->error);

	exposure_max = mode->vts_def - 4;
	exposure_def = (exposure_max < mode->exp_def) ?
		exposure_max : mode->exp_def;
	gc2093->exposure = v4l2_ctrl_new_std(handler, &gc2093_ctrl_ops,
					      V4L2_CID_EXPOSURE,
					      GC2093_EXPOSURE_MIN,
						  exposure_max,
					      GC2093_EXPOSURE_STEP,
					      exposure_def);
    dev_info(&client->dev,"end exposure ret=[%d] \n",handler->error);

	v4l2_ctrl_new_std(handler, &gc2093_ctrl_ops,
			  V4L2_CID_ANALOGUE_GAIN, GC2093_GAIN_MIN,
			  GC2093_GAIN_MAX, GC2093_GAIN_STEP,
			  GC2093_GAIN_DEFAULT);
    dev_info(&client->dev,"end gain ret=[%d] \n",handler->error);

	v4l2_ctrl_new_std_menu_items(handler, &gc2093_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(gc2093_test_pattern_menu) - 1,
				     0, 0, gc2093_test_pattern_menu);
    dev_info(&client->dev,"end test patten ret=[%d] \n",handler->error);

	if (handler->error) {
		ret = handler->error;
		dev_err(&client->dev, "failed to init controls(%d)\n", ret);
		goto err_free_handler;
	}

	gc2093->subdev.ctrl_handler = handler;

	return 0;

err_free_handler:
	v4l2_ctrl_handler_free(handler);

	return ret;
}

static int gc2093_check_hwcfg(struct device *dev, struct gc2093 *gc2093)
{
	struct fwnode_handle *ep;
	struct fwnode_handle *fwnode = dev_fwnode(dev);
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	unsigned int i, j;
	//u32 clk_volt;
	int ret;

	if (!fwnode)
		return -EINVAL;

	ep = fwnode_graph_get_next_endpoint(fwnode, NULL);
	if (!ep)
		return -ENXIO;

	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
	fwnode_handle_put(ep);
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(link_freq_menu_items); i++) {
		for (j = 0; j < bus_cfg.nr_of_link_frequencies; j++) {
			if (link_freq_menu_items[i] ==
				bus_cfg.link_frequencies[j])
				break;
		}

		if (j == bus_cfg.nr_of_link_frequencies) {
			dev_err(dev, "no link frequency %lld supported\n",
				link_freq_menu_items[i]);
			ret = -EINVAL;
			break;
		}
	}

	v4l2_fwnode_endpoint_free(&bus_cfg);

	return ret;
}

static int gc2093_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct gc2093 *gc2093;
	unsigned int i;
	unsigned int rotation;
	int ret;

	gc2093 = devm_kzalloc(dev, sizeof(*gc2093), GFP_KERNEL);
	if (!gc2093)
		return -ENOMEM;

	ret = gc2093_check_hwcfg(dev, gc2093);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to check HW configuration\n");

	v4l2_i2c_subdev_init(&gc2093->subdev, client, &gc2093_subdev_ops);

	gc2093->fmt.code = MEDIA_BUS_FMT_SBGGR10_1X10;

	/* Optional indication of physical rotation of sensor */
	rotation = 0;
	device_property_read_u32(dev, "rotation", &rotation);
	if (rotation == 180) {
		gc2093->upside_down = true;
		gc2093->fmt.code = MEDIA_BUS_FMT_SRGGB10_1X10;
	}

	gc2093->eclk = devm_clk_get(dev, NULL);
	if (IS_ERR(gc2093->eclk))
		return dev_err_probe(dev, PTR_ERR(gc2093->eclk),
				     "failed to get eclk\n");

	if (clk_get_rate(gc2093->eclk) != GC2093_ECLK_FREQ)
		return dev_err_probe(dev, PTR_ERR(gc2093->eclk),"eclk mismatched, mode is based on 24MHz\n");


	// gc2093->pd_gpio = devm_gpiod_get(dev, "powerdown", GPIOD_OUT_HIGH);
	// if (IS_ERR(gc2093->pd_gpio))
	// 	return dev_err_probe(dev, PTR_ERR(gc2093->pd_gpio),
	// 			     "failed to get powerdown-gpios\n");

	gc2093->rst_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(gc2093->rst_gpio))
		return dev_err_probe(dev, PTR_ERR(gc2093->rst_gpio),
				     "failed to get reset-gpios\n");

	for (i = 0; i < ARRAY_SIZE(gc2093_supply_names); i++)
		gc2093->supplies[i].supply = gc2093_supply_names[i];

	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(gc2093_supply_names),
				      gc2093->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	mutex_init(&gc2093->mutex);

	/* Set default mode */
	gc2093->cur_mode = &supported_modes[0];

	ret = gc2093_initialize_controls(gc2093);
	if (ret) {
		dev_err_probe(dev, ret, "failed to initialize controls\n");
		goto err_destroy_mutex;
	}

	/* Initialize subdev */
	gc2093->subdev.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	gc2093->subdev.entity.ops = &gc2093_subdev_entity_ops;
	gc2093->subdev.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	gc2093->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&gc2093->subdev.entity, 1, &gc2093->pad);
	if (ret < 0) {
		dev_err_probe(dev, ret, "failed to initialize entity pads\n");
		goto err_free_handler;
	}

	pm_runtime_enable(dev);
	if (!pm_runtime_enabled(dev)) {
		ret = gc2093_power_on(dev);
		if (ret < 0) {
			dev_err_probe(dev, ret, "failed to power on\n");
			goto err_clean_entity;
		}
	}

	ret = v4l2_async_register_subdev(&gc2093->subdev);
	if (ret) {
		dev_err_probe(dev, ret, "failed to register V4L2 subdev\n");
		goto err_power_off;
	}

	return 0;

err_power_off:
	if (pm_runtime_enabled(dev))
		pm_runtime_disable(dev);
	else
		gc2093_power_off(dev);
err_clean_entity:
	media_entity_cleanup(&gc2093->subdev.entity);
err_free_handler:
	v4l2_ctrl_handler_free(gc2093->subdev.ctrl_handler);
err_destroy_mutex:
	mutex_destroy(&gc2093->mutex);

	return ret;
}

static void gc2093_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct gc2093 *gc2093 = to_gc2093(sd);

	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(sd->ctrl_handler);
	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		gc2093_power_off(&client->dev);
	pm_runtime_set_suspended(&client->dev);
	mutex_destroy(&gc2093->mutex);
}

static const struct of_device_id gc2093_of_match[] = {
	{ .compatible = "ovti,gc2093" },
	{}
};
MODULE_DEVICE_TABLE(of, gc2093_of_match);

static struct i2c_driver gc2093_i2c_driver = {
	.driver = {
		.name = "gc2093",
		.pm = &gc2093_pm_ops,
		.of_match_table = gc2093_of_match,
	},
	.probe		= gc2093_probe,
	.remove		= gc2093_remove,
};
module_i2c_driver(gc2093_i2c_driver);

//MODULE_AUTHOR("Dongchun Zhu <dongchun.zhu@mediatek.com>");
MODULE_AUTHOR("Emsr <shanggl@wo.cn>");
MODULE_DESCRIPTION("OmniVision GC2093 sensor driver");
MODULE_LICENSE("GPL v2");
