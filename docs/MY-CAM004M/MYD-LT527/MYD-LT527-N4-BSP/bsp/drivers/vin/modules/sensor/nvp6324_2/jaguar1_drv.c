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

#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/fcntl.h>
#include <linux/mm.h>
#include <linux/proc_fs.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <asm/uaccess.h>
#include <asm/io.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/string.h>
#include <linux/list.h>
#include <asm/delay.h>
#include <linux/timer.h>
#include <linux/delay.h>
#include <linux/proc_fs.h>
#include <linux/poll.h>
#include <asm/bitops.h>
#include <asm/uaccess.h>
#include <asm/irq.h>
#include <linux/moduleparam.h>
#include <linux/ioport.h>
#include <linux/interrupt.h>
#include <linux/semaphore.h>
#include <linux/kthread.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#include "jaguar1_common2.h"
#include "jaguar1_video2.h"
#include "jaguar1_coax_protocol2.h"
#include "jaguar1_motion2.h"
#include "jaguar1_ioctl2.h"
#include "jaguar1_video_eq2.h"
#include "jaguar1_mipi2.h"



#define I2C_0       (0)
#define I2C_1       (1)
#define I2C2       (2)
#define I2C_3       (3)

#define JAGUAR1_4PORT_R0_ID 0xB0
#define JAGUAR12PORT_R0_ID 0xA0
#define JAGUAR1_1PORT_R0_ID 0xA2
#define AFE_NVP6134E_R0_ID 	0x80

#define JAGUAR1_4PORT_REV_ID 0x00
#define JAGUAR12PORT_REV_ID 0x00
#define JAGUAR1_1PORT_REV_ID 0x00

static int chip_id2[4];
static int rev_id2[4];
static int jaguar1_cnt;
unsigned int jaguar1_i2c_addr2[4] = {0x60, 0x62, 0x64, 0x66};

struct semaphore jaguar1_lock;
struct i2c_client *jaguar1_client;
static struct i2c_board_info hi_info = {
	I2C_BOARD_INFO("jaguar1", 0x60),
};
decoder_get_information_str decoder_inform2;

static void vd_pattern_enable(void)
{
	gpio_i2c_write2(0x60, 0xFF, 0x00);
	gpio_i2c_write2(0x60, 0x1C, 0x1A);
	gpio_i2c_write2(0x60, 0x1D, 0x1A);
	gpio_i2c_write2(0x60, 0x1E, 0x1A);
	gpio_i2c_write2(0x60, 0x1F, 0x1A);

	gpio_i2c_write2(0x60, 0xFF, 0x05);
	gpio_i2c_write2(0x60, 0x6A, 0x80);
	gpio_i2c_write2(0x60, 0xFF, 0x06);
	gpio_i2c_write2(0x60, 0x6A, 0x80);
	gpio_i2c_write2(0x60, 0xFF, 0x07);
	gpio_i2c_write2(0x60, 0x6A, 0x80);
	gpio_i2c_write2(0x60, 0xFF, 0x08);
	gpio_i2c_write2(0x60, 0x6A, 0x80);
}

/*******************************************************************************
 *	Description		: Sample function - for select video format
 *	Argurments		: int dev_num(i2c_address array's num)
 *	Return value	: void
 *	Modify			:
 *	warning			:
 *******************************************************************************/
#ifdef FMT_SETTING_SAMPLE
static void set_default_video_fmt2(int dev_num)
{
	int i;
	video_input_init  video_val;

/*
	for (i = 0; i < 4 ; i++) {
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

/*******************************************************************************
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
		printk("[DRV || %s] ch%d / fmt:%d / input:%d / interface:%d\n", __func__
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
		video_val[i].interface = param->ch_param[i].interface;

		vd_jaguar1_init_set2(&video_val[i]);
		mipi_video_format_set2(&video_val[i]);
	}
	arb_init2(dev_num);
	disable_parallel2(dev_num);
	/* vd_pattern_enable(); */
}

/*******************************************************************************
 *	Description		: Check ID
 *	Argurments		: dec(slave address)
 *	Return value	: Device ID
 *	Modify			:
 *	warning			:
 *******************************************************************************/
static int check_id2(unsigned int dec)
{
	int ret;
	gpio_i2c_write2(dec, 0xFF, 0x00);
	ret = gpio_i2c_read2(dec, 0xf4);
	return ret;
}

/*******************************************************************************
 *	Description		: Get rev ID
 *	Argurments		: dec(slave address)
 *	Return value	: rev ID
 *	Modify			:
 *	warning			:
 *******************************************************************************/
static int (unsigned int dec)
{
	int ret;
	gpio_i2c_write2(dec, 0xFF, 0x00);
	ret = gpio_i2c_read2(dec, 0xf5);
	return ret;
}

/*******************************************************************************
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

	for (chip = 0; chip < 4; chip++) {
		chip_id2[chip] = check_id2(jaguar1_i2c_addr2[chip]);
		rev_id2[chip]  = (jaguar1_i2c_addr2[chip]);
		if ((chip_id2[chip] != JAGUAR1_4PORT_R0_ID)  	&&
				(chip_id2[chip] != JAGUAR12PORT_R0_ID) 		&&
				(chip_id2[chip] != JAGUAR1_1PORT_R0_ID)		&&
				(chip_id2[chip] != AFE_NVP6134E_R0_ID)
		  ) {
			printk("Device ID Error... %x, Chip Count:[%d]\n", chip_id2[chip], chip);
			jaguar1_i2c_addr2[chip] = 0xFF;
			chip_id2[chip] = 0xFF;
		} else {
			printk("Device (0x%x) ID OK... %x , Chip Count:[%d]\n", jaguar1_i2c_addr2[chip], chip_id2[chip], chip);
			printk("Device (0x%x) REV %x\n", jaguar1_i2c_addr2[chip], rev_id2[chip]);
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
	printk("Chip Count = %d\n", jaguar1_cnt);
	printk("Address [0x%x][0x%x][0x%x][0x%x]\n", jaguar1_i2c_addr2[0], jaguar1_i2c_addr2[1], jaguar1_i2c_addr2[2], jaguar1_i2c_addr2[3]);
	printk("Chip Id [0x%x][0x%x][0x%x][0x%x]\n", chip_id2[0], chip_id2[1], chip_id2[2], chip_id2[3]);
	printk("Rev Id  [0x%x][0x%x][0x%x][0x%x]\n", rev_id2[0], rev_id2[1], rev_id2[2], rev_id2[3]);

	for (i = 0; i < 4; i++) {
		decoder_inform2.chip_id2[i] = chip_id2[i];
		decoder_inform2.chip_rev[i] = rev_id2[i];
		decoder_inform2.chip_addr[i] = jaguar1_i2c_addr2[i];
	}
	decoder_inform2.Total_Chip_Cnt = jaguar1_cnt;
	ret = jaguar1_cnt;

	return ret;
}

/*******************************************************************************
 *	Description		: Video decoder initial
 *	Argurments		: void
 *	Return value	: void
 *	Modify			:
 *	warning			:
 *******************************************************************************/
void video_decoder_init2(void)
{
	int ii = 0;

	gpio_i2c_write2(jaguar1_i2c_addr2[0], 0xff, 0x04);

	for (ii = 0; ii < 36; ii++) {
		gpio_i2c_write2(jaguar1_i2c_addr2[0], 0xa0 + ii, 0x24);
	}

	gpio_i2c_write2(jaguar1_i2c_addr2[0], 0xff, 0x01);
	for (ii = 0; ii < 4; ii++) {
		gpio_i2c_write2(jaguar1_i2c_addr2[0], 0xcc + ii, 0x64);
	}

}
/*
/*******************************************************************************
 *	Description		: Driver open
 *	Argurments		:
 *	Return value	:
 *	Modify			:
 *	warning			:
 *******************************************************************************
int jaguar1_open(struct inode *inode, struct file *file)
{
	printk("[DRV] Jaguar1 Driver Open\n");
	printk("[DRV] Jaguar1 Driver Ver::%s\n", DRIVER_VER2);
	return 0;
}

/*******************************************************************************
 *	Description		: Driver close
 *	Argurments		:
 *	Return value	:
 *	Modify			:
 *	warning			:
 *******************************************************************************
int jaguar1_close(struct inode *inode, struct file *file)
{
	printk("[DRV] Jaguar1 Driver Close\n");
	return 0;
}

/*******************************************************************************
 *	Description		: Driver IOCTL function
 *	Argurments		:
 *	Return value	:
 *	Modify			:
 *	warning			:
 *******************************************************************************
long jaguar1_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	int cpy2usr_ret;
	unsigned int __user *argp = (unsigned int __user *)arg;

	/* AllVideo Variable
	video_init_all all_vd_val;

	/* Video Variable
	video_input_init  video_val;
	video_output_init vo_seq_set;
	video_equalizer_info_s video_eq;
	video_video_loss_s vidloss;

	/* Coaxial Protocol Variable
	NC_VD_COAX_STR2           coax_val;
	NC_VD_COAX_BANK_DUMP_STR2 coax_bank_dump;
	FIRMWARE_UP_FILE_INFO2    coax_fw_val;
	NC_VD_COAX_TEST_STR2      coax_test_val;

	/* Motion Variable
	motion_mode motion_set;

	down(&jaguar1_lock);

	switch (cmd) {
		/*===============================================================================================
		 * Set All - for MIPI Interface
		 *===============================================================================================
	case IOC_VDEC_INIT_ALL:
			if (copy_from_user(&all_vd_val, argp, sizeof(video_init_all)))
				printk("IOC_VDEC_INPUT_INIT error\n");
			vd_set_all(&all_vd_val);
			break;
		/*===============================================================================================
		 * Video Initialize
		 *===============================================================================================
	case IOC_VDEC_INPUT_INIT:
			if (copy_from_user(&video_val, argp, sizeof(video_input_init)))
				printk("IOC_VDEC_INPUT_INIT error\n");
			vd_jaguar1_init_set2(&video_val);
			break;
	case IOC_VDEC_OUTPUT_SEQ_SET:
			if (copy_from_user(&vo_seq_set, argp, sizeof(video_output_init)))
				printk("IOC_VDEC_INPUT_INIT error\n");
			vd_jaguar1_vo_ch_seq_set2(&vo_seq_set);
			break;
	case IOC_VDEC_VIDEO_EQ_SET:
			if (copy_from_user(&video_eq, argp, sizeof(video_equalizer_info_s)))
				printk("IOC_VDEC_INPUT_INIT error\n");
			video_input_eq_val_set2(&video_eq);
			break;
	case IOC_VDEC_VIDEO_SW_RESET:
			if (copy_from_user(&video_val, argp, sizeof(video_input_init)))
				printk("IOC_VDEC_INPUT_INIT error\n");
			vd_jaguar1_sw_reset2(&video_val);
			break;
	case IOC_VDEC_VIDEO_EQ_CABLE_SET:
			if (copy_from_user(&video_eq, argp, sizeof(video_equalizer_info_s)))
				printk("IOC_VDEC_INPUT_INIT error\n");
			video_input_eq_cable_set2(&video_eq);
			break;
	case IOC_VDEC_VIDEO_EQ_ANALOG_INPUT_SET:
			if (copy_from_user(&video_eq, argp, sizeof(video_equalizer_info_s)))
				printk("IOC_VDEC_INPUT_INIT error\n");
			video_input_eq_analog_input_set2(&video_eq);
			break;
	case IOC_VDEC_VIDEO_GET_VIDEO_LOSS:
			if (copy_from_user(&vidloss, argp, sizeof(video_video_loss_s)))
				printk("IOC_VDEC_VIDEO_GET_VIDEO_LOSS error\n");
			vd_jaguar1_get_novideo2(&vidloss);
			cpy2usr_ret = copy_to_user(argp, &vidloss, sizeof(video_video_loss_s));
			break;
			/*===============================================================================================
			 * Coaxial Protocol
			 *===============================================================================================
	case IOC_VDEC_COAX_TX_INIT:   /* SK_CHANGE 170703
			if (copy_from_user(&coax_val, argp, sizeof(NC_VD_COAX_STR2)))
				printk("IOC_VDEC_COAX_TX_INIT error\n");
			coax_tx_init2(&coax_val);
			break;
	case IOC_VDEC_COAX_TX_16BIT_INIT:   /* SK_CHANGE 170703
			if (copy_from_user(&coax_val, argp, sizeof(NC_VD_COAX_STR2)))
				printk("IOC_VDEC_COAX_TX_INIT error\n");
			coax_tx_16bit_init2(&coax_val);
			break;
	case IOC_VDEC_COAX_TX_CMD_SEND: /* SK_CHANGE 170703
			if (copy_from_user(&coax_val, argp, sizeof(NC_VD_COAX_STR2)))
				printk(" IOC_VDEC_COAX_TX_CMD_SEND error\n");
			coax_tx_cmd_send2(&coax_val);
			break;
	case IOC_VDEC_COAX_TX_16BIT_CMD_SEND: /* SK_CHANGE 170703
			if (copy_from_user(&coax_val, argp, sizeof(NC_VD_COAX_STR2)))
				printk(" IOC_VDEC_COAX_TX_CMD_SEND error\n");
			coax_tx_16bit_cmd_send2(&coax_val);
			break;
	case IOC_VDEC_COAX_TX_CVI_NEW_CMD_SEND: /* SK_CHANGE 170703
			if (copy_from_user(&coax_val, argp, sizeof(NC_VD_COAX_STR2)))
				printk(" IOC_VDEC_COAX_TX_CMD_SEND error\n");
			coax_tx_cvi_new_cmd_send2(&coax_val);
			break;
	case IOC_VDEC_COAX_RX_INIT:
			if (copy_from_user(&coax_val, argp, sizeof(NC_VD_COAX_STR2)))
				printk(" IOC_VDEC_COAX_RX_INIT error\n");
			coax_rx_init2(&coax_val);
			break;
	case IOC_VDEC_COAX_RX_DATA_READ:
			if (copy_from_user(&coax_val, argp, sizeof(NC_VD_COAX_STR2)))
				printk(" IOC_VDEC_COAX_RX_DATA_READ error\n");
			coax_rx_data_get2(&coax_val);
			cpy2usr_ret = copy_to_user(argp, &coax_val, sizeof(NC_VD_COAX_STR2));
			break;
	case IOC_VDEC_COAX_RX_BUF_CLEAR:
			if (copy_from_user(&coax_val, argp, sizeof(NC_VD_COAX_STR2)))
				printk(" IOC_VDEC_COAX_RX_BUF_CLEAR error\n");
			coax_rx_buffer_clear2(&coax_val);
			break;
	case IOC_VDEC_COAX_RX_DEINIT:
			if (copy_from_user(&coax_val, argp, sizeof(NC_VD_COAX_STR2)))
				printk("IOC_VDEC_COAX_RX_DEINIT error\n");
			coax_rx_deinit2(&coax_val);
			break;
	case IOC_VDEC_COAX_BANK_DUMP_GET:
			if (copy_from_user(&coax_bank_dump, argp, sizeof(NC_VD_COAX_BANK_DUMP_STR2)))
				printk("IOC_VDEC_COAX_BANK_DUMP_GET error\n");
			coax_test_Bank_dump_get2(&coax_bank_dump);
			cpy2usr_ret = copy_to_user(argp, &coax_bank_dump, sizeof(NC_VD_COAX_BANK_DUMP_STR2));
			break;
	case IOC_VDEC_COAX_RX_DETECTION_READ:
			if (copy_from_user(&coax_val, argp, sizeof(NC_VD_COAX_STR2)))
				printk(" IOC_VDEC_COAX_RX_DATA_READ error\n");
			coax_acp_rx_detect_get2(&coax_val);
			cpy2usr_ret = copy_to_user(argp, &coax_val, sizeof(NC_VD_COAX_STR2));
			break;
			/*===============================================================================================
			 * Coaxial Protocol. Function
			 *===============================================================================================
	case IOC_VDEC_COAX_RT_NRT_MODE_CHANGE_SET:
			if (copy_from_user(&coax_val, argp, sizeof(NC_VD_COAX_STR2)))
				printk(" IOC_VDEC_COAX_SHOT_SET error\n");
			coax_option_rt_nrt_mode_change_set(&coax_val);
			cpy2usr_ret = copy_to_user(argp, &coax_val, sizeof(NC_VD_COAX_STR2));
			break;
			/*===============================================================================================
			 * Coaxial Protocol FW Update
			 *===============================================================================================
	case IOC_VDEC_COAX_FW_ACP_HEADER_GET:
			if (copy_from_user(&coax_fw_val, argp, sizeof(FIRMWARE_UP_FILE_INFO2)))
				printk("IOC_VDEC_COAX_FW_READY_CMD_SET error\n");
			coax_fw_ready_header_check_from_isp_recv2(&coax_fw_val);
			cpy2usr_ret = copy_to_user(argp, &coax_fw_val, sizeof(FIRMWARE_UP_FILE_INFO2));
			break;
	case IOC_VDEC_COAX_FW_READY_CMD_SET:
			if (copy_from_user(&coax_fw_val, argp, sizeof(FIRMWARE_UP_FILE_INFO2)))
				printk("IOC_VDEC_COAX_FW_READY_CMD_SET error\n");
			coax_fw_ready_cmd_to_isp_send2(&coax_fw_val);
			cpy2usr_ret = copy_to_user(argp, &coax_fw_val, sizeof(FIRMWARE_UP_FILE_INFO2));
			break;
	case IOC_VDEC_COAX_FW_READY_ACK_GET:
			if (copy_from_user(&coax_fw_val, argp, sizeof(FIRMWARE_UP_FILE_INFO2)))
				printk("IOC_VDEC_COAX_FW_READY_ISP_STATUS_GET error\n");
			coax_fw_ready_cmd_ack_from_isp_recv2(&coax_fw_val);
			cpy2usr_ret = copy_to_user(argp, &coax_fw_val, sizeof(FIRMWARE_UP_FILE_INFO2));
			break;
	case IOC_VDEC_COAX_FW_START_CMD_SET:
			if (copy_from_user(&coax_fw_val, argp, sizeof(FIRMWARE_UP_FILE_INFO2)))
				printk("IOC_VDEC_COAX_FW_START_CMD_SET error\n");
			coax_fw_start_cmd_to_isp_send2(&coax_fw_val);
			cpy2usr_ret = copy_to_user(argp, &coax_fw_val, sizeof(FIRMWARE_UP_FILE_INFO2));
			break;
	case IOC_VDEC_COAX_FW_START_ACK_GET:
			if (copy_from_user(&coax_fw_val, argp, sizeof(FIRMWARE_UP_FILE_INFO2)))
				printk("IOC_VDEC_COAX_FW_START_CMD_SET error\n");
			coax_fw_start_cmd_ack_from_isp_recv2(&coax_fw_val);
			cpy2usr_ret = copy_to_user(argp, &coax_fw_val, sizeof(FIRMWARE_UP_FILE_INFO2));
			break;
	case IOC_VDEC_COAX_FW_SEND_DATA_SET:
			if (copy_from_user(&coax_fw_val, argp, sizeof(FIRMWARE_UP_FILE_INFO2)))
				printk("IOC_VDEC_COAX_FW_START_CMD_SET error\n");
			coax_fw_one_packet_data_to_isp_send2(&coax_fw_val);
			cpy2usr_ret = copy_to_user(argp, &coax_fw_val, sizeof(FIRMWARE_UP_FILE_INFO2));
			break;
	case IOC_VDEC_COAX_FW_SEND_ACK_GET:
			if (copy_from_user(&coax_fw_val, argp, sizeof(FIRMWARE_UP_FILE_INFO2)))
				printk("IOC_VDEC_COAX_FW_START_CMD_SET error\n");
			coax_fw_one_packet_data_ack_from_isp_recv2(&coax_fw_val);
			cpy2usr_ret = copy_to_user(argp, &coax_fw_val, sizeof(FIRMWARE_UP_FILE_INFO2));
			break;
	case IOC_VDEC_COAX_FW_END_CMD_SET:
			if (copy_from_user(&coax_fw_val, argp, sizeof(FIRMWARE_UP_FILE_INFO2)))
				printk("IOC_VDEC_COAX_FW_START_CMD_SET error\n");
			coax_fw_end_cmd_to_isp_send2(&coax_fw_val);
			cpy2usr_ret = copy_to_user(argp, &coax_fw_val, sizeof(FIRMWARE_UP_FILE_INFO2));
			break;
	case IOC_VDEC_COAX_FW_END_ACK_GET:
			if (copy_from_user(&coax_fw_val, argp, sizeof(FIRMWARE_UP_FILE_INFO2)))
				printk("IOC_VDEC_COAX_FW_START_CMD_SET error\n");
			coax_fw_end_cmd_ack_from_isp_recv2(&coax_fw_val);
			cpy2usr_ret = copy_to_user(argp, &coax_fw_val, sizeof(FIRMWARE_UP_FILE_INFO2));
			break;
			/*===============================================================================================
			 * Test Function
			 *===============================================================================================
	case IOC_VDEC_COAX_TEST_TX_INIT_DATA_READ:
			if (copy_from_user(&coax_test_val, argp, sizeof(NC_VD_COAX_TEST_STR2)))
				printk("IOC_VDEC_COAX_INIT_SET error\n");
			coax_test_tx_init_read2(&coax_test_val);
			cpy2usr_ret = copy_to_user(argp, &coax_test_val, sizeof(NC_VD_COAX_TEST_STR2));
			break;
	case IOC_VDEC_COAX_TEST_DATA_SET:
			if (copy_from_user(&coax_test_val, argp, sizeof(NC_VD_COAX_TEST_STR2)))
				printk("IOC_VDEC_COAX_TEST_DATA_SET error\n");
			coax_test_data_set(&coax_test_val);
			break;
	case IOC_VDEC_COAX_TEST_DATA_READ:
			if (copy_from_user(&coax_test_val, argp, sizeof(NC_VD_COAX_TEST_STR2)))
				printk("IOC_VDEC_COAX_TEST_DATA_SET error\n");
			coax_test_data_get2(&coax_test_val);
			cpy2usr_ret = copy_to_user(argp, &coax_test_val, sizeof(NC_VD_COAX_TEST_STR2));
			break;
			/*===============================================================================================
			 * Motion
			 *===============================================================================================
	case IOC_VDEC_MOTION_DETECTION_GET:
			if (copy_from_user(&motion_set, argp, sizeof(motion_set)))
				printk("IOC_VDEC_MOTION_SET error\n");
			motion_detection_get2(&motion_set);
			cpy2usr_ret = copy_to_user(argp, &motion_set, sizeof(motion_mode));
			break;
	case IOC_VDEC_MOTION_SET:
			if (copy_from_user(&motion_set, argp, sizeof(motion_set)))
				printk("IOC_VDEC_MOTION_SET error\n");
			motion_onoff_set2(&motion_set);
			break;
	case IOC_VDEC_MOTION_PIXEL_SET:
			if (copy_from_user(&motion_set, argp, sizeof(motion_set)))
				printk("IOC_VDEC_MOTION_Pixel_SET error\n");
			motion_pixel_onoff_set2(&motion_set);
			break;
	case IOC_VDEC_MOTION_PIXEL_GET:
			if (copy_from_user(&motion_set, argp, sizeof(motion_set)))
				printk("IOC_VDEC_MOTION_Pixel_SET error\n");
			motion_pixel_onoff_get2(&motion_set);
			cpy2usr_ret = copy_to_user(argp, &motion_set, sizeof(motion_mode));
			break;
	case IOC_VDEC_MOTION_ALL_PIXEL_SET:
			if (copy_from_user(&motion_set, argp, sizeof(motion_set)))
				printk("IOC_VDEC_MOTION_Pixel_SET error\n");
			motion_pixel_all_onoff_set2(&motion_set);
			break;
	case IOC_VDEC_MOTION_TSEN_SET:
			if (copy_from_user(&motion_set, argp, sizeof(motion_set)))
				printk("IOC_VDEC_MOTION_TSEN_SET error\n");
			motion_tsen_set2(&motion_set);
			break;
	case IOC_VDEC_MOTION_PSEN_SET:
			if (copy_from_user(&motion_set, argp, sizeof(motion_set)))
				printk("IOC_VDEC_MOTION_PSEN_SET error\n");
			motion_psen_set2(&motion_set);
			break;
	}

	up(&jaguar1_lock);

	return 0;
}
*/

/*******************************************************************************
 *	End of file
 *******************************************************************************/
