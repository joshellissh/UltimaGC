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

#ifndef _JAGUAR1_VIDEO2_
#define _JAGUAR1_VIDEO2_

//#include "jaguar1_common2.h"
#include "../nvp6324/jaguar1_video.h"

void vd_jaguar1_init_set2(void *p_param);
void vd_jaguar1_vo_ch_seq_set2(void *p_param);
void vd_jaguar1_eq_set2(void *p_param);
void vd_jaguar1_sw_reset2(void *p_param);
void vd_jaguar1_get_novideo2(video_video_loss_s *vidloss);

void current_bank_set2(unsigned char bank);
unsigned char current_bank_get2(void);
void vd_register_set2(int dev, unsigned char bank, unsigned char addr, unsigned char val, int pos, int size);
void reg_val_print_flag_set2(int set);

#endif