# NVP6324 (Jaguar1 / "N4") — AHD20_1080P_25P, 4‑lane MIPI, UYVY — VERIFIED register sequence

Ground-truth extraction from the vendor BSP for porting into a mainline V4L2 driver.
Every line is annotated with the vendor `file:line` (and the `.h` table line where the
value is table-driven). Values are for **channel 0**, `format = AHD20_1080P_25P`,
`input = SINGLE_ENDED`, `interface = YUV_422`, all 4 channels enabled.

Source dir: `.../vin/modules/sensor/nvp6324/` (the `nvp6324_2/` sibling is ignored).

> Conventions: `{bank, reg, val}` = plain 8/8 write to the currently-selected bank.
> `RMW {bank, reg, mask_keep, set}` = read reg, `new = (old & mask_keep) | set`.
> `DELAY n unit`. Bank is selected by writing reg `0xFF = bank`. All numbers hex unless noted.

---

## 0. Entry path and the CRITICAL call-graph resolution

`sensor_reg_init()` (`nvp6324_mipi_driver.c:421`) → for the 1080p case
`nvp6324_init(AHD20_1080P_25P)` (`:435`).
`nvp6324_init` (`mipi_dev_nvp6324.c:388`): `check_decoder_count()` →
`video_decoder_init()` (`:403`) → builds `sVideoall` with every channel
`{format=AHD20_1080P_25P, input=SINGLE_ENDED, interface=YUV_422}` (`:405‑410`) →
`vd_set_all(&sVideoall)` (`:411`).

`vd_set_all` (`mipi_dev_nvp6324.c:139`) top-level order (`:153‑178`):
`mipi_datatype_set(VD_DATA_TYPE_YUV422)` → `mipi_tx_init(0)` →
for ch 0..3 `{ vd_jaguar1_init_set(); mipi_video_format_set(); }` →
`arb_init(0)` → `disable_parallel(0)` → `vd_pattern_enable()` **[OMITTED — color-bar test pattern]**.

### Which definition each call binds to (RESOLVED)

The **Makefile is decisive** (`Makefile:8`):
```
nvp6324_mipi-objs := nvp6324_mipi_driver.o mipi_dev_nvp6324.o jaguar1_video.o \
                     jaguar1_coax_protocol.o jaguar1_motion.o jaguar1_video_eq.o jaguar1_mipi.o
```
`jaguar1_drv.c` and `jaguar1_i2c.c` are **NOT compiled** (`jaguar1_i2c.o` is commented
out on `:9`; `jaguar1_drv.o` is absent). Therefore the duplicate `mipi_tx_init` /
`arb_init` / `vd_pattern_enable` in `jaguar1_drv.c` are **dead code** and are not on the path.

| Call in `vd_set_all` | Resolves to | Note |
|---|---|---|
| `mipi_datatype_set` | `jaguar1_mipi.c:130` | only definition compiled |
| `mipi_tx_init` | `jaguar1_mipi.c:157` | the `#if 1` 1080p/1242MHz branch |
| `vd_jaguar1_init_set` | `jaguar1_video.c:790` | (drags in EQ + coax, see below) |
| `mipi_video_format_set` | `jaguar1_mipi.c:118` | |
| `arb_init` | `jaguar1_mipi.c:66` | |
| `disable_parallel` | `jaguar1_mipi.c:244` | |
| `vd_pattern_enable` | `mipi_dev_nvp6324.c:111` (static) | OMITTED |

### I2C layer
`gpio_i2c_write/read` are `#define`d to `nvp6324_i2c_write/read`
(`jaguar1_common.h:19‑20`), which call `cci_write_a8_d8 / cci_read_a8_d8` on the
subdev (`mipi_dev_nvp6324.c:60‑78`). **The `0x60` "slave address" literal passed
everywhere is ignored** — cci uses the subdev's DT‑configured i2c client address (this
board = 0x31 7‑bit). `jaguar1_i2c.c`'s `__I2CWriteByte8` with its `udelay(300)` is dead
code (not compiled). So **there is no per-write delay** in this build.

### Bank-select model and the stateful cache (matters for reproduction)
`vd_register_set` (`jaguar1_video.c:78`) keeps a **file-static bank cache** (`cur_bank`,
`:27`) and emits `0xFF=bank` only when the requested bank differs (`:92‑95`). But many
functions do a **raw** `gpio_i2c_write(0xFF, …)` that does **not** update that cache
(e.g. `:172`, `:494`, `:592`, and all of `mipi_tx_init`/`arb_init`/coax). This desyncs
the cache from the chip's real bank. Consequences are documented at each site; the only
one that changes a *value's destination* is the CLK_AUTO write (§3, seq1). For a
regmap port, ignore the cache and just select the bank in every tuple below.

### Delays — item 5 verdict
**No delays anywhere in the init register path.** `jaguar1_video.c`, `jaguar1_mipi.c`,
`jaguar1_video_eq.c`, and both coax init functions (`coax_tx_init`, `coax_rx_init`)
contain zero `msleep/mdelay/udelay/usleep_range`. `video_decoder_init`, `mipi_tx_init`,
`vd_jaguar1_init_set`, `mipi_video_format_set`, `arb_init`, `disable_parallel`: **none**.
The `usleep_range` calls in `nvp6324_mipi_driver.c:138‑213` live in `sensor_power`
(MCLK/PWDN/regulator sequencing) and `sensor_reset` (RESET GPIO) — they run **before**
any I2C and are outside this sequence. `:576` is in `s_stream`. Coax `msleep`s
(`jaguar1_coax_protocol.c:411+`) are all in command-send functions, never called at init.

---

## 1. FULL channel-0 sequence in exact execution order

Table-driven values cite the code line **and** the `.h` row line.
VI values ← `jaguar1_video_table.h:773‑853` `[AHD20_1080P_25P]`.
VO values ← `jaguar1_video_table.h:3251‑3264` `[AHD20_1080P_30P]` (note: init always
fetches the **1080P_30P** VO row regardless of format, `jaguar1_video.c:805`).
EQ values ← `jaguar1_cableA_video_eq_table.h:741‑830` `[AHD20_1080P_25P_SINGLE_ENDED]`, index `[0]` (STAGE_0).

### A. `video_decoder_init()` — once (`mipi_dev_nvp6324.c:308`)
```
{0x04, 0xA0..0xC3, 0x24}   x36  (loop, :314-316)   bank4 0xA0..0xC3 all = 0x24
{0x01, 0xCC, 0x64} {0x01,0xCD,0x64} {0x01,0xCE,0x64} {0x01,0xCF,0x64}  (:319-321)  [†zeroed by disable_parallel]
{0x21, 0x07, 0x80}  (:324)   MIPI reset assert
{0x21, 0x07, 0x00}  (:325)   MIPI reset release
{0x0A, 0x77, 0x8F} {0x0A, 0xF7, 0x8F}  (:330-331)
{0x0B, 0x77, 0x8F} {0x0B, 0xF7, 0x8F}  (:333-334)
```

### B. `mipi_datatype_set(YUV422)` (`jaguar1_mipi.c:130`)
No I2C. Sets file-statics `mipi_dtype = 0x1E`, `arb_dtype = 0x00` (`:136‑139`).

### C. `mipi_tx_init(0)` — MIPI PLL / D‑PHY, 1080p ~1.242 Gbps (`jaguar1_mipi.c:157`, the `#if 1` branch)
**Order is load-bearing** (0x44/0x49 is a PLL latch/lock pulse). All in bank 0x21:
```
{0x21, 0xFF sel}                       (:160)
{0x21, 0x40, 0xB4} {0x21,0x41,0x00} {0x21,0x42,0x03} {0x21,0x43,0x43}   PLL   (:163-167)
{0x21, 0x11, 0x08} {0x21,0x10,0x13} {0x21,0x12,0x0B} {0x21,0x13,0x12}          (:169-172)
{0x21, 0x17, 0x02} {0x21,0x18,0x12} {0x21,0x15,0x07} {0x21,0x14,0x2D}   D-PHY  (:173-176)
{0x21, 0x16, 0x0B} {0x21,0x19,0x09} {0x21,0x1A,0x15} {0x21,0x1B,0x11}          (:177-180)
{0x21, 0x1C, 0x0E}                                                              (:181)
{0x21, 0x44, 0x00} {0x21,0x49,0xF3} {0x21,0x49,0xF0} {0x21,0x44,0x02}   PLL latch pulse (:184-187)
{0x21, 0x08, 0x40}                     frame options            (:189)
{0x21, 0x0F, 0x01}                     MIPI_TX_FRAME_CNT_EN     (:192)
{0x21, 0x38, 0x1E} {0x21,0x39,0x1E} {0x21,0x3A,0x1E} {0x21,0x3B,0x1E}   VC0..3 datatype = mipi_dtype(0x1E, YUV422) (:194-197)
{0x21, 0x07, 0x0F}                     4-lane enable            (:200)
{0x21, 0x2D, 0x01}                                              (:201)
```

### D. Per channel: `vd_jaguar1_init_set(ch0)` (`jaguar1_video.c:790`)
`ch = video_init->ch % 4 = 0`, `dev = ch/4 = 0`, `fmt = AHD20_1080P_25P`, `analog_input = SINGLE_ENDED`.

**D0 — each-set** (`:807`)
```
{0x00, 0x00, 0x10}     REG_SET_0x00_0_8_EACH_SET (:807, reg-def :29)
```

**D1 — analog input, `vd_jaguar1_single_differ_set(SINGLE_ENDED)`** (`:811` → def `:650`)
```
{0x00, 0x18, 0x13}     EX_CBAR_ON            (:652)
{0x05, 0x00, 0xD0}     A_CMP_PW_MODE (CMP)   (:660)
{0x05, 0x01, 0xA2}     CML  [† overwritten → 0x2C]   (:661)
{0x05, 0x92, 0x00}     PWM                   (:662)
```

**D2 — VO glue** (`vo_param` = 1080P_30P row)
```
{0x01, 0xEC, 0x00}     yc_merge = mux_yc_merge      vd_vo_port_y_c_merge_set (:816) ← table :3255
{0x01, 0xC8, 0x30}     vport_out_sel = 0x30 (4-mux) vd_vo_mux_mode_set       (:817) ← table :3259
--- vd_vo_manual_mode_set (:818, def :613): raw bank switch to 0x13, RMW 3 regs ---
{0x13, 0xFF sel} (raw :635)
RMW {0x13, 0x30, keep 0xEE, set 0x00}     clear bits ch(0) & ch+4(4)   (:636-644)
RMW {0x13, 0x31, keep 0xEE, set 0x00}     clear bits ch(0) & ch+4(4)
RMW {0x13, 0x32, keep 0xFE, set 0x00}     clear bit ch(0)
```

**D3 — `vd_vi_manual_set_seq1`** (`:824`, def `:121`)
```
RMW {0x01, 0x7C, keep 0xFE, set 0x00}   CLK_AUTO_1 clear bit0   (:159, reg-def :48)
        ⚠️ SEE §3: due to the stale bank cache this write actually lands in BANK 0x13,
        NOT bank 0x01. Bank1 0x7C is left untouched in the working vendor sequence.
{0x05, 0x32, 0x10}     NOVIDEO_DET_A                 (:169)
{0x05, 0xB9, 0xB2}     HAFC_LPF_SEL  [† overwritten → 0x72]   (:170)
--- raw bank 0x13, RMW det-en (:172-183) ---
{0x13, 0xFF sel}
RMW {0x13, 0x30, keep 0xEE, set 0x00}
RMW {0x13, 0x31, keep 0xEE, set 0x00}
RMW {0x13, 0x32, keep 0xFE, set 0x00}
{0x09, 0x44, 0x00}     FSC_EXT_EN (0x44+ch)          (:185)
{0x05, 0x6E, 0x00}     VBLK_END_SEL = vblk_end_sel   (:186) ← table :846
{0x05, 0x6F, 0x00}     VBLK_END_EXT = vblk_end_ext   (:187) ← table :847
```

**D4 — `vd_vi_vafe_set_seq2`** (all hardcoded, `:825`, def `:191`)
```
{0x05, 0x00, 0xD0}  {0x05,0x02,0x0C}  {0x05,0x1E,0x00}  {0x05,0x58,0x00}[†→0x77]
{0x05, 0x59, 0x00}  {0x05,0x5A,0x00}  {0x05,0x5B,0x41}  {0x05,0x5C,0x78}
{0x05, 0x94, 0x00}  {0x05,0x95,0x00}  {0x05,0x65,0x80}[†→0x00]       (:193-203)
```

**D5 — `vd_vi_format_set_seq3`** (`:826`, def `:207`)
```
{0x00, 0x10, 0x20}     VD_FMT = video_format         (:241) ← table :809
{0x00, 0x0C, 0x00}     SPL_MODE = spl_mode           (:242) ← table :812
{0x00, 0x04, 0x00}     SD_MODE = sd_mode             (:243) ← table :810
{0x00, 0x08, 0x03}     AHD_MODE = ahd_mode           (:244) ← table :811
RMW {0x05, 0x69, keep 0xFE, set 0x00}   SD_FREQ_SEL[0]=sd_freq_sel   (:245) ← table :813
{0x05, 0x62, 0x20}     SYNC_SEL = sync_sel           (:246) ← table :852
```

**D6 — `vd_vi_chroma_set_seq4`** (`:827`, def `:250`)
```
{0x00, 0x5C, 0x82}     PAL_CM_OFF = pal_cm_off       (:279) ← table :815
{0x05, 0x28, 0x90}     S_POINT = s_point [†→0x80]    (:280) ← table :816
{0x05, 0x25, 0xDC}     FSC_LOCK_MODE = fsc_lock_mode (:281) ← table :817
{0x05, 0x90, 0x01}     COMB_MODE = comb_mode         (:282) ← table :818
```

**D7 — `vd_vi_h_timing_set_seq5`** (`:828`, def `:286`)
```
{0x00, 0x68, 0x48}     H_DLY_LSB = h_delay_lsb       (:340) ← table :821
{0x00, 0x6C, 0x00}     H_DLY_MSB = h_dly_msb         (:341) ← table :845
{0x00, 0x60, 0x10}     Y_DLY = y_delay               (:342) ← table :826
{0x00, 0x78, 0x80}     V_BLK_END_A = v_blk_end_a [†→0x21]   (:343) ← table :828
RMW {0x05, 0x38, keep 0xEF, set 0x10}   H_MASK_ON[4]=h_mask_on(1)    (:345) ← table :822
RMW {0x05, 0x38, keep 0xF0, set 0x03}   H_MASK_SEL[3:0]=h_mask_sel(3) (:346) ← table :823   [net 0x13; EQ re-writes 0x13]
{0x00, 0x64, 0x00}     V_BLK_END_B = v_blk_end_b [†→0x05]   (:348) ← table :825
RMW {0x00, 0x14, keep 0xEF, set 0x00}   FLD_INV[4]=fld_inv(0)        (:349) ← table :827
{0x05, 0x64, 0x00}     MEM_RDP = mem_rdp             (:351) ← table :824
{0x05, 0x47, 0xEE}     SYNC_RS = sync_rs             (:352) ← table :820
{0x05, 0xA9, 0x00}     V_BLK_END_B(dup) = v_blk_end_b(:353) ← table :825
```

**D8 — `vd_vi_h_scaler_mode_set_seq6`** (`:829`, def `:357`)
```
RMW {0x05, 0x53, keep 0xF3, set 0x00}   LINEMEM_MD[3:2]=line_mem_mode(0)  (:390) ← table :834
{0x09, 0x96, 0x00}     H_DOWN_SCALER  (0x96+0x20*ch) (:392) ← table :830
{0x09, 0x97, 0x00}     H_SCALER_MODE  (0x97+0x20*ch) (:393) ← table :831
{0x09, 0x98, 0x00}     REF_BASE_LSB   (0x98+0x20*ch) (:394) ← table :832
{0x09, 0x99, 0x00}     REF_BASE_MSB   (0x99+0x20*ch) (:395) ← table :833
{0x09, 0x9E, 0x00}     H_SCALER_ACTIVE(0x9E+0x20*ch) (:396) ← table :848
```

**D9 — `vd_vi_hpll_set_seq7`** (`:831`, def `:399`)
```
{0x05, 0x50, 0xC6}     HPLL_MASK_ON = hpll_mask_on   (:428) ← table :836
{0x05, 0xB8, 0x39}     HAFC_OP_MD = hafc_op_md       (:429) ← table :839
{0x05, 0xBB, 0x0F}     HAFC_BYP_TH_E = hafc_byp_th_e (:430) ← table :837
{0x05, 0xB7, 0xFC}     HAFC_BYP_TH_S = hafc_byp_th_s (:431) ← table :838
```

**D10 — `vd_vi_color_set_seq8`** (`:832`, def `:435`; `fmt`≠TVI_5M and ≠H960_2EX_Btype → else branch)
```
{0x00, 0x20, 0x00}     BRIGHTNESS = brightnees       (:462) ← table :795
{0x00, 0x24, 0x86}     CONTRAST = contrast           (:463) ← table :796
{0x00, 0x28, 0x80}     BLACK_LEVEL = black_level     (:464) ← table :797
{0x00, 0x58, 0x80}     SATURATION_A = saturation_a   (:465) ← table :803
{0x00, 0x40, 0x00}     HUE = hue                     (:466) ← table :798
{0x00, 0x44, 0x00}     U_GAIN = u_gain               (:467) ← table :799
{0x00, 0x48, 0x00}     V_GAIN = v_gain               (:468) ← table :800
{0x00, 0x4C, 0xF8}     U_OFFSET = u_offset [†→0xFE]  (:469) ← table :801
{0x00, 0x50, 0xF8}     V_OFFSET = v_offset [†→0xFB]  (:470) ← table :802
{0x05, 0x2B, 0xA8}     SATURATION_B = saturation_b   (:471) ← table :804
{0x05, 0x24, 0x2A}     BURST_DEC_A = burst_dec_a     (:472) ← table :805
{0x05, 0x5F, 0x00}     BURST_DEC_B = burst_dec_b     (:473) ← table :806
{0x05, 0xD1, 0x30}     BURST_DEC_C = burst_dec_c     (:474) ← table :807
{0x09, 0x44, 0x00}     FSC_EXT_EN (0x44+ch)          (:476)
{0x09, 0x50, 0x30}     FSC_EXT_VAL_7_0  (0x50+4*ch)  (:477)
{0x09, 0x51, 0x6F}     FSC_EXT_VAL_15_8 (0x51+4*ch)  (:478)
{0x09, 0x52, 0x67}     FSC_EXT_VAL_23_16(0x52+4*ch)  (:479)
{0x09, 0x53, 0x48}     FSC_EXT_VAL_31_24(0x53+4*ch)  (:480)
{0x05, 0x26, 0x40}     FSC_LOCK_SENSE (else, not TVI_5M) (:485)
{0x05, 0xB8, 0x39}     HPLL_MASK_END (else branch)   (:491)
{0x09, 0x40, 0x00}     FSC_DET_MODE (0x40+ch)        (:492)
{0x05, 0xFF sel}       raw bank switch to 0x05+ch    (:494)
{0x05, 0xB5, 0x80}     (raw)                         (:495)
```

**D11 — `vd_vo_port_ch_id_set`** (`:833`, def `:582`)
```
{0x00, 0xFF sel}       raw (:592)
RMW {0x00, 0x14, keep 0x10, set 0x00}   0x14+ch = (old&0x10)|chid_vin(0x00)  (:593-596) ← table :3258
```

**D12 — `vd_vi_clock_set_seq9`** (`:834`, def `:500`)
```
{0x01, 0x84, 0x44}     CLK_ADC = clk_adc  [†→0x04]   (:532) ← table :841
{0x01, 0x88, 0x01}     CLK_PRE = clk_pre             (:533) ← table :842
{0x01, 0x8C, 0x02}     CLK_POST = clk_post           (:534) ← table :843
{0x05, 0x01, 0x2C}     CML_MODE = cml_mode           (:536) ← table :849
{0x05, 0x05, 0x24}     AGC_OP = agc_op               (:537) ← table :850
{0x05, 0x1D, 0x0C}     G_SEL = g_sel                 (:538) ← table :851
```

**D13 — format-specific block** (`:840‑861`): `AHD20_1080P_25P` matches none of the
special cases → `else` (`:860`, no writes). **Nothing emitted.**

**D14 — EQ Stage-0** (`:869‑876`, `#if 1`). See §4 for the full list and proof. Runs
here, in-line, before coax. `current_bank_set(0xFF)` at `:879`.

**D15 — Coax init** (`:885‑891`). See §5. `coax_tx_init` + `coax_rx_init`
(`coax_tx_16bit_init` is skipped because `bit8 == 1`, `mipi_dev_nvp6324.c:50`).

### E. Per channel: `mipi_video_format_set(ch0)` (`jaguar1_mipi.c:118`)
`mipi_vd_fmt = decoder_mipi_fmtdef[AHD20_1080P_25P]` = `{arb_scale=0x00, mipi_frame_opt=0x00}`
(`jaguar1_mipi_table.h:88‑90`).
```
(interface YUV_422 ≠ _DISABLE) → en_param |= 0x11<<ch   (:122-123)   [static accumulator, NO i2c here]
--- mipi_frame_opt_set(ch,0x00) (:126, def :87) ---
{0x21, 0xFF sel}
RMW {0x21, 0x3E, keep 0xF0, set 0x00}   ch0 low nibble = mipi_frame_opt(0x00)   (:96-98)
--- arb_scale_set(ch,0x00) (:127, def :33) ---
{0x20, 0xFF sel}
RMW {0x20, 0x01, keep 0xFC, set 0x00}   ch0 bits[1:0] = arb_scale(0x00)         (:40-44)
```

### F. `arb_init(0)` (`jaguar1_mipi.c:66`) — after the 4-channel loop
```
--- arb_disable (:60) ---
{0x20, 0xFF sel}  {0x20, 0x00, 0x00}
{0x20, 0xFF sel}                                (:70)
{0x20, 0x40, 0x01}                              (:72)
{0x20, 0x0F, 0x00}   = arb_dtype (YUV422)        (:73)
{0x20, 0x0D, 0x01}                              (:74)
{0x20, 0x40, 0x00}                              (:75)
--- arb_enable (:47) ---
{0x20, 0xFF sel}
{0x20, 0x00, 0xFF}   = en_param (all 4 ch: 0x11|0x22|0x44|0x88)   (:55)   [ch0-only would be 0x11]
```

### G. `disable_parallel(0)` (`jaguar1_mipi.c:244`)
```
{0x01, 0xFF sel}
{0x01, 0xC8, 0x00} {0x01,0xC9,0x00} {0x01,0xCA,0x00} {0x01,0xCB,0x00}
{0x01, 0xCC, 0x00} {0x01,0xCD,0x00} {0x01,0xCE,0x00} {0x01,0xCF,0x00}   (:248-255)
```
(0xCC–0xCF here override the `0x64` written in `video_decoder_init` → net **0x00**.)

---

## 2. Per-channel arithmetic (generate ch1/ch2/ch3 from ch0)

`ch` is the channel; `dev=0` always (single chip). Concrete transforms:

| Register family | ch0 → ch formula |
|---|---|
| Bank-0 color/format/timing (0x00,0x04,0x08,0x0C,0x10,0x14,0x18,0x20,0x24,0x28,0x30,0x34,0x40,0x44,0x48,0x4C,0x50,0x58,0x5C,0x60,0x64,0x68,0x6C,0x78) | `(0x00, reg) → (0x00, reg + ch)` |
| Per-ch decoder bank (AFE/HPLL/chroma: 0x00,0x01,0x02,0x05,0x1D,0x1E,0x24,0x25,0x26,0x27,0x28,0x2B,0x31,0x32,0x38,0x47,0x50,0x53,0x57,0x58,0x59,0x5A,0x5B,0x5C,0x5F,0x62,0x64,0x65,0x69,0x6E,0x6F,0x90,0x92,0x94,0x95,0xA9,0xB5,0xB7,0xB8,0xB9,0xBB,0xD1,0xD5) | `(0x05, reg) → (0x05 + ch, reg)`  (banks 5,6,7,8 for ch0..3) |
| Bank-1 clocks (0x84,0x88,0x8C) and VO (0xC8,0xEC) | `(0x01, reg) → (0x01, reg + ch)` |
| CLK_AUTO | reg `0x01/0x7C` fixed; **bit position = ch** (RMW clear bit `ch`) |
| Bank-9 H-scaler (0x96..0x9E) | `(0x09, reg) → (0x09, reg + 0x20*ch)` |
| Bank-9 FSC_EXT_EN / FSC_DET_MODE (0x44 / 0x40) | `(0x09, reg) → (0x09, reg + ch)` |
| Bank-9 FSC_EXT_VAL (0x50,0x51,0x52,0x53) | `(0x09, reg) → (0x09, reg + 4*ch)` |
| Bank-13 det-en (0x30,0x31) | same regs; RMW clears bits `ch` **and** `ch+4` |
| Bank-13 det-en (0x32) | same reg; RMW clears bit `ch` |
| **EQ bank-A** (base/coeff/color-A/y_filter: 0x25,0x27,0x30..0x3B,0x3C,0x3D) | `bank = 0x0A + ((ch%4)/2)` (ch0/1→0x0A, ch2/3→0x0B); `reg = base + (ch%2)*0x80` (ch0/2→base, ch1/3→base+0x80) |
| MIPI frame-opt (bank 0x21) | ch0/1→reg 0x3E, ch2/3→reg 0x3F; ch0/2 → low nibble, ch1/3 → high nibble |
| ARB scale (bank 0x20 reg 0x01) | field `[2*ch +: 2]` |
| ARB enable accumulator | `en_param |= 0x11 << ch` |
| Coax (bank 0x02/0x03) | `bank = 0x02 + ((ch%4)/2)`; `reg = base + (ch%2)*0x80` |
| Port CHID (0x14+ch) / VC datatype | CHID = `chid_vin` = 0x00 for all ch (VO table). MIPI VC datatypes are per-VC in `mipi_tx_init` (0x38=VC0..0x3B=VC3), all 0x1E, written once — not per init-channel. |

**Per-channel value changes:** For 1080p25 the VI/VO/EQ/MIPI table values are identical
across channels — only the bank/reg addresses and the RMW bit position change. `CHID_VIN`
is 0x00 for every channel. `en_param` differs (accumulates `0x11<<ch`).

---

## 3. Every RMW — exact keep-mask and set byte (ch0), with per-channel formula

`new = (old & keep) | set`. "keep" = `~(field mask)`.

| Site (file:line) | bank/reg | field | keep (ch0) | set (ch0) | per-ch |
|---|---|---|---|---|---|
| seq1 CLK_AUTO `video.c:159`/def`:48` | 0x01/0x7C | bit0 | 0xFE | 0x00 | keep `~(1<<ch)` (ch1 0xFD, ch2 0xFB, ch3 0xF7) — **but see ⚠️ below** |
| seq3 SD_FREQ_SEL `:245`/`:78` | 0x05+ch/0x69 | bit0 | 0xFE | 0x00 | same reg/field, bank 0x05+ch |
| seq5 H_MASK_ON `:345`/`:94` | 0x05+ch/0x38 | bit4 | 0xEF | 0x10 | bank 0x05+ch |
| seq5 H_MASK_SEL `:346`/`:95` | 0x05+ch/0x38 | [3:0] | 0xF0 | 0x03 | bank 0x05+ch (net 0x13) |
| seq5 FLD_INV `:349`/`:97` | 0x00/0x14+ch | bit4 | 0xEF | 0x00 | reg 0x14+ch |
| seq6 LINEMEM_MD `:390`/`:104` | 0x05+ch/0x53 | [3:2] | 0xF3 | 0x00 | bank 0x05+ch |
| vo_manual & seq1 det-en 0x30/0x31 `:640-641`,`:177-178` | 0x13/0x30,0x31 | bits ch,ch+4 | 0xEE | 0x00 | keep `~((1<<(ch+4))|(1<<ch))` — ch1 0xDD, ch2 0xBB, ch3 0x77 |
| vo_manual & seq1 det-en 0x32 `:642`,`:179` | 0x13/0x32 | bit ch | 0xFE | 0x00 | keep `~(1<<ch)` |
| port CHID `:593-596` | 0x00/0x14+ch | keep bit4 only | 0x10 | `chid_vin`=0x00 | reg 0x14+ch; set = chid_vin (0x00 all ch) |
| EQ timing_a H_DELAY_C `eq.c:119`/`:238` | 0x00/0x6C+ch | [3:0] | 0xF0 | 0x00 | reg 0x6C+ch |
| mipi_frame_opt `mipi.c:96-98` | 0x21/0x3E | low nibble (ch0) | 0xF0 | 0x00 | ch1: reg 0x3E keep 0x0F; ch2: reg 0x3F keep 0xF0; ch3: reg 0x3F keep 0x0F. **NB:** vendor does not shift `val` into the high nibble for ch1/ch3 — latent bug, harmless at val=0x00. |
| arb_scale `mipi.c:40-44` | 0x20/0x01 | [2*ch +:2] | 0xFC | 0x00 | keep `~(0x3<<(2*ch))` — ch1 0xF3, ch2 0xCF, ch3 0x3F |

> ⚠️ **CLK_AUTO destination (DELTA on recommendation vs regseq.md).** In the working
> vendor sequence, the seq1 `REG_SET_1x7C` RMW does **not** hit bank 0x01. Trace (ch0):
> `vd_vo_mux_mode_set` leaves the static cache = 0x01; `vd_vo_manual_mode_set` then does a
> **raw** `0xFF=0x13` (`video.c:635`) that the cache never sees; the next op is
> `REG_SET_1x7C` which asks for bank 0x01, the cache already says 0x01, so **no `0xFF` is
> emitted** and the read-modify-write of `0x7C` executes against the chip's **real** bank
> 0x13. Net effect of the shipped code: **bank 0x01 0x7C is left at its reset default;
> bank 0x13 0x7C gets bit `ch` cleared** (in addition to the deliberate 0x30/0x31/0x32
> clears). This repeats identically for every channel. `regseq.md`'s advice ("program
> bank1 0x7C[ch]=0 explicitly") is an **inference of intent with no hardware evidence** —
> the sequence that is known to produce video never touches bank1 0x7C. seq9 + EQ set
> `clk_adc/pre/post` (1x84/88/8C) explicitly regardless. **Recommendation for the port:
> replicate the proven behavior first — leave bank1 0x7C alone.** Only try clearing
> bank1 0x7C[ch] as a separate experiment if you see clock/auto-format issues; do not
> treat it as required.

---

## 4. EQ — VERIFIED ON THE INIT PATH (item 4)

**The cable equalizer runs as part of init**, not from a separate ioctl/monitor thread.
`vd_jaguar1_init_set` calls `video_input_eq_val_set(&eq_set)` at **`jaguar1_video.c:875`**,
inside `#if 1` (`:869‑876`), with:
```
eq_set.Ch = ch;  FmtDef = AHD20_1080P_25P;  Cable = CABLE_A;  Input = SINGLE_ENDED;  stage = STAGE_0;
```
Execution position: **per channel, after seq9 + the format-specific block, before coax**,
i.e. between D12/D13 and D15 above. There is also a `video_input_eq_analog_input_set`
(`video_eq.c:221`) and `video_input_eq_cable_set` (`:213`) in the same file, but **neither
is called on the init path** (only `video_input_eq_val_set` is).

Lookup: `NC_VD_EQ_FindFormatDef(AHD20_1080P_25P, SINGLE_ENDED)` (`:176`) → row
`[AHD20_1080P_25P_SINGLE_ENDED]` in `equalizer_value_fmtdef_cableA[]`
(`jaguar1_cableA_video_eq_table.h:741`). CABLE_A/B/C/D all resolve to the same cableA
table (`:178‑187`). `stage=STAGE_0` → **array index `[0]`** of every field.

Apply order (`video_input_eq_val_set:194‑199`) with ch0 concrete values (table `:748‑829`):

**base** (`__eq_base_set_value:44`)
```
{0x05, 0x65, 0x00}   EQ_BYPASS = eq_bypass[0]        ← :749   [overwrites seq2 0x80]
{0x05, 0x58, 0x77}   EQ_BAND_SEL = eq_band_sel[0]    ← :750   [overwrites seq2 0x00]
{0x05, 0x5C, 0x78}   EQ_GAIN_SEL = eq_gain_sel[0]    ← :751
{0x0A, 0x3D, 0x00}   DEQ_A_ON = deq_a_on[0]          ← :752   (bank 0x0A+((ch%4)/2), 0x3D+(ch%2)*0x80)
{0x0A, 0x3C, 0x00}   DEQ_A_SEL = deq_a_sel[0]        ← :753
```
**coeff** (`__eq_coeff_set_value:57`) — bank A 0x30..0x3B:
```
{0x0A,0x30,0xAC} {0x0A,0x31,0x78} {0x0A,0x32,0x17} {0x0A,0x33,0xC1}   ← :757-760
{0x0A,0x34,0x40} {0x0A,0x35,0x00} {0x0A,0x36,0xC3} {0x0A,0x37,0x0A}   ← :761-764
{0x0A,0x38,0x00} {0x0A,0x39,0x02} {0x0A,0x3A,0x00} {0x0A,0x3B,0xB2}   ← :765-768
```
**color** (`__eq_color_set_value:78`)
```
{0x00,0x24,0x86}  contrast[0]         ← :772
{0x00,0x30,0x00}  y_peaking_mode[0]   ← :773
{0x00,0x34,0x00}  y_fir_mode[0]       ← :774
{0x05,0x31,0x82}  c_filter[0]         ← :775
{0x00,0x5C,0x82}  pal_cm_off[0]       ← :776
{0x00,0x40,0x00}  hue[0]              ← :777
{0x00,0x44,0x00}  u_gain[0]           ← :778
{0x00,0x48,0x00}  v_gain[0]           ← :779
{0x00,0x4C,0xFE}  u_offset[0]  [overwrites seq8 0xF8]   ← :780
{0x00,0x50,0xFB}  v_offset[0]  [overwrites seq8 0xF8]   ← :781
{0x00,0x28,0x80}  black_level[0]      ← :782
{0x05,0x27,0x57}  acc_ref[0]          ← :783
{0x05,0x28,0x80}  cti_delay[0]  [overwrites seq4 0x90]  ← :784
{0x05,0x2B,0xA8}  saturation_b[0]     ← :785
{0x05,0x24,0x2A}  burst_dec_a[0]      ← :786
{0x05,0x5F,0x00}  burst_dec_b[0]      ← :787
{0x05,0xD1,0x30}  burst_dec_c[0]      ← :788
{0x05,0xD5,0x80}  c_option[0]         ← :789
{0x0A,0x25,0x10}  y_filter_b[0]       ← :790
{0x0A,0x27,0x1E}  y_filter_b_sel[0]   ← :791
```
**timing_a** (`__eq_timing_a_set_value:112`)
```
{0x00,0x68,0x48}  h_delay_a[0]        ← :795
{0x05,0x38,0x13}  h_delay_b[0]  (FULL 8-bit write — replaces seq5's two RMWs; net still 0x13)  ← :796
RMW {0x00,0x6C, keep 0xF0, set 0x00}  h_delay_c[0] [3:0]   ← :797
{0x00,0x64,0x05}  y_delay[0]   [overwrites seq5 0x00]      ← :798
```
**clk** (`__eq_clk_set_value:125`)  ⚠ field mapping is EQ_CLOCK_ADC←clk_adc, PRE←clk_adc_pre, POST←clk_adc_post
```
{0x01,0x84,0x04}  clk_adc[0]   [overwrites seq9 0x44]      ← :802
{0x01,0x88,0x01}  clk_adc_pre[0]                            ← :803
{0x01,0x8C,0x02}  clk_adc_post[0]                           ← :804
```
**timing_b** (`__eq_timing_b_set_value:135`)
```
{0x09,0x96,0x00}..{0x09,0x9E,0x00}  h_scaler1..9[0] (0x96..0x9E, +0x20*ch)  ← :808-816
{0x09,0x40,0x00}  pn_auto[0] (0x40+ch)     ← :817
{0x05,0x90,0x01}  comb_mode[0]             ← :818
{0x05,0xB9,0x72}  h_pll_op_a[0]  [overwrites seq1 0xB2]    ← :819
{0x05,0x57,0x00}  mem_path[0]              ← :820
{0x05,0x25,0xDC}  fsc_lock_speed[0]        ← :821
{0x00,0x04,0x00}  sd_mode[0]               ← :823
{0x00,0x08,0x03}  ahd_mode[0]              ← :822
{0x00,0x0C,0x00}  spl_mode[0]              ← :824
{0x00,0x78,0x21}  vblk_end[0]   [overwrites seq5 0x80]     ← :825
{0x05,0x1D,0x0C}  afe_g_sel[0]             ← :826
{0x05,0x01,0x2C}  afe_ctr_clp[0]           ← :827
{0x05,0x05,0x24}  d_agc_option[0]          ← :828
```
(The `if (AHD20_SD_H960_2EX_Btype…)` block at `video_eq.c:201‑207` has empty bodies — no writes.)

No delays in any EQ function.

---

## 5. Coax control channel — present in init, but NOT video (item: completeness)

`vd_jaguar1_init_set` also calls `coax_tx_init` + `coax_rx_init` (`video.c:888,891`).
This is the AHD coaxial control protocol (up-the-coax camera OSD/PTZ). **It is not
required for video capture — omit it for a pure-capture MIPI driver.** Listed here so the
"complete list" claim holds. `distance` is hardcoded 0 (`coax_protocol.c:167`; the
cable-distance auto-read is commented out). Values ← `jaguar1_coax_table.h:937`
`[AHD20_1080P_25P]`, distance index `[0]`. ch0: bank `0x02+((ch%4)/2)=0x02`, offset
`(ch%2)*0x80=0`. No delays in either function.

`coax_tx_init` (`:161`):
```
{0x01,0xA8,0x00} {0x01,0xA9,0x00} {0x01,0xAA,0x00} {0x01,0xAB,0x00}   (:189-192, constants)
{0x02,0x7C,0x01}  rx_src            ← :940
{0x02,0x7D,0x80}  rx_slice_lev      ← :941
{0x02,0x00,0x26}  tx_baud[0]        ← :946
{0x02,0x02,0x00}  tx_pel_baud[0]    ← :947
{0x02,0x03,0x0D}  tx_line_pos0[0]   ← :948
{0x02,0x04,0x00}  tx_line_pos1[0]   ← :949
{0x02,0x05,0x03}  tx_line_count     ← :950
{0x02,0x07,0x00}  tx_pel_line_pos0[0] ← :951
{0x02,0x08,0x00}  tx_pel_line_pos1[0] ← :952
{0x02,0x0A,0x08}  tx_line_count_max ← :953
{0x02,0x0B,0x10}  tx_mode           ← :954
{0x02,0x0D,0xA0}  tx_sync_pos0[0]   ← :955
{0x02,0x0E,0x01}  tx_sync_pos1[0]   ← :956
{0x02,0x2F,0x00}  tx_even           ← :957
{0x02,0x0C,0x00}  tx_zero_length    ← :958
```
`coax_rx_init` (`:591`), bank 0x02:
```
{0x02,0x63,0x01}  rx_comm_on        ← :960
{0x02,0x62,0x05}  rx_area           ← :961
{0x02,0x66,0x81}  rx_signal_enhance ← :962
{0x02,0x69,0x2D}  rx_manual_duty    ← :963
{0x02,0x60,0x55}  rx_head_matching  ← :964
{0x02,0x61,0x00}  rx_data_rz        ← :965
{0x02,0x68,0x60}  rx_sz             ← :966
```

---

## 6. `mipi_video_format_set` behavior for an enabled 1080p25 channel (item 6)

`jaguar1_mipi.c:118`. Looks up `decoder_mipi_fmtdef[format]`; for AHD20_1080P_25P that is
`{arb_scale=0x00, mipi_frame_opt=0x00}` (`jaguar1_mipi_table.h:88`). Then:
1. **Enable gate** (`:122`): `if (interface != _DISABLE) en_param |= 0x11 << ch`. With
   `interface = YUV_422` this is true → sets the channel's two bits (`0x11<<ch`) in the
   arbiter-enable accumulator. This is the ONLY thing gated by interface.
2. `mipi_frame_opt_set` (`:126`): RMW of bank 0x21 reg 0x3E/0x3F nibble to
   `mipi_frame_opt = 0x00` — **runs unconditionally**, even for a disabled channel.
3. `arb_scale_set` (`:127`): RMW of bank 0x20 reg 0x01 field `[2*ch +:2]` to
   `arb_scale = 0x00` (FHD = bypass) — **runs unconditionally**.

So nothing skips the register writes per-channel; only `en_param` (hence `arb_enable`,
which writes `{0x20,0x00,en_param}`) reflects which channels are enabled. For all-4-on,
`en_param` = `0x11|0x22|0x44|0x88 = 0xFF`. For ch0-only it would be 0x11.

---

## 7. Net-final values where seq and EQ disagree (for a write-once regmap driver)

Use these single values (they match `regseq.md` LIST 4 — all confirmed):

| bank/reg | seq writes | EQ writes | **net final** |
|---|---|---|---|
| 0x01/0x84 | 0x44 | 0x04 | **0x04** |
| 0x05/0x65 | 0x80 | 0x00 | **0x00** |
| 0x05/0x58 | 0x00 | 0x77 | **0x77** |
| 0x05/0x01 | 0xA2→0x2C | 0x2C | **0x2C** |
| 0x05/0xB9 | 0xB2 | 0x72 | **0x72** |
| 0x05/0x28 | 0x90 | 0x80 | **0x80** |
| 0x00/0x4C | 0xF8 | 0xFE | **0xFE** |
| 0x00/0x50 | 0xF8 | 0xFB | **0xFB** |
| 0x00/0x64 | 0x00 | 0x05 | **0x05** |
| 0x00/0x78 | 0x80 | 0x21 | **0x21** |
| 0x05/0x38 | RMW→0x13 | 0x13 (full) | **0x13** |

---

## 8. DELTAs vs `nvp6324-1080p25-regseq.md`

I compared every value in this extraction against `regseq.md`'s LIST 1/2/3/4. **All
register VALUES agree** — the prior extraction's numbers are correct. The deltas are
structural / advisory, not numeric:

1. ⚠️ **CLK_AUTO recommendation (the important one).** `regseq.md` says "program bank1
   0x7C[ch]=0 explicitly." The *shipped, working* vendor sequence never writes bank1 0x7C
   at all — the RMW lands in bank 0x13 due to the stale bank cache (proof in §3, trace
   from `video.c:635` raw `0xFF=0x13` + `:159`). Prefer replicating the proven behavior
   (leave bank1 0x7C alone); clearing it is an untested hypothesis, not a fact.

2. ⚠️ **Coax init omitted.** `regseq.md` doesn't mention `coax_tx_init`/`coax_rx_init`,
   which ARE on the init path (banks 0x01 0xA8‑0xAB and 0x02 …). Not needed for video —
   see §5 — but it is part of `vd_jaguar1_init_set`.

3. ⚠️ **Interface value.** The path uses `interface = YUV_422` (enum `NC_D2S_OUTPUT_INTERFACE`,
   value 1; `jaguar1_common.h:399‑403`), set in `nvp6324_init:409`. `MIPI` is **not** a
   value of that enum — it only appears in the commented-out `set_default_video_fmt`
   sample (`mipi_dev_nvp6324.c:209`). The MIPI-ness comes from `mipi_tx_init` +
   `mipi_video_format_set`, not from an "interface = MIPI" selector.

4. ⚠️ **Channel count trap (affects "all 4 channels").** For one 4-channel chip at 0x60,
   `check_decoder_count` finds a single chip → `jaguar1_cnt = 1`. `nvp6324_init` then fills
   only `sVideoall.ch_param[0]` (`:405` loop bound `< jaguar1_cnt`), yet `vd_set_all`
   always iterates `i = 0..3` (`:156`) reading `ch_param[1..3]` from **uninitialized
   stack**. As written the vendor sample reliably programs **only ch0**. The port must set
   all four channels explicitly (which is the intent here); use §2 arithmetic.

5. **`en_param` is a never-reset file-static** (`jaguar1_mipi.c:25`). Fine for a single
   init; on a re-init it would keep OR-ing bits. A clean port should zero it per init.

---

## 9. Quick facts for the port
- Slave address: the `0x60` literals are ignored; cci uses the DT i2c client (0x31 7-bit here).
- Register model: `0xFF` = bank select; all else 8-bit reg / 8-bit val to current bank.
- `mipi_dtype = 0x1E` (YUV422 / RAW8-style UYVY datatype), 4 lanes (`0x21/0x07 = 0x0F`), VC0..3.
- DROP `vd_pattern_enable()` (color bars).
- No delays in the register-init path; power/reset delays belong to GPIO/MCLK sequencing only.
