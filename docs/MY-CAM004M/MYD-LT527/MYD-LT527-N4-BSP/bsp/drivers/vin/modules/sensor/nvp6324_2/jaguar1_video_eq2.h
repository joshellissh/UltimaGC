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

#ifndef _JAGUAR1_VIDEO_EQ2_H_
#define _JAGUAR1_VIDEO_EQ2_H_

#include "../nvp6324/jaguar1_video_eq.h"

void video_input_eq_val_set2(video_equalizer_info_s *pvin_eq_set);
void video_input_eq_cable_set2(video_equalizer_info_s *pvin_eq_set);
void video_input_eq_analog_input_set2(video_equalizer_info_s *pvin_eq_set);

#endif /* _JAGUAR1_VIDEO_EQ_H_ */
