# Firmware QA report: display and Bluetooth note sync

- **Repo:** https://github.com/jakepi-bot/esp32-note-device
- **Commit under test:** `0588422ab7c607bd86916648f6cb50ad95117cf5` (`0588422`, main)
- **Verdict:** **FAIL on hardware.** The display work passes; Bluetooth note sync does not start on the board. One defect is a blocker, reproduced 100%, with a verified one-line fix. Three further defects/limitations are recorded below.
- **Date:** 2026-11-20 21:40–22:10 CST
- **Auditor:** qabot (read-only; no repository file was modified or pushed)

## 1. Environment

| Item | Value |
|---|---|
| Host | Linux 6.12.117+deb13-amd64, Debian 13, x86_64 |
| Board | Waveshare ESP32-S3-ePaper-1.54 V2 (ESP32-S3-PICO-1-N8R8, 8 MB flash, 8 MB PSRAM) |
| Serial | `/dev/ttyACM0`, USB Serial/JTAG, 115200 baud |
| Toolchain | PlatformIO Core 6.2.0, ESP-IDF 6.1.0 (framework-espidf 4.60100.0), xtensa-esp-elf 15.2.0 |
| Host compiler | CMake 3.x + system GCC (`ctest --test-dir build/host`) |
| Node | v22 (`node --test`) |
| Bluetooth on host | **none** (`/sys/class/bluetooth` absent, no HCI device), so no Linux BLE central was available |
| iPhone / Bluefy | **not available to this run** |

The working tree was a fresh `git clone` of the public repo. Builds were run from
`firmware/esp32/` because that is where `platformio.ini` lives; the README's
`pio run -e waveshare-epaper-154` assumes that directory.

## 2. Build verification

Clean build (`.pio/build` removed first):

```sh
cd firmware/esp32
rm -rf .pio/build
PYTHONPATH= ~/.hermes/tools/platformio-venv/bin/platformio run -e waveshare-epaper-154
```

Result: **SUCCESS**, 104.2 s, from an empty build directory.

| Metric | Measured | Parent claim |
|---|---|---|
| App size | 2,030,667 B | 2,030,667 B |
| `firmware.bin` on disk | 2,031,184 B | 2,031,184 B |
| OTA slot | 4,063,232 B (`ota_0`, 8 MB partition table) | 3,968 KB |
| Slot free | 50.0% | 50% |
| Static RAM | 86,492 B / 327,680 B (26.4%) | 86,492 B |
| `firmware.bin` SHA-256 | `2d21563c543799064722153d2d5dc1b2e502023b3b404ace961335fa388458cd` | — |

The size and RAM figures reproduce exactly. The binary is not bit-reproducible
across builds because ESP-IDF embeds the build timestamp in `app_desc`
(`Compile time: Nov 20 2026 21:44:09`); size and configuration are deterministic.

CI on the tested commit was re-checked independently:

```text
gh run list --limit 8
  0588422  Firmware success 4m15s | CI success 6m53s | Installer site success 46s | Hermes success 13m9s
gh api .../commits/0588422/check-runs
  all 14 checks completed/success (incl. ESP32 firmware, Host core tests, Windows core)
```

## 3. Test results

Legend: PASS = measured on the board or by re-running the test; FAIL = measured
failure; NOT TESTED = could not be exercised with the equipment in this run.

| # | Test case | Result | Evidence |
|---|---|---|---|
| T1 | Clean build of `0588422` | **PASS** | §2; sizes match the parent handoff |
| T2 | Host test suite `ctest` | **PASS** | 7/7: core, cores3 board, CJK, e-paper mode, e-paper BLE, e-paper notes, tailtiny |
| T3 | e-paper status renderer, 12 cases | **PASS** | `./build/host/hg_epaper_mode_tests` → `12 cases, 0 failed` |
| T4 | BLE note protocol, 16 cases | **PASS** | `./build/host/hg_epaper_ble_tests` → `16 cases, 0 failed` |
| T5 | Phone half (relay), 11 cases | **PASS** | `node --test web/ble-note-relay/selftest.mjs` → 11 pass, 0 fail |
| T6 | Flash `0588422` over USB and boot | **PASS (flash)** / **FAIL (BLE init)** | `Wrote 2031184 bytes at 0x00020000`, hash verified; boot then logs the D1 error |
| T7 | BLE service registers on real NimBLE | **FAIL** | D1: `E NimBLE: ble_gatts_count_resources rc=3`, `E hg.phone: can't register the phone service (3)` |
| T8 | `phone` console command | **FAIL** | `@error Bluetooth is not enabled on this board` |
| T9 | `notes ble` / `notes status` | **FAIL** | `@error this build has no Bluetooth note sync`; `… | 0 B queued | ble off` |
| T10 | Resting screen text and icons | **PASS** | `8 NOTES` / `Newest note 01:47, 16 s.` / `Tap PWR to add another.`; clock, Wi-Fi whole, Bluetooth cut, battery bolt |
| T11 | Legibility floor | **PASS** | Measured on the 200×200 dump: clock and hero 21 px (2.91 mm at 183 dpi), body/footer 14 px (1.94 mm). Meets the 1.94 mm floor; no clipping or truncation |
| T12 | Pending-note indicator | **PASS** | With one queued note: hero `9 NOTES`, footer `1 QUEUED` |
| T13 | Battery level and local time | **PASS** | With `set battery_estimate liion` + `set timezone CST6CDT,…`: footer `96% · ON POWER`, clock `21:53` |
| T14 | Default out-of-box battery/time | **FAIL (UX)** | D2: `get battery_estimate` → `""`, `get timezone` → `""`; screen shows `NO BATTERY` and UTC |
| T15 | Pairing screen renders and is legible | **PASS** | `Pair a phone`, `Bluetooth: Hermes Gadget`, instructions and `CANCEL` all present |
| T16 | Pairing screen visual consistency | **FAIL (low)** | D3: 94% of the panel is black (white-on-black), the resting screen is 11% black (black-on-white) |
| T17 | BLE service registers after the D1 fix | **PASS** | `I hg.phone: Bluetooth ready`; 0 error lines in the boot log |
| T18 | `notes ble` after the fix | **PASS** | `@notes ble link down mtu 23 idle` |
| T19 | `notes status` after the fix | **PASS** | `… | 0 B queued | ble away` |
| T20 | `phone pair` opens advertising | **PASS** | `@ok phone pairing open`; NimBLE `GAP procedure initiated: advertise; adv_itvl_min=48 adv_itvl_max=96` |
| T21 | iOS pairing, passkey, MTU, throughput | **NOT TESTED** | No iPhone and no BLE central on the host |
| T22 | End-to-end BLE note pull + POST to Hermes | **NOT TESTED** | Same as T21; the host-only relay test (T5) covers only the packet/upload half |
| T23 | Pairing survives a USB flash | **NOT TESTED** | No phone paired; settings in NVS did survive two USB flashes, which is indirect evidence only |
| T24 | Wi-Fi + BLE coexist while transferring | **PARTIAL** | Wi-Fi associated/online and BLE advertising together after the fix; no simultaneous transfer observed |
| T25 | Note upload over Wi-Fi (regression check) | **PASS** | A 2 s note uploaded from the board: `sent 9 pending 0`, `notes: note … stored: 99.4 KB`, vault file `2026-11-20_2154_hg-0123456789abcdef_8f8f694c.md` created |
| T26 | Console command during a full refresh | **FAIL (medium)** | D4: `notes stop` → `@error device busy`, then `W hg.events: event queue full; dropping event 6` repeatedly; reproduced 2/2 |
| T27 | New note recorded in the first seconds after reset | **LIMITATION** | D5: newest-note line reads `0s, no clock yet.` although the clock shows the time; a note recorded 10 s after boot shows `03:54, 2 s.` |

## 4. Defects

### D1 — BLOCKER: BLE service never registers; `ble_gatts_count_resources rc=3`

**Where:** `firmware/esp32/main/port_ble.cpp`, the note data characteristic
(`g_note_chrs[1]`, around lines 445–448).

**Symptom on hardware (deterministic; observed on the boot after flashing `0588422`):**

```text
I (1282) BLE_INIT: Bluetooth MAC: ac:27:6e:d3:75:ca
E (1282) NimBLE: ble_gatts_count_resources rc=3
E (1282) hg.phone: can't register the phone service (3)
```

Consequences:

- `nimble_port_freertos_init()` is never reached, so GAP never advertises and
  `NoteLink::available()` is false forever.
- `phone` answers `@error Bluetooth is not enabled on this board`.
- `notes ble` answers `@error this build has no Bluetooth note sync`.
- `notes status` ends `| ble off`.
- The resting screen keeps the Bluetooth icon cut, correctly, because there is no
  link to show.

**Root cause:** the data characteristic is declared with flags `NOTIFY` and a
value handle but **no `access_cb`**. NimBLE's `ble_gatts_chr_is_sane()`
(`components/bt/host/nimble/nimble/host/src/ble_gatts.c:494`) returns 0 when
`chr->access_cb == NULL`; `ble_gatts_count_resources()` therefore returns
`BLE_HS_EINVAL` (3), and `PhoneLink::begin()` aborts before registering any
service, including the pre-existing phone console service.

```cpp
g_note_chrs[1] = {};
g_note_chrs[1].uuid = &g_note_data_uuid.u;
g_note_chrs[1].flags = BLE_GATT_CHR_F_NOTIFY;   // no access_cb -> EINVAL
g_note_chrs[1].val_handle = &g_note_data_handle;
```

This escaped the parent's tests because `port_ble.cpp` is ESP-only: the 16-case
`hg_epaper_ble_tests` exercise the protocol, not the GATT table, and CI only
compiles the file.

**Fix (validated, not committed):**

```diff
   g_note_chrs[1] = {};
   g_note_chrs[1].uuid = &g_note_data_uuid.u;
+  g_note_chrs[1].access_cb = on_note_access;  // NimBLE refuses a characteristic with no access callback
   g_note_chrs[1].flags = BLE_GATT_CHR_F_NOTIFY;
   g_note_chrs[1].val_handle = &g_note_data_handle;
```

`on_note_access` already rejects anything that is not a WRITE, and the
characteristic has no READ flag, so this only satisfies the GATT table validator.

**Fix verification:** applied in a scratch clone, clean build 2,031,184 B
(`firmware.bin` SHA-256 `12a65cc9a278697c8c44986c1d7975c5db144fc9d7d2a0b3fb335cf825fb42ed`),
flashed to the same board:

```text
I (1302) hg.phone: Bluetooth ready
E lines in boot log: 0
phone       -> @phone {"paired":0,"connected":false,"pairing":false}
notes status-> … | 0 B queued | ble away
notes ble   -> @notes ble link down mtu 23 idle
phone pair  -> @ok phone pairing open + NimBLE advertise initiated
```

The board is currently running this patched build, not `0588422`.

### D2 — MEDIUM: out-of-the-box screen contradicts the brief (NO BATTERY, UTC)

`battery_estimate` defaults to unset (treated as `off`) and `timezone` to unset
(UTC0). Fresh from the factory image the user sees:

- `NO BATTERY` in the footer even though the pack is fitted and the bolt is lit;
- the clock in UTC (`03:46` when local time is `21:46`).

The brief asked for battery and time status. On the same hardware, with the
documented settings applied, the screen renders `96% · ON POWER` and `21:53`
correctly, so this is a default-configuration/UX defect, not a renderer defect.
Recommendation: default `battery_estimate` to `liion` when a pack is present, or
fall back to a voltage reading, and treat unset timezone as a visible warning
rather than silently showing UTC.

### D3 — LOW: pairing screen polarity is inverted relative to the resting screen

- Resting screen: black ink on white, 4,557 black pixels of 40,000 (11%).
- Phone-pairing screen: white ink on black, 37,611 black of 40,000 (94%).

Both are legible, but the flip is jarring and white-on-black uses far more ink
and is more prone to ghosting. The design card deliberately left non-resting
screens to the core UI, so this is an inconsistency rather than a regression;
record it as a design follow-up.

### D4 — MEDIUM: `notes stop` during a panel refresh reports failure and floods the log

Reproduced twice by issuing `notes stop` about 2 s after `notes record`, while the
post-boot full refresh is running:

```text
notes stop
W (8632) hg.events: event queue full; dropping event 7
@error device busy
gadget> W (8642) hg.events: event queue full; dropping event 6
… (repeats ~10/s)
notes -> … | mode recording | …
```

The stop is eventually applied (the note is stored and, after the next boot,
`pending`), but the console told the user it failed, and the event queue
overflowed for several seconds. With a settled screen the same sequence is clean:

```text
notes stop
I (17892) hg.notes: note … stored: 99.4 KB, 2 s
I (17972) hg.ui: recording -> idle
@ok stored …
```

Cause is the console's 3 s reply deadline against an app task blocked in a full
e-paper refresh; the write path has no back-pressure. Suggested fixes: raise the
console wait for app-task commands, or answer `@queued` instead of `device busy`
when the command was posted, and coalesce screen events instead of enqueuing
one per tick.

### D5 — LOW: note recorded immediately after reset says "no clock yet"

A note recorded ~6.5 s after USB reset (clock already visible as `03:51`) was
shown as `Newest note 0s, no clock yet.`. A note recorded 10 s after boot shows
`03:54, 2 s.`. Cause is presumably `recorded_at` being stamped before SNTP
completes; the note is restamped at upload, so it is cosmetic. Document it or
defer the note's timestamp display until `recorded_at` is non-zero.

## 5. What could not be verified, and why

- **iPhone pairing, passkey entry, iOS MTU, throughput, resumption, and the
  Bluefy relay page against a real phone.** No iPhone was available to this run
  and the host has no Bluetooth adapter, so no central could be driven.
- **End-to-end BLE note delivery.** Consequence of the above. The device half is
  now proven to initialise and advertise; the phone half is covered only by the
  host relay test (11/11) against packets captured from the firmware.
- **E-paper during a real transfer** (progress percentage, repaint cadence).
  Requires a phone pulling a note.
- **Pairing survival across the USB flash.** No pairing existed to preserve.
  NVS settings *did* survive two USB flashes, which supports the documented
  assumption but does not prove bond survival.
- **Wi-Fi + BLE during a simultaneous transfer.** Both radios were up together
  (Wi-Fi online, BLE advertising) with no errors; no concurrent transfer was run.

## 6. Notes on the phone-side workflow

The card asks for "end-to-end BLE note transfer to an iPhone". Neither iOS itself
nor any app without a BLE central can read the GATT service, so the phone step is
the Bluefy-hosted relay page (`web/ble-note-relay/`), which holds the link and
posts the note to `POST /notes/v1` when it is done. That is the only path this
run could test the shape of, and the card should describe it as the phone step
rather than a separate app. The server half is now a container
(`deploy/bluetooth-sync/`); the iPhone half still needs a real device.

## 7. Test data and device state left behind

- Device is running `0588422` + the one-line D1 fix (uncommitted), not the
  canonical HEAD build.
- Settings were returned to their previous values: `battery_estimate=off`,
  `timezone=""` (UTC0). They were temporarily set to `liion` /
  `CST6CDT,M3.2.0,M11.1.0` for T13.
- Two short test recordings were created and deleted (`…-0027`, `…-0028`).
- A third 2 s silent recording (`…-0029`) was created to test the settled-screen
  stop path; it uploaded over Wi-Fi and is now in the vault as
  `~/obsidian/Notes/voice notes/2026-11-20_2154_hg-0123456789abcdef_8f8f694c.md`
  (test artifact, no speech).
- Device remains at 9 notes, 0 pending, 9 sent. Card contents unchanged.

## 8. Recommendation

1. Apply the D1 one-line fix, rebuild, flash, and re-run at least T17–T20.
2. Fix or document D2 before the release notes claim "battery and time status".
3. Do not publish the release or message Jake that Bluetooth sync works until an
   iPhone has completed T21/T22. If no iPhone is available to the agent, the
   release must carry that limitation explicitly.
4. Commit this report (or a link to it) and update `docs/ble-note-sync.md` §11:
   its "Not yet verified" list should gain "the GATT table registers on real
   NimBLE", which this pass falsified for `0588422`.

## 9. Attachments

- `qa_panels_contact_sheet.png` — resting (default), resting (battery/time set), pending note, pairing screen.
- `panel_ble_fail.pbm` / `panel_ble_fail.txt` — resting screen of `0588422`, exact 1-bit dump.
- `panel_configured.pbm`, `panel_pending.pbm`, `panel_pair.pbm` — other states.
- `postflash.log` (canonical boot + console), `postfix.log` (fixed boot + console), `restore.log`, `settings.log`.
