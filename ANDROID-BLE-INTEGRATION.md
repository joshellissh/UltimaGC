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

## Status: implemented, not yet hardware-verified

As of 2026-08-20 (commit `b401514`), everything in this document is
implemented dash-side: `BluetoothManager` registers the Ultima AUX Control
Service (`ensureAuxService()`) with all 6 characteristics below, and
`CanBus` has a real Tx path (`setAux()`/`allAuxOff()`/`sendAuxFrame()`)
that sends the AUX-command CAN frame described in "CAN Tx protocol" below.
Advertising itself was already confirmed working on real hardware before
this (HCI trace + seen from Windows; see `beagleplay-falcon/NOTES.md`
"Bluetooth via CC1352P7" › "Hardware-verified working") — that part hasn't
changed.

**What's actually been verified, and what hasn't:**
- Compiles clean on both the macOS Qt6 dev build and the real Qt5/Yocto
  target toolchain (`beagleplay-falcon/build.sh ultima-app`, via Docker —
  no board needed for that part).
- On the macOS dev build, `addService()` was exercised at runtime and
  returned null (Qt's Darwin/CoreBluetooth backend needs an Info.plist
  entitlement this bare dev binary doesn't carry) — confirmed the null
  path is handled gracefully rather than crashing, but this says nothing
  about the target's Qt5/BlueZ backend, which is a different
  implementation entirely.
- **`addService()` has never run on the actual target stack (Qt5/BlueZ on
  the BeaglePlay), and no AUX CAN frame has ever reached a real MCE18.**
  No board was available this session. Treat the whole GATT-server-actually-
  works question as open until that pass happens — see "Connection &
  security model" below for why that's a real risk, not routine caution.
- The AUX-command CAN ID (`0x640`) and the periodic-refresh behavior are
  both still datasheet assumptions, not wire-confirmed — see "CAN Tx
  protocol" below.

Treat the GATT table below as the actual current contract (not a proposal)
for both `BluetoothManager`'s dash-side implementation and the Android app,
so the two sides stay in sync — but validate the dash side on real hardware
before trusting it beyond "it compiles."

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
- **Bonding.** All four AUX write characteristics require an encrypted,
  authenticated link — see the per-characteristic table below. Android
  auto-triggers pairing the first time an app touches an attribute that
  requires encryption and the link isn't encrypted yet (fires
  `BluetoothDevice.ACTION_PAIRING_REQUEST`), but that shows up to the app
  as a failed write that needs a retry once bonding completes, which is a
  worse UX than doing it upfront. Recommend proactively bonding right after
  service discovery, before any writes:
  ```kotlin
  if (device.bondState == BluetoothDevice.BOND_NONE) {
      device.createBond()
      // wait for ACTION_BOND_STATE_CHANGED -> BOND_BONDED before writing
  }
  ```
- **Pairing method — real hardware bug found and fixed, 2026-08-20.**
  `bluetoothmanager.cpp` used to assume `QBluetoothLocalDevice`'s Qt5/BlueZ
  backend registered a working BlueZ pairing agent internally (that's what
  its `pairingDisplayConfirmation`/`pairingDisplayPinCode` signals implied).
  First real end-to-end pairing attempt against actual hardware proved that
  wrong: every attempt failed with `bluetoothd` itself logging `No agent
  available for request type 2` / `device_confirm_passkey: Operation not
  permitted`, regardless of the adapter's `Pairable` state — there was
  simply nobody for BlueZ to ask. Fixed by implementing `org.bluez.Agent1`
  directly (`bluetoothagent.h`/`.cpp`, `BluetoothAgent`), registered via
  `BluetoothManager::registerAgent()` with IO capability `DisplayYesNo` —
  this dash has a screen and can show yes/no, but no keyboard. That drives
  BlueZ toward Numeric Comparison (`RequestConfirmation`) when the peer also
  supports display+confirm, which the existing `BluetoothScreen.qml`
  Accept/Reject panel already assumed. `confirmPairing()` now replies to
  the deferred D-Bus call `BluetoothAgent::RequestConfirmation()` hands off,
  not to `QBluetoothLocalDevice` (that path is removed — it never worked).
  **As of 2026-08-20 the AUX write characteristics also require
  `AttAuthenticationRequired` in addition to `AttEncryptionRequired`**
  (`bluetoothmanager.cpp`'s `makeAuxWriteChar()`), specifically so a plain
  unauthenticated "Just Works" bond can't satisfy a write — BlueZ has to
  reach an MITM-protected method (numeric comparison, now that an agent
  actually exists to complete one) or the write fails. **This matters more
  here than it did for the old config design**: a successful write to one
  of these characteristics directly energizes a physical low-side output in
  the car, not just a config value. Don't wire anything safety-relevant
  (fuel, ignition, anything ECU-adjacent) to an AUX output reachable this
  way — that's a wiring decision outside this app's control, but worth
  stating plainly given what this service can now actually do.
  Hardware-verified: the refusal path (an unbonded phone's write correctly
  comes back `ATT error 0x0F Insufficient Encryption`, then the link resets
  when BlueZ can't complete pairing while `Pairable: no`), and — once the
  agent above actually existed — a phone completing a Passkey Entry bond
  for real (`bluetoothctl info <addr>` showed `Paired: yes` / `Bonded: yes`
  / `Connected: yes`) with a subsequent AUX write succeeding (visible as a
  burst of periodic AUX CAN-frame sends in the journal, `CanBus`'s
  100ms-refresh-while-nonzero behavior — see "Failsafe on BLE disconnect"
  above).

  **Second real hardware bug found in the same test, also fixed
  2026-08-20**: after that successful pairing, `BluetoothScreen.qml`'s
  "Pairing with..." prompt never closed. Root cause: Passkey Entry's
  display-only side (`BluetoothAgent::DisplayPasskey`/`DisplayPinCode`) has
  no explicit "you're done" callback on the Agent1 interface at all — BlueZ
  completes the procedure entirely on its own once the peer finishes
  typing. This dash was relying on `QBluetoothLocalDevice::pairingFinished`
  to notice that and close the dialog, and — matching the *exact* class of
  bug the agent registration above already found once — that signal simply
  never fired on this stack either. Fixed by watching `org.bluez.Device1`'s
  `Paired` property directly over D-Bus
  (`BluetoothManager::watchDevicePaired()`/`onDevicePropertiesChanged()`),
  scoped to the specific device path via `showPairingCode()`, which now
  receives that path from `BluetoothAgent` instead of guessing from
  whichever device happens to be currently connected. Also fixed in the
  same pass: `DisplayPasskey`'s code wasn't zero-padded to 6 digits
  (`QString::number(passkey)` would show "42" instead of "000042") — not
  just a cosmetic gap, since that's literally the wrong code to type in for
  any passkey under 100000.
- One nuance specific to this dash's Bluetooth stack, worth knowing before
  trusting the GATT server dash-side: the raw-HCI path Qt5's BlueZ
  backend uses for *advertising* bypasses `bluetoothd` entirely (confirmed
  during hardware bring-up — see NOTES.md). The app log line seen live
  during that same testing, `qt.bluetooth: Using BlueZ kernel ATT
  interface`, confirms the ATT path itself — used for connections and GATT
  traffic — is reachable and going through the normal
  kernel/`bluetoothd`-mediated interface, not the advertising path's raw
  socket. **That's not the same as confirming a full local GATT server
  works** — `QLowEnergyController::addService()` plus characteristic
  read/write callbacks is a different, historically flakier corner of
  QtBluetooth's BlueZ backend. The full characteristic set (below) is now
  implemented against this API and compiles on the target Qt5 toolchain,
  but **it has never actually run against BlueZ** — no board was available
  when it was built. Treat "does a central actually see this service, and
  can it read/write/subscribe to it" as the first thing to check once
  hardware is available, not something already settled by the code
  existing.

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

Implemented as `CanBus::setAux(int index, int value)` — updates an
in-memory 4-byte state array and writes one CAN frame (`struct can_frame`,
ID `0x640`) by reusing the same `m_fd` the class already reads from
(SocketCAN sockets are bidirectional; no separate fd needed). `allAuxOff()`
zeroes the state and sends the same frame unconditionally — see "Failsafe
on BLE disconnect" below.

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
- `QLowEnergyController::addService()` and the full characteristic set are
  implemented and compile on the target toolchain, but have never run
  against the target's Qt5/BlueZ backend — this is the first thing to check
  once a board is available (connect a central, confirm the service is
  discoverable, confirm a write actually reaches `onAuxCharacteristicChanged()`).
  If it turns out to be broken on BlueZ, this may need a fallback design,
  not just a bugfix.
- **Answered, 2026-08-20: bonded devices are remembered.** Two pieces,
  both implemented, neither yet hardware-verified:
  - Bonds now persist across reboots — `tisdk-base-image.bbappend`'s
    `ultima_bluetooth_persist_bonds()` bind-mounts `/data/bluetooth` (the
    one partition that survives a power cycle) over BlueZ's
    `/var/lib/bluetooth`, which otherwise lives on this image's tmpfs
    `/var/lib` and gets wiped every boot.
  - New bonding is closed by default and only opens for a timed window the
    driver has to deliberately start — `BluetoothManager::enterPairingMode()`
    (wired to a "PAIR NEW DEVICE" button on `BluetoothScreen.qml`) sets
    `org.bluez.Adapter1`'s `Pairable` property true via QtDBus for 2 minutes
    (`kPairingModeTimeoutMs`) or until the driver closes the screen;
    outside that window the adapter is non-bondable, which refuses a new
    bonding attempt at the adapter level regardless of association method
    (unlike gating the `pairingDisplayConfirmation`/`pairingDisplayPinCode`
    signals, which a Just Works negotiation with no display step would
    bypass entirely). An already-bonded phone connects and reconnects at
    any time either way — reconnecting reuses the stored link key and never
    triggers a new bonding procedure, so it's unaffected by `Pairable`.
  - No "forget this dash" UX needed on the Android side beyond what
    Android's own Bluetooth settings already provide (unpair from there) —
    nothing here needs its own forget-bond flow.
  - Needs hardware verification like everything else newer in this
    document: confirm `Pairable=false` actually refuses a new bond attempt
    rather than merely hiding it from `bluetoothctl`, and confirm the
    bind-mounted bond database actually survives a real reboot.
- Once real loads are wired to AUX1-4, rename the characteristics/UI labels
  to match (e.g. "Light Bar", "Aux Fan") instead of the generic `AUX1..4`
  used throughout this document.
