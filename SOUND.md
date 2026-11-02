# Alert sounds over HDMI audio

The dash makes short alert/feedback sounds (warnings, drive-mode change, turn-signal
ticks, dashcam chimes) through the panel's HDMI audio. Status: **hardware path verified
2026-11-02; the app code below is written and compile-checked against Qt 5.15.13 /
alsa-lib 1.2.11 but has not yet been built into an image or heard on the board. No
`.wav` files are in the repo yet, so until some are added every sound plays its
built-in tone.**

## Hardware path (verified on the board, 6.12.43-ti)

McASP1 (I2S) -> IT66122 bridge -> HDMI -> panel. Nothing needed enabling: the bb.org
kernel/DT already registers it.

- `cat /proc/asound/cards` -> `0 [HDMI] simple-card - it66122 HDMI`; PCM is
  `davinci-mcasp.0-i2s-hifi`. It is the only card, so it is card 0.
- Drivers (`snd_soc_davinci_mcasp`, `snd_soc_hdmi_codec`, `snd_soc_simple_card`) are
  **modules** loaded by udev coldplug, which `udev-trigger-after-dash.conf` defers until
  after the first frame. Audio is therefore unavailable for the first few seconds of a
  boot; a sound requested before then is dropped, not queued.
- The raw `hw:0,0` device accepts **only S32_LE, stereo, 32 or 48 kHz**. S16_LE and
  44.1 kHz are rejected. ALSA's built-in `default` / `plughw:0,0` PCMs convert, so no
  `/etc/asound.conf` is needed (the stock one is just a comment).
- The Waveshare panel's EDID (the one baked into the kernel for `drm.edid_firmware`)
  advertises basic audio + stereo LPCM 32/44.1/48 kHz, 16/20/24 bit.
- `speaker-test -D hw:0,0 -c2 -t sine -f 440 -F S32_LE` was audible on the panel.
- There is no hardware mixer on this card, so volume is software gain in the app.

## App design

- `ultima-app/soundplayer.{h,cpp}` — `SoundPlayer`. Each sound name is a **WAV file**,
  `<name>.wav`, searched in `$ULTIMA_SOUND_DIR`, then `/data/sounds` (writable — swap a
  sound on a running car without a rebuild), then `/usr/share/ultima/sounds` (shipped
  from `ultima-app/assets/sounds/` by the recipe; see the README there for the file
  list). Any ordinary WAV works (PCM 8/16/24/32-bit or float32, mono/stereo, any rate):
  it is converted on first use to the stream format, 48 kHz stereo S32_LE, and scaled
  by the software volume — export at 48 kHz to skip the resample. A name with **no
  usable file falls back to a built-in synthesised tone** (the `library()` table in
  `soundplayer.cpp`), so a missing file is never silent; the journal says which was
  used (`[sound] alarm <- /usr/share/...` vs `no .wav found, using the built-in tone`).
  The loader was tested on the dev machine against 8/16/24/32-bit, float, EXTENSIBLE,
  mono/stereo and 8–96 kHz files (length, level and pitch all preserved). A worker
  thread owns ALSA (`libasound` directly, no QtMultimedia), so
  `play()` is safe from any thread and a stalled device can't hold up a frame.
  - ALSA opens lazily on the first sound (zero boot cost) and closes after 4 s idle.
  - HDMI sinks mute/relock when a stream starts and clip the first ~100-250 ms. A fresh
    stream gets a 250 ms silent lead-in, and while open the worker feeds silence so a
    burst (ticks, a repeating alarm) isn't clipped. **Both numbers are a first guess —
    tune `kLeadInMs` / `kIdleCloseMs` after hearing it on the panel.**
  - Compiled for real only with `CONFIG+=ultima_alsa` (set by the Yocto recipe; the
    recipe also adds `alsa-lib` to DEPENDS/RDEPENDS). Without it (macOS/WSL dev builds)
    `play()` is a no-op, optionally logging with `ULTIMA_SOUND_LOG=1`.
- `ultima-app/soundevents.{h,cpp}` — `SoundEvents`. Pure signal wiring over `CanBus`,
  `DashcamRecorder` and `main.qml`'s root window; none of those were modified. All the
  policy (what, when, cooldowns) lives here.

| Event | Sound | Rule |
|---|---|---|
| Oil pressure / coolant warn | `alarm` (3 urgent beeps) | on rising edge; repeats every 4 s while active |
| Battery warn | `warn` (falling two-tone) | only if still low with engine running (rpm >= 600) for 2-4 s — `vbat < 12.5 V` is simply true at key-on / cranking; re-alerts every 60 s |
| Low fuel | `warn` | rising edge, 120 s cooldown (sender slosh at 1/4 tank) |
| Check engine / ABS | `warn` | rising edge, 10 s cooldown |
| Drive mode change | `mode_valet` 1 beep, `mode_sport` 2, `mode_sportplus` 3 rising, `mode_race` 4 fast | beep count = mode |
| Turn signal / hazard | `tick` (lit phase) / `tock` (dark phase) | off the QML `blinkPhase` clock, so it is in step with the icon; plus an immediate tick on engage |
| Dashcam recording start / stop | `rec_start` rising / `rec_stop` falling | on `recordingChanged`, 3 s cooldown |
| Dashcam "format this drive?" | `prompt` (double ping) | when the prompt opens |
| Format result | `success` / `error` | |

CAN-derived sounds (warnings, drive mode) are **held back for 8 s after launch**: the app
starts with every flag false and the first CAN frames flip several to their real value,
which is state catching up rather than an event. Consequence: a drive-mode frame that
first arrives *after* 8 s (CAN link up late) will chime once for the initial mode.
Dashcam and turn-signal sounds are not held back.

## Configuration / testing

Environment (set in the unit file if needed): `ULTIMA_SOUND=0` disables;
`ULTIMA_SOUND_VOLUME=0..1` (default 0.6); `ULTIMA_SOUND_DEVICE=` ALSA PCM (default
`default`); `ULTIMA_SOUND_DIR=` extra WAV directory searched first.

To try a recording without a rebuild: copy `<name>.wav` to `/data/sounds/` on the board
(`mount -o remount,rw` is not needed — `/data` is its own writable partition) and
restart `ultima-app`; WAVs are read on first use and cached for the life of the process.

On the board, audition a sound without provoking the real event (same polled-file
pattern as the other debug hooks in `main.cpp`):

    echo alarm > /tmp/ultima-sound.request        # one sound
    echo all   > /tmp/ultima-sound.request        # every sound, 1.5 s apart

Names: alarm, error, mode_race, mode_sport, mode_sportplus, mode_valet, prompt,
rec_start, rec_stop, success, tick, tock, warn. The existing hooks also exercise the
events end to end: `/tmp/ultima-indicator.request` (left/right/hazard -> ticks) and
`/tmp/ultima-dvrtest.request` (prompt/success/error -> chimes). QML can play one-offs
via the `soundPlayer` context property: `soundPlayer.play("success")`.
