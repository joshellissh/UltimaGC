# Alert sounds

Drop `<name>.wav` files here; the Yocto recipe installs every `*.wav` in this
directory to `/usr/share/ultima/sounds/` on the image. A name with no file plays a
built-in tone instead. To try a sound without rebuilding, copy it to `/data/sounds/`
on the board (that directory wins) and `echo <name> > /tmp/ultima-sound.request`
— the file is read on first use, so restart `ultima-app` to pick up a replaced one.

Any ordinary WAV works: PCM 8/16/24/32-bit or 32-bit float, mono or stereo, any
sample rate (48 kHz skips a resample). Keep them short and trim silence at both ends;
volume is the file's own level times the app's software gain (default 0.6), so export
at a sensible peak level.

| File | Played when |
|---|---|
| `alarm.wav` | oil pressure / coolant warning (repeats every 4 s while active) |
| `warn.wav` | battery, low fuel, check engine, ABS |
| `tick.wav` `tock.wav` | turn signal / hazard, lit phase / dark phase (~14 ms each by default — keep them tiny) |
