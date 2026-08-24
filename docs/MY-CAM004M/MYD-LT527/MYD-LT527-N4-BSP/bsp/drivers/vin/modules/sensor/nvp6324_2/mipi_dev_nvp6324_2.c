/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved. */
/*
 * A V4L2 driver for nvp6324 cameras and AHD Coax protocol.
 *
 * Copyright (c) 2017 by Allwinnertech Co., Ltd.  http://www.allwinnertech.com
 *
 * Authors:  Li Huiyu <lihuiyu@allwinnertech.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include "../../../utility/vin_log.h"
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/videodev2.h>
#include <media/v4l2-device.h>
#include <media/v4l2-mediabus.h>
#include <media/v4l2-subdev.h>
#include "type2.h"

#include "jaguar1_video2.h"
#include "jaguar1_mipi2.h"
//#include "jaguar1_common2.h"

#include "../camera.h"
#include "../sensor_helper.h"

#define JAGUAR1_4PORT_R0_ID 0xB0
#define JAGUAR12PORT_R0_ID 0xA0
#define JAGUAR1_1PORT_R0_ID 0xA2
#define AFE_NVP6134E_R0_ID 	0x80

#define JAGUAR1_4PORT_REV_ID 0x00
#define JAGUAR12PORT_REV_ID 0x00
#define JAGUAR1_1PORT_REV_ID 0x00



int chip_id2[4];
int rev_id2[4];
static int jaguar1_cnt;
unsigned int jaguar1_i2c_addr2[4] = {0x60, 0x62, 0x64, 0x66};
decoder_get_information_str decoder_inform2;
unsigned int bit82 = 1;
unsigned int chn2 = 4;


extern struct v4l2_subdev *nvp6324_sd2;

#define FMT_SETTING_SAMPLE
#define SENSOR_NAME "nvp6324_mipi_2"


u32 nvp6324_i2c_write2(u8 da, u8 reg, u8 val)
{
	u32 ret;

	ret = cci_write_a8_d8(nvp6324_sd2, reg, val);
	/* u_twi_wr_8_8(reg, val, da>>1,3); */

	return ret;
}

u32 nvp6324_i2c_read2(u8 da, u8 reg)
{
	u8 ret;
	u8 val = 0;
	ret = cci_read_a8_d8(nvp6324_sd2, reg, &val);
	/* ret = u_twi_rd_8_8(reg, &val, da>>1,3); */

	return val;
}

/*******************************************************************************
*	Description		: Get Device ID
*	Argurments		: dec(slave address)
*	Return value	: Device ID
*	Modify			:
*	warning			:
*******************************************************************************/
int check_id2(unsigned int dec)
{
	int ret;

	nvp6324_i2c_write2(dec, 0xFF, 0x00);
	ret = nvp6324_i2c_read2(dec, 0xf4);
	return ret;
}

/*******************************************************************************
*	Description		: Get rev ID
*	Argurments		: dec(slave address)
*	Return value	: rev ID
*	Modify			:
*	warning			:
*******************************************************************************/
int check_rev2(unsigned int dec)
{
	int ret;
	nvp6324_i2c_write2(dec, 0xFF, 0x00);
	ret = nvp6324_i2c_read2(dec, 0xf5);
	return ret;
}

static void vd_pattern_enable(void)
{
	nvp6324_i2c_write2(0x60, 0xFF, 0x00);
	nvp6324_i2c_write2(0x60, 0x1C, 0x1A);
	nvp6324_i2c_write2(0x60, 0x1D, 0x1A);
	nvp6324_i2c_write2(0x60, 0x1E, 0x1A);
	nvp6324_i2c_write2(0x60, 0x1F, 0x1A);

	nvp6324_i2c_write2(0x60, 0xFF, 0x05);
	nvp6324_i2c_write2(0x60, 0x6A, 0x80);
	nvp6324_i2c_write2(0x60, 0xFF, 0x06);
	nvp6324_i2c_write2(0x60, 0x6A, 0x80);
	nvp6324_i2c_write2(0x60, 0xFF, 0x07);
	nvp6324_i2c_write2(0x60, 0x6A, 0x80);
	nvp6324_i2c_write2(0x60, 0xFF, 0x08);
	nvp6324_i2c_write2(0x60, 0x6A, 0x80);
}


/******************************************************************************
*
 *	Description		: Check ID
 *	Argurments		: dec(slave address)
 *	Return value	: Device ID
 *	Modify			:
 *	warning			:

*******************************************************************************/
static void vd_set_all(video_init_all *param)
{
		int i, dev_num = 0;
		video_input_init  video_val[4];

/*
		for (i = 0; i < 4; i++) {
			sensor_dbg("[DRV || %s] ch%d / fmt:%d / input:%d / interface:%d\n", __func__
					, param->ch_param[i].ch
					, param->ch_param[i].format
					, param->ch_param[i].input
					, param->ch_param[i].interface);
		}
*/
		mipi_datatype_set2(VD_DATA_TYPE_YUV422);
		mipi_tx_init2(dev_num);

		for (i = 0; i < 4; i++) {
			video_val[i].ch = param->ch_param[i].ch;
			video_val[i].format = param->ch_param[i].format;
			video_val[i].input = param->ch_param[i].input;
			if (i < chn2)
				video_val[i].interface = param->ch_param[i].interface;
			else
				video_val[i].interface = DISABLE;

			vd_jaguar1_init_set2(&video_val[i]);
			mipi_video_format_set2(&video_val[i]);
#ifdef FOR_IMX6
			set_imx_video_format(&video_val[i]);
			if (video_val[i].interface == DISABLE) {
				sensor_dbg("[DRV] Nothing selected [video ch : %d]\n", i);
			} else {
				init_imx_mipi(i);
			}
#endif
		}
		arb_init2(dev_num);
		disable_parallel2(dev_num);
		vd_pattern_enable();

}


/*******************************************************************************
 *	Description		: Sample function - for select video format
 *	Argurments		: int dev_num(i2c_address array's num)
 *	Return value	: void
 *	Modify			:
 *	warning			:
 *******************************************************************************/
#ifdef FMT_SETTING_SAMPLE
void set_default_video_fmt2(int dev_num)
{

/*
	int i;
	video_input_init  video_val;

	for (i = 0; i < 4; i++) {
		video_val.ch = i;

		/* select video format, include struct'vd_vi_init_list2' in jaguar1_video_table.h
		 *  ex > AHD20_1080P_30P2 / AHD20_720P25P2_EX_Btype / AHD20_SD_H9602EX_Btype_NT2
		video_val.format = AHD20_720P_30P2_EX_Btype;

		/* select analog input type, SINGLE_ENDED2 or DIFFERENTIAL2
		video_val.input = SINGLE_ENDED2;

		/* select decoder to soc interface
		video_val.interface = MIPI;

		/* run video setting
		vd_jaguar1_init_set2(&video_val);

		/* run video format setting for mipi/arbiter
		mipi_video_format_set2(&video_val);
	}
	arb_init2(dev_num);
	disable_parallel2(dev_num);
*/
/*
	video_val.ch = 0;
	video_val.format = AHD20_1080P_30P2;
	video_val.input = SINGLE_ENDED2;
	vd_jaguar1_init_set2(&video_val);

	mipi_video_format_set2(&video_val);

	arb_init2(dev_num);
*/
}
#endif


/******************************************************************************
*
 *	Description		: Check decoder count
 *	Argurments		: void
 *	Return value	: (total chip count - 1) or -1(not found any chip)
 *	Modify			:
 *	warning			:

*******************************************************************************/
int check_decoder_count2(void)
{
	int chip, i;
	int ret = -1;

	jaguar1_cnt = 0;
	for (chip = 0; chip < 4; chip++) {
		chip_id2[chip] = check_id2(jaguar1_i2c_addr2[chip]);
		rev_id2[chip]  = check_rev2(jaguar1_i2c_addr2[chip]);
		if ((chip_id2[chip] != JAGUAR1_4PORT_R0_ID)  	&&
				(chip_id2[chip] != JAGUAR12PORT_R0_ID) 		&&
				(chip_id2[chip] != JAGUAR1_1PORT_R0_ID)		&&
				(chip_id2[chip] != AFE_NVP6134E_R0_ID)
		  ) {
			sensor_err("Device ID Error... %x, Chip Count:[%d]\n", chip_id2[chip], chip);
			jaguar1_i2c_addr2[chip] = 0xFF;
			chip_id2[chip] = 0xFF;
		} else {
			sensor_dbg("Device (0x%x) ID OK... %x , Chip Count:[%d]\n",
				jaguar1_i2c_addr2[chip], chip_id2[chip], chip);
			sensor_dbg("Device (0x%x) REV %x\n", jaguar1_i2c_addr2[chip], rev_id2[chip]);
				jaguar1_i2c_addr2[jaguar1_cnt] = jaguar1_i2c_addr2[chip];

			if (jaguar1_cnt < chip) {
				jaguar1_i2c_addr2[chip] = 0xFF;
			}

			chip_id2[jaguar1_cnt] = chip_id2[chip];
			rev_id2[jaguar1_cnt]  = rev_id2[chip];

			jaguar1_cnt++;
		}

		if ((chip == 3) && (jaguar1_cnt < chip)) {
			for (i = jaguar1_cnt; i < 4; i++) {
				chip_id2[i] = 0xff;
				rev_id2[i]  = 0xff;
			}
		}
	}
	sensor_dbg("Chip Count = %d\n", jaguar1_cnt);
	sensor_dbg("Address [0x%x][0x%x][0x%x][0x%x]\n", jaguar1_i2c_addr2[0], jaguar1_i2c_addr2[1], jaguar1_i2c_addr2[2], jaguar1_i2c_addr2[3]);
	sensor_dbg("Chip Id [0x%x][0x%x][0x%x][0x%x]\n", chip_id2[0], chip_id2[1], chip_id2[2], chip_id2[3]);
	sensor_dbg("Rev Id  [0x%x][0x%x][0x%x][0x%x]\n", rev_id2[0], rev_id2[1], rev_id2[2], rev_id2[3]);

	for (i = 0; i < 4; i++) {
		decoder_inform2.chip_id[i] = chip_id2[i];
		decoder_inform2.chip_rev[i] = rev_id2[i];
		decoder_inform2.chip_addr[i] = jaguar1_i2c_addr2[i];
	}
	decoder_inform2.Total_Chip_Cnt = jaguar1_cnt;
	ret = jaguar1_cnt;

	return ret;
}

/******************************************************************************
*
 *	Description		: Video decoder initial
 *	Argurments		: void
 *	Return value	: void
 *	Modify			:
 *	warning			:

*******************************************************************************/
void video_decoder_init2(void)
{
		int ii = 0;

		nvp6324_i2c_write2(jaguar1_i2c_addr2[0], 0xff, 0x04);

		for (ii = 0; ii < 36; ii++) {
			nvp6324_i2c_write2(jaguar1_i2c_addr2[0], 0xa0 + ii, 0x24);
		}

		nvp6324_i2c_write2(jaguar1_i2c_addr2[0], 0xff, 0x01);
		for (ii = 0; ii < 4; ii++) {
			nvp6324_i2c_write2(jaguar1_i2c_addr2[0], 0xcc + ii, 0x64);
		}
#if 1
		nvp6324_i2c_write2(jaguar1_i2c_addr2[0], 0xff, 0x21);
		nvp6324_i2c_write2(jaguar1_i2c_addr2[0], 0x07, 0x80);
		nvp6324_i2c_write2(jaguar1_i2c_addr2[0], 0x07, 0x00);
#endif

#if 1
		nvp6324_i2c_write2(jaguar1_i2c_addr2[0], 0xff, 0x0A);
		nvp6324_i2c_write2(jaguar1_i2c_addr2[0], 0x77, 0x8F);
		nvp6324_i2c_write2(jaguar1_i2c_addr2[0], 0xF7, 0x8F);
		nvp6324_i2c_write2(jaguar1_i2c_addr2[0], 0xff, 0x0B);
		nvp6324_i2c_write2(jaguar1_i2c_addr2[0], 0x77, 0x8F);
		nvp6324_i2c_write2(jaguar1_i2c_addr2[0], 0xF7, 0x8F);
#endif

}


/* For 0, 1, 5 */
void nvp6324_dump_bank2(int bank)
{
	int i = 0;
	u32 ret = 0;
	printk("\n----------------- Bank%d Start ---------------------\n", bank);
	nvp6324_i2c_write2(jaguar1_i2c_addr2[0], 0xFF, bank);
	for (i = 0; i < 0xF6; i++) {

		if (i == 0 || i % 16 == 0)
			printk("0x%02x-0x%02x: ", i, i + 15);
		ret = nvp6324_i2c_read2(jaguar1_i2c_addr2[0], i);
		printk("0x%02x ", ret);
		if ((i > 0) && ((i + 1) % 16) == 0)
			printk("\n");
	}
	printk("\n\t\t----------------- Bank%d End  ---------------------\n",
	       bank);
}
EXPORT_SYMBOL(nvp6324_dump_bank2);

void nvp6324_read_bank_value2(void)
{

	u8 ret = 0;
	u8 i = 0;

	nvp6324_i2c_write2(jaguar1_i2c_addr2[0], 0xFF, 0x00);
	for (i = 0; i < 0xF6; i++) {
		ret = nvp6324_i2c_read2(jaguar1_i2c_addr2[0], i);
		printk("Bank0[0x%2.2x] = 0x%2.2x \n", i, ret);
	}
	printk("\n");

	nvp6324_i2c_write2(jaguar1_i2c_addr2[0], 0xFF, 0x01);
	for (i = 0; i < 0xF6; i++) {
		ret = nvp6324_i2c_read2(jaguar1_i2c_addr2[0], i);
		printk("Bank1[0x%2.2x] = 0x%2.2x \n", i, ret);
	}
	printk("\n");

	nvp6324_i2c_write2(jaguar1_i2c_addr2[0], 0xFF, 0x05);
	for (i = 0; i < 0xF0; i++) {
		ret = nvp6324_i2c_read2(jaguar1_i2c_addr2[0], i);
		printk("Bank5[0x%2.2x] = 0x%2.2x \n", i, ret);
	}
}

int  nvp6324_init2(int Format)
{
	int ret = 0;
	int ch = 0;
	video_init_all sVideoall;

	/* decoder count function */
	ret = check_decoder_count2();
	if (ret == -1) {
		sensor_err("ERROR: could not find jaguar1 devices:%#x \n", ret);
		return ret;
	} else {
		sensor_dbg("check_decoder_count2 ok! ret=%d\n", ret);
	}

	video_decoder_init2();
#ifdef FMT_SETTING_SAMPLE
		for (ch = 0; ch < jaguar1_cnt; ch++) {
			sVideoall.ch_param[ch].ch = ch;
			sVideoall.ch_param[ch].format = Format;
			sVideoall.ch_param[ch].input = SINGLE_ENDED;
			sVideoall.ch_param[ch].interface = YUV_422;
		}
	vd_set_all(&sVideoall);
#endif

	return 0;
}
