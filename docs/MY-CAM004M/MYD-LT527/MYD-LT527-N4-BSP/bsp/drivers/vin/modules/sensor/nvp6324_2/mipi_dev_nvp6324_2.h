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

u32 nvp6324_i2c_write2(u8 da, u8 reg, u8 val);
u32 nvp6324_i2c_read2(u8 da, u8 reg);
int check_id2(unsigned int dec);
int check_rev2(unsigned int dec);
void read_bank_value2(void);
int nvp6324_init2(int mode);
