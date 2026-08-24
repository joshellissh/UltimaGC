# BY-J 360 Bird View — `.mx` 3D car model format

Reverse-engineering notes for the `A360K0*.mx` files in this folder. Written
2026-08-22.

## Provenance

- Product: **BY-J 360 Degree Bird View Surround System** (Amazon
  `B0FGJBVQX6`), a 4/6-channel 1080P AVM (around-view monitor) dash unit.
  Chipset: **HiSilicon Hi3520DV500**.
- These 4 files (`A360K000.mx`, `A360K010.mx`, `A360K020.mx`, `A360K030.mx`)
  were obtained from the seller via a NetEase Mail Master ("邮箱大师")
  cloud-attachment link (`dashi.163.com/html/cloud-attachment-download`).
  That's a generic "send a big file by email" feature, not a public tool
  portal — these were emailed directly by the seller/support, likely as 4
  alternate car-body/color skins.
- Extensive web searching (English + Chinese, targeting the file names,
  the "A360K" naming, the Hi3520DV500 chipset, and the BY-J brand) found no
  public PC tool that generates/edits this format. It's very likely
  internal/private dealer tooling belonging to whichever Shenzhen reference-
  design house built the firmware, never released publicly. The most
  realistic way to get a genuinely custom car model is to ask the seller
  directly (they already handed over 4 files once via the same channel).

## Container format

Each `.mx` file is a **standard GNU tar archive** with one small obfuscation:

- **The first 250 bytes of the file are bitwise-inverted** (`byte ^ 0xFF`).
  This covers just the name/mode/uid/gid/size/mtime/chksum/typeflag/most-of-
  linkname fields of the *first* tar header — enough to defeat a naive
  `file`/`tar` magic-byte sniff (checksum won't validate against the
  garbled bytes) without actually protecting anything.
- Byte 250 onward — the rest of that first header (`ustar  \0`, `root`
  uname/gname) plus **every subsequent header and all file content** — is
  completely plain, standard tar. Verified against real `tar` after
  patching just those 250 bytes.
- Boundary was confirmed empirically by brute-forcing which inversion
  length makes the first header's stored checksum match a freshly computed
  one (byte 249 is inverted, byte 250 already reads as a genuine `\x00`
  in the raw file, i.e. the boundary is exact, not a rounding coincidence).
- Verified this 250-byte-invert rule holds for **all 4** `.mx` files.
- Every member inside the tar is **independently zlib-compressed** (raw
  zlib, `78 da`/`78 9c`/`78 01`/`78 5e` headers — standard compression-level
  variants, not meaningful per se).

### Round-trip / repack

`tools/mx_tool.py` implements this: `list_members()` deobfuscates + walks
the tar; `build_tar_header()` reproduces the vendor's *exact* GNU header
byte layout (this matters — `bsdtar`/`gtar` emit different magic spacing,
uname/gname, and checksum text form, so re-tarring with the system `tar`
binary does **not** reproduce a matching file). Round-tripping all 4
original files through `list_members` → `repack` (unpack then rebuild with
no changes) produces a **byte-identical** copy of the original — confirms
the format model is complete and correct, not just "good enough to read."

To inject new content: swap one member's bytes, keep everyone else's
`(name, blob, mtime, mode)` unchanged, call `repack()`. Sizes/checksums are
recomputed automatically. (Device firmware almost certainly doesn't
byte-compare against a reference file, so exact fidelity beyond "valid tar"
is not required for a device to accept an edited file — that's just what
we used as our correctness bar here.)

## What's actually inside — 35 members per `.mx`

`car.config` is a plain INI giving canvas pixel dimensions:

```ini
[offset]
y=44
[car]
2dw=170
2dh=314
zoomfw=400
zoomfh=216
zoombw=400
zoombh=216
3dw=768
3dh=480
```

Every other member is a **raw RGBA8888 pixel buffer** (row-major,
top-to-bottom, confirmed correct — not BGRA — by rendering `2d_car.z` and
`3d_total.m` frame 0: taillight LEDs render blue as expected, would be red
under a channel swap), zlib-compressed on disk. There is **no mesh/vertex
format anywhere in this file** — despite the `3d_*` naming, "3D" is
implemented as **pre-rendered, baked image frames**, not runtime geometry.

| Member(s) | Size (px) | Purpose |
|---|---|---|
| `2d_car.z` | 170×314 | Flat top-down car icon (2D bird's-eye view) |
| `2d_door0.z`..`2d_door3.z` | 170×314 | Door-open overlay for the 2D view, one per door |
| `2d_zoom0.z` / `2d_zoom1.z` | 400×216 | Close-up front/rear bumper "zoom" parking-assist images |
| `3d_door0_0..4.z`, `3d_door1_0..4.z` | 768×480 | 5-frame door-open overlay animation, one set per door |
| `3d_led0.z` / `3d_led1.z` | 768×480 | LED/indicator-light overlay (e.g. welcome-light animation) |
| `3d_lun0_t*_x*.z`, `3d_lun1_t*_x*.z` (12 files) | 768×480 | Wheel-turned overlays — `t0/t1/t2` = steering angle steps, `x1/x2` = left/right wheel pair, `lun0/lun1` = front/rear axle |
| `3d_total.m` | 768×480 × **120 frames** | Full main-body 360° orbit turntable (~3°/frame) |
| `3d_total_lun1.m` | 768×480 × 120 frames | Matching 120-frame wheel-only turntable (composited separately so wheels can spin/turn independent of body rotation) |
| `3d_total_lun2.m` | 768×480 × 120 frames | Second wheel-only turntable (front vs. rear axle) |
| `car.config` | — | INI, canvas dimensions |

All `3d_*` frames have alpha + a soft drop shadow baked in — they're meant
to alpha-composite onto the app's own background/floor. The door/LED/wheel
overlay frames are mostly-transparent except the relevant part (door,
wheel, light) — meant to blend on top of the matching `3d_total.m` base
frame at the same orbit angle.

### `3d_total*.m` internal structure

Each `.m` file is **not one compressed blob** — it's 120 independent zlib
streams concatenated back-to-back (each decompressing to exactly one
768×480×4 = 1,474,560-byte RGBA frame), followed by a **728-byte trailer**:

```
offset 0:   uint32 LE  frame count            (120)
offset 4:   uint32 LE  total compressed bytes  (== byte length of the
                                                 120 concatenated streams,
                                                 i.e. file size minus 728)
offset 8:   uint32 LE [120]  start-byte-offset of each frame within the
                              file (frame 0 starts at 0; this is a
                              random-seek index, presumably so the device
                              doesn't have to decompress sequentially when
                              jumping to an arbitrary rotation angle)
offset 488: uint16 LE [120]  per-frame value, meaning **not conclusively
                              identified**. Many consecutive values were
                              observed identical (e.g. `0x7878` = 30840
                              repeated) with occasional differing values in
                              [1532, 65430] — could be a per-frame angle,
                              duration, checksum, or unused/placeholder
                              data. Unresolved — see Open questions.
```

Verified identical structure (120 streams + 728-byte trailer) in all three
`.m` files.

## Open questions / not yet resolved

- **Meaning of the 120×uint16 array** at trailer offset 488 in `3d_total*.m`.
  Doesn't obviously map to per-frame compressed size, cumulative offset, or
  anything else checked so far. Matters if regenerating `3d_total*.m` from
  scratch (a repack that only swaps frame *content* without changing frame
  *count* can probably leave this array untouched — untested).
- **No confirmation this device will actually accept an edited `.mx`** —
  round-trip fidelity was validated against the original files only, not
  against real hardware (target BeaglePlay/car hardware in this repo is
  unrelated to this AVM box; the BY-J unit itself hasn't been tested with a
  modified file).
- **Camera rig / lighting parameters for the 120-frame orbit** (FOV,
  distance, key-light angle, shadow softness) were only eyeballed from
  frames 0/10/29 of `3d_total.m`, not measured — would need reverse
  engineering from multiple frames (e.g. via feature tracking) to replicate
  precisely enough for a from-scratch render pipeline to visually match.

## Tools (in `tools/`)

- **`mx_tool.py`** — core library + CLI.
  - `python3 tools/mx_tool.py list <file.mx>` — list all members with
    size/mtime/mode.
  - `python3 tools/mx_tool.py extract <file.mx> <outdir>` — dump raw
    (still zlib-compressed) tar members.
  - `python3 tools/mx_tool.py selftest <file.mx>` — round-trip
    unpack→repack and diff against the original; should print
    `ROUNDTRIP: byte-identical to original`.
  - Importable functions: `list_members()`, `extract_all()`,
    `build_tar_header()`, `build_tar()`, `repack()`.
- **`extract_and_render.py`** — full extraction to viewable PNGs (decodes
  every zlib member, including splitting `3d_total*.m` into 120
  individual `frame_NNN.png` files). Usage:
  `python3 tools/extract_and_render.py <file.mx> <outdir>`.

## Extracted output

`extracted_A360K000/` — full PNG extraction of `A360K000.mx` (produced by
`extract_and_render.py`), ~48MB. Browse `3d_total/frame_000.png` through
`frame_119.png` to see the full orbit sequence in order.

## Practical paths forward

1. **Ask the seller for the actual modeling/baking tool**, or for a `.mx`
   of your specific car if one already exists in whatever library they
   pull from — highest odds of a clean result, since a from-scratch DIY
   render is a real content-creation project (see next).
2. **DIY**: render your own 120-frame 360° turntable (Blender + a scripted
   camera orbit is the natural approach) matching the lighting/shadow/alpha
   style seen in `extracted_A360K000/3d_total/`, plus matching door/wheel/
   LED overlay sets, then pack with `mx_tool.repack()`. All the container-
   level mechanics (compression, per-frame indexing, tar packing,
   obfuscation) are solved; only the actual 3D content creation remains.
3. **Cheap/low-effort win**: the flat 2D top-down icon (`2d_car.z`, 170×314)
   and its door overlays are trivial to swap — just author a
   170×314 RGBA PNG and feed it through `repack()`. No 3D rendering needed
   for that piece.
