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
#include "jaguar1_video_eq2.h"
#include "jaguar1_cableA_video_eq_table2.h"
#include "jaguar1_reg_set_def2.h"
#include "jaguar1_video2.h"
#include "../sensor_helper.h"

#define SENSOR_NAME "nvp6324_mipi_2"

NC_JAGUAR1_EQ2 NC_VD_EQ_FindFormatDef2(NC_VIVO_CH_FORMATDEF2 format_standard, NC_ANALOG_INPUT22 analog_input)
{
	int ii;

	for (ii = 0; ii < NC_EQ_SETTING_FMT_MAX2; ii++) {
		_jaguar1_video_eq_value_table_s *pFmt = &equalizer_value_fmtdef_cableA2[ii];

		if (pFmt->video_fmt == format_standard)
			if (pFmt->analog_input == analog_input)
				return ii;
	}

	sensor_dbg("NC_VD_EQ_FindFormatDef2 UNKNOWN format!!!\n");

	return NC_EQ_SETTING_FMT_UNKNOWN2;
}

void __eq_base_set_value2(video_equalizer_info_s *pvin_eq_set, video_equalizer_base_s *pbase)
{
	unsigned char ch = pvin_eq_set->Ch;
	unsigned char dist = pvin_eq_set->stage;

	REG_SET_5x65_0_8_EQ_BYPASS2(ch, pbase->eq_bypass[dist]);
	REG_SET_5x58_0_8_EQ_BAND_SEL2(ch, pbase->eq_band_sel[dist]);
	REG_SET_5x5C_0_8_EQ_GAIN_SEL2(ch, pbase->eq_gain_sel[dist]);
	REG_SET_Ax3D_0_8_EQ_DEQ_A_ON2(ch, pbase->deq_a_on[dist]);
	REG_SET_Ax3C_0_8_EQ_DEQ_A_SEL2(ch, pbase->deq_a_sel[dist]);

}

void __eq_coeff_set_value2(video_equalizer_info_s *pvin_eq_set, video_equalizer_coeff_s *pcoeff)
{

	unsigned char ch = pvin_eq_set->Ch;
	unsigned char dist = pvin_eq_set->stage;

	REG_SET_Ax30_0_8_EQ_DEQ_A_012(ch, pcoeff->deqA_01[dist]);
	REG_SET_Ax31_0_8_EQ_DEQ_A_022(ch, pcoeff->deqA_02[dist]);
	REG_SET_Ax32_0_8_EQ_DEQ_A_032(ch, pcoeff->deqA_03[dist]);
	REG_SET_Ax33_0_8_EQ_DEQ_A_042(ch, pcoeff->deqA_04[dist]);
	REG_SET_Ax34_0_8_EQ_DEQ_A_052(ch, pcoeff->deqA_05[dist]);
	REG_SET_Ax35_0_8_EQ_DEQ_A_062(ch, pcoeff->deqA_06[dist]);
	REG_SET_Ax36_0_8_EQ_DEQ_A_072(ch, pcoeff->deqA_07[dist]);
	REG_SET_Ax37_0_8_EQ_DEQ_A_082(ch, pcoeff->deqA_08[dist]);
	REG_SET_Ax38_0_8_EQ_DEQ_A_092(ch, pcoeff->deqA_09[dist]);
	REG_SET_Ax39_0_8_EQ_DEQ_A_102(ch, pcoeff->deqA_10[dist]);
	REG_SET_Ax3A_0_8_EQ_DEQ_A_112(ch, pcoeff->deqA_11[dist]);
	REG_SET_Ax3B_0_8_EQ_DEQ_A_122(ch, pcoeff->deqA_12[dist]);

}

void __eq_color_set_value2(video_equalizer_info_s *pvin_eq_set, video_equalizer_color_s *pcolor)
{
	unsigned char ch = pvin_eq_set->Ch;
	unsigned char dist = pvin_eq_set->stage;

	REG_SET_0x24_0_8_EQ_COLOR_CONTRAST2(ch, pcolor->contrast[dist]);
	REG_SET_0x30_0_8_EQ_COLOR_H_PEAKING_12(ch, pcolor->y_peaking_mode[dist]);
	REG_SET_0x34_0_8_EQ_COLOR_H_PEAKING22(ch, pcolor->y_fir_mode[dist]);


	REG_SET_5x31_0_8_EQ_COLOR_C_FILTER2(ch, pcolor->c_filter[dist]);


	REG_SET_0x5c_0_8_EQ_PAL_CM_OFF2(ch, pcolor->pal_cm_off[dist]);

	REG_SET_0x40_0_8_EQ_COLOR_HUE2(ch, pcolor->hue[dist]);
	REG_SET_0x44_0_8_EQ_COLOR_U_GAIN2(ch, pcolor->u_gain[dist]);
	REG_SET_0x48_0_8_EQ_COLOR_V_GAIN2(ch, pcolor->v_gain[dist]);
	REG_SET_0x4C_0_8_EQ_COLOR_U_OFFSET2(ch, pcolor->u_offset[dist]);
	REG_SET_0x50_0_8_EQ_COLOR_V_OFFSET2(ch, pcolor->v_offset[dist]);
	REG_SET_0x28_0_8_EQ_COLOR_BLACK_LEVEL2(ch, pcolor->black_level[dist]);

	REG_SET_5x27_0_8_EQ_COLOR_ACC_REF2(ch, pcolor->acc_ref[dist]);
	REG_SET_5x28_0_8_EQ_COLOR_CTI_DELAY2(ch, pcolor->cti_delay[dist]);
	REG_SET_5x2b_0_8_EQ_COLOR_SUB_SATURATION2(ch, pcolor->saturation_b[dist]);
	REG_SET_5x24_0_8_EQ_COLOR_BURST_DEC_A2(ch, pcolor->burst_dec_a[dist]);
	REG_SET_5x5F_0_8_EQ_COLOR_BURST_DEC_B2(ch, pcolor->burst_dec_b[dist]);
	REG_SET_5xD1_0_8_EQ_COLOR_BURST_DEC_C2(ch, pcolor->burst_dec_c[dist]);
	REG_SET_5xD5_0_8_EQ_COLOR_C_OPTION2(ch, pcolor->c_option[dist]);
	REG_SET_Ax25_0_8_EQ_COLOR_Y_FILTER_B2(ch, pcolor->y_filter_b[dist]);
	REG_SET_Ax27_0_8_EQ_COLOR_Y_FILTER_B_SEL2(ch, pcolor->y_filter_b_sel[dist]);

}

void __eq_timing_a_set_value2(video_equalizer_info_s *pvin_eq_set, video_equalizer_timing_a_s *ptiming_a)
{
	unsigned char ch = pvin_eq_set->Ch;
	unsigned char dist = pvin_eq_set->stage;

	REG_SET_0x68_0_8_EQ_TIMING_A_H_DELAY_A2(ch, ptiming_a->h_delay_a[dist]);
	REG_SET_5x38_0_8_EQ_TIMING_A_H_DELAY_B2(ch, ptiming_a->h_delay_b[dist]);
	REG_SET_0x6C_0_4_EQ_TIMING_A_H_DELAY_C2(ch, ptiming_a->h_delay_c[dist]);

	REG_SET_0x64_0_8_EQ_TIMING_A_Y_DELAY2(ch, ptiming_a->y_delay[dist]);

}

void __eq_clk_set_value2(video_equalizer_info_s *pvin_eq_set, video_equalizer_clk_s *pclk)
{
	unsigned char ch = pvin_eq_set->Ch;
	unsigned char dist = pvin_eq_set->stage;

	REG_SET_1x84_0_8_EQ_CLOCK_ADC_CLK2(ch, pclk->clk_adc[dist]);
	REG_SET_1x88_0_8_EQ_CLOCK_PRE_CLK2(ch, pclk->clk_adc_pre[dist]);
	REG_SET_1x8C_0_8_EQ_CLOCK_POST_CLK2(ch, pclk->clk_adc_post[dist]);

}
static void __eq_timing_b_set_value(video_equalizer_info_s *pvin_eq_set, video_equalizer_timing_b_s *ptiming_b)
{
	unsigned char ch = pvin_eq_set->Ch;
	unsigned char dist = pvin_eq_set->stage;

	REG_SET_9x96_0_8_EQ_TIMING_B_HSCALER_12(ch, ptiming_b->h_scaler1[dist]);
	REG_SET_9x97_0_8_EQ_TIMING_B_HSCALER22(ch, ptiming_b->h_scaler2[dist]);
	REG_SET_9x98_0_8_EQ_TIMING_B_HSCALER_3(ch, ptiming_b->h_scaler3[dist]);
	REG_SET_9x99_0_8_EQ_TIMING_B_HSCALER_42(ch, ptiming_b->h_scaler4[dist]);
	REG_SET_9x9A_0_8_EQ_TIMING_B_HSCALER_52(ch, ptiming_b->h_scaler5[dist]);
	REG_SET_9x9B_0_8_EQ_TIMING_B_HSCALER_62(ch, ptiming_b->h_scaler6[dist]);
	REG_SET_9x9C_0_8_EQ_TIMING_B_HSCALER_72(ch, ptiming_b->h_scaler7[dist]);
	REG_SET_9x9D_0_8_EQ_TIMING_B_HSCALER_82(ch, ptiming_b->h_scaler8[dist]);
	REG_SET_9x9E_0_8_EQ_TIMING_B_HSCALER_9(ch, ptiming_b->h_scaler9[dist]);
	REG_SET_9x40_0_8_EQ_TIMING_B_PN_AUTO2(ch, ptiming_b->pn_auto[dist]);
	REG_SET_5x90_0_8_EQ_TIMINING_B_COMB_MODE2(ch, ptiming_b->comb_mode[dist]);
	REG_SET_5xB9_0_8_EQ_TIMING_B_HPLL_OP_A2(ch, ptiming_b->h_pll_op_a[dist]);
	REG_SET_5x57_0_8_EQ_TIMING_B_MEM_PATH2(ch, ptiming_b->mem_path[dist]);
	REG_SET_5x25_0_8_EQ_TIMING_B_FSC_LOCK_SPD2(ch, ptiming_b->fsc_lock_speed[dist]);

	REG_SET_0x04_0_8_EQ_TIMING_B_SD_MD2(ch, ptiming_b->sd_mode[dist]);
	REG_SET_0x08_0_8_EQ_TIMING_B_AHD_MD2(ch, ptiming_b->ahd_mode[dist]);
	REG_SET_0x0C_0_8_EQ_TIMING_B_SPECIAL_MD2(ch, ptiming_b->spl_mode[dist]);
	REG_SET_0x78_0_8_EQ_TIMING_B_VBLK_END2(ch, ptiming_b->vblk_end[dist]);

	REG_SET_5x1D_0_8_EQ_AFE_G_SEL2(ch, ptiming_b->afe_g_sel[dist]);
	REG_SET_5x01_0_8_EQ_AFE_CTR_CLP2(ch, ptiming_b->afe_ctr_clp[dist]);
	REG_SET_5x05_0_8_EQ_D_AGC_OPTION2(ch, ptiming_b->d_agc_option[dist]);

}

void video_input_eq_val_set2(video_equalizer_info_s *pvin_eq_set)
{
	NC_JAGUAR1_EQ2 eq_fmt;
	__maybe_unused unsigned char ch = pvin_eq_set->Ch;
	int fmt = pvin_eq_set->FmtDef;
	int input = pvin_eq_set->Input;
	int cable = pvin_eq_set->Cable;
	/* int stage = pvin_eq_set->stage; */
	_jaguar1_video_eq_value_table_s eq_value;

	eq_fmt = NC_VD_EQ_FindFormatDef2(fmt, input);

	if (cable == CABLE_A2)
		eq_value = (_jaguar1_video_eq_value_table_s)equalizer_value_fmtdef_cableA2[eq_fmt];
	else if (cable == CABLE_B2)
		eq_value = (_jaguar1_video_eq_value_table_s)equalizer_value_fmtdef_cableA2[eq_fmt];
	else if (cable == CABLE_C2)
		eq_value = (_jaguar1_video_eq_value_table_s)equalizer_value_fmtdef_cableA2[eq_fmt];
	else if (cable == CABLE_D2)
		eq_value = (_jaguar1_video_eq_value_table_s)equalizer_value_fmtdef_cableA2[eq_fmt];
	else
		eq_value = (_jaguar1_video_eq_value_table_s)equalizer_value_fmtdef_cableA2[eq_fmt];

	if (eq_value.name == NULL) {
		sensor_dbg("[drv_eq]Error - Unknown EQ Table!!\n");
		return;
	} else {
		/* set_eq_value */
		__eq_base_set_value2(pvin_eq_set, &eq_value.eq_base);
		__eq_coeff_set_value2(pvin_eq_set, &eq_value.eq_coeff);
		__eq_color_set_value2(pvin_eq_set, &eq_value.eq_color);
		__eq_timing_a_set_value2(pvin_eq_set, &eq_value.eq_timing_a);
		__eq_clk_set_value2(pvin_eq_set, &eq_value.eq_clk);
		__eq_timing_b_set_value(pvin_eq_set, &eq_value.eq_timing_b);

		if (AHD20_SD_H9602EX_Btype_NT2_SINGLE_ENDED2 || AHD20_SD_H9602EX_Btype_NT2_DIFFERENTIAL2) {

		} else if (AHD20_SD_H9602EX_Btype_PAL2_SINGLE_ENDED2 || AHD20_SD_H9602EX_Btype_PAL2_DIFFERENTIAL2) {

		} else {

		}
		sensor_dbg("[drv_eq]ch::%d >>> fmt::%s\n", ch, eq_value.name);
	}
}


void video_input_eq_cable_set2(video_equalizer_info_s *pvin_eq_set)
{
	/* unsigned char ch = pvin_eq_set->Ch;
	int cable = pvin_eq_set->Cable;

	sensor_dbg("[DRV]video_input_eq_cable_set2::ch(%d) cable(%d)\n", ch, cable); */
}

void video_input_eq_analog_input_set2(video_equalizer_info_s *pvin_eq_set)
{
	unsigned char ch = pvin_eq_set->Ch;
	int input = pvin_eq_set->Input;

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
		sensor_dbg("Jaguar1 Analog Input Setting Fail !!!\n");
	}

	sensor_dbg("[DRV]video_input_eq_analog_input_set2::ch(%d) input(%d)\n", ch, input);
}
