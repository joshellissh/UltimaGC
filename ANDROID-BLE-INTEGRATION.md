# Android BLE Integration — Ultima Configuration Service

Spec for an Android companion app to connect to the Ultima gauge cluster over
BLE and read/write its configuration. Covers both sides: what the dash needs
to expose (currently doesn't) and exactly what the Android app needs to call.

## Status: nothing below exists yet except advertising

As of 2026-08-19, `BluetoothManager` (`ultima-app/src/bluetoothmanager.cpp`)
only advertises the dash as a connectable, discoverable BLE peripheral named
`"Ultima RS"` — confirmed working on real hardware (HCI trace + seen from
Windows; see `beagleplay-falcon/NOTES.md` "Bluetooth via CC1352P7" ›
"Hardware-verified working"). **It registers no GATT service and no
characteristics.** A central can connect to it today, but there is nothing
to read or write once connected.

Everything in this document — the service, every characteristic, the WiFi
live-apply behavior — is a design to implement, not a description of
working code. Treat the GATT table below as the target for
`BluetoothManager`'s dash-side implementation and for the Android app both,
so the two sides are built against the same contract instead of drifting.

Also relevant going in: **a phone's own Bluetooth Settings app cannot do any
of this.** Neither iOS nor Android's built-in Settings pairs with or talks
to a bare BLE peripheral like this one — confirmed by testing (see NOTES.md,
same section). A dedicated app using Android's `BluetoothGatt` APIs — i.e.
exactly what this document specifies — is the only way to reach this
service from a phone. That's not a limitation of this design; it's why a
companion app is necessary at all.

## Scope

Four configuration surfaces, matched to what a human can already edit on
the dash's own touchscreen (no new capability beyond that — this moves
existing settings to a phone, it doesn't add new ones):

| Surface | Dash-side screen today | Backing object |
|---|---|---|
| System time | `SetTimeScreen.qml` | `SystemClock` |
| Trip odometer reset | Reset button on `main.qml` | `OdoStore` |
| 360° camera calibration | `CalibrationSettingsScreen.qml` | `CalibrationStore` |
| WiFi credentials | SSH-only manual step (`NOTES.md` "WiFi client mode") | `/data/wifi-client.conf` |

## Advertising & discovery

Dash advertises (confirmed live, `btmon` trace in NOTES.md):
- Flags: `LE General Discoverable Mode` + `BR/EDR Not Supported`
- Local name (complete): `Ultima RS`
- **No service UUID in the advertisement today.** Recommend adding the
  Ultima Configuration Service UUID (below) to the advertising data or scan
  response once the GATT service exists — lets the Android app filter scans
  by service UUID instead of by name string, which is more robust (names
  aren't guaranteed unique or stable; a phone that's seen the SSID once
  could also just remember the name, but a service UUID filter is the
  standard, more reliable pattern). Until then, name-based filtering is the
  only option.

```kotlin
val scanFilter = ScanFilter.Builder()
    .setDeviceName("Ultima RS")
    // Once the dash advertises the service UUID:
    // .setServiceUuid(ParcelUuid(ULTIMA_SERVICE_UUID))
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
- **Request a larger MTU before touching the Calibration characteristic** —
  its JSON payload won't fit in the default 23-byte ATT MTU (20 usable
  bytes). Do this right after `STATE_CONNECTED`, before
  `discoverServices()`:
  ```kotlin
  override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
      if (newState == BluetoothProfile.STATE_CONNECTED) {
          gatt.requestMtu(247)   // wait for onMtuChanged before large writes
      }
  }
  override fun onMtuChanged(gatt: BluetoothGatt, mtu: Int, status: Int) {
      gatt.discoverServices()
  }
  ```
  247 is a safe, widely-supported target (fits comfortably under BLE's
  517-byte ceiling and under what most controllers, including the CSR
  dongle currently in use, negotiate without issue). Treat MTU negotiation
  as the primary way to fit the calibration JSON, not a fallback — Android
  *can* split an oversized characteristic write into GATT "long write"
  Prepare-Write/Execute-Write sequences automatically, but that behavior is
  stack- and OS-version-dependent in practice, not a guarantee to build on.
  If a negotiated MTU still ends up smaller than the JSON payload on some
  device, don't assume long-write will silently handle it — test on the
  actual OS/stack combination in question before relying on it.

- **Bonding.** Characteristics that write real config (time, trip reset,
  calibration, WiFi) require an encrypted link — see the per-characteristic
  table below. Android auto-triggers pairing the first time an app touches
  an attribute that requires encryption and the link isn't encrypted yet
  (fires `BluetoothDevice.ACTION_PAIRING_REQUEST`), but that shows up to the
  app as a failed write that needs a retry once bonding completes, which is
  a worse UX than doing it upfront. Recommend proactively bonding right
  after service discovery, before any writes:
  ```kotlin
  if (device.bondState == BluetoothDevice.BOND_NONE) {
      device.createBond()
      // wait for ACTION_BOND_STATE_CHANGED -> BOND_BONDED before writing
  }
  ```
- **Pairing method:** the dash's existing `QBluetoothLocalDevice` plumbing
  (`bluetoothmanager.cpp`) already handles "Just Works" and PIN-display
  pairing callbacks — no numeric-comparison/MITM-protected pairing is
  wired up dash-side today. That means the encrypted link protects against
  passive eavesdropping but not an active attacker present at pairing
  time. Acceptable for time/calibration; worth a second look before
  shipping the WiFi PSK characteristic (see its own note below) if a wider
  threat model matters.
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

**Service: Ultima Configuration Service**
UUID: `debf0001-4f5e-4061-a31c-cd017e5c7fb7`

(Freshly generated for this document, not yet used anywhere — fine to keep
or regenerate before real implementation, just don't reuse a UUID from
another product.)

**UUID rule:** every characteristic UUID is the service UUID with *only the
first group* changed to the value shown in the table — the remaining four
groups (`4f5e-4061-a31c-cd017e5c7fb7`) are identical across all of them.
So characteristic 1 (`...0002` below) is, in full,
`debf0002-4f5e-4061-a31c-cd017e5c7fb7`.

| # | Characteristic | UUID (first group; rest is `-4f5e-4061-a31c-cd017e5c7fb7`) | Properties | Security | Payload |
|---|---|---|---|---|---|
| 1 | Time Set | `debf0002` | Write | Encrypted (bonded) | 2 bytes: `hour` (u8, 0–23), `minute` (u8, 0–59) |
| 2 | Odometer | `debf0003` | Read, Notify | None | 8 bytes: `totalOdo` (f32 LE), `tripOdo` (f32 LE) |
| 3 | Trip Reset | `debf0004` | Write | Encrypted (bonded) | 1 byte: must be `0x01` |
| 4 | Calibration Config | `debf0005` | Read, Write | Encrypted (bonded) | UTF-8 JSON, `CalibrationSet` shape (below) |
| 5 | WiFi SSID | `debf0006` | Write | Encrypted (bonded) | UTF-8 string, ≤32 bytes |
| 6 | WiFi PSK | `debf0007` | Write (no read) | Encrypted (bonded) | UTF-8 string, 8–63 bytes |
| 7 | WiFi Apply | `debf0008` | Write | Encrypted (bonded) | 1 byte: must be `0x01` |
| 8 | Status | `debf0009` | Read, Notify | None | UTF-8 string (see "Status characteristic" below) |

All multi-byte numeric fields are **little-endian**.

Characteristic #8 (Status) isn't something the user asked for by name, but
is close to required for a usable app: without it, the Android app only
ever knows a write reached the ATT layer, not whether the dash actually
applied it (e.g. "trip reset succeeded" vs. "WiFi psk was too short and
got rejected"). Recommended, not mandatory — flagging it as an addition
rather than folding it in silently.

### 1. Time Set

Maps directly to `SystemClock::setTime(int hour, int minute)`
(`systemclock.h`) — same semantics as `SetTimeScreen.qml`: sets *today's*
date at the given wall-clock hour/minute, no timezone or date component.
Seconds are zeroed by the existing implementation.

```kotlin
val payload = byteArrayOf(hour.toByte(), minute.toByte())
gatt.writeCharacteristic(timeSetChar, payload, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT)
```
(Android 13+/API 33 signature shown; pre-33 uses the deprecated
`characteristic.value = payload; gatt.writeCharacteristic(characteristic)`
pattern — both are common to see in the wild depending on `minSdk`.)

### 2. Odometer

Read-only mirror of `OdoStore::totalOdo`/`tripOdo` (`odostore.h`). Notify
so the app can show a live value without polling — dash-side, wire this to
fire on the same 30s autosave tick `main.qml` already runs
(`sim.save()`), not on every CAN-driven value change (that would be a lot
of BLE notification traffic for a slowly-changing number).

### 3. Trip Reset

Write `0x01` to reset — maps to `OdoStore::setTripOdo(0.0)` followed by
`save()`, the same effect as `main.qml`'s existing reset button. Any other
byte value: reject (respond `GATT_INVALID_ATTRIBUTE_VALUE_LENGTH` or a
custom application error, dash-side implementer's call), don't silently
ignore it — a phone-side bug that sends garbage here should be visible,
not swallowed.

### 4. Calibration Config

Read/write the same JSON shape `CalibrationSet::toJson()` /
`CalibrationSet::fromJson()` already produce/consume
(`cameracalibration.h`) — the dash side should be able to implement this
characteristic as close to "hand the ATT read/write straight to the
existing (de)serializer" as possible, no new format to invent.

**Scope the writable fields to exactly what `CalibrationSettingsScreen.qml`
already exposes on the touchscreen** — i.e. `CalibrationCameraModel`'s and
`CalibrationGeometryModel`'s `Q_PROPERTY` sets, not the full underlying
structs:

- Per camera (`front`, `rear`, `left`, `right`), each a JSON object:
  `posXInches`, `posYInches`, `posZInches`, `yawDegrees`, `pitchDegrees`,
  `fovDegrees` — all `double`.
- `geometry`: `vehicleLengthInches`, `vehicleWidthInches`,
  `groundHalfExtentXInches`, `groundHalfExtentYInches`,
  `wedgeOverlapDegrees`, `carIconScale` — all `double`.

`CameraCalibration`'s other fields (`imageWidth`, `imageHeight`,
`principalPointX/Y`, `k1`–`k4`) and `SurroundGeometryConfig`'s
`meshGridResolution` are deliberately **not** user-tunable today (per
`calibrationstore.h`'s own comments) — a real read will still include them
(since they're part of the JSON `CalibrationSet::toJson()` shape), but a
write should either omit them or have the dash ignore changes to them,
matching the existing touchscreen UI's own restrictions rather than
quietly granting the phone more control than the dash itself has.

Example JSON shape (values illustrative, not the real defaults — see
`cameracalibration.h`'s `defaultCalibration()` for those):
```json
{
  "front": { "posXInches": 82.0, "posYInches": 0.0, "posZInches": 20.0,
             "yawDegrees": 0.0, "pitchDegrees": 52.0, "fovDegrees": 185.0 },
  "rear":  { "...": "..." },
  "left":  { "...": "..." },
  "right": { "...": "..." },
  "geometry": {
    "vehicleLengthInches": 164.0, "vehicleWidthInches": 75.0,
    "groundHalfExtentXInches": 216.5, "groundHalfExtentYInches": 216.5,
    "wedgeOverlapDegrees": 20.0, "carIconScale": 1.0
  }
}
```

Write should call the same path `CalibrationSettingsScreen.qml` triggers on
edit — apply to the live `CalibrationStore` (so `SurroundView` re-warps
immediately via `calibrationChanged()`, same as a touchscreen edit) — then
`save()` to persist to `/data/calibration.json`. Validate before applying:
this JSON is arriving from a phone over BLE, not a trusted local UI, so
malformed/missing fields or out-of-range values (e.g. a FOV of -50) should
be rejected via the Status characteristic rather than fed straight into
`WarpMesh`.

### 5–7. WiFi SSID / WiFi PSK / WiFi Apply

**This is the one surface that needs genuinely new dash-side behavior, not
just a GATT wrapper around an existing object.** Today,
`/data/wifi-client.conf` is provisioned manually over SSH (see NOTES.md
"WiFi client mode") and is only ever read once, at boot, by
`ultima-wpa-supplicant-config.service` (a `oneshot` that concatenates a
static base config with `/data/wifi-client.conf` into
`/run/wpa_supplicant-wlan0.conf`, which `wpa_supplicant@wlan0.service`
then uses). Writing the file while the system is already running has no
effect until the next reboot, as things stand.

For BLE provisioning to actually be useful (connect to WiFi without asking
the driver to power-cycle the board), the WiFi Apply handler needs to:
1. Assemble a `network={ ssid="..." psk="..." }` block from the two
   characteristics' current values — standard wpa_supplicant config
   syntax, matching what a human would have typed by hand over SSH.
   **Escape `"` and `\` in both fields** before embedding them in the
   quoted config syntax — an SSID or password containing either character
   would otherwise break the generated config.
2. Write it to `/data/wifi-client.conf` (same file, same format the boot
   path already expects — no schema change).
3. Re-run the same assembly the boot-time oneshot does (`cat` base +
   new file → `/run/wpa_supplicant-wlan0.conf`) and
   `systemctl restart wpa_supplicant@wlan0.service`, so the new
   credentials take effect immediately. This part doesn't exist anywhere
   in the codebase yet — it's new work, not a wrapper.
4. Report success/failure (did wpa_supplicant actually associate, or does
   it silently keep retrying against an unreachable/wrong-password AP?)
   via the Status characteristic. wpa_supplicant's own state
   (`wpa_cli status` / D-Bus `wpa_supplicant1.Interface.State`) is the
   source of truth for this — polling or subscribing to that after the
   restart is how the dash would know what to report back, rather than
   just assuming success once the service restarts cleanly.

**Split into SSID/PSK/Apply as three characteristics (not one combined
write)** deliberately: it lets the Android app show which specific field
failed validation (e.g. PSK too short — WPA2-PSK requires 8–63 characters)
before committing to an apply attempt, and keeps the password itself out
of any characteristic that also needs read permission for anything else.

**WiFi PSK security note:** the PSK transits the BLE link in plaintext,
protected only by link-layer encryption (see the pairing-method note under
"Connection & security model" above — this dash only does "Just Works"
pairing today, no MITM protection). That's a real, active-attacker-in-range
threat model, not just theoretical — worth a second look (e.g. requiring
numeric-comparison pairing instead of Just Works, specifically gating this
characteristic) before treating this as production-ready for anything more
sensitive than a home WiFi password the user already typed into the router
once.

```kotlin
// After validating locally (SSID non-empty & ≤32 bytes, PSK 8-63 bytes):
gatt.writeCharacteristic(wifiSsidChar, ssid.toByteArray(Charsets.UTF_8), WRITE_TYPE_DEFAULT)
gatt.writeCharacteristic(wifiPskChar, psk.toByteArray(Charsets.UTF_8), WRITE_TYPE_DEFAULT)
gatt.writeCharacteristic(wifiApplyChar, byteArrayOf(0x01), WRITE_TYPE_DEFAULT)
// Then watch the Status characteristic's notifications for the result.
```
(Writes must complete sequentially — wait for each `onCharacteristicWrite`
callback before issuing the next; `BluetoothGatt` only allows one
outstanding operation at a time.)

### 8. Status (recommended)

Simple text-based command/response, since this isn't a high-throughput
channel and human-readable strings make dash-side logging and Android-side
debugging both easier than inventing a binary status-code enum:

```
"<char-index>:OK"
"<char-index>:ERROR:<short message>"
```
e.g. `"7:ERROR:psk too short (min 8 chars)"` after a failed WiFi Apply, or
`"4:OK"` after a successful calibration write. Enable notifications on
connect:

```kotlin
gatt.setCharacteristicNotification(statusChar, true)
val cccd = statusChar.getDescriptor(CCCD_UUID) // 00002902-0000-1000-8000-00805f9b34fb
gatt.writeDescriptor(cccd, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
```

## Open questions for whoever implements the dash side

Not Android-side concerns, but worth surfacing since they affect what the
Android app should expect:

- Does the dash reject a Calibration write outright on invalid JSON/fields,
  or clamp to safe ranges? Changes what the Android app should validate
  client-side vs. rely on the Status characteristic to catch.
- Should bonded devices be remembered (whitelist) so the driver doesn't
  re-pair every drive, or should the dash forget bonds on
  reboot/`stopAdvertising()`? Affects whether the Android app needs its
  own "forget this dash" UX.
- WiFi Apply's "did it actually connect" check (state (4) above) needs a
  concrete timeout — wpa_supplicant can sit in a slow retry loop against a
  wrong password indefinitely. Recommend the dash bound this (e.g. 15s)
  and report a timeout as its own Status error rather than leaving the
  Android app waiting.
