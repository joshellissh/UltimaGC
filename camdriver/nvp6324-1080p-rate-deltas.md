# NVP6324 (Jaguar1 "N4") — 1080-line format rate DELTAS vs AHD20_1080P_25P

Companion to `nvp6324-regseq-verified.md` (which has the full verified 25P sequence). This
doc gives ONLY the register deltas needed to sweep the other 1080-line formats. Source dir:
`.../vin/modules/sensor/nvp6324/` (`nvp6324_2/` ignored). ch0 values; use §2 of the 25P doc
for per-channel arithmetic. All hex unless noted.

**Bottom line up front:** Among the 1080-line AHD formats, only **1080P_30P** is a real,
usable alternate, and it differs from 1080P_25P by exactly **one net register**:
`{0x00, 0x08+ch}: 0x03 → 0x02` (AHD_MODE). The `1080P_60P` / `1080P_50P` enums are
**unpopulated "For Test" stubs** — programming them writes an all-zero video config and will
NOT produce video. There is **no 1080I** format in the AHD enum. The MIPI TX PLL is fixed at
1.242 Gbps for all formats (no 60fps high-rate branch exists).

---

## 1. Every 1080-line format enum the vendor supports

From `NC_VIVO_CH_FORMATDEF` (`jaguar1_common.h:172‑175`):

| Enum | line | Status |
|---|---|---|
| `AHD20_1080P_60P` | :172 | **"For Test" — no table data (stub, non-functional)** |
| `AHD20_1080P_50P` | :173 | **"For Test" — no table data (stub, non-functional)** |
| `AHD20_1080P_30P` | :174 | real; has a `nvp6324_init()` case at `nvp6324_mipi_driver.c:440` |
| `AHD20_1080P_25P` | :175 | real; the working baseline (`nvp6324_mipi_driver.c:435`) |

- **No `AHD20_1080I` / interlaced 1080 format exists** anywhere in the enum. The `AHD30_*`
  group starts at `AHD30_4M` (1440-line), so there is no AHD30 1080-line format either.
- EQ-table format enum `NC_JAGUAR1_EQ` has SINGLE_ENDED/DIFFERENTIAL rows **only** for 30P
  and 25P (`jaguar1_common.h:312‑315`) — none for 60P/50P.
- (Out of AHD scope but also 1920×1080: `TVI_FHD_30P`/`TVI_FHD_25P` (`:204‑205`) and
  `CVI_FHD_30P`/`CVI_FHD_25P` (`:222‑223`) — different signal standards, not AHD.)

---

## 2. AHD20_1080P_30P — complete delta vs AHD20_1080P_25P

### 2a. The only NET register change

| bank/reg (ch0) | 25P → 30P | source |
|---|---|---|
| `{0x00, 0x08}` AHD_MODE | **0x03 → 0x02** | see below |

`AHD_MODE` (bank0 reg `0x08+ch`) is written **twice**, both table-driven, both agreeing:
1. seq3 `vd_vi_format_set_seq3` — `jaguar1_video.c:244`; VI-table field `ahd_mode`:
   30P = `0x02` (`jaguar1_video_table.h:729`), 25P = `0x03` (`:811`).
2. EQ timing_b `__eq_timing_b_set_value` — `jaguar1_video_eq.c:156`; EQ-table field
   `ahd_mode[0]`: 30P = `0x2` (`jaguar1_cableA_video_eq_table.h:731`), 25P = `0x3` (`:822`).

**Per-channel:** bank-0 color/format family → reg = `0x08 + ch` (25P-doc §2). Value identical
across channels. So for all-4-ch 30P, write `{0x00, 0x08}=0x02, {0x00,0x09}=0x02,
{0x00,0x0A}=0x02, {0x00,0x0B}=0x02`. Everything else stays exactly as the 25P sequence.

### 2b. Table fields that DIFFER but have NO net effect (do not chase these)

Two more VI-table fields differ between the rows, but the EQ pass (which runs later, per
25P-doc D14) rewrites both registers to the **same** value for 30P and 25P:

| bank/reg | VI-table diff (25P→30P) | EQ overwrite (both fmts) | net |
|---|---|---|---|
| `{0x05, 0xD1}` BURST_DEC_C | `0x30 → 0x1E` (VI: `video.c:474`; table 30P `:725`=`30` decimal, 25P `:807`=`0x30`) | `0x30` (EQ color, table 30P `:697` / 25P `:788` both `0x30`) | **0x30 (no change)** |
| `{0x05, 0x38}` (H_MASK / H_DELAY_B) | h_mask_sel `0x03 → 0x04` (VI seq5 RMW `video.c:346`; table 30P `:741` / 25P `:823`) → RMW net `0x13→0x14` | full 8-bit write `0x13` (EQ timing_a; table h_delay_b 30P `:705` / 25P `:796` both `0x13`) | **0x13 (no change)** |

> Note: the 30P VI `burst_dec_c = 30` (decimal `0x1E`, `jaguar1_video_table.h:725`) is almost
> certainly a vendor typo for `0x30` — but it is dead either way (EQ overwrites it).

### 2c. Everything else confirmed IDENTICAL for 30P

- **VI row** (`vi_param = __NC_VD_VI_Init_Val_Get(fmt)`, `video.c:804`): mechanically diffed all
  70 fields of `[AHD20_1080P_30P]` (`:691‑771`) vs `[AHD20_1080P_25P]` (`:773‑853`) — only the
  three fields above differ. All AFE (`ref_vol`, `gain`, `clk_sel`, …), color, H-timing, HPLL
  (`hpll_mask_on`, `hafc_*`), clock (`clk_adc`, …), `cml_mode`/`agc_op`/`g_sel`/`sync_sel`,
  `video_format`(0x20) fields are identical.
- **VO row**: format-independent — `vo_param = __NC_VD_VO_Init_Val_Get(AHD20_1080P_30P)` is
  **hardcoded** to the 30P VO row regardless of `fmt` (`video.c:805`). No delta.
- **EQ row** (`[AHD20_1080P_30P_SINGLE_ENDED]` `:650‑739` vs `[AHD20_1080P_25P_SINGLE_ENDED]`
  `:741‑830`, index `[0]`): mechanically diffed all 65 fields — the **only** difference is
  `ahd_mode[0]` (already counted in 2a). base/coeff/color/timing_a/clk/timing_b otherwise
  identical.
- **`decoder_mipi_fmtdef` / `mipi_video_format_set`**: `[AHD20_1080P_30P]`
  (`jaguar1_mipi_table.h:84‑86`) and `[AHD20_1080P_25P]` (`:88‑90`) are identical —
  `arb_scale=0x00, mipi_frame_opt=0x00`. **No delta.**
- **`mipi_tx_init`**: takes `dev_num` only (not format); the active `#if 1` branch
  (`jaguar1_mipi.c:161`) is a fixed 1080P/1242 MHz PLL. **Confirmed format-independent — no
  change for 30P.**
- **Format-specific block** (`video.c` `if/else if` chain ~`:840‑861`): checks
  `AHD20_720P_960P_30P/25P`, `AHD20_SD_H960_2EX_Btype_PAL`, `SD_SH720/H1440` — **both** 1080P_30P
  and 25P fall to the empty `else`. No writes. seq8's only fmt branches
  (`fmt==TVI_5M_12_5P`, `fmt==H960_2EX_Btype_*`) also miss 1080P → same `else` for both.
- **Coax** (control channel, **not needed for video**): the coax table rows do differ —
  `rx_area 0x05→0x06`, `tx_line_pos0 0x0D→0x0E` (`jaguar1_coax_table.h` 30P `:902` vs 25P `:937`).
  Omit coax for a pure-capture driver.

---

## 3. AHD20_1080P_60P / AHD20_1080P_50P — NON-FUNCTIONAL STUBS

These enums exist but the vendor **never populated their tables**. Concretely:

- **VI table**: `vd_vi_init_list[]` has **no** `[AHD20_1080P_60P]` / `[AHD20_1080P_50P]`
  designated initializer (grep of `jaguar1_video_table.h` returns nothing). As a C designated
  array, those indices are **zero-filled** — so `vd_jaguar1_init_set` would write
  `video_format=0x00, ahd_mode=0x00, hpll_mask_on=0x00, clk_adc=0x00, cml_mode=0x00,
  sync_sel=0x00, …` (every VI field 0). That is not a valid 1080p config.
- **EQ**: no EQ row has `video_fmt == AHD20_1080P_60P/50P`, so
  `NC_VD_EQ_FindFormatDef` returns `NC_EQ_SETTING_FMT_UNKNOWN` (=0)
  (`jaguar1_video_eq.c:41`; enum `jaguar1_common.h:295`). Row `[0]` is unpopulated → `.name ==
  NULL` → `video_input_eq_val_set` hits the guard at `jaguar1_video_eq.c:189` and **returns
  without writing any EQ register**. So the EQ pass is entirely skipped.
- **`decoder_mipi_fmtdef`**: no designated entry → zero `{arb_scale=0x00, mipi_frame_opt=0x00}`
  (coincidentally the same as real 1080p, so `mipi_video_format_set` is harmless but irrelevant).
- **Coax**: no `[AHD20_1080P_60P]/[50P]` row either (zeros) — irrelevant for video.
- **`mipi_tx_init` PLL**: **fixed at 1.242 Gbps/lane for every format.** There is NO
  higher-rate branch for 60fps. The active `#if 1` block (`jaguar1_mipi.c:161‑201`, all bank
  `0x21`) programs:
  ```
  {0x21,0x40,0xB4} {0x21,0x41,0x00} {0x21,0x42,0x03} {0x21,0x43,0x43}   PLL divider  (:163‑167)
  {0x21,0x11,0x08} {0x21,0x10,0x13} {0x21,0x12,0x0B} {0x21,0x13,0x12}   D‑PHY        (:169‑172)
  {0x21,0x17,0x02} {0x21,0x18,0x12} {0x21,0x15,0x07} {0x21,0x14,0x2D}                (:173‑176)
  {0x21,0x16,0x0B} {0x21,0x19,0x09} {0x21,0x1A,0x15} {0x21,0x1B,0x11} {0x21,0x1C,0x0E} (:177‑181)
  ```
  Vendor comment (`:162`): "SET_MIPI_1242MHZ 1080P" → **~1.242 Gbps per lane, 4 lanes ≈ 4.97
  Gbps aggregate; DDR link/clock ≈ 621 MHz**. The `#else` branch (`:203‑239`) is the 720P
  profile (`0x40=0xDC,0x41=0x10` → lower rate), and it is compiled OUT (`#if 1`). A genuine
  1080p50/60 would need roughly double this lane rate; **the driver has no such setting.**

**Consequence for the "maybe it's 50/60 fps" hypothesis:** you **cannot** program 1080P_50P or
1080P_60P with this vendor code — they are dead stubs. Do not put them in the hardware sweep.
This driver's decoder-config path simply cannot target a true 50/60 fps 1080p mode (neither the
per-channel VI/EQ registers nor the shared MIPI PLL exist for it). That is a finding about the
code, not a diagnosis of your camera.

---

## 4. 1080I (interlaced)

**None.** There is no `AHD20_1080I` (or any interlaced 1080-line) format in
`NC_VIVO_CH_FORMATDEF`. No delta to give. (Interlace-related bits like `fld_inv` /
`FLD_INV_CHID` exist and are used by the SD paths, but no 1080-line interlaced format selects
them; for both 1080p rows `fld_inv = 0x00`.)

---

## Summary sweep table

| Target format | What to change vs the working 25P sequence |
|---|---|
| **1080P_30P** | `{0x00, 0x08+ch}` : `0x03 → 0x02` (all 4 ch). Nothing else. |
| **1080P_50P** | ✗ not supported — unpopulated stub, would write all-zero video config. |
| **1080P_60P** | ✗ not supported — unpopulated stub; MIPI PLL is fixed 1.242 Gbps regardless. |
| **1080I** | ✗ does not exist in the enum. |
