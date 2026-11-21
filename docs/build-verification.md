# Clean-checkout build verification

Recorded 2026-11-20 to prove the README's build steps work from the pushed
repository and nothing required from a developer's machine is missing.

## Command, exactly as documented

```bash
git clone https://github.com/jakepi-bot/esp32-note-device.git
cd esp32-note-device/firmware/esp32
pip install platformio==6.2.0
pio run -e waveshare-epaper-154
```

The clone was made fresh (`git clone` into an empty directory), with no
`managed_components/`, no `sdkconfig`, and no prior `.pio/` in the tree. Python
came from a clean virtualenv; the PlatformIO package cache in `~/.platformio`
was warm from earlier work, so the download time here is not representative of
a first-ever build (PlatformIO fetches ESP-IDF 6.1 and the managed components on
a cold cache).

## Result

```
Successfully created ESP32-S3 image.
Linking .pio/build/waveshare-epaper-154/firmware.elf
Retrieving maximum program size .pio/build/waveshare-epaper-154/firmware.elf
Checking size .pio/build/waveshare-epaper-154/firmware.elf
RAM:   [===       ]  25.6% (used 83820 bytes from 327680 bytes)
Flash: [========= ]  88.9% (used 1806123 bytes from 2031616 bytes)
esptool v5.4.0
Creating ESP32-S3 image...
Merged 4 ELF sections.
Successfully created ESP32-S3 image.
check_size: app 1764 KB in the 1984 KB 'ota_0' slot, 11% free
========================= [SUCCESS] Took 98.49 seconds =========================

Environment           Status    Duration
--------------------  --------  ------------
waveshare-epaper-154  SUCCESS   00:01:38.489
========================= 1 succeeded in 00:01:38.489 =========================
```

| Fact | Value |
|---|---|
| Result | success, no warnings promoted to errors |
| Wall clock | 98 s (warm package cache) |
| App image | 1 806 123 B (1764 KB) in the 1 984 KB `ota_0` slot — **11% headroom** |
| ELF | 28 MB, `firmware.elf` |
| RAM | 83 820 B static (25.6% of 320 KB) |
| Framework | `platformio/framework-espidf @ 4.60100.0` (ESP-IDF 6.1) |
| Toolchain | `toolchain-xtensa-esp-elf @ 15.2.0+20251204` |
| Python | 3.13, PlatformIO Core 6.2.0 |

Repeated from a fresh clone after the CI and CMake changes in the same series
(commit `c8d6c2e`): same image sizes, 94.9 s, `[SUCCESS]`.

## Release packaging (the step CI runs after the build)

```
package_release: hermes-gadget-waveshare-epaper-154-0.6.0.bin (1892 KB),
                 hermes-gadget-waveshare-epaper-154-0.6.0-app.bin (1764 KB)
```

`dist/` contained the merged flash image, the OTA app image, `manifest.json`,
`SHA256SUMS`, and the license bundle — the same artifact set `.github/workflows/firmware.yml`
uploads.

## Second verification: Bluetooth on, 8 MB partitions

Recorded 2026-11-20 after the [Bluetooth note sync](ble-note-sync.md) landed, from
the same tree at commit `61281a9` (working tree clean apart from that card's own
follow-up), with `.pio/build/waveshare-epaper-154` deleted first so nothing was
reused:

```
Flash: [=====     ]  50.0% (used 2030667 bytes from 4063232 bytes)
check_size: app 1984 KB in the 3968 KB 'ota_0' slot, 50% free
======================== [SUCCESS] Took 106.64 seconds ========================
```

| Fact | Value |
|---|---|
| Result | success from an empty build directory |
| Wall clock | 107 s (warm package cache) |
| App image | 2 030 667 B (1984 KB) in the 3 968 KB `ota_0` slot — **50% free** |
| Growth over the Wi-Fi-only build | +224 544 B (~219 KB), the NimBLE stack and the note-sync service |
| RAM | 86 492 B static (26.4% of 320 KB) |
| Partition table in the image | `partitions-epaper-8mb.csv`: ota_0 `0x20000`, ota_1 `0x400000`, 3 968 KB each (read back from `partitions.bin`) |

`firmware/tests/test_epaper_ble_sync.cpp` (16 cases) and
`web/ble-note-relay/selftest.mjs` (11 cases) pass on that tree; re-running
`hg_ble_capture` reproduces the committed relay fixture byte for byte.

## Third verification: the GATT table fix

Recorded 2026-11-20 on the commit that fixes the null `access_cb` the
[hardware QA pass](qa-report-0588422.md) found (D1), with `.pio/build/waveshare-epaper-154`
and the generated `sdkconfig.waveshare-epaper-154` deleted first — the stale
`sdkconfig` from the earlier tree is what `check_config` refuses, and a clean
clone does not have one:

```
Flash: [=====     ]  50.0% (used 2030671 bytes from 4063232 bytes)
RAM:   [===       ]  26.4% (used 86492 bytes from 327680 bytes)
======================== [SUCCESS] Took 104.83 seconds ========================
```

| Fact | Value |
|---|---|
| App image | 2 030 671 B — +4 B on `0588422`: one `access_cb` initialiser |
| `firmware.bin` | 2 031 184 B, SHA-256 `63346524acc991e7f450cd91f71e97fcb7f8f47e11926ae75b86b2ddf36a38d8` |
| RAM | 86 492 B static (26.4%), unchanged |
| Upload | `Wrote 2031184 bytes … Hash of data verified` over USB Serial/JTAG |
| Host tests | `ctest` 8/8 (the new `hg_ble_gatt_table_tests` included), relay `selftest.mjs` 11/11, `site` 32/32 |

The image was rebuilt once more from the tag itself before publishing, which is why the
hashes above are the release's: ESP-IDF stamps the build time into `app_desc`, so the
merged image's SHA-256 moves between builds while its size does not. Compare a download
against the release's `SHA256SUMS` and `manifest.json`, never against a number in this file.

The size, RAM and slot figures reproduce the earlier run to within the added
pointer; the binary is not bit-reproducible because ESP-IDF stamps the build
time into `app_desc`.

## Fourth verification: the 0.7.0 release build and package

Recorded 2026-11-20 on the 0.7.0 tree (`414a3ac`), in a fresh clone with no `.pio`
directory and no generated `sdkconfig`, using the release workflow's own commands:

```
RAM:   [===       ]  26.4% (used 86492 bytes from 327680 bytes)
Flash: [=====     ]  50.0% (used 2030671 bytes from 4063232 bytes)
check_size: app 1984 KB in the 3968 KB 'ota_0' slot, 50% free
======================== [SUCCESS] Took 110.01 seconds ========================
package_release: hermes-gadget-waveshare-epaper-154-0.7.0.bin (2111 KB),
                 hermes-gadget-waveshare-epaper-154-0.7.0-app.bin (1983 KB)
```

| Fact | Value |
|---|---|
| App image | 2 030 671 B (1984 KB), 50% of the `ota_0` slot free — the same size as the third verification, because 0.7.0 changes documentation and the version string only; the SHA-256 differs because `app_desc` carries the build time |
| Merged flash image | `hermes-gadget-waveshare-epaper-154-0.7.0.bin`, 2 162 256 B — [published](https://github.com/jakepi-bot/esp32-note-device/releases/tag/v0.7.0) with SHA-256 `39cd41c8daabfdd010b9686fd260446aa4212dad1e1d0288ec3b5bf8bcf96df4` |
| OTA app image | `hermes-gadget-waveshare-epaper-154-0.7.0-app.bin`, 2 031 184 B, SHA-256 `1aa913fb35b3a4a7308cf15695660cd472ec2fbc79abb4fe62600896e7a666c0` |
| License bundle | `hermes-gadget-0.7.0-licenses.zip`, 294 140 B, SHA-256 `22ea0adaed000dba3f194a795fad430dfba2f31b0530e91f382171950c034310` (reproducible) |
| Manifest | one build, `waveshare-epaper-154`, ESP32-S3, 8 MB flash, octal PSRAM, settings `0x9000`+24576 — read back out of the image, not passed in |
| Version gates | `check_versions.py --tag v0.7.0` (all four files), `--expect-version 0.7.0` against the built app |
| Host tests | `ctest` 8/8, relay `selftest.mjs` 11/11, `site` `node --test` 32/32 |

`python -m pytest tests/` on this host gives **617 passed, 377 skipped, 20 failed**,
and those same 20 fail on the pre-release commit `d20f578` in a second worktree —
they are the simulator's timing and widget tests plus one dashboard race, not this
release. Two files (`test_sim_window.py`, `test_linux_peripherals.py`) cannot even
be collected here because the host's Python has no `tkinter`; CI runs them.

## Consequence worth knowing before adding features

The board now builds against `partitions-epaper-8mb.csv`, not `partitions.csv`,
because NimBLE plus the note sync does not fit the shared 1.9 MB slots (it needed
~219 KB where ~180 KB was free). Under the 8 MB table the app is at **50% of its
`ota_0` slot**. Changing that table needs one USB flash — an OTA update cannot
move partitions — and `nvs` keeps its offset, so settings and paired phones
survive it. Check the size line after any firmware change;
`tools/check_size.py` fails the build when a slot drops under 10% free.

## Fifth verification: the notes-only firmware from a fresh clone

Recorded 2026-11-21, on the commit that rewrote the documentation for the
notes-only device. This is the build the README documents, run exactly as the
README documents it.

```bash
git clone https://github.com/jakepi-bot/esp32-note-device.git
cd esp32-note-device/firmware/esp32
pio run -e notes
```

The clone was made into an empty directory (`git clone` from the pushed
repository), with no `.pio/`, no `sdkconfig.notes`, no `managed_components/` and
no prior build of this tree. PlatformIO 6.2.0 and the `~/.platformio` package
cache were warm from earlier work, so the 147 s below is not a cold-cache number.

```
RAM:   [==        ]  19.3% (used 63136 bytes from 327680 bytes)
Flash: [====      ]  36.0% (used 1464175 bytes from 4063232 bytes)
check_size: app 1430 KB in the 3968 KB 'ota_0' slot, 64% free
======================== [SUCCESS] Took 146.80 seconds ========================
```

| Fact | Value |
|---|---|
| Result | success, no warnings promoted to errors |
| Wall clock | 147 s (warm package cache) |
| App image | 1 464 175 B, 36.0% of the 3 968 KB `ota_0` slot, 64% free |
| Static RAM | 63 136 B (19.3% of 320 KB) |
| Against the Hermes app | -574 KiB flash (2 052 223 B), -24.2 KiB static RAM (87 948 B) |
| Framework | ESP-IDF 6.1, PlatformIO 6.2.0 |

The Hermes environment still builds from the same clone
(`pio run -e waveshare-epaper-154`, 2 052 223 B): it is kept as the regression
check until the notes app has passed the same bench run.

Host tests from the same clone:

```bash
cmake -S firmware -B build-host
cmake --build build-host -j8
ctest --test-dir build-host --output-on-failure
```

`100% tests passed, 0 tests failed out of 9`, 79 s of wall clock. `hg_notes_ui_tests`
(19 cases) covers the screen machine, the copy, list order and marks, the list
window, refresh classes, delete and playback against a real card in a temp
directory, and an ink-bounds check of all nine screens; `hg_epaper_notes_tests`
and `hg_ble_gatt_table_tests` cover the card tree, the queue and the GATT table.

What this does not prove: nothing was flashed by this run. Display, audio, the
card and the radio are verified on the board, not in this file, and the notes
image has not been tagged as a release - 0.7.1 is the last Hermes-app release.

## What CI runs

| Workflow | Trigger | Jobs |
|---|---|---|
| `firmware.yml` | push to `main`, every pull request, manual | build `waveshare-epaper-154`, package the images, run the host core tests |
| `ci.yml` (upstream) | push, pull request, manual | changed-area filter, host core on Ubuntu and Windows, Python 3.10/3.12, docs site, the `waveshare-epaper-154` build, and the two ESP32-P4 boards with `idf.py` in the ESP-IDF container |
| `hermes.yml` (upstream) | pull request, nightly, manual | the plugin's tests against a real Hermes Agent |
| `pages.yml` (upstream) | push touching `site/` or `docs/`, releases | build and publish the installer site to GitHub Pages |

Two host-side pieces are Unix-only in this fork because the notes stack does its
file I/O and sockets with POSIX APIs: the notes card-tree tests
(`hg_epaper_notes_tests`) and the notes POST dev tool (`hg_notes_request`). Both
are skipped on MSVC rather than stubbed, so the Windows core job still builds and
tests the rest of the core. Upstream's per-diff board picker was dropped, and
the board matrix is this repository's shipped boards rather than upstream's set:
`waveshare-epaper-154` on PlatformIO and the two ESP32-P4 boards on `idf.py`.
Nothing here builds the other board profiles the repository still carries.

## What this does not prove

- Not flashed from this clone: the image content is the same source that was
  flashed and validated on hardware, but this verification stops at a successful
  link and package step.
- No hardware-in-the-loop test is automated yet; display, audio and SD behaviour
  are still verified by hand on the board (see [audit.md](audit.md) §10).
