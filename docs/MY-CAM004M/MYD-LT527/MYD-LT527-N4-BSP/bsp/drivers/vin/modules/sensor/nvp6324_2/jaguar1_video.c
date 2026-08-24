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
#include <linux/string.h>
#include <linux/delay.h>
#include "jaguar1_common2.h"
#include "jaguar1_video2.h"
#include "jaguar1_video_eq2.h"
#include "jaguar1_video_table2.h"
#include "jaguar1_coax_protocol2.h"
#include "jaguar1_reg_set_def2.h"
#include "../sensor_helper.h"

#define SENSOR_NAME "nvp6324_mipi_2"

static unsigned char cur_bank = 0xff;
static int print_flag;

extern unsigned int bit8;

/**************************************************************************************
 * Jaguar1 Video Input initialize value get from table
 ***************************************************************************************/
NC_VD_VI_Init_STR2 *__NC_VD_VI_Init_Val_Get2(NC_VIVO_CH_FORMATDEF2 def)
{
	NC_VD_VI_Init_STR2 *pRet = &vd_vi_init_list2[def];
	if (pRet == NULL) {
		sensor_dbg("[DRV]vd_vi_init_list2 Not Supported format Yet!!!(%d)\n", def);
	}
	return  pRet;
}

NC_VD_VO_Init_STR2 *__NC_VD_VO_Init_Val_Get2(NC_VIVO_CH_FORMATDEF2 def)
{
	NC_VD_VO_Init_STR2 *pRet = &vd_vo_init_list2[def];
	if (pRet == NULL) {
		sensor_dbg("[DRV]vd_vo_init_list2 Not Supported format Yet!!!(%d)\n", def);
	}
	return  pRet;
}

/**************************************************************************************
 * Jaguar1 Register Setting Function
 *
 *
 ***************************************************************************************/
void reg_val_print_flag_set2(int set)
{
	print_flag = set;
}

int reg_val_print_flag_get2(void)
{
	return print_flag;
}

void current_bank_set2(unsigned char bank)
{
	cur_bank = bank;
}

unsigned char current_bank_get2(void)
{
	return cur_bank;
}

void vd_register_set2(int dev, unsigned char bank, unsigned char addr, unsigned char val, int pos, int size)
{
	unsigned char ReadVal = 0x00;
	unsigned char Mask = 0x00;
	unsigned char rstbit = 0x01;
	unsigned char WriteVal = val;
	unsigned char cur_bank = 0x00;
	int ii = 0;

	if (8 < (pos + size)) {
		sensor_dbg("vd_register_set2 Error!!dev[%d] Bank[0x%02X] Addr[0x%02X] pos[%d] size[%d]\n", dev, bank, addr, pos, size);
	}

	cur_bank = current_bank_get2();
	if (cur_bank != bank) {
		JAGUAR1_BANK_CHANGE2(bank);
		current_bank_set2(bank);
	}

	if (!(pos == 0 && size == 8)) {
		for (ii = 0; ii < size; ii++) {
			Mask = Mask|(rstbit<<(pos+ii));
		}
		Mask = ~Mask;
		WriteVal = WriteVal<<pos;

		ReadVal = gpio_i2c_read2(jaguar1_i2c_addr2[dev], addr);
		ReadVal = ReadVal & Mask;
		WriteVal = WriteVal | ReadVal;
	}

	gpio_i2c_write2(jaguar1_i2c_addr2[dev], addr, WriteVal);

	if (reg_val_print_flag_get2())
		sensor_dbg("[DRV]%Xx%02X > 0x%02X\n", current_bank_get2(), addr, WriteVal);

}

/**************************************************************************************
 * Jaguar1 Video Input Setting Function
 *
 *
 ***************************************************************************************/
void vd_vi_manual_set_seq12(unsigned char dev, unsigned char ch, void *p_param)
{
	/*====================================================================
	 * Bank 1x7c
	 *|   7   |   6   |   5   |    4   |   3        |   2        |   1        |   0        |
	 *|       |       |       |        | CLK_AUTO_4 | CLK_AUTO_3 | CLK_AUTO2 | CLK_AUTO_1 |
	 *====================================================================*/
	/*====================================================================
	 * Bank 0x14
	 *|   7   |   6   |   5   |    4      |   3   |   2  |   1  |   0  |
	 *|       |       |       | FLD_INV_x |         CHID_VIN_x         |
	 *====================================================================*/
	/*====================================================================
	 * Bank 0x14
	 *|   7   |   6   |   5   |    4      |   3   |   2  |   1  |   0  |
	 *|       |       |       | FLD_INV_x |         CHID_VIN_x         |
	 *====================================================================*/
	/*====================================================================
	 * Bank 5x32
	 *|   7   |   6   |   5   |    4   |   3  |   2  |   1   |   0   |
	 *|       |       |  FLD_DET_MODE  |      |      |   NOVID_DET_A |
	 *====================================================================*/
	/*====================================================================
	 * Bank 13x30 ~ 33  - SK_ing
	 *|   7   |   6   |   5   |   4   |   3   |   2   |   1   |   0   |
	 *|       |       |det_en |det_en |det_en |det_en |det_en |det_en |
	 *====================================================================*/
	/*====================================================================
	 * Bank 9x44
	 *|   7   |   6   |   5   |   4   |   3   |   2   |   1   |   0   		|
	 *|       |       | 	  |		  |		  |		  |		  |FSC_EXT_EN_1 |
	 *====================================================================*/
	NC_VD_VI_Init_STR2 *param = (NC_VD_VI_Init_STR2 *)p_param;
	unsigned char val_13x30;
	unsigned char val_13x31;
	unsigned char val_13x32;

	if (ch == 0)
		REG_SET_1x7C_0_1_clk_auto_12(ch, 0x0);
	else if (ch == 1)
		REG_SET_1x7C_1_1_clk_auto22(ch, 0x0);
	else if (ch == 2)
		REG_SET_1x7C2_1_clk_auto_32(ch, 0x0);
	else if (ch == 3)
		REG_SET_1x7C_3_1_clk_auto_42(ch, 0x0);
	else
		printk("[DRV]Clock Auto Set Fail!!:: %x\n", ch);

	REG_SET_5x32_0_8_NOVIDEO_DET_A2(ch, 0x10);
	REG_SET_5xB9_0_8_HAFC_LPF_SEL2(ch, 0xb2);

	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0xFF, 0x13);
	val_13x30 = gpio_i2c_read2(jaguar1_i2c_addr2[dev], 0x30);
	val_13x31 = gpio_i2c_read2(jaguar1_i2c_addr2[dev], 0x31);
	val_13x32 = gpio_i2c_read2(jaguar1_i2c_addr2[dev], 0x32);

	val_13x30 &= (~(1 << (ch + 4)) & (~(1 << ch)));
	val_13x31 &= (~(1 << (ch + 4)) & (~(1 << ch)));
	val_13x32 &= (~(1 << ch));

	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x30, val_13x30);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x31, val_13x31);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x32, val_13x32);

	REG_SET_9x44_0_8_FSC_EXT_EN22(ch, 0x00);
	REG_SET_5x6E_0_8_VBLK_END_SEL2(ch, param->vblk_end_sel);
	REG_SET_5x6F_0_8_VBLK_END_EXT2(ch, param->vblk_end_ext);

}

void vd_vi_vafe_set_seq22(unsigned char dev, unsigned char ch)
{
	REG_SET_5x00_0_8_A_CMP_PW_MODE2(ch, 0xd0);
	REG_SET_5x02_0_8_A_CMP_TIMEUNIT2(ch, 0x0c);
	REG_SET_5x1E_0_8_VAFEMD2(ch, 0x00);
	REG_SET_5x58_0_8_VAFE1_EQ_BAND_SEL2(ch, 0x00);
	REG_SET_5x59_0_8_LPF_BYPASS2(ch, 0x00);
	REG_SET_5x5A_0_8_VAFE_IMP_CNT2(ch, 0x00);
	REG_SET_5x5B_0_8_VAFE_DUTY2(ch, 0x41);
	REG_SET_5x5C_0_8_VAFE_B_LPF_SEL2(ch, 0x78);
	REG_SET_5x94_0_8_PWM_DELAY_H2(ch, 0x00);
	REG_SET_5x95_0_8_PWM_DELAY_L2(ch, 0x00);
	REG_SET_5x65_0_8_VAFE_CML_SPEED2(ch, 0x80);

}

void vd_vi_format_set_seq32(unsigned char dev, unsigned char ch, void *p_param)
{
	/*============================================================================================
	 * Bank 0x10
	 *|   7   |   6   |   5   |   4   |   3  |  2  |   1  |  0  |
	 *|       |   BSF_MODE_1  |           VIDEO_FORMAT_1        |
	 *============================================================================================*/
	/*============================================================================================
	 * Bank 0x0c
	 *|   7   |   6   |   5   |   4   |   3  |  2  |   1  |  0  |
	 *|       |       |       |       |     SPECIAL_MODE        |
	 *============================================================================================*/
	/*============================================================================================
	 * Bank 0x04
	 *|   7   |   6   |   5   |   4   |   3  |  2  |   1  |  0  |
	 *|       |       |       |       |           SD_MD         |
	 *============================================================================================*/
	/*============================================================================================
	 * Bank 0x08
	 *|   7   |   6   |   5   |   4   |   3  |  2  |   1  |  0  |
	 *|       |       |       |       |           AHD_MD        |
	 *============================================================================================*/
	/*============================================================================================
	 * Bank 5x69
	 *|   7          |   6   |         5         |   4    |   3  |  2  |   1  |      0      |
	 *| NO_VIDEO_OFF |       | OUTPUT PATTERN_ON | MEM_EN |      |     |      | SD_FREQ_SEL |
	 *============================================================================================*/
	NC_VD_VI_Init_STR2 *param = (NC_VD_VI_Init_STR2 *)p_param;

	if (ch > 3) {
		printk("[DRV] %s CHID Error\n", __func__);
		return;
	}

	REG_SET_0x10_0_8_VD_FMT2(ch, param->video_format);
	REG_SET_0x0C_0_8_SPL_MODE2(ch, param->spl_mode);
	REG_SET_0x04_0_8_SD_MODE2(ch, param->sd_mode);
	REG_SET_0x08_0_8_AHD_MODE2(ch, param->ahd_mode);
	REG_SET_5x69_0_1_SD_FREQ_SEL2(ch, param->sd_freq_sel);
	REG_SET_5x62_0_8_SYNC_SEL2(ch, param->sync_sel);

}

void vd_vi_chroma_set_seq42(unsigned char dev, unsigned char ch, void *p_param)
{
	/*============================================================================================
	 * Bank 0x5c
	 *|   7        |   6   |   5   |     4    |  3  |  2  |   1  |  0  |
	 *| PAL_CM_OFF |       |       | COLOROFF |           C_KILL       |
	 *============================================================================================*/
	/*============================================================================================
	 * Bank 5x28
	 *|      7        |    6    |   5    |    4   |  3  |  2  |   1  |  0  |
	 *| CTI_CORE_MODE | S_POINT |   CTI_DELAY_SEL |     |     |      |     |
	 *============================================================================================*/
	/*============================================================================================
	 * Bank 5x25
	 *|  7  |  6  |  5  |  4  |  3  |  2  |   1  |  0  |
	 *|      FSC_LOCK_MODE    |      FSC_LOCK_SPD      |
	 *============================================================================================*/
	/*============================================================================================
	 * Bank 5x90
	 *|     7      |  6  |   5   |   4   |  3  |  2  |  1  |  0  |
	 *| C_LH_SEL_1 |     |    YL_SEL_1   |      COMB_MODE_1      |
	 *============================================================================================*/
	NC_VD_VI_Init_STR2 *param = (NC_VD_VI_Init_STR2 *)p_param;

	if (ch > 3) {
		printk("[DRV] %s CHID Error\n", __func__);
		return;
	}

	REG_SET_0x5C_0_8_PAL_CM_OFF2(ch, param->pal_cm_off);
	REG_SET_5x28_0_8_S_POINT2(ch, param->s_point);
	REG_SET_5x25_0_8_FSC_LOCK_MODE2(ch, param->fsc_lock_mode);
	REG_SET_5x90_0_8_COMB_MODE2(ch, param->comb_mode);

}

void vd_vi_h_timing_set_seq52(unsigned char dev, unsigned char ch, void *p_param)
{
	/*============================================================================================
	 * Bank 0x68
	 *|  7  |  6  |  5  |  4  |  3  |  2  |   1  |  0  |
	 *|                     H_DELAY                    |
	 *============================================================================================*/
	/*============================================================================================
	 * Bank 0x60
	 *|  7  |  6  |  5  |  4  |  3  |  2  |   1  |  0  |
	 *|                 |            Y_DELAY           |
	 *============================================================================================*/
	/*============================================================================================
	 * Bank 0x78
	 *|  7  |  6  |  5  |  4  |  3  |  2  |   1  |  0  |
	 *|                      HBLK_END                  |
	 *============================================================================================*/
	/*============================================================================================
	 * Bank 5x38
	 *|  7  |  6  |  5  |    4    |  3   |   2  |    1  |  0   |
	 *|                 | MASK_ON | MASK_SEL1 (Bank0 0x8E[3:0) |
	 *============================================================================================*/
	/*============================================================================================
	 * Bank 0x64
	 *|  7  |  6  |  5  |  4  |  3  |  2  |  1  |  0  |
	 *|       DF_CDELAY       |       DF_YDELAY       |
	 *============================================================================================*/
	/*============================================================================================
	 * Bank 0x14
	 *|  7  |  6  |  5  |  4      |  3  |  2  |  1  |  0  |
	 *|                 | FLD_INV |       CHID_VIN        |
	 *============================================================================================*/
	/*============================================================================================
	 * Bank 5x64
	 *|  7  |  6  |  5  |  4  |  3  |  2  |  1  |  0  |
	 *|     |     |     |     |       MEM_RDP_01      |
	 *============================================================================================*/
	/*============================================================================================
	 * Bank 5x47
	 *|  7  |  6  |  5  |  4  |  3  |  2  |  1  |  0  |
	 *|                 CONTROL_MODES                 |
	 *============================================================================================*/
	/*============================================================================================
	 * Bank 5xa9
	 *|  7                    |  6  |  5  |  4  |  3  |  2  |  1  |  0  |
	 *| SIGNED_ADV_STP_DELAY1 |             ADV_STP_DELAY1              |
	 *============================================================================================*/
	NC_VD_VI_Init_STR2 *param = (NC_VD_VI_Init_STR2 *)p_param;

	if (ch > 3) {
		sensor_dbg("[DRV] %s CHID Error\n", __func__);
		return;
	}

	REG_SET_0x68_0_8_H_DLY_LSB2(ch, param->h_delay_lsb);
	REG_SET_0x6c_0_8_H_DLY_MSB2(ch, param->h_dly_msb);
	REG_SET_0x60_0_8_Y_DLY2(ch, param->y_delay);
	REG_SET_0x78_0_8_V_BLK_END_A2(ch, param->v_blk_end_a);

	REG_SET_5x38_4_1_H_MASK_ON2(ch, param->h_mask_on);
	REG_SET_5x38_0_4_H_MASK_SEL2(ch, param->h_mask_sel);

	REG_SET_0x64_0_8_V_BLK_END_B2(ch, param->v_blk_end_b);
	REG_SET_0x14_4_1_FLD_INV2(ch, param->fld_inv);

	REG_SET_5x64_0_8_MEM_RDP2(ch, param->mem_rdp);
	REG_SET_5x47_0_8_SYNC_RS2(ch, param->sync_rs);
	REG_SET_5xA9_0_8_V_BLK_END_B2(ch, param->v_blk_end_b);

}

void vd_vi_h_scaler_mode_set_seq62(unsigned char dev, unsigned char ch, void *p_param)
{
	/*============================================================================================
	 * Bank 5x53
	 *|  7  |  6  |  5             |  4         |  3  |  2   |  1  |  0          |
	 *|     |     | PROTECTION_OFF | BT_601_SEL | LINEMEM_MD |     | C_DITHER_ON |
	 *============================================================================================*/
	/*============================================================================================
	 * Bank 9x96
	 *|  7  |  6  |  5  |  4                    |  3  |  2  |  1                   |  0                  |
	 *|     |     |     | CH1_H_DOWN_SCALER_EN  |     |     | CH1_H_SCALER_TRS_SEL | CH1_H_SCALER_ENABLE |
	 *============================================================================================*/
	/*============================================================================================
	 * Bank 9x97
	 *|  7  |  6  |  5  |  4   |  3       |  2        |  1                      |  0                |
	 *|     CH1_H_SCALER_MODE  | CH1_H_SCALER_RD_MODE | CH1_H_SCALER_AUTO_H_REF | CH1_H_SCALER_AUTO |
	 *============================================================================================*/
	/*============================================================================================
	 * Bank 9x98
	 *|  7  |  6  |  5  |  4  |  3  |  2  |  1  |  0  |
	 *|          CH1_H_SCALER_H_REF_BASE[7:0]         |
	 * Bank 9x99
	 *|  7  |  6  |  5  |  4  |  3  |  2  |  1  |  0  |
	 *|          CH1_H_SCALER_H_REF_BASE[15:8]        |
	 *============================================================================================*/

	NC_VD_VI_Init_STR2 *param = (NC_VD_VI_Init_STR2 *)p_param;

	if (ch > 3) {
		sensor_dbg("[DRV] %s CHID Error\n", __func__);
		return;
	}

	REG_SET_5x5322_LINEMEM_MD2(ch, param->line_mem_mode);

	REG_SET_9x96_0_8_H_DOWN_SCALER2(ch, param->h_down_scaler);
	REG_SET_9x97_0_8_H_SCALER_MODE2(ch, param->h_scaler_mode);
	REG_SET_9x98_0_8_REF_BASE_LSB2(ch, param->ref_base_lsb);
	REG_SET_9x99_0_8_REF_BASE_MSB2(ch, param->ref_base_msb);
	REG_SET_9x9E_0_8_H_SCALER_OUTPUT_H_ACTIVE2(ch, param->h_scaler_active);
}

void vd_vi_hpll_set_seq72(unsigned char dev, unsigned char ch, void *p_param)
{
	/*============================================================================================
	 * Bank 5x50
	 *|  7  |  6               |  5  |  4                |  3      |  2       |  1           |  0       |
	 *|     | NCO_GDF_COEFF_IV |     | NCO_GDF_COEFF_OFF | Y_TEMP_SEL(5T,15T) | HPLL_MASK_ON | CONT_SUB |
	 *============================================================================================*/
	/*============================================================================================
	 * Bank 5xb8
	 *|  7          |  6  |  5  |  4  |  3  |  2  |  1  |  0  |
	 *| HAFC_BYPASS | HAFC_HCOEFF_SEL |       HAFC_OP_MD      |
	 *============================================================================================*/
	/*============================================================================================
	 * Bank 5xbb
	 *|  7  |  6  |  5  |  4  |  3  |  2  |  1  |  0  |
	 *|                 HPLL_MASK_END                 |
	 *============================================================================================*/
	/*============================================================================================
	 * Bank 5xbb
	 *|  7  |  6  |  5  |  4  |  3  |  2  |  1  |  0  |
	 *|            HAFC_BYP_TH_S(write)               |
	 *============================================================================================*/
	NC_VD_VI_Init_STR2 *param = (NC_VD_VI_Init_STR2 *)p_param;

	if (ch > 3) {
		sensor_dbg("[DRV] %s CHID Error\n", __func__);
		return;
	}

	REG_SET_5x50_0_8_HPLL_MASK_ON2(ch, param->hpll_mask_on);
	REG_SET_5xB8_0_8_HAFC_OP_MD2(ch, param->hafc_op_md);
	REG_SET_5xBB_0_8_HAFC_BYP_TH_E2(ch, param->hafc_byp_th_e);
	REG_SET_5xB7_0_8_HAFC_BYP_TH_S2(ch, param->hafc_byp_th_s);

}

void vd_vi_color_set_seq82(unsigned char dev, unsigned char ch, void *p_param, NC_VIVO_CH_FORMATDEF2 fmt)
{
	/*============================================================================================
	 * gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x22 + (ch*4), 0x0B ); /* Raptor3
	 * Bank 0x5c
	 *|  7         |  6  |  5  |   4      |  3  |  2  |  1  |  0  |
	 *| PAL_CM_OFF |     |     | COLOROFF |         C_KILL        |
	 *============================================================================================*/
	/*============================================================================================
	 * Bank 5x26
	 *|  7  |  6  |  5  |  4  |  3  |  2  |  1  |  0  |
	 *|                 FSC_LOCK_SENSE                |
	 *============================================================================================*/
	/*============================================================================================
	 * Bank 5xb8
	 *|  7          |  6  |  5  |  4  |  3  |  2  |  1  |  0  |
	 *| HAFC_BYPASS | HAFC_HCOEFF_SEL |       HAFC_OP_MD      |
	 *============================================================================================*/
	/*============================================================================================
	 * Bank 9x40
	 *|  7        |   6      |  5       |    4     |    3     |     2       |  1  |  0       |
	 *| FSC_DET_  | FSC_DET_ | FSC_DET_ | FSC_DET_ | FSC_DET_ |   FSC_DET_  |     | FSC_RST_ |
	 *| AUTO_RST1 | UNLIM1   | AUTO1    | PRESET1  | MODE1    | REFER_AUTO1 |     | STRB1    |
	 *============================================================================================*/

	NC_VD_VI_Init_STR2 *param = (NC_VD_VI_Init_STR2 *)p_param;

	REG_SET_0x20_0_8_BRIGHTNESS2(ch, param->brightnees);
	REG_SET_0x24_0_8_CONTARST2(ch, param->contrast);
	REG_SET_0x28_0_8_BLACK_LEVEL2(ch, param->black_level);
	REG_SET_0x58_0_8_SATURATION_A2(ch, param->saturation_a);
	REG_SET_0x40_0_8_HUE2(ch, param->hue);
	REG_SET_0x44_0_8_U_GAIN2(ch, param->u_gain);
	REG_SET_0x48_0_8_V_GAIN2(ch, param->v_gain);
	REG_SET_0x4C_0_8_U_OFFSET2(ch, param->u_offset);
	REG_SET_0x50_0_8_V_OFFSET2(ch, param->v_offset);
	REG_SET_5x2B_0_8_SATURATION_B2(ch, param->saturation_b);
	REG_SET_5x24_0_8_BURSET_DEC_A2(ch, param->burst_dec_a);
	REG_SET_5x5F_0_8_BURSET_DEC_B2(ch, param->burst_dec_b);
	REG_SET_5xD1_0_8_BURSET_DEC_C2(ch, param->burst_dec_c);

	REG_SET_9x44_0_8_FSC_EXT_EN22(ch, 0x00);
	REG_SET_9x50_0_8_FSC_EXT_VAL_7_02(ch, 0x30);
	REG_SET_9x51_0_8_FSC_EXT_VAL_15_82(ch, 0x6f);
	REG_SET_9x52_0_8_FSC_EXT_VAL23_162(ch, 0x67);
	REG_SET_9x53_0_8_FSC_EXT_VAL_31242(ch, 0x48);

	if (fmt == TVI_5M_12_5P2) {
		REG_SET_5x26_0_8_FSC_LOCK_SENSE2(ch, 0x20);
	} else
		REG_SET_5x26_0_8_FSC_LOCK_SENSE2(ch, 0x40);

	if (fmt == AHD20_SD_H9602EX_Btype_NT2 || fmt == AHD20_SD_H9602EX_Btype_PAL2) {
		REG_SET_5xB8_0_8_HPLL_MASK_END2(ch, 0xb8);
		REG_SET_9x40_0_8_FSC_DET_MODE2(ch, 0x00);
	} else {
		REG_SET_5xB8_0_8_HPLL_MASK_END2(ch, 0x39);
		REG_SET_9x40_0_8_FSC_DET_MODE2(ch, 0x00);

		gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0xff, 0x05 + ch);
		gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0xb5, 0x80);
	}

}

void vd_vi_clock_set_seq92(unsigned char dev, unsigned char ch, void *p_param)
{
	/*============================================================================================
	 * Bank 1x84
	 *|  7  |  6  |  5  |  4  |  3  |  2  |  1  |  0  |
	 *|   VADC_CLK1_DLY_SEL   |     VADC_CLK1_SEL     |
	 *============================================================================================*/
	/*============================================================================================
	 * Bank 1x88
	 *|  7  |  6  |  5  |  4  |  3  |  2  |   1   |   0   |
	 *|     |     |     |     |     |     |  DEC_PRECLK   |
	 * Bank 1x8c
	 *|  7  |  6  |  5  |  4  |  3  |  2  |   1   |   0   |
	 *|     |     |     |     |     |     |  DEC_POSTCLK  |
	 *============================================================================================*/
	/*============================================================================================
	 * ADC -> PRE -> POST -> VCLK
	 * ADC_CLK 1x84[3:0]
	 * 0 ~ 3 : 37.125 MHz
	 * 4 ~ 5 : 74.25 MHz
	 * 8 ~ 9 : 148.5 MHz
	 * Pre_Clock 1x88 / Post Clock 1x8C
	 * 0 : 37.125
	 * 1 : 74.25
	 * 2 : 148.5
	 * VCLK 1xCC[7:4]
	 * 4 ~ 5 : 74.25 MHz
	 * 6 ~ 7 : 148.5 MHz
	 *============================================================================================*/

	NC_VD_VI_Init_STR2 *param = (NC_VD_VI_Init_STR2 *)p_param;

	REG_SET_1x84_0_8_CLK_ADC2(ch, param->clk_adc);
	REG_SET_1x88_0_8_CLK_PRE2(ch, param->clk_pre);
	REG_SET_1x8c_0_8_CLK_POST2(ch, param->clk_post);

	REG_SET_5x01_0_8_CML2_MODE2(ch, param->cml_mode);
	REG_SET_5x05_0_8_AGC_OP2(ch, param->agc_op);
	REG_SET_5x1D_0_8_G_SE2L(ch, param->g_sel);

}

//==================================================================================================================

/**************************************************************************************
 * Jaguar1 Video Output Setting Function
 *
 *
 ***************************************************************************************/
void vd_vo_seq_set2(unsigned char dev, unsigned char ch, void *p_param)
{
	/*
	 * BT656 or BT1120 Set????...
	 * */
	NC_VD_VO_Init_STR2 *param = (NC_VD_VO_Init_STR2 *)p_param;

	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0xFF, 0x01);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0xc0 + (ch * 0x02), param->port_seq_ch01[ch]);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0xc1 + (ch * 0x02), param->port_seq_ch23[ch]);

}

void vd_vo_output_seq_set2(unsigned char dev, unsigned char port, unsigned char out_ch)
{
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0xFF, 0x01);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0xc0 + (port * 0x02), out_ch);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0xc1 + (port * 0x02), out_ch);
}

void vd_vo_port_y_c_merge_set2(unsigned char dev, unsigned char ch, void *p_param)
{
	NC_VD_VO_Init_STR2 *param = (NC_VD_VO_Init_STR2 *)p_param;

	/*============================================================================================
	 * Address: 1xec
	 *|  7  |  6  |  5  |  4  |  3  |  2  |  1  |        0      |
	 *|     |     |     |     |     |     |     | MUX_YC_MERGE1 |
	 *============================================================================================*/
	REG_SET_1xEC_0_8_yc_merge2(ch, param->mux_yc_merge);

}

void vd_vo_port_ch_id_set2(unsigned char dev, unsigned char ch, void *p_param)
{
	NC_VD_VO_Init_STR2 *param = (NC_VD_VO_Init_STR2 *)p_param;
	unsigned char val_0x14 = 0x00;

	/*============================================================================================
	 * Address: 0x14
	 *|  7  |  6  |  5  |      4    |  3  |  2  |  1  |  0  |
	 *|     |     |     | FLD_INV_1 |       CHID_VIN1       |
	 *============================================================================================*/
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0xFF, 0x00);
	val_0x14 = gpio_i2c_read2(jaguar1_i2c_addr2[dev], 0x14 + ch);
	val_0x14 = val_0x14 & 0x10;
	val_0x14 = val_0x14 | param->chid_vin;
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x14 + ch, val_0x14);

}

void vd_vo_mux_mode_set2(unsigned char dev, unsigned char ch, void *p_param)
{
	NC_VD_VO_Init_STR2 *param = (NC_VD_VO_Init_STR2 *)p_param;

	/*============================================================================================
	 * Address: 1xc8
	 *|  7  |  6  |     5     |     4    |  3  |  2  |  1  |  0  |
	 *|     |     | VCLK_1_EN | VDO_1_EN |   VPORT_1_CH_OUT_SEL  |
	 *============================================================================================*/
	REG_SET_1xC8_0_8_out_sel2(ch, param->vport_out_sel);

}

void vd_vo_manual_mode_set2(unsigned char dev, unsigned char ch, void *p_param)
{
	unsigned char val_0x30;
	unsigned char val_0x31;
	unsigned char val_0x32;

	/*============================================================================================
	 * Address: 13x30
	 *|  7  |  6  |  5  |  4  |             3            |  2  |  1  |  0  |
	 *|     |     |     |     | NOVIDEO_VFC_INIT_EN[3:0] |     |     |     |
	 *============================================================================================*/
	/*============================================================================================
	 * Address: 13x31
	 *|  7  |  6  |       5       |        4      |       3       |       2       |       1       |        0      |
	 *|     |     | AHD_8M_det_en | AHD_5M_det_en | AHD_4M_det_en | AHD_3M_det_en | AHD2M_det_en | AHD_1M_det_en |
	 *============================================================================================*/
	/*============================================================================================
	 * Address: 13x32
	 *|  7  |  6  |       5       |        4      |       3       |       2       |       1       |        0      |
	 *|     |     | CVI_8M_det_en | CVI_5M_det_en | CVI_4M_det_en | CVI_3M_det_en | CVI2M_det_en | CVI_1M_det_en |
	 *============================================================================================*/

	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0xFF, 0x13);
	val_0x30 = gpio_i2c_read2(jaguar1_i2c_addr2[dev], 0x30);
	val_0x31 = gpio_i2c_read2(jaguar1_i2c_addr2[dev], 0x31);
	val_0x32 = gpio_i2c_read2(jaguar1_i2c_addr2[dev], 0x32);

	val_0x30 &= (~(1 << (ch + 4)) & (~(1 << ch)));
	val_0x31 &= (~(1 << (ch + 4)) & (~(1 << ch)));
	val_0x32 &= (~(1 << ch));

	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x30, val_0x30);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x31, val_0x31);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x32, val_0x32);

}

void vd_jaguar1_single_differ_set2(unsigned char dev, unsigned char ch, int input)
{
	REG_SET_0x18_0_8_EX_CBAR_ON2(ch, 0x13);

	if (input == DIFFERENTIAL2) {
		REG_SET_5x00_0_8_CMP2(ch, 0xd0);
		REG_SET_5x01_0_8_CML2(ch, 0x2c);
		REG_SET_5x1D_0_8_AFE2(ch, 0x8c);
		REG_SET_5x92_0_8_PWM2(ch, 0x00);
	} else if (input == SINGLE_ENDED2) {
		REG_SET_5x00_0_8_CMP2(ch, 0xd0);
		REG_SET_5x01_0_8_CML2(ch, 0xa2);
		REG_SET_5x92_0_8_PWM2(ch, 0x00);
	} else {
		printk("Jaguar1 Analog Input Setting Fail !!!\n");
	}

}

void vd_jaguar1_960p_30P_test_set2(unsigned char dev, unsigned char ch)
{
	printk("[drv]vd_jaguar1_960p_30P_test_set >>> ch%d!!\n", ch);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0xff, 0x00);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x68 + ch, 0x4E);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x69 + ch, 0x80);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x6a + ch, 0x80);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x6b + ch, 0x80);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x04 + ch, 0x00);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x08 + ch, 0x02);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x0c + ch, 0x00);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x18 + ch, 0x01);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x64 + ch, 0x06);

	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0xff, 0x01);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x84 + ch, 0x04);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x88 + ch, 0x01);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x8c + ch, 0x02);

	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0xff, 0x05 + ch);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x6e, 0x10);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x6f, 0x82);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x76, 0x00);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x77, 0x80);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x78, 0x00);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x79, 0x11);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0xB5, 0x80);

	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0xff, 0x11);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x00 + (ch * 0x20), 0x0f);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x01 + (ch * 0x20), 0x00);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x02 + (ch * 0x20), 0x9d);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x03 + (ch * 0x20), 0x05);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x04 + (ch * 0x20), 0x00);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x05 + (ch * 0x20), 0x08);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x06 + (ch * 0x20), 0xca);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x0a + (ch * 0x20), 0x03);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x0b + (ch * 0x20), 0xc0);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x0c + (ch * 0x20), 0x04);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x0d + (ch * 0x20), 0x4b);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x10 + (ch * 0x20), 0x00);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x11 + (ch * 0x20), 0x96);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x12 + (ch * 0x20), 0x00);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x13 + (ch * 0x20), 0x82);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x14 + (ch * 0x20), 0x00);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x15 + (ch * 0x20), 0x30);

}

void vd_jaguar1_960p25P_test_set2(unsigned char dev, unsigned char ch)
{
	printk("[drv]vd_jaguar1_960p25P_test_set >>> ch%d!!\n", ch);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0xff, 0x00);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x68 + ch, 0x59);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x69 + ch, 0x80);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x6a + ch, 0x80);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x6b + ch, 0x80);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x04 + ch, 0x00);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x08 + ch, 0x03);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x0c + ch, 0x00);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x18 + ch, 0x01);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x64 + ch, 0x06);

	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0xff, 0x01);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x84 + ch, 0x04);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x88 + ch, 0x01);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x8c + ch, 0x02);

	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0xff, 0x05 + ch);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x6e, 0x10);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x6f, 0x82);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x76, 0x00);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x77, 0x80);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x78, 0x00);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x79, 0x11);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0xB5, 0x80);

	/* Only AHD20_720P_960P25P2 */
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0xff, 0x09);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x53 + (ch * 0x04), 0x52);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x52 + (ch * 0x04), 0xd2);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x51 + (ch * 0x04), 0x1c);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x50 + (ch * 0x04), 0x10);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x44 + ch, 0x01);

	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0xff, 0x11);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x00 + (ch * 0x20), 0x0f);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x01 + (ch * 0x20), 0x00);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x02 + (ch * 0x20), 0x97);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x03 + (ch * 0x20), 0x05);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x04 + (ch * 0x20), 0x00);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x05 + (ch * 0x20), 0x0a);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x06 + (ch * 0x20), 0x8c);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x0a + (ch * 0x20), 0x03);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x0b + (ch * 0x20), 0xc0);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x0c + (ch * 0x20), 0x04);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x0d + (ch * 0x20), 0x4c);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x10 + (ch * 0x20), 0x00);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x11 + (ch * 0x20), 0x96);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x12 + (ch * 0x20), 0x00);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x13 + (ch * 0x20), 0x82);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x14 + (ch * 0x20), 0x00);
	gpio_i2c_write2(jaguar1_i2c_addr2[dev], 0x15 + (ch * 0x20), 0x30);

}

/*****************************************************************************************************************************************
 * Jaguar1 Video ioctl function
 * video vi_vo initialize
 *
 ******************************************************************************************************************************************/
void vd_jaguar1_vo_ch_seq_set2(void *p_param)
{
	video_output_init *vo_seq = (video_output_init *)p_param;
	unsigned char dev = 0;
	unsigned char port   = vo_seq->port;
	unsigned char out_ch = vo_seq->out_ch;

	vd_vo_output_seq_set2(dev, port, out_ch);
}

void vd_jaguar1_init_set2(void *p_param)
{
	video_input_init *video_init = (video_input_init *)p_param;
	unsigned char ch  = video_init->ch % 4;
	unsigned char fmt = video_init->format;
	int analog_input  = video_init->input;

	video_equalizer_info_s eq_set;
	NC_VD_COAX_STR2 coax_init;
	NC_VD_VI_Init_STR2 *vi_param;
	NC_VD_VO_Init_STR2 *vo_param;

	int dev =  ch / 4 ;

	vi_param = __NC_VD_VI_Init_Val_Get2(fmt);
	vo_param = __NC_VD_VO_Init_Val_Get2(AHD20_1080P_30P2);

	REG_SET_0x00_0_8_EACH_SET2(ch, 0x10);
	/*=====================================================
	 * vd_Analog Input Setting
	 *=====================================================*/
	vd_jaguar1_single_differ_set2(dev, ch, analog_input);

	/*=====================================================
	 * vd_vo Setting
	 *=====================================================*/
	vd_vo_port_y_c_merge_set2(dev, ch, vo_param);
	vd_vo_mux_mode_set2(dev, ch, vo_param);
	vd_vo_manual_mode_set2(dev, ch, vo_param);

	/*=====================================================
	 * vd_vi Setting
	 *=====================================================*/

	vd_vi_manual_set_seq12(dev, ch, vi_param);
	vd_vi_vafe_set_seq22(dev, ch);
	vd_vi_format_set_seq32(dev, ch, vi_param);
	vd_vi_chroma_set_seq42(dev, ch, vi_param);
	vd_vi_h_timing_set_seq52(dev, ch, vi_param);
	vd_vi_h_scaler_mode_set_seq62(dev, ch, vi_param);

	vd_vi_hpll_set_seq72(dev, ch, vi_param);
	vd_vi_color_set_seq82(dev, ch, vi_param, fmt);
	vd_vo_port_ch_id_set2(dev, ch, vo_param);
	vd_vi_clock_set_seq92(dev, ch, vi_param);

	/*=====================================================
	 * AHD 1280x960P Test
	 *
	 *=====================================================*/
	if (fmt == AHD20_720P_960P_30P2) {
		vd_jaguar1_960p_30P_test_set2(0, ch);
		current_bank_set2(0xFF);
	} else if (fmt == AHD20_720P_960P25P2) {
		vd_jaguar1_960p25P_test_set2(0, ch);
		current_bank_set2(0xFF);
	} else if (fmt == AHD20_SD_H9602EX_Btype_PAL2) {
		REG_SET_0x70_0_8_V_DELAY2(ch, 0x3F);
	} else if (fmt == AHD20_SD_SH720_PAL2 || fmt == AHD20_SD_SH720_NT2 || fmt == AHD20_SD_H1440_PAL2 || fmt == AHD20_SD_H1440_NT2) {
		REG_SET_0x14_0_8_FLD_INV_CHID2(ch, 0x00);
		REG_SET_0x34_0_8_Y_FIR_MODE2(ch, 0x00);
		REG_SET_1xCC_0_8_VPORT_OCLK_SEL_VPORT_OVCLK_DLY_SEL2(ch, 0x40);
		REG_SET_1xA0_0_8_TM_CLK_EN_SET2(ch, 0x10);
		REG_SET_5x21_0_8_CONT_SUB2(ch, 0x24);
		REG_SET_5x55_0_8_C_MEM_CLK_SEL(ch, 0x00);
		REG_SET_5x56_0_8_FREQ_MEM_CLK_SEL2(ch, 0x00);
		REG_SET_5x57_0_8_LINE_MEM_CLK_INV2(ch, 0x00);
		REG_SET_5xB5_0_8_HAFC_MASK_SEL2(ch, 0x00);
		REG_SET_5xB8_0_8_HAFC_HCOEFF_SEL2(ch, 0x39);
		REG_SET_0x7C_0_8_HZOOM2(ch, 0x8F);
	} else
		sensor_dbg("\n");

	sensor_dbg("[drv_vi]ch::%d >>> fmt::%s\n", ch, vi_param->name);

	/*=====================================================
	 * EQ Stage 0 Setting
	 *
	 *=====================================================*/
#if 1
	eq_set.Ch     = ch;
	eq_set.FmtDef = fmt;
	eq_set.Cable  = CABLE_A2;
	eq_set.Input  = SINGLE_ENDED2;
	eq_set.stage  = STAGE_02;
	video_input_eq_val_set2(&eq_set);
#endif

	sensor_dbg("[drv_vi]ch::%d >>> fmt::%s\n", ch, vi_param->name);
	current_bank_set2(0xFF);

	/*=====================================================
	 * Coaxial Initialize
	 *
	 *=====================================================*/
	coax_init.ch       = ch;
	coax_init.vivo_fmt = fmt;
	coax_init.vd_dev   = dev;
	coax_tx_init2(&coax_init);
	if (bit8 == 0)
		coax_tx_16bit_init2(&coax_init);
	coax_rx_init2(&coax_init);

}

void vd_jaguar1_get_novideo2(video_video_loss_s *vidloss)
{
	gpio_i2c_write2(jaguar1_i2c_addr2[vidloss->devnum], 0xFF, 0x00);
	vidloss->videoloss = gpio_i2c_read2(jaguar1_i2c_addr2[vidloss->devnum], 0xA0);
}

void vd_jaguar1_sw_reset2(void *p_param)
{

	REG_SET_1x81_0_1_VPLL_RST2(0, 0x1);
	REG_SET_1x80_0_1_VPLL_C2(0, 0x1);
	REG_SET_1x80_0_1_VPLL_C2(0, 0x0);
	REG_SET_1x81_0_1_VPLL_RST2(0, 0x0);
	sensor_dbg("[drv]jaguar1_sw_reset complete!!\n");
}
