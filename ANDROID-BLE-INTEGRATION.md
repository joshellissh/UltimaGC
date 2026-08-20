# Android BLE Integration — Ultima AUX Control Service

Spec for an Android companion app to connect to the Ultima gauge cluster over
BLE and control the AUX (auxiliary) outputs on the MCE18 CAN bus expander
wired to CAN2. Covers both sides: what the dash needs to expose (currently
doesn't) and exactly what the Android app needs to call.

**This supersedes an earlier design.** Before 2026-08-20 this document
specified a *configuration* service (system time, trip odometer reset,
camera calibration, WiFi provisioning — mirroring what the touchscreen's own
settings screens already do). That design was never implemented dash-side
(advertising only, same as today) and has been dropped in favor of AUX
control, not layered alongside it. If you need the old config-service spec,
pull this file from git history before this commit.

## Status: nothing below exists yet except advertising

As of 2026-08-20, `BluetoothManager` (`ultima-app/src/bluetoothmanager.cpp`)
only advertises the dash as a connectable, discoverable BLE peripheral named
`"Ultima RS"` — confirmed working on real hardware (HCI trace + seen from
Windows; see `beagleplay-falcon/NOTES.md` "Bluetooth via CC1352P7" ›
"Hardware-verified working"). **It registers no GATT service and no
characteristics.** A central can connect to it today, but there is nothing
to read or write once connected.

Also new: `CanBus` (`ultima-app/src/canbus.cpp`) has never transmitted a CAN
frame. It opens `can0` and only ever reads (`onReadable()` off a
`QSocketNotifier`) — every existing gauge value flows one way, off the wire
into Qt properties. AUX control needs a write path added to `CanBus` (or a
small sibling class) that doesn't exist in any form today; this is a
different, larger piece of new work than the old config design's GATT
wrappers around already-writable objects (`SystemClock`, `OdoStore`,
`CalibrationStore`).

Everything in this document — the service, every characteristic, the
CAN Tx behavior — is a design to implement, not a description of working
code. Treat the GATT table below as the target for `BluetoothManager`'s
dash-side implementation and for the Android app both, so the two sides are
built against the same contract instead of drifting.

Also relevant going in: **a phone's own Bluetooth Settings app cannot do any
of this.** Neither iOS nor Android's built-in Settings pairs with or talks
to a bare BLE peripheral like this one — confirmed by testing (see NOTES.md,
same section). A dedicated app using Android's `BluetoothGatt` APIs — i.e.
exactly what this document specifies — is the only way to reach this
service from a phone.

## Scope

The car's MCE18 is a **V4** unit (confirmed 2026-08-20): 4 low-side AUX
outputs, 3A each, with AUX4 additionally supporting 0-100% PWM duty (PWM is
a V4-only feature — V3 units only have 3 outputs and no PWM at all). None of
the four outputs are wired to real loads yet, so this spec treats them
generically (`AUX1`..`AUX4`) rather than naming them after a function —
rename the characteristics/labels once real wiring assigns each one to
something (a light bar, a fan, whatever) in both this doc and the Android
app.

**Explicitly out of scope, on purpose:** system time, trip odometer, camera
calibration, WiFi provisioning — see "supersedes" note above. This app's
whole job is now AUX control.

## Advertising & discovery

Advertising starts once at boot (Linux target — `main.cpp`) and stays on
continuously; it does not depend on `BluetoothScreen` being open on the
touchscreen. A companion app can scan and connect at any time the dash is
powered.

Dash advertises (confirmed live, `btmon` trace in NOTES.md):
- Flags: `LE General Discoverable Mode` + `BR/EDR Not Supported`
- Local name (complete): `Ultima RS`
- **No service UUID in the advertisement today.** Recommend adding the
  Ultima AUX Control Service UUID (below) to the advertising data or scan
  response once the GATT service exists, so the Android app can filter scans
  by service UUID instead of by name string. Until then, name-based
  filtering is the only option.

```kotlin
val scanFilter = ScanFilter.Builder()
    .setDeviceName("Ultima RS")
    // Once the dash advertises the service UUID:
    // .setServiceUuid(ParcelUuid(ULTIMA_AUX_SERVICE_UUID))
    .build()

val scanSettings = ScanSettings.Builder()
    .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
    .build()

bluetoothLeScanner.startScan(listOf(scanFilter), scanSettings, scanCallback)
```

### Permissions (Android manifest + runtime)

- **API 31+ (Android 12+):** `BLUETOOTH_SCAN` and `BLUETOOTH_CONNECT`,
  both runtime-requested. Since this app doesn't derive physical location
  from scan results, declare `BLUETOOTH_SCAN` with
  `android:usesPermissionFlags="neverForLocation"` — this avoids also
  needing `ACCESS_FINE_LOCATION` on these OS versions.
  ```xml
  <uses-permission android:name="android.permission.BLUETOOTH_SCAN"
      android:usesPermissionFlags="neverForLocation" />
  <uses-permission android:name="android.permission.BLUETOOTH_CONNECT" />
  ```
- **Pre-API 31:** manifest-only `BLUETOOTH` + `BLUETOOTH_ADMIN`, plus a
  runtime `ACCESS_FINE_LOCATION` grant (BLE scan results are
  location-sensitive on these versions regardless of what the app actually
  does with them — this is a platform requirement, not optional).

## Connection & security model

```kotlin
val gatt = device.connectGatt(context, /* autoConnect = */ false, gattCallback,
    BluetoothDevice.TRANSPORT_LE)
```

- `autoConnect = false` for the initial connect — faster for "user just
  tapped a scan result," at the cost of not auto-reconnecting if the link
  drops. Reconnect logic (retry `connectGatt` on `onConnectionStateChange`
  disconnect) is the app's job.
- **No large-MTU negotiation needed.** The old config design's Calibration
  characteristic carried a JSON blob and needed a 247-byte MTU request
  before touching it. Every characteristic here is 1-4 bytes — the default
  23-byte ATT MTU (20 usable) is comfortably enough, so skip that step
  entirely.
- **Bonding.** All four AUX write characteristics require an encrypted link
  — see the per-characteristic table below. Android auto-triggers pairing
  the first time an app touches an attribute that requires encryption and
  the link isn't encrypted yet (fires `BluetoothDevice.ACTION_PAIRING_REQUEST`),
  but that shows up to the app as a failed write that needs a retry once
  bonding completes, which is a worse UX than doing it upfront. Recommend
  proactively bonding right after service discovery, before any writes:
  ```kotlin
  if (device.bondState == BluetoothDevice.BOND_NONE) {
      device.createBond()
      // wait for ACTION_BOND_STATE_CHANGED -> BOND_BONDED before writing
  }
  ```
- **Pairing method:** the dash's existing `QBluetoothLocalDevice` plumbing
  (`bluetoothmanager.cpp`) already handles "Just Works" and PIN-display
  pairing callbacks — no numeric-comparison/MITM-protected pairing is wired
  up dash-side today. That means the encrypted link protects against
  passive eavesdropping but not an active attacker present at pairing time.
  **This matters more here than it did for the old config design**: a
  successful write to one of these characteristics directly energizes a
  physical low-side output in the car, not just a config value. Don't wire
  anything safety-relevant (fuel, ignition, anything ECU-adjacent) to an
  AUX output reachable this way — that's a wiring decision outside this
  app's control, but worth stating plainly given what this service can now
  actually do.
- One nuance specific to this dash's Bluetooth stack, worth knowing before
  implementing the GATT server dash-side: the raw-HCI path Qt5's BlueZ
  backend uses for *advertising* bypasses `bluetoothd` entirely (confirmed
  during hardware bring-up — see NOTES.md). The app log line seen live
  during that same testing, `qt.bluetooth: Using BlueZ kernel ATT
  interface`, confirms the ATT path itself — used for connections and GATT
  traffic — is reachable and going through the normal
  kernel/`bluetoothd`-mediated interface, not the advertising path's raw
  socket. **That's not the same as confirming a full local GATT server
  works** — `QLowEnergyController::addService()` plus characteristic
  read/write callbacks is a different, historically flakier corner of
  QtBluetooth's BlueZ backend that this session never exercised (only
  advertising + connection were tested). Treat `addService()` as unproven
  on this stack and spike it first, dash-side, before building the full
  characteristic set against it.

## GATT service definition

**Service: Ultima AUX Control Service**
UUID: `f6090001-ad4f-48f1-9b7f-a8d8a68b8c0b`

(Freshly generated for this document, not yet used anywhere — fine to keep
or regenerate before real implementation, just don't reuse a UUID from
another product. Replaces the old Configuration Service UUID entirely, not
alongside it.)

**UUID rule:** every characteristic UUID is the service UUID with *only the
first group* changed to the value shown in the table — the remaining four
groups (`ad4f-48f1-9b7f-a8d8a68b8c0b`) are identical across all of them. So
characteristic 1 (`...0002` below) is, in full,
`f6090002-ad4f-48f1-9b7f-a8d8a68b8c0b`.

| # | Characteristic | UUID (first group; rest is `-ad4f-48f1-9b7f-a8d8a68b8c0b`) | Properties | Security | Payload |
|---|---|---|---|---|---|
| 1 | AUX1 | `f6090002` | Write | Encrypted (bonded) | 1 byte: `0x00`=OFF, `0x01`=ON |
| 2 | AUX2 | `f6090003` | Write | Encrypted (bonded) | 1 byte: `0x00`=OFF, `0x01`=ON |
| 3 | AUX3 | `f6090004` | Write | Encrypted (bonded) | 1 byte: `0x00`=OFF, `0x01`=ON |
| 4 | AUX4 (PWM) | `f6090005` | Write | Encrypted (bonded) | 1 byte: `0`-`100` (PWM duty %) |
| 5 | AUX State | `f6090006` | Read, Notify | None | 4 bytes: last-commanded `AUX1..AUX4` values, same encoding as above |
| 6 | Status | `f6090007` | Read, Notify | None | UTF-8 string (see "Status characteristic" below) |

All writes are single bytes — no endianness concern.

### 1–3. AUX1 / AUX2 / AUX3 (on/off)

Maps to bytes 0/1/2 of the MCE18's AUX-command CAN frame (see "CAN Tx
protocol" below). Any byte value other than `0x00`/`0x01` should be rejected
via the Status characteristic rather than silently clamped — a phone-side
bug that sends garbage here should be visible, not swallowed, same principle
the old config spec applied to its Trip Reset characteristic.

Because the MCE18 expects all four outputs in a single combined CAN frame,
not one frame per output, `BluetoothManager` (or whatever owns the CAN Tx
path) needs to track the current value of all four AUX outputs itself and
recompose/resend the full 4-byte frame on every single-characteristic write
— a write to AUX2 must not clobber AUX1/AUX3/AUX4's last-commanded values.

```kotlin
gatt.writeCharacteristic(aux1Char, byteArrayOf(if (on) 0x01 else 0x00),
    BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT)
```

### 4. AUX4 (PWM-capable)

Byte value `0`-`100` (percent duty). Values above `100` should be rejected
via Status, not clamped. Maps to byte 3 of the same CAN frame, per the
CANchecked MCE18/CFE18 manual (Rev 2.0), which documents that byte as
"AUX4 (0|1 or pwm 0-100%)" — `0` is off, `100` is full/continuous on, values
between are PWM duty. **Unconfirmed against real hardware**: whether a
mid-range value (e.g. `50`) actually produces ~50% duty on the load side, or
whether the datasheet's phrasing means something subtly different — bench
test with a real load before trusting this for anything the driver depends
on. AUX1-3 on V3 hardware, and any car with a V3 MCE18 instead of V4, don't
support this at all — this characteristic only makes sense on V4 units.

### 5. AUX State (readback)

Read/Notify. Mirrors the dash's own last-commanded `AUX1..AUX4` values —
authoritative because the dash generated them, **not** because the MCE18
confirmed receipt or because the physical load actually switched. Notify
fires on every successful write plus once on connect, so a freshly-connected
app can sync its toggle/slider UI to actual state without guessing (e.g.
after being killed and relaunched, or after a different device made the
last change).

This is deliberately not wired to the MCE18's own outgoing status frame
(`0x702`, base ID `0x700` + 2), which per the same manual echoes a live
AUX1-3 bitmask back onto the bus at byte 3 of that frame (byte 0-1 = AIN8,
byte 2 = DIN0-7 — matching what `canbus.cpp` already decodes there — byte 3
= AUX1-3, byte 4 = Ethanol Temp, byte 5 = Freq Enabled mask, byte 6 =
Internal Temp, byte 7 = version) — real electrical-state confirmation, not
just "the dash thinks it sent this." Cross-checking a write against that
byte would be a genuine v2 improvement (only covers AUX1-3, not AUX4 — this
frame predates the V4 4th output). That whole frame is still flagged in
`GAUGE-CLUSTER.md`'s MCE18 section as "unverified, datasheet default, not
wire-confirmed" overall, so treat the AUX1-3 byte the same way: plausible
from the manual, not yet bench-confirmed against this car's actual unit.

### 6. Status

Simple text-based command/response, since this isn't a high-throughput
channel and human-readable strings make dash-side logging and Android-side
debugging both easier than inventing a binary status-code enum:

```
"<char-index>:OK"
"<char-index>:ERROR:<short message>"
```
e.g. `"4:ERROR:pwm value 150 out of range (0-100)"` after a rejected AUX4
write, or `"1:OK"` after AUX1 is successfully applied. Enable notifications
on connect:

```kotlin
gatt.setCharacteristicNotification(statusChar, true)
val cccd = statusChar.getDescriptor(CCCD_UUID) // 00002902-0000-1000-8000-00805f9b34fb
gatt.writeDescriptor(cccd, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
```

## CAN Tx protocol (dash → MCE18)

Per the CANchecked MCE18/CFE18 manual (Rev 2.0), "Configuration via CAN bus
frames": the MCE18 listens for AUX commands on a configurable "receive base
ID," **default `0x640`** — separate from the `0x700` base ID it *transmits*
sensor data on (which `canbus.cpp` already reads). This dash's MCE18 hasn't
been confirmed to actually be configured at the default receive ID any more
than its transmit ID has (see `GAUGE-CLUSTER.md`'s existing "not
wire-confirmed" caveat for the input side) — verify via the DSS config tool
or a candump session before trusting `0x640` in real code.

Frame layout (8 bytes, all others besides AUX1-4 unused):

| Byte | 0 | 1 | 2 | 3 | 4-7 |
|---|---|---|---|---|---|
| Meaning | AUX1 | AUX2 | AUX3 | AUX4 (0\|1 or PWM 0-100%) | unused |

`CanBus` needs a write path added to send this frame — nothing in the class
does this today (see "Status" above). A minimal shape: a
`setAux(int index, int value)`-style entry point that updates an in-memory
4-byte state array and writes one CAN frame (`struct can_frame` over the
existing `can0` socket, ID `0x640` — likely needs to be its own writable fd
use or reuse of `m_fd`, since the class currently only reads).

### Failsafe on BLE disconnect

This is the most safety-relevant new design decision here, so it gets its
own section rather than being buried in a characteristic table.

**When the BLE link drops (`QLowEnergyController::disconnected`), or
advertising is explicitly stopped, the dash must immediately send one CAN
frame of all zeros (`00 00 00 00`) — force every AUX output OFF — rather
than just stop sending.** Don't rely on a hypothetical MCE18-side receive
timeout to do this instead:

- The manual's own "Configuration via CAN bus frames" section documents a
  transmit rate/frequency setting for the MCE18's *outgoing* sensor data
  (`0x700` frames) — that's the opposite direction from the AUX command
  frame this doc is about, and the manual doesn't document any
  receive-timeout or required refresh rate for the AUX command frame
  itself.
- A secondary, non-manual source (a product page, not the datasheet)
  claims AUX commands must be resent every 100ms and that a 500ms gap
  triggers a failsafe that deactivates the outputs. Plausible, but
  unconfirmed against the primary document — worth a bench test (command an
  output on, stop sending entirely, watch whether the physical output
  actually drops around 500ms) before this dash-side design leans on it.

Given that uncertainty, treat the explicit all-zero send-on-disconnect as
the dash's own responsibility and the only failsafe to actually rely on.

**This uncertainty isn't just a safety margin — it decides whether the
feature works at all.** If the MCE18 really does require that ~100ms
refresh, a one-shot write per characteristic write means every AUX output
would silently switch itself back off ~500ms after being commanded, on
real hardware, the first time anyone tests this end-to-end — that reads as
a broken feature, not a missing safety net. So `CanBus` resends the current
4-byte AUX frame every 100ms whenever any output is non-zero (implemented,
see `sendAuxFrame()`/`updateAuxRefreshTimer()`), stopping once every output
is back to 0. This is the harmless-superset choice: negligible extra bus
traffic if the MCE18 turns out to latch, load-bearing if it doesn't. It is
**not** a substitute for the unconditional all-zero send on disconnect
above — that still fires immediately and separately, rather than waiting
for the refresh timer to notice outputs should be off.

## Open questions for whoever implements the dash side

Not Android-side concerns, but worth surfacing since they affect what the
Android app should expect:

- Confirm the MCE18's actual configured AUX-command receive ID via the DSS
  tool or `candump can0` (with a known command sent from a bench PC/DSS) —
  this doc assumes the datasheet default `0x640`.
- Confirm whether the AUX command frame actually needs periodic refreshing
  or is a one-shot latch — see "Failsafe on BLE disconnect" above.
  `CanBus` already resends every 100ms defensively (harmless either way),
  so this doesn't block anything working; it's just worth knowing for real,
  since a confirmed one-shot latch would let a future cleanup drop the
  timer.
- Bench-test AUX4's PWM duty behavior against a real load before exposing
  it as more than on/off in the Android UI.
- AUX4 PWM likely needs to be explicitly enabled in the DSS first (see the
  manual's §7.16/PWM-output option, §4.1) — a V4 unit may not accept or
  apply duty values on AUX4 until that's configured, even though the wire
  protocol accepts 0-100 unconditionally.
- `QLowEnergyController::addService()` is unproven on this stack (same
  caveat carried over from the old config-service design) — spike it first,
  dash-side, before building the full characteristic set against it.
- Should bonded devices be remembered (whitelist) so the driver doesn't
  re-pair every drive, or should the dash forget bonds on
  reboot/`stopAdvertising()`? Affects whether the Android app needs its own
  "forget this dash" UX.
- Once real loads are wired to AUX1-4, rename the characteristics/UI labels
  to match (e.g. "Light Bar", "Aux Fan") instead of the generic `AUX1..4`
  used throughout this document.
