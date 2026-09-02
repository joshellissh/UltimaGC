/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Register map for the Nextchip NVP6324 ("N4") 4-channel AHD-to-MIPI-CSI2
 * decoder, as used on the MY-CAM004M board.
 *
 * Values are distilled from the Nextchip "N4" datasheet and the vendor
 * reference driver's chip-programming files (jaguar1_video.c / jaguar1_mipi.c /
 * mipi_dev_nvp6324.c and their register tables) — NOT from any prior in-repo
 * driver. See ../docs/MY-CAM004M and PLAN.md.
 *
 * The chip is heavily paged: register 0xFF selects the active bank; all other
 * register addresses are bank-relative. See nvp6324.c's banked accessors.
 */
#ifndef NVP6324_REGS_H
#define NVP6324_REGS_H

/* Bank-select register (write the bank number here before touching a bank). */
#define NVP6324_REG_BANK		0xFF

/* --- Bank 0x00: chip basics / identity ------------------------------------ */
#define NVP6324_BANK_DEVICE		0x00
#define NVP6324_REG_CHIP_ID		0xF4	/* expect 0xB0 (4-port part)      */
#define NVP6324_REG_REV_ID		0xF5	/* expect 0x00                    */
#define NVP6324_CHIP_ID_4PORT		0xB0
#define NVP6324_REV_ID			0x00
/* Per-channel format selector that lives in bank 0 (see the 25P vs 30P delta) */
#define NVP6324_REG_AHD_MODE		0x08	/* 0x03 = 1080p25, 0x02 = 1080p30 */
#define NVP6324_AHD_MODE_1080P25	0x03
#define NVP6324_AHD_MODE_1080P30	0x02

/* --- Bank 0x01: clocks / video-output port -------------------------------- */
#define NVP6324_BANK_CLOCK		0x01
#define NVP6324_REG_SW_RESET_A		0x80	/* soft video-PLL reset (pulse)   */
#define NVP6324_REG_SW_RESET_B		0x81

/* --- Per-channel analog decoder banks: 0x05 + channel (ch 0..3 -> 5,6,7,8) - */
#define NVP6324_BANK_CH(ch)		(0x05 + (ch))
#define NVP6324_REG_BURST_DEC_C		0xD1	/* 0x30 (25P) / 0x1E (30P)        */

/* --- Bank 0x20: MIPI arbiter ---------------------------------------------- */
#define NVP6324_BANK_ARB		0x20
#define NVP6324_REG_ARB_ENABLE		0x00	/* per-channel enable bits        */
#define NVP6324_REG_ARB_SCALE		0x01	/* 2 bits/ch: 0=FHD,1=HD/2,2=SD/4 */
#define NVP6324_ARB_EN_ALL		0xFF	/* all four channels enabled      */

/* --- Bank 0x21: MIPI D-PHY / TX ------------------------------------------- */
#define NVP6324_BANK_MIPI		0x21
#define NVP6324_REG_MIPI_RESET		0x07	/* also lane-enable mask          */
#define NVP6324_MIPI_LANES_4		0x0F	/* enable 4 data lanes            */
#define NVP6324_REG_MIPI_FRMCNT_EN	0x0F
#define NVP6324_REG_MIPI_DPHY_CTL	0x08
#define NVP6324_REG_MIPI_PLL0		0x40	/* PLL block 0x40..0x43           */
#define NVP6324_REG_MIPI_PLL1		0x41
#define NVP6324_REG_MIPI_PLL2		0x42
#define NVP6324_REG_MIPI_PLL3		0x43
/* Per-virtual-channel CSI-2 data-type registers (VC0..VC3). */
#define NVP6324_REG_MIPI_VC_DT(vc)	(0x38 + (vc))

/* MIPI CSI-2 data type carried on every VC: YUV422 8-bit (== MIPI_CSI2_DT_YUV422_8B). */
#define NVP6324_CSI2_DT_YUV422_8B	0x1E

/* Fixed geometry for the initial bring-up target. */
#define NVP6324_ACTIVE_WIDTH		1920
#define NVP6324_ACTIVE_HEIGHT		1080
#define NVP6324_NUM_CHANNELS		4

#endif /* NVP6324_REGS_H */
