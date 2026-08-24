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

#ifndef _MOTION2_H_
#define _MOTION2_H_

#include "../nvp6324/jaguar1_motion.h"

void motion_onoff_set2(motion_mode *motion_set);
void motion_display_onoff_set2(motion_mode *motion_set);
void motion_pixel_all_onoff_set2(motion_mode *motion_set);
void motion_pixel_onoff_set2(motion_mode *motion_set);
void motion_pixel_onoff_get2(motion_mode *motion_set);
void motion_tsen_set2(motion_mode *motion_set);
void motion_psen_set2(motion_mode *motion_set);
void motion_detection_get2(motion_mode *motion_set);

#endif /* _MOTION_H_ */
