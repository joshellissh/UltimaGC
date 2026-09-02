// SPDX-License-Identifier: GPL-2.0
/*
 * V4L2 sub-device driver for the Nextchip NVP6324 ("N4") 4-channel
 * AHD-to-MIPI-CSI2 decoder, as fitted to the MY-CAM004M board and wired to
 * CSI0 on the BeagleY-AI (TI AM67A / J722S).
 *
 * The chip decodes four analog AHD cameras and muxes them onto ONE 4-lane
 * MIPI CSI-2 link as four virtual channels (VC0..VC3), UYVY / data type 0x1E.
 * The J722S CSI2RX stack (cdns-csi2rx bridge + TI SHIM) demuxes those VCs into
 * four independent /dev/videoN nodes; this driver is the CSI-2 *source*
 * sub-device that describes the four streams (one per VC) and programs the
 * decoder over I2C. It implements the V4L2 multiplexed-streams API
 * (routing / get_frame_desc / enable_streams) — modelled on the in-tree
 * ds90ub960 deserializer, which has the same "N sources -> VCs on one link"
 * shape — but as a leaf source with no upstream sub-devices.
 *
 * The MY-CAM004M manages power, reset, clocking (on-board 27 MHz crystal) and
 * enable entirely on-board, so this driver drives NO reset/power/clock GPIOs:
 * bring-up is I2C configuration only. See PLAN.md.
 *
 * Chip register programming is ported from the Nextchip "N4" datasheet and the
 * vendor reference's chip-only files; deliberately NOT from any prior in-repo
 * driver.
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/regmap.h>

#include <media/mipi-csi2.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-mediabus.h>
#include <media/v4l2-subdev.h>

#include "nvp6324_regs.h"

/*
 * Pad model: pads 0..3 are the four analog AHD inputs (sinks, unlinked — the
 * cameras are analog, not sub-devices), pad 4 is the MIPI CSI-2 output (source)
 * carrying all four streams.
 */
#define NVP6324_PAD_SINK0	0
#define NVP6324_PAD_SOURCE	NVP6324_NUM_CHANNELS	/* == 4 */
#define NVP6324_NUM_PADS	(NVP6324_NUM_CHANNELS + 1)

#define NVP6324_MBUS_CODE	MEDIA_BUS_FMT_UYVY8_1X16

/*
 * MIPI CSI-2 D-PHY link frequency (Hz). The cdns-csi2rx bridge reads this via
 * the mandatory V4L2_CID_LINK_FREQ control to configure the external D-PHY;
 * without it stream-on fails "Unable to calculate link frequency".
 *
 * The rate the chip actually drives is the discriminator for the framing bug:
 * my provisional 621 MHz (~1.242 Gbps/lane DDR) matches the vendor MIPI table's
 * "1242MHZ" comment, BUT the vendor v4l2 driver (nvp6324_mipi_driver.c) declares
 * mipi_bps = 567 Mbps/lane for EVERY mode incl. 1080p25 -> 283.5 MHz. A too-high
 * link freq miscalibrates the D-PHY HS timing and can drop lines + shear. Made a
 * module param so the real rate can be swept by hot-reload (no DT rebuild). The
 * value seeds the single-entry V4L2_CID_LINK_FREQ menu at probe.
 */
/*
 * Bring-up sweep: the correct D-PHY link frequency is unknown (621 MHz gives 720
 * valid lines + shear; 283.5 MHz gives ~900 + less shear -> the real rate is
 * lower still). Expose the whole candidate ladder as a WRITABLE V4L2_CID_LINK_FREQ
 * menu so it can be swept at runtime with `v4l2-ctl --set-ctrl link_frequency=<idx>`
 * + a stream restart — cdns-csi2rx re-reads the control's current value at each
 * stream-on (verified: 283.5 MHz streamed fine even though the DT lists only 621,
 * so cdns does NOT validate against the DT). No reload/cold-boot per value.
 * Descending so higher index = lower rate = (empirically) more valid lines.
 */
static s64 nvp6324_link_freqs[] = {
	621000000, 500000000, 450000000, 400000000, 360000000, 330000000,
	300000000, 283500000, 260000000, 240000000, 220000000, 200000000,
	190000000, 180000000, 170000000, 160000000, 150000000, 140000000,
	130000000, 120000000,
};

/* Index into nvp6324_link_freqs[] the control defaults to at probe. Must match the
 * TX rate (mipi_mclk): 6 = 300 MHz DDR = 600 Mbps band, which pairs with the default
 * 594 Mbps TX and gives CRC=0. (For 1242 TX use idx 0; for 756 use idx 4.) */
static int link_freq_idx = 6;
module_param(link_freq_idx, int, 0444);
MODULE_PARM_DESC(link_freq_idx,
	"Default index into the CSI-2 link-frequency menu (0=621MHz .. higher=lower rate). Sweep live via v4l2-ctl --set-ctrl link_frequency=<idx>.");

/*
 * Bring the chip's MIPI TX up at PROBE and keep it transmitting continuously,
 * rather than (re)programming it inside enable_streams. The cdns-csi2rx bridge
 * powers on + calibrates its RX D-PHY (csi2rx_start) BEFORE it calls the source
 * subdev's enable_streams — so if the chip's TX clock lane is still down / mid
 * MIPI-reset at that moment, the RX locks onto a bad signal, giving persistent
 * truncated-header / CRC errors and nondeterministic lock (a band-independent
 * error floor). Programming at probe makes the TX clock lane stable before any
 * RX power-on, and keeping it up across stream off/on avoids re-glitching it.
 * Default on for bring-up.
 */
static bool program_at_probe = true;
module_param(program_at_probe, bool, 0444);
MODULE_PARM_DESC(program_at_probe,
	"Bring MIPI TX up at probe and keep it running (fixes RX-calibrates-before-TX-up ordering). Default 1.");

/*
 * AHD_MODE (bank0 reg 0x08) selects the frame rate: 0x03 = 1080p25, 0x02 =
 * 1080p30. This is the ONLY register that differs between the vendor's 1080p25
 * and 1080p30 sequences (everything else is identical or reconciled by the EQ
 * pass). Exposed as a writable module param for bring-up: the chip is fully
 * reprogrammed on every stream-on, so `echo 2 > /sys/module/nvp6324/parameters/
 * ahd_mode` then restarting the stream switches 25p<->30p with no reload. The
 * vendor's 1080p50/60 tables are non-functional stubs (no high-rate MIPI PLL),
 * so only 0x03/0x02 are valid. Default 0x03 (1080p25).
 */
static int ahd_mode = NVP6324_AHD_MODE_1080P25;
module_param(ahd_mode, int, 0644);
MODULE_PARM_DESC(ahd_mode, "AHD_MODE reg 0x08: 0x03=1080p25 (default), 0x02=1080p30");

/*
 * vc_mask: which MIPI virtual channels (= AHD input channels) the arbiter
 * enables, one bit per channel. Enabling a VC that has no camera makes the
 * arbiter interleave a free-running/signal-less channel onto the link; the TI
 * CSI2RX SHIM then sees two frame-starts per real frame and splits each 1080p25
 * frame across two DMA buffers (measured: fps doubles to ~50, top ~788 lines in
 * one buffer + ~292 in the next). This is NOT link-bandwidth oversubscription —
 * it happens even at 1242 Mbps. So only enable the VCs that actually carry a
 * camera. Default 0x1 (VC0 only) for the current single-camera bring-up; set
 * 0x3/0x7/0xF as more AHD inputs are populated.
 */
static int vc_mask = 0x1;
module_param(vc_mask, int, 0444);
MODULE_PARM_DESC(vc_mask, "Bitmask of MIPI VCs/channels to enable in the arbiter (default 0x1 = VC0 only). Only enable VCs with a camera, else the frame splits.");

/*
 * mipi_mclk: MIPI TX D-PHY lane rate in Mbps. Selects a matched PLL + HS-timing
 * block (see nvp6324_mipi_pll_*). 594 (default) gives CRC=0 for one 1080p25 stream
 * on this board; the stock 1242 ("FHD x4ch") is a marginal eye here and floors at
 * ~4500 CRC/s + shear. Pair with a matching link_freq_idx (1242->0, 756->4, 594->6)
 * so the Cadence RX picks the right D-PHY band. A 4-camera 1080p build needs 1242.
 */
static int mipi_mclk = 594;
module_param(mipi_mclk, int, 0444);
MODULE_PARM_DESC(mipi_mclk, "MIPI TX lane rate Mbps: 594 (default, CRC-clean 1x1080p), 756, 1242 (4x1080p), 378. Pair with matching link_freq_idx.");

struct nvp6324 {
	struct i2c_client	*client;
	struct regmap		*regmap;
	struct v4l2_subdev	sd;
	struct media_pad	pads[NVP6324_NUM_PADS];

	struct v4l2_ctrl_handler ctrl_handler;

	struct mutex		lock;	/* guards bank cache + streaming state */
	int			cur_bank;	/* cached 0xFF value, -1 = unknown */
	u64			enabled_streams;	/* on the source pad */
	bool			hw_running;	/* chip MIPI TX up (program_at_probe) */
};

static inline struct nvp6324 *sd_to_nvp6324(struct v4l2_subdev *sd)
{
	return container_of(sd, struct nvp6324, sd);
}

/* ------------------------------------------------------------------ *
 * Banked register access (register 0xFF selects the active bank).
 * All helpers take priv->lock so the bank switch + access are atomic.
 * ------------------------------------------------------------------ */

static int __nvp6324_set_bank(struct nvp6324 *priv, u8 bank)
{
	int ret;

	if (priv->cur_bank == bank)
		return 0;

	ret = regmap_write(priv->regmap, NVP6324_REG_BANK, bank);
	if (ret) {
		priv->cur_bank = -1;	/* force re-sync next time */
		return ret;
	}
	priv->cur_bank = bank;
	return 0;
}

static int nvp6324_write(struct nvp6324 *priv, u8 bank, u8 reg, u8 val)
{
	int ret;

	mutex_lock(&priv->lock);
	ret = __nvp6324_set_bank(priv, bank);
	if (!ret)
		ret = regmap_write(priv->regmap, reg, val);
	mutex_unlock(&priv->lock);
	if (ret)
		dev_err(&priv->client->dev,
			"write bank 0x%02x reg 0x%02x = 0x%02x failed: %d\n",
			bank, reg, val, ret);
	return ret;
}

static int nvp6324_read(struct nvp6324 *priv, u8 bank, u8 reg, u8 *val)
{
	unsigned int v;
	int ret;

	mutex_lock(&priv->lock);
	ret = __nvp6324_set_bank(priv, bank);
	if (!ret)
		ret = regmap_read(priv->regmap, reg, &v);
	mutex_unlock(&priv->lock);
	if (ret)
		dev_err(&priv->client->dev,
			"read bank 0x%02x reg 0x%02x failed: %d\n",
			bank, reg, ret);
	else
		*val = v;
	return ret;
}

/*
 * Banked read-modify-write: only the bits in @mask are changed to @val (which
 * must already be positioned in the field). The bank switch, read and write are
 * one atomic critical section so a concurrent access can't move the bank in
 * between. Used for the handful of bitfield registers in the init sequence.
 */
static int nvp6324_update_bits(struct nvp6324 *priv, u8 bank, u8 reg,
			       u8 mask, u8 val)
{
	unsigned int v;
	int ret;

	mutex_lock(&priv->lock);
	ret = __nvp6324_set_bank(priv, bank);
	if (!ret)
		ret = regmap_read(priv->regmap, reg, &v);
	if (!ret)
		ret = regmap_write(priv->regmap, reg, (v & ~mask) | (val & mask));
	mutex_unlock(&priv->lock);
	if (ret)
		dev_err(&priv->client->dev,
			"update bank 0x%02x reg 0x%02x mask 0x%02x = 0x%02x failed: %d\n",
			bank, reg, mask, val, ret);
	return ret;
}

/*
 * A plain {bank, reg, val} write, and a walker for the fixed (non-per-channel)
 * init sequences. Mirrors how the vendor reference stores its register tables:
 * the sequences are transcribed verbatim in chip order, so later writes to the
 * same register intentionally overwrite earlier ones (rather than pre-reconciled
 * to a single net-final value) — that keeps each line verifiable against the
 * vendor source.
 */
struct nvp6324_reg {
	u8 bank;
	u8 reg;
	u8 val;
};

static int nvp6324_write_regs(struct nvp6324 *priv,
			      const struct nvp6324_reg *regs, size_t n)
{
	size_t i;
	int ret;

	for (i = 0; i < n; i++) {
		ret = nvp6324_write(priv, regs[i].bank, regs[i].reg, regs[i].val);
		if (ret)
			return ret;
	}
	return 0;
}

/* ------------------------------------------------------------------ *
 * Chip detect + programming.
 * ------------------------------------------------------------------ */

static int nvp6324_detect(struct nvp6324 *priv)
{
	struct device *dev = &priv->client->dev;
	u8 id, rev;
	int ret;

	ret = nvp6324_read(priv, NVP6324_BANK_DEVICE, NVP6324_REG_CHIP_ID, &id);
	if (ret)
		return ret;
	ret = nvp6324_read(priv, NVP6324_BANK_DEVICE, NVP6324_REG_REV_ID, &rev);
	if (ret)
		return ret;

	if (id != NVP6324_CHIP_ID_4PORT) {
		dev_err(dev, "unexpected chip id 0x%02x (expected 0x%02x)\n",
			id, NVP6324_CHIP_ID_4PORT);
		return -ENODEV;
	}

	dev_info(dev, "NVP6324 detected: id 0x%02x rev 0x%02x\n", id, rev);
	return 0;
}

/*
 * The programming below is ported verbatim, in the vendor reference's own call
 * order, for AHD20_1080P_25P / SINGLE_ENDED / 4-lane MIPI / UYVY. The order is
 * that of nvp6324_init() -> vd_set_all(): video_decoder_init -> mipi_tx_init ->
 * (per ch: vd_jaguar1_init_set [video table + cable-EQ] -> mipi_video_format_set)
 * -> arb_init -> disable_parallel. The color-bar test pattern (vd_pattern_enable)
 * is deliberately omitted. Register values that a later phase overwrites are left
 * in place (not pre-reconciled) so every line matches the vendor source and can
 * be checked against camdriver/nvp6324-regseq-verified.md, which cites the exact
 * jaguar1_*.c/.h file:line for each. Banks not covered by a NVP6324_BANK_* name
 * are written as literals with the reference name in a comment.
 */

/*
 * mipi_tx_init(): MIPI D-PHY PLL (0x40-0x43) + HS/CLK timing (0x10-0x1C), bank 0x21.
 * Each lane rate has its OWN timing block — the 0x10-0x1C values are NOT independent
 * of the PLL and must be programmed as a matched set (Nextchip jaguar1_mipi.c
 * mipi_tx_init() per-mclk blocks). The stock vendor rate is 1242 Mbps/lane (its
 * "FHD x4ch" setting); on this board that eye is marginal and produced a persistent
 * ~4500/s CRC-error floor + horizontal shear even for ONE 1080p25 stream. 594 Mbps
 * (its "HD x4ch" rate, ~3x the ~207 Mbps/lane a single 1080p25 UYVY stream needs)
 * gives CRC=0. Selected by the mipi_mclk param; pair it with a matching link_freq_idx
 * so the Cadence RX picks the right D-PHY band (1242->idx0/621MHz, 594->idx6/300MHz).
 * NB: 4x 1080p25 needs ~829 Mbps/lane, so a 4-camera build needs 1242 (or lower res).
 * The 0x44/0x49 latch pulse and the tail below are common to all rates.
 */
static const struct nvp6324_reg nvp6324_mipi_pll_1242[] = {
	{ 0x21, 0x40, 0xB4 }, { 0x21, 0x41, 0x00 }, { 0x21, 0x42, 0x03 }, { 0x21, 0x43, 0x43 },
	{ 0x21, 0x11, 0x08 }, { 0x21, 0x10, 0x13 }, { 0x21, 0x12, 0x0B }, { 0x21, 0x13, 0x12 },
	{ 0x21, 0x17, 0x02 }, { 0x21, 0x18, 0x12 }, { 0x21, 0x15, 0x07 }, { 0x21, 0x14, 0x2D },
	{ 0x21, 0x16, 0x0B }, { 0x21, 0x19, 0x09 }, { 0x21, 0x1A, 0x15 }, { 0x21, 0x1B, 0x11 },
	{ 0x21, 0x1C, 0x0E },
};
static const struct nvp6324_reg nvp6324_mipi_pll_756[] = {
	{ 0x21, 0x40, 0xDC }, { 0x21, 0x41, 0x10 }, { 0x21, 0x42, 0x03 }, { 0x21, 0x43, 0x43 },
	{ 0x21, 0x11, 0x05 }, { 0x21, 0x10, 0x0C }, { 0x21, 0x12, 0x07 }, { 0x21, 0x13, 0x0B },
	{ 0x21, 0x17, 0x01 }, { 0x21, 0x18, 0x0E }, { 0x21, 0x15, 0x04 }, { 0x21, 0x14, 0x1C },
	{ 0x21, 0x16, 0x07 }, { 0x21, 0x19, 0x06 }, { 0x21, 0x1A, 0x0D }, { 0x21, 0x1B, 0x0B },
	{ 0x21, 0x1C, 0x09 },
};
static const struct nvp6324_reg nvp6324_mipi_pll_594[] = {
	{ 0x21, 0x40, 0xCC }, { 0x21, 0x41, 0x10 }, { 0x21, 0x42, 0x03 }, { 0x21, 0x43, 0x43 },
	{ 0x21, 0x11, 0x04 }, { 0x21, 0x10, 0x0A }, { 0x21, 0x12, 0x06 }, { 0x21, 0x13, 0x09 },
	{ 0x21, 0x17, 0x01 }, { 0x21, 0x18, 0x0D }, { 0x21, 0x15, 0x04 }, { 0x21, 0x14, 0x16 },
	{ 0x21, 0x16, 0x05 }, { 0x21, 0x19, 0x05 }, { 0x21, 0x1A, 0x0A }, { 0x21, 0x1B, 0x08 },
	{ 0x21, 0x1C, 0x07 },
};
static const struct nvp6324_reg nvp6324_mipi_pll_378[] = {
	{ 0x21, 0x40, 0xDC }, { 0x21, 0x41, 0x20 }, { 0x21, 0x42, 0x03 }, { 0x21, 0x43, 0x43 },
	{ 0x21, 0x11, 0x03 }, { 0x21, 0x10, 0x07 }, { 0x21, 0x12, 0x04 }, { 0x21, 0x13, 0x06 },
	{ 0x21, 0x17, 0x01 }, { 0x21, 0x18, 0x0B }, { 0x21, 0x15, 0x02 }, { 0x21, 0x14, 0x0E },
	{ 0x21, 0x16, 0x04 }, { 0x21, 0x19, 0x03 }, { 0x21, 0x1A, 0x07 }, { 0x21, 0x1B, 0x06 },
	{ 0x21, 0x1C, 0x05 },
};

/* Common MIPI-TX tail (all rates): PLL latch pulse, frame options, per-VC datatype,
 * 4-lane enable. Order is load-bearing — 0x44/0x49 latches/locks the PLL. */
static const struct nvp6324_reg nvp6324_mipi_tail_regs[] = {
	{ 0x21, 0x44, 0x00 }, { 0x21, 0x49, 0xF3 }, { 0x21, 0x49, 0xF0 }, { 0x21, 0x44, 0x02 }, /* PLL latch pulse */
	{ 0x21, 0x08, 0x40 },					/* frame options */
	{ 0x21, 0x0F, 0x01 },					/* MIPI_TX_FRAME_CNT_EN */
	{ 0x21, 0x38, 0x1E }, { 0x21, 0x39, 0x1E }, { 0x21, 0x3A, 0x1E }, { 0x21, 0x3B, 0x1E }, /* VC0..3 datatype = YUV422 */
	{ 0x21, 0x07, 0x0F },					/* 4-lane enable */
	{ 0x21, 0x2D, 0x01 },
};

/* arb_init(): arbiter disable + config. The final arb_enable (bank0x20 0x00 =
 * en_param) is written separately in nvp6324_start() from vc_mask, so only the
 * VCs that carry a camera are enabled (vendor accumulates en_param |= 0x11<<ch;
 * all 4 = 0xFF). (jaguar1_mipi.c:66) */
static const struct nvp6324_reg nvp6324_arb_regs[] = {
	{ 0x20, 0x00, 0x00 },					/* arb_disable */
	{ 0x20, 0x40, 0x01 },
	{ 0x20, 0x0F, 0x00 },					/* = arb_dtype (YUV422) */
	{ 0x20, 0x0D, 0x01 },
	{ 0x20, 0x40, 0x00 },
};

/* video_decoder_init(): common one-time init. (mipi_dev_nvp6324.c:308) */
static int nvp6324_setup_common(struct nvp6324 *priv)
{
	static const struct nvp6324_reg tail[] = {
		{ 0x0A, 0x77, 0x8F }, { 0x0A, 0xF7, 0x8F },
		{ 0x0B, 0x77, 0x8F }, { 0x0B, 0xF7, 0x8F },
	};
	unsigned int i;
	int ret;

	for (i = 0; i <= 0x23; i++) {			/* bank4 0xA0..0xC3 = 0x24 */
		ret = nvp6324_write(priv, 0x04, 0xA0 + i, 0x24);
		if (ret)
			return ret;
	}
	for (i = 0; i < 4; i++) {			/* bank1 0xCC..0xCF = 0x64 (zeroed later) */
		ret = nvp6324_write(priv, 0x01, 0xCC + i, 0x64);
		if (ret)
			return ret;
	}
	ret = nvp6324_write(priv, 0x21, 0x07, 0x80);	/* MIPI reset assert */
	if (ret)
		return ret;
	ret = nvp6324_write(priv, 0x21, 0x07, 0x00);	/* MIPI reset release */
	if (ret)
		return ret;

	return nvp6324_write_regs(priv, tail, ARRAY_SIZE(tail));
}

static int nvp6324_setup_mipi(struct nvp6324 *priv)
{
	const struct nvp6324_reg *pll;
	size_t n;
	int ret;

	switch (mipi_mclk) {
	case 1242: pll = nvp6324_mipi_pll_1242; n = ARRAY_SIZE(nvp6324_mipi_pll_1242); break;
	case 756:  pll = nvp6324_mipi_pll_756;  n = ARRAY_SIZE(nvp6324_mipi_pll_756);  break;
	case 378:  pll = nvp6324_mipi_pll_378;  n = ARRAY_SIZE(nvp6324_mipi_pll_378);  break;
	case 594:
	default:   pll = nvp6324_mipi_pll_594;  n = ARRAY_SIZE(nvp6324_mipi_pll_594);  break;
	}

	ret = nvp6324_write_regs(priv, pll, n);
	if (ret)
		return ret;
	return nvp6324_write_regs(priv, nvp6324_mipi_tail_regs,
				  ARRAY_SIZE(nvp6324_mipi_tail_regs));
}

/*
 * vd_jaguar1_init_set() video table for one channel (AHD20_1080P_25P). The ch0
 * addresses are transformed per channel: bank-0 families take reg+ch, the
 * per-channel decoder bank is 0x05+ch, bank-1 clocks take reg+ch, bank-9 H-scaler
 * takes reg+0x20*ch (FSC_EXT reg+4*ch, others reg+ch), bank-13 det-en clears
 * bits ch and ch+4. See nvp6324-regseq-verified.md §1/§2/§3. (jaguar1_video.c:790)
 *
 * CLK_AUTO (seq1 bank1 0x7C) is intentionally NOT written: the working vendor
 * sequence never touches bank1 0x7C (a stale bank-cache bug diverts that RMW to
 * bank 0x13). Revisit only if clock/auto-format problems appear.
 */
static int nvp6324_setup_channel(struct nvp6324 *priv, unsigned int ch)
{
	const u8 dec = NVP6324_BANK_CH(ch);		/* per-channel decoder bank */
	const u8 detm = (1 << ch) | (1 << (ch + 4));	/* bank-13 det-en bits */
	int ret;

#define W(bank, reg, val) do { \
		ret = nvp6324_write(priv, (bank), (reg), (val)); \
		if (ret) \
			return ret; \
	} while (0)
#define U(bank, reg, mask, val) do { \
		ret = nvp6324_update_bits(priv, (bank), (reg), (mask), (val)); \
		if (ret) \
			return ret; \
	} while (0)

	/* D0 each-set; D1 analog input (SINGLE_ENDED). */
	W(0x00, 0x00 + ch, 0x10);
	W(0x00, 0x18 + ch, 0x13);			/* EX_CBAR_ON */
	W(dec, 0x00, 0xD0);
	W(dec, 0x01, 0xA2);				/* CML (EQ -> 0x2C) */
	W(dec, 0x92, 0x00);

	/* D2 VO glue + manual mode (bank-13 det-en clears). */
	W(0x01, 0xEC + ch, 0x00);			/* yc_merge */
	W(0x01, 0xC8 + ch, 0x30);			/* vport_out_sel = 4-mux */
	U(0x13, 0x30, detm, 0x00);
	U(0x13, 0x31, detm, 0x00);
	U(0x13, 0x32, 1 << ch, 0x00);

	/* D3 seq1 (CLK_AUTO omitted, see above). */
	W(dec, 0x32, 0x10);				/* NOVIDEO_DET_A */
	W(dec, 0xB9, 0xB2);				/* HAFC_LPF_SEL (EQ -> 0x72) */
	U(0x13, 0x30, detm, 0x00);
	U(0x13, 0x31, detm, 0x00);
	U(0x13, 0x32, 1 << ch, 0x00);
	W(0x09, 0x44 + ch, 0x00);			/* FSC_EXT_EN */
	W(dec, 0x6E, 0x00);
	W(dec, 0x6F, 0x00);

	/* D4 vafe seq2. */
	W(dec, 0x00, 0xD0);
	W(dec, 0x02, 0x0C);
	W(dec, 0x1E, 0x00);
	W(dec, 0x58, 0x00);				/* EQ -> 0x77 */
	W(dec, 0x59, 0x00);
	W(dec, 0x5A, 0x00);
	W(dec, 0x5B, 0x41);
	W(dec, 0x5C, 0x78);
	W(dec, 0x94, 0x00);
	W(dec, 0x95, 0x00);
	W(dec, 0x65, 0x80);				/* EQ -> 0x00 */

	/* D5 format seq3 (ahd_mode = 0x03 => 1080p25). */
	W(0x00, 0x10 + ch, 0x20);			/* VD_FMT */
	W(0x00, 0x0C + ch, 0x00);			/* SPL_MODE */
	W(0x00, 0x04 + ch, 0x00);			/* SD_MODE */
	W(0x00, 0x08 + ch, ahd_mode);			/* AHD_MODE (25p/30p param) */
	U(dec, 0x69, 0x01, 0x00);			/* SD_FREQ_SEL[0] */
	W(dec, 0x62, 0x20);				/* SYNC_SEL */

	/* D6 chroma seq4. */
	W(0x00, 0x5C + ch, 0x82);			/* PAL_CM_OFF */
	W(dec, 0x28, 0x90);				/* S_POINT (EQ -> 0x80) */
	W(dec, 0x25, 0xDC);				/* FSC_LOCK_MODE */
	W(dec, 0x90, 0x01);				/* COMB_MODE */

	/* D7 H-timing seq5. */
	W(0x00, 0x68 + ch, 0x48);			/* H_DLY_LSB */
	W(0x00, 0x6C + ch, 0x00);			/* H_DLY_MSB */
	W(0x00, 0x60 + ch, 0x10);			/* Y_DLY */
	W(0x00, 0x78 + ch, 0x80);			/* V_BLK_END_A (EQ -> 0x21) */
	U(dec, 0x38, 0x10, 0x10);			/* H_MASK_ON[4] */
	U(dec, 0x38, 0x0F, 0x03);			/* H_MASK_SEL[3:0] (net 0x13) */
	W(0x00, 0x64 + ch, 0x00);			/* V_BLK_END_B (EQ -> 0x05) */
	U(0x00, 0x14 + ch, 0x10, 0x00);			/* FLD_INV[4] */
	W(dec, 0x64, 0x00);				/* MEM_RDP */
	W(dec, 0x47, 0xEE);				/* SYNC_RS */
	W(dec, 0xA9, 0x00);				/* V_BLK_END_B dup */

	/* D8 H-scaler seq6. */
	U(dec, 0x53, 0x0C, 0x00);			/* LINEMEM_MD[3:2] */
	W(0x09, 0x96 + 0x20 * ch, 0x00);		/* H_DOWN_SCALER */
	W(0x09, 0x97 + 0x20 * ch, 0x00);		/* H_SCALER_MODE */
	W(0x09, 0x98 + 0x20 * ch, 0x00);		/* REF_BASE_LSB */
	W(0x09, 0x99 + 0x20 * ch, 0x00);		/* REF_BASE_MSB */
	W(0x09, 0x9E + 0x20 * ch, 0x00);		/* H_SCALER_ACTIVE */

	/* D9 HPLL seq7. */
	W(dec, 0x50, 0xC6);				/* HPLL_MASK_ON */
	W(dec, 0xB8, 0x39);				/* HAFC_OP_MD */
	W(dec, 0xBB, 0x0F);				/* HAFC_BYP_TH_E */
	W(dec, 0xB7, 0xFC);				/* HAFC_BYP_TH_S */

	/* D10 color seq8. */
	W(0x00, 0x20 + ch, 0x00);			/* BRIGHTNESS */
	W(0x00, 0x24 + ch, 0x86);			/* CONTRAST */
	W(0x00, 0x28 + ch, 0x80);			/* BLACK_LEVEL */
	W(0x00, 0x58 + ch, 0x80);			/* SATURATION_A */
	W(0x00, 0x40 + ch, 0x00);			/* HUE */
	W(0x00, 0x44 + ch, 0x00);			/* U_GAIN */
	W(0x00, 0x48 + ch, 0x00);			/* V_GAIN */
	W(0x00, 0x4C + ch, 0xF8);			/* U_OFFSET (EQ -> 0xFE) */
	W(0x00, 0x50 + ch, 0xF8);			/* V_OFFSET (EQ -> 0xFB) */
	W(dec, 0x2B, 0xA8);				/* SATURATION_B */
	W(dec, 0x24, 0x2A);				/* BURST_DEC_A */
	W(dec, 0x5F, 0x00);				/* BURST_DEC_B */
	W(dec, 0xD1, 0x30);				/* BURST_DEC_C */
	W(0x09, 0x44 + ch, 0x00);			/* FSC_EXT_EN */
	W(0x09, 0x50 + 4 * ch, 0x30);			/* FSC_EXT_VAL_7_0 */
	W(0x09, 0x51 + 4 * ch, 0x6F);			/* FSC_EXT_VAL_15_8 */
	W(0x09, 0x52 + 4 * ch, 0x67);			/* FSC_EXT_VAL_23_16 */
	W(0x09, 0x53 + 4 * ch, 0x48);			/* FSC_EXT_VAL_31_24 */
	W(dec, 0x26, 0x40);				/* FSC_LOCK_SENSE */
	W(dec, 0xB8, 0x39);				/* HPLL_MASK_END */
	W(0x09, 0x40 + ch, 0x00);			/* FSC_DET_MODE */
	W(dec, 0xB5, 0x80);

	/* D11 port channel-id: keep bit4, set CHID (=0) in the rest. */
	U(0x00, 0x14 + ch, 0xEF, 0x00);

	/* D12 clock seq9. */
	W(0x01, 0x84 + ch, 0x44);			/* CLK_ADC (EQ -> 0x04) */
	W(0x01, 0x88 + ch, 0x01);			/* CLK_PRE */
	W(0x01, 0x8C + ch, 0x02);			/* CLK_POST */
	W(dec, 0x01, 0x2C);				/* CML_MODE */
	W(dec, 0x05, 0x24);				/* AGC_OP */
	W(dec, 0x1D, 0x0C);				/* G_SEL */

#undef W
#undef U
	return 0;
}

/*
 * video_input_eq_val_set(): cable equalizer for one channel, CABLE_A /
 * SINGLE_ENDED / STAGE_0 (index [0]). Runs per channel after the video table,
 * overwriting the marked seq values. EQ bank-A registers live at bank
 * 0x0A+(ch/2), reg base+(ch&1)*0x80. (jaguar1_video_eq.c:166) — see §4.
 */
static int nvp6324_setup_channel_eq(struct nvp6324 *priv, unsigned int ch)
{
	const u8 dec = NVP6324_BANK_CH(ch);
	const u8 eqb = 0x0A + (ch / 2);			/* EQ bank A/B */
	const u8 eqo = (ch & 1) * 0x80;			/* EQ reg offset */
	int ret;

#define W(bank, reg, val) do { \
		ret = nvp6324_write(priv, (bank), (reg), (val)); \
		if (ret) \
			return ret; \
	} while (0)

	/* base */
	W(dec, 0x65, 0x00);				/* EQ_BYPASS */
	W(dec, 0x58, 0x77);				/* EQ_BAND_SEL */
	W(dec, 0x5C, 0x78);				/* EQ_GAIN_SEL */
	W(eqb, 0x3D + eqo, 0x00);			/* DEQ_A_ON */
	W(eqb, 0x3C + eqo, 0x00);			/* DEQ_A_SEL */

	/* coeff (deqA_01..12) */
	W(eqb, 0x30 + eqo, 0xAC);
	W(eqb, 0x31 + eqo, 0x78);
	W(eqb, 0x32 + eqo, 0x17);
	W(eqb, 0x33 + eqo, 0xC1);
	W(eqb, 0x34 + eqo, 0x40);
	W(eqb, 0x35 + eqo, 0x00);
	W(eqb, 0x36 + eqo, 0xC3);
	W(eqb, 0x37 + eqo, 0x0A);
	W(eqb, 0x38 + eqo, 0x00);
	W(eqb, 0x39 + eqo, 0x02);
	W(eqb, 0x3A + eqo, 0x00);
	W(eqb, 0x3B + eqo, 0xB2);

	/* color */
	W(0x00, 0x24 + ch, 0x86);			/* contrast */
	W(0x00, 0x30 + ch, 0x00);			/* y_peaking_mode */
	W(0x00, 0x34 + ch, 0x00);			/* y_fir_mode */
	W(dec, 0x31, 0x82);				/* c_filter */
	W(0x00, 0x5C + ch, 0x82);			/* pal_cm_off */
	W(0x00, 0x40 + ch, 0x00);			/* hue */
	W(0x00, 0x44 + ch, 0x00);			/* u_gain */
	W(0x00, 0x48 + ch, 0x00);			/* v_gain */
	W(0x00, 0x4C + ch, 0xFE);			/* u_offset */
	W(0x00, 0x50 + ch, 0xFB);			/* v_offset */
	W(0x00, 0x28 + ch, 0x80);			/* black_level */
	W(dec, 0x27, 0x57);				/* acc_ref */
	W(dec, 0x28, 0x80);				/* cti_delay */
	W(dec, 0x2B, 0xA8);				/* saturation_b */
	W(dec, 0x24, 0x2A);				/* burst_dec_a */
	W(dec, 0x5F, 0x00);				/* burst_dec_b */
	W(dec, 0xD1, 0x30);				/* burst_dec_c */
	W(dec, 0xD5, 0x80);				/* c_option */
	W(eqb, 0x25 + eqo, 0x10);			/* y_filter_b */
	W(eqb, 0x27 + eqo, 0x1E);			/* y_filter_b_sel */

	/* timing_a */
	W(0x00, 0x68 + ch, 0x48);			/* h_delay_a */
	W(dec, 0x38, 0x13);				/* h_delay_b (full write; net 0x13) */
	ret = nvp6324_update_bits(priv, 0x00, 0x6C + ch, 0x0F, 0x00); /* h_delay_c[3:0] */
	if (ret)
		return ret;
	W(0x00, 0x64 + ch, 0x05);			/* y_delay */

	/* clk */
	W(0x01, 0x84 + ch, 0x04);			/* clk_adc */
	W(0x01, 0x88 + ch, 0x01);			/* clk_adc_pre */
	W(0x01, 0x8C + ch, 0x02);			/* clk_adc_post */

	/* timing_b */
	W(0x09, 0x96 + 0x20 * ch, 0x00);
	W(0x09, 0x97 + 0x20 * ch, 0x00);
	W(0x09, 0x98 + 0x20 * ch, 0x00);
	W(0x09, 0x99 + 0x20 * ch, 0x00);
	W(0x09, 0x9A + 0x20 * ch, 0x00);
	W(0x09, 0x9B + 0x20 * ch, 0x00);
	W(0x09, 0x9C + 0x20 * ch, 0x00);
	W(0x09, 0x9D + 0x20 * ch, 0x00);
	W(0x09, 0x9E + 0x20 * ch, 0x00);
	W(0x09, 0x40 + ch, 0x00);			/* pn_auto */
	W(dec, 0x90, 0x01);				/* comb_mode */
	W(dec, 0xB9, 0x72);				/* h_pll_op_a */
	W(dec, 0x57, 0x00);				/* mem_path */
	W(dec, 0x25, 0xDC);				/* fsc_lock_speed */
	W(0x00, 0x04 + ch, 0x00);			/* sd_mode */
	W(0x00, 0x08 + ch, ahd_mode);			/* ahd_mode (25p/30p param) */
	W(0x00, 0x0C + ch, 0x00);			/* spl_mode */
	W(0x00, 0x78 + ch, 0x21);			/* vblk_end */
	W(dec, 0x1D, 0x0C);				/* afe_g_sel */
	W(dec, 0x01, 0x2C);				/* afe_ctr_clp */
	W(dec, 0x05, 0x24);				/* d_agc_option */

#undef W
	return 0;
}

/*
 * mipi_video_format_set(): per-channel MIPI glue. arb_scale = 0 (FHD, no
 * downscale) and mipi_frame_opt = 0. The MIPI frame-opt nibble is reg 0x3E for
 * ch0/1, 0x3F for ch2/3; low nibble for even ch, high nibble for odd ch. The
 * en_param the vendor accumulates here is instead applied as the fixed 0xFF in
 * arb_init (all four VCs enabled). (jaguar1_mipi.c:118) — see §E.
 */
static int nvp6324_setup_channel_mipi(struct nvp6324 *priv, unsigned int ch)
{
	const u8 fo_reg = (ch < 2) ? 0x3E : 0x3F;
	const u8 fo_mask = (ch & 1) ? 0xF0 : 0x0F;
	int ret;

	ret = nvp6324_update_bits(priv, 0x21, fo_reg, fo_mask, 0x00);
	if (ret)
		return ret;
	return nvp6324_update_bits(priv, 0x20, 0x01, 0x3 << (2 * ch), 0x00);
}

static int nvp6324_start(struct nvp6324 *priv)
{
	unsigned int ch;
	u8 en_param = 0;
	int ret;

	ret = nvp6324_setup_common(priv);
	if (ret)
		return ret;
	ret = nvp6324_setup_mipi(priv);
	if (ret)
		return ret;

	for (ch = 0; ch < NVP6324_NUM_CHANNELS; ch++) {
		ret = nvp6324_setup_channel(priv, ch);
		if (ret)
			return ret;
		ret = nvp6324_setup_channel_eq(priv, ch);
		if (ret)
			return ret;
		ret = nvp6324_setup_channel_mipi(priv, ch);
		if (ret)
			return ret;
	}

	ret = nvp6324_write_regs(priv, nvp6324_arb_regs,
				 ARRAY_SIZE(nvp6324_arb_regs));
	if (ret)
		return ret;

	/* Enable only the VCs that carry a camera (see vc_mask): enabling a
	 * signal-less VC makes the arbiter split each frame across two DMA
	 * buffers on the TI receiver. en_param accumulates 0x11<<ch per VC. */
	for (ch = 0; ch < NVP6324_NUM_CHANNELS; ch++)
		if (vc_mask & BIT(ch))
			en_param |= 0x11u << ch;
	ret = nvp6324_write(priv, NVP6324_BANK_ARB, NVP6324_REG_ARB_ENABLE,
			    en_param);
	if (ret)
		return ret;

	/* disable_parallel(): bank1 0xC8..0xCF = 0x00. (jaguar1_mipi.c:244) */
	for (ch = 0; ch < 8; ch++) {
		ret = nvp6324_write(priv, 0x01, 0xC8 + ch, 0x00);
		if (ret)
			return ret;
	}

	return 0;
}

static int nvp6324_stop(struct nvp6324 *priv)
{
	/* Disable the arbiter (stops all VCs on the link). */
	return nvp6324_write(priv, NVP6324_BANK_ARB, NVP6324_REG_ARB_ENABLE,
			     0x00);
}

/* ------------------------------------------------------------------ *
 * V4L2 sub-device: pad / streams ops.
 * ------------------------------------------------------------------ */

static int nvp6324_enum_mbus_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index)
		return -EINVAL;

	code->code = NVP6324_MBUS_CODE;
	return 0;
}

static int nvp6324_set_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *state,
			   struct v4l2_subdev_format *format)
{
	struct v4l2_mbus_framefmt *fmt;

	/* Source formats mirror the sink; no transcoding. Fixed geometry. */
	if (format->pad == NVP6324_PAD_SOURCE)
		return v4l2_subdev_get_fmt(sd, state, format);

	format->format.code = NVP6324_MBUS_CODE;
	format->format.width = NVP6324_ACTIVE_WIDTH;
	format->format.height = NVP6324_ACTIVE_HEIGHT;
	format->format.field = V4L2_FIELD_NONE;
	format->format.colorspace = V4L2_COLORSPACE_SMPTE170M;

	fmt = v4l2_subdev_state_get_format(state, format->pad, format->stream);
	if (!fmt)
		return -EINVAL;
	*fmt = format->format;

	/* Keep the routed source stream's format in sync. */
	fmt = v4l2_subdev_state_get_opposite_stream_format(state, format->pad,
							   format->stream);
	if (fmt)
		*fmt = format->format;

	return 0;
}

static int nvp6324_get_frame_desc(struct v4l2_subdev *sd, unsigned int pad,
				  struct v4l2_mbus_frame_desc *fd)
{
	struct v4l2_subdev_state *state;
	struct v4l2_subdev_route *route;

	if (pad != NVP6324_PAD_SOURCE)
		return -EINVAL;

	memset(fd, 0, sizeof(*fd));
	fd->type = V4L2_MBUS_FRAME_DESC_TYPE_CSI2;

	state = v4l2_subdev_lock_and_get_active_state(sd);

	for_each_active_route(&state->routing, route) {
		struct v4l2_mbus_frame_desc_entry *entry;

		if (route->source_pad != pad)
			continue;

		entry = &fd->entry[fd->num_entries++];
		entry->stream = route->source_stream;
		entry->flags = 0;
		entry->pixelcode = NVP6324_MBUS_CODE;
		/*
		 * One virtual channel per stream. The VC the chip tags each
		 * channel with must match this; confirm/lock the VC-assignment
		 * register from the N4 datasheet (TODO M2). Default 1:1.
		 */
		entry->bus.csi2.vc = route->source_stream;
		entry->bus.csi2.dt = NVP6324_CSI2_DT_YUV422_8B;
	}

	v4l2_subdev_unlock_state(state);
	return 0;
}

static int _nvp6324_set_routing(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_krouting *routing)
{
	static const struct v4l2_mbus_framefmt format = {
		.code = NVP6324_MBUS_CODE,
		.width = NVP6324_ACTIVE_WIDTH,
		.height = NVP6324_ACTIVE_HEIGHT,
		.field = V4L2_FIELD_NONE,
		.colorspace = V4L2_COLORSPACE_SMPTE170M,
	};
	int ret;

	/*
	 * 4 sink pads (analog inputs) -> 1 source pad carrying 4 streams, so
	 * the source pad legitimately mixes streams from different sink pads:
	 * use NO_SINK_STREAM_MIX (each sink's streams stay together), NOT
	 * NO_STREAM_MIX (which also forbids source-side mixing and rejects this
	 * topology with -EINVAL). Same flags as the ds90ub960 deserializer.
	 */
	ret = v4l2_subdev_routing_validate(sd, routing,
					   V4L2_SUBDEV_ROUTING_ONLY_1_TO_1 |
					   V4L2_SUBDEV_ROUTING_NO_SINK_STREAM_MIX);
	if (ret)
		return ret;

	return v4l2_subdev_set_routing_with_fmt(sd, state, routing, &format);
}

static int nvp6324_set_routing(struct v4l2_subdev *sd,
			       struct v4l2_subdev_state *state,
			       enum v4l2_subdev_format_whence which,
			       struct v4l2_subdev_krouting *routing)
{
	struct nvp6324 *priv = sd_to_nvp6324(sd);

	if (which == V4L2_SUBDEV_FORMAT_ACTIVE && priv->enabled_streams)
		return -EBUSY;

	return _nvp6324_set_routing(sd, state, routing);
}

static int nvp6324_enable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state, u32 pad,
				  u64 streams_mask)
{
	struct nvp6324 *priv = sd_to_nvp6324(sd);
	int ret = 0;

	if (pad != NVP6324_PAD_SOURCE)
		return -EINVAL;

	mutex_lock(&priv->lock);
	/*
	 * Normally the chip is programmed as a whole on the first enabled stream.
	 * With program_at_probe it is already up and transmitting continuously
	 * (brought up before the RX D-PHY calibrates), so enable_streams only
	 * tracks state — it must NOT reprogram/glitch the live TX clock lane.
	 */
	if (!priv->enabled_streams && !priv->hw_running) {
		mutex_unlock(&priv->lock);
		ret = nvp6324_start(priv);
		mutex_lock(&priv->lock);
		if (ret)
			goto out;
	}
	priv->enabled_streams |= streams_mask;
out:
	mutex_unlock(&priv->lock);
	return ret;
}

static int nvp6324_disable_streams(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *state, u32 pad,
				   u64 streams_mask)
{
	struct nvp6324 *priv = sd_to_nvp6324(sd);
	int ret = 0;

	if (pad != NVP6324_PAD_SOURCE)
		return -EINVAL;

	mutex_lock(&priv->lock);
	priv->enabled_streams &= ~streams_mask;
	/*
	 * With program_at_probe the TX is kept running across stream off/on so the
	 * RX never has to re-lock against a down/reset clock lane — so do NOT stop
	 * the chip here. Otherwise, stop on the last disabled stream as usual.
	 */
	if (!priv->enabled_streams && !priv->hw_running) {
		mutex_unlock(&priv->lock);
		ret = nvp6324_stop(priv);
		mutex_lock(&priv->lock);
	}
	mutex_unlock(&priv->lock);
	return ret;
}

static int nvp6324_init_state(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *state)
{
	struct v4l2_subdev_route routes[NVP6324_NUM_CHANNELS];
	struct v4l2_subdev_krouting routing = {
		.num_routes = NVP6324_NUM_CHANNELS,
		.routes = routes,
	};
	unsigned int i;

	/* Default 1:1 routing: analog input i -> source stream i (== VC i). */
	for (i = 0; i < NVP6324_NUM_CHANNELS; i++) {
		routes[i] = (struct v4l2_subdev_route){
			.sink_pad = NVP6324_PAD_SINK0 + i,
			.sink_stream = 0,
			.source_pad = NVP6324_PAD_SOURCE,
			.source_stream = i,
			.flags = V4L2_SUBDEV_ROUTE_FL_ACTIVE,
		};
	}

	return _nvp6324_set_routing(sd, state, &routing);
}

static const struct v4l2_subdev_pad_ops nvp6324_pad_ops = {
	.enum_mbus_code	= nvp6324_enum_mbus_code,
	.get_fmt	= v4l2_subdev_get_fmt,
	.set_fmt	= nvp6324_set_fmt,
	.get_frame_desc	= nvp6324_get_frame_desc,
	.set_routing	= nvp6324_set_routing,
	.enable_streams	= nvp6324_enable_streams,
	.disable_streams = nvp6324_disable_streams,
};

static const struct v4l2_subdev_ops nvp6324_subdev_ops = {
	.pad = &nvp6324_pad_ops,
};

static const struct v4l2_subdev_internal_ops nvp6324_internal_ops = {
	.init_state = nvp6324_init_state,
};

static const struct media_entity_operations nvp6324_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

/* ------------------------------------------------------------------ *
 * Probe / remove.
 * ------------------------------------------------------------------ */

static const struct regmap_config nvp6324_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
};

static int nvp6324_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct nvp6324 *priv;
	unsigned int i;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->client = client;
	priv->cur_bank = -1;
	mutex_init(&priv->lock);

	priv->regmap = devm_regmap_init_i2c(client, &nvp6324_regmap_config);
	if (IS_ERR(priv->regmap)) {
		ret = PTR_ERR(priv->regmap);
		goto err_mutex;
	}

	ret = nvp6324_detect(priv);
	if (ret)
		goto err_mutex;

	v4l2_i2c_subdev_init(&priv->sd, client, &nvp6324_subdev_ops);
	priv->sd.internal_ops = &nvp6324_internal_ops;
	priv->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE | V4L2_SUBDEV_FL_STREAMS;
	priv->sd.entity.function = MEDIA_ENT_F_VID_IF_BRIDGE;
	priv->sd.entity.ops = &nvp6324_entity_ops;

	/*
	 * V4L2_CID_LINK_FREQ — required by the cdns-csi2rx bridge to configure
	 * the external D-PHY (this subdev is a multi-format/multi-stream CSI-2
	 * source). Read-only single-entry menu at the fixed link frequency.
	 */
	if (link_freq_idx < 0 || link_freq_idx >= (int)ARRAY_SIZE(nvp6324_link_freqs))
		link_freq_idx = 0;
	v4l2_ctrl_handler_init(&priv->ctrl_handler, 1);
	v4l2_ctrl_new_int_menu(&priv->ctrl_handler, NULL, V4L2_CID_LINK_FREQ,
			       ARRAY_SIZE(nvp6324_link_freqs) - 1, link_freq_idx,
			       nvp6324_link_freqs);
	priv->sd.ctrl_handler = &priv->ctrl_handler;
	if (priv->ctrl_handler.error) {
		ret = priv->ctrl_handler.error;
		goto err_ctrl;
	}

	for (i = 0; i < NVP6324_NUM_CHANNELS; i++)
		priv->pads[i].flags = MEDIA_PAD_FL_SINK;
	priv->pads[NVP6324_PAD_SOURCE].flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&priv->sd.entity, NVP6324_NUM_PADS,
				     priv->pads);
	if (ret)
		goto err_ctrl;

	ret = v4l2_subdev_init_finalize(&priv->sd);
	if (ret)
		goto err_entity;

	ret = v4l2_async_register_subdev(&priv->sd);
	if (ret) {
		dev_err(dev, "failed to register async subdev: %d\n", ret);
		goto err_finalize;
	}

	/*
	 * Bring the MIPI TX up now so the clock lane is stable before the
	 * cdns-csi2rx RX D-PHY is ever powered/calibrated (see program_at_probe).
	 * A failure here is non-fatal: log it and continue; a later stream-on
	 * will program the chip the old way.
	 */
	if (program_at_probe) {
		ret = nvp6324_start(priv);
		if (ret) {
			dev_warn(dev, "program_at_probe: nvp6324_start failed: %d (will program at stream-on)\n",
				 ret);
			ret = 0;
		} else {
			priv->hw_running = true;
			dev_info(dev, "MIPI TX brought up at probe (continuous)\n");
		}
	}

	return 0;

err_finalize:
	v4l2_subdev_cleanup(&priv->sd);
err_entity:
	media_entity_cleanup(&priv->sd.entity);
err_ctrl:
	v4l2_ctrl_handler_free(&priv->ctrl_handler);
err_mutex:
	mutex_destroy(&priv->lock);
	return ret;
}

static void nvp6324_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct nvp6324 *priv = sd_to_nvp6324(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&priv->ctrl_handler);
	mutex_destroy(&priv->lock);
}

static const struct of_device_id nvp6324_of_match[] = {
	{ .compatible = "nextchip,nvp6324" },
	{ }
};
MODULE_DEVICE_TABLE(of, nvp6324_of_match);

static struct i2c_driver nvp6324_i2c_driver = {
	.driver = {
		.name = "nvp6324",
		.of_match_table = nvp6324_of_match,
	},
	.probe = nvp6324_probe,
	.remove = nvp6324_remove,
};
module_i2c_driver(nvp6324_i2c_driver);

MODULE_DESCRIPTION("Nextchip NVP6324 4-channel AHD-to-MIPI-CSI2 decoder driver");
MODULE_LICENSE("GPL");
