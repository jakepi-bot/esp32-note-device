# Firmware module interfaces (e-paper board)

The Waveshare ESP32-S3-ePaper-1.54 port ships its board, display driver and a
small foundation the two voice modes build on: **live mode** (record, stream,
play against Hermes) and **offline notes** (record to the SD card, sync
later). This page is the contract between them: what the foundation provides,
what each mode implements, and who owns which button, phase and byte of
state. The board's pins and first-flash checks are in
[hardware and wiring](hardware.md#waveshare-esp32-s3-epaper-154); the
integration decisions (transport, note format, upload endpoint) are in the
[integration spec](waveshare-epaper-154-v2.md).

Every file below compiles only for `CONFIG_HG_BOARD_WS_EPAPER_154`;
`main/CMakeLists.txt` adds them to that board's image alone, so no other
board grows. The exception is `main/talk_hold.hpp`: header-only button
logic shared through `port.hpp`, inert on every board that does not set
`ButtonConfig.talk_hold_cancel_ms`.

## What the foundation provides

| Module | Files | Role |
|---|---|---|
| Display driver | `main/port_display_epd.{hpp,cpp}` | `EpdDisplay`: the whole panel in one RGB565 framebuffer (PSRAM), thresholded to 1 bit on flush, pushed through a 1 KiB internal DMA staging buffer, refreshed full or partial by a task off the app loop |
| UI state machine | `main/ui_state.{hpp,cpp}` | `UiState`: idle / recording / uploading / error, published on the core's notice line |
| Durable config | `main/device_config.{hpp,cpp}` | `DeviceConfig`: NVS-backed typed settings that survive reboots |
| Mode interfaces | `main/modes/live_mode.hpp`, `main/modes/notes_mode.hpp` | `LiveMode` (implemented: `LiveModeImpl` over the hardware-free `LiveState`) and `NotesMode`, still the stub the offline task replaces |
| Status screen | `main/modes/status_screen.{hpp,cpp}`, `main/modes/notes_screen.{hpp,cpp}` | `compose_status` + `StatusScreen` (the 200x200 resting face, hardware-free) and `NotesScreen` (the hand-off that feeds it) |
| Button router | `main/modes/mode_router.{hpp,cpp}` | `ModeRouter`: PWR short → live turn (`App::talk_toggle`), BOOT tap → note toggle, BOOT hold → cancel (`HomeKeys`); while a phone asks to pair, BOOT allows and PWR refuses. Every loop pass ticks both modes. The board has no touch input, so nothing is routed from a screen. |

`main.cpp` wires them, all under `#if CONFIG_HG_BOARD_WS_EPAPER_154`:

- `g_config.begin(g_storage)` right after NVS opens (app_main);
- `g_epd.begin(board.epd)` in the display block, next to the LCD branch;
- after `app.begin()`: `g_ui.bind(app)`,
  `g_modes.begin(board.latch_power.key, &g_live, &g_notes)`,
  `g_live.begin(app, g_ui)`, `g_notes.begin(g_ui, g_config)`;
- right after `g_notes.begin(...)`: `g_status.begin(app, g_notes, hal.power,
  &g_system, g_epd)` and `app.rest_screen = ...`, the hand-off that lets the
  status screen own the panel while the device rests;
- in the loop, after `g_buttons.poll(app)`: `g_modes.tick(now)` and
  `g_ui.tick(now)`;
- the transport's open/close signals: `dispatch()` next to
  `app.on_transport_open` / `on_transport_closed`, and the loop's
  lost-connection check, call `g_live.set_connected(open, why)` with the
  close reason (`why` empty when the transport opened).

## UiState: the four phases

```cpp
enum class UiPhase : uint8_t { Idle, Recording, Uploading, Error };
void UiState::bind(hg::App& app);
void UiState::set(UiPhase phase, std::string detail = {});
void UiState::clear_error();
UiPhase phase() const;
const std::string& detail() const;
uint32_t elapsed_ms(uint32_t now_ms) const;
void tick(uint32_t now_ms);                 // call from the app loop
std::function<void(UiPhase from, UiPhase to, const std::string& detail)> on_change;
```

Rules:

- **Transitions**: any phase may fail into `Error`; `Error` leaves only for
  `Idle` (use `clear_error()`); everything else may move freely. An illegal
  transition is logged and ignored.
- **Rendering**: `set()` publishes the phase on the notice line through the
  core's port seam `App::local_notice(text, ttl_ms)`. `Recording` re-publishes
  `Recording mm:ss` every second from `tick()`; `Uploading` and `Error`
  publish once and expire on their own (4 s and 10 s); `Idle` publishes an
  empty line, so nothing of the previous phase lingers.
- **Observers**: a mode that renders its own band (pending counts, progress)
  subscribes with `on_change` instead of, or as well as, the notice line.
  Two modes must not publish the notice line at the same time: whoever owns
  the screen sets the phase.

Everything runs on the task that calls `App::tick()`; there is no locking.

## DeviceConfig: durable settings

```cpp
void DeviceConfig::begin(hg::Storage& storage);
int  note_max_s() const;                 // 86400 (24 h) by default, clamped 10..86400
bool set_note_max_s(int seconds);
uint32_t boot_count() const;             // +1 every boot, logged at start
std::optional<std::string> get(std::string_view key) const;
bool set(std::string_view key, std::string_view value);
```

The store is the core's own NVS namespace (`hgadget`), the same one `get` /
`set` on the console use, so a value written here survives reboots and shows
up in `diag`. `begin()` increments `boot_count` on every boot: if that number
stops growing in the log, NVS writes have stopped. Keys are strings limited
to 14 characters (NVS's limit); each `set` commits.

## LiveMode

```cpp
class LiveMode {
 public:
  virtual void begin(hg::App& app, UiState& ui) = 0;
  virtual void tick(uint32_t now_ms) = 0;
  virtual void set_connected(bool connected, std::string_view why) = 0;
};
```

The core already owns the voice pipeline: BOOT reaches `App::on_button`,
which records, streams over the WebSocket and plays the reply. `LiveModeImpl`
adds the device-side state around it, with the decisions kept hardware-free in
`LiveState` (`main/modes/live_state.hpp`) so
`firmware/tests/test_epaper_live_mode.cpp` runs them on the host:

- `begin()` runs once after `app.begin()` and binds the mode to the app and
  the `UiState`.
- `set_connected()` is the transport's open/closed signal: `main.cpp` calls it
  where `EventType::WsOpen` / `WsClosed` are dispatched and where the loop
  picks up a loss whose event was dropped, with the close reason.
- `tick()` runs every loop pass (~10 ms), reads `hg::App` public state
  (`screen()`, `in_turn()`) and the `UiState`'s phase, and applies what
  `LiveState` decides.

What shows on the notice line, and who owns it:

- `Listening` -> `Recording` (the counter `UiState` republishes), `Thinking`
  -> `Uploading`, everything else -> `Idle`, which clears a line this mode
  set. A phase another mode set is left alone: only a line the mode still
  sees as its own is cleared, so the line has one owner at a time (a live
  turn's states win over the notes mode's).
- A connection that drops **mid-turn** (this board's recording buffer keeps
  the turn alive) claims the line as `Error` with the close reason
  (`Hermes: disconnected`); a failed retry clears and re-publishes it instead
  of letting it expire unnoticed, and the reconnect clears it through
  `clear_error()`. A close while nothing is happening claims nothing, and a
  device that has never connected claims nothing at all: the core's
  Offline/Connecting screen and the talk hint carry the status, and the line
  stays free for the notes mode.

## NotesMode

```cpp
struct NoteCounts { uint32_t stored, pending, synced, failed; };

class NotesMode {
 public:
  virtual void begin(hg::App&, UiState&, DeviceConfig&, const NotesIo& io) = 0;
  virtual void on_note_button() = 0;      // a BOOT tap
  virtual void tick(uint32_t now_ms) = 0;
  virtual NoteCounts counts() const = 0;
  virtual std::string console(std::string_view args) = 0;  // the `notes` command
};
```

`NotesModeImpl` (`main/modes/notes_mode.cpp`) is the shipped offline mode,
wired in `main.cpp` with the board's microphone, battery and card slot
(`NotesIo`). Its work is split so the app loop never stalls:

| File | Owns |
|---|---|
| `main/modes/notes_mode.cpp` | recording state, the status line, the retry clock |
| `main/notes/notes_queue.*` | note ids, `note.json`, multipart framing, response rules |
| `main/notes/notes_store.*` | the card tree (`pending/`, `sent/`, `failed/`), its atomic commits and its deletes |
| `main/notes/notes_capture.*` | microphone → `recording.wav` |
| `main/notes/capture_writer.*` | the recording's ring and its own task: the card write never runs on the app loop |
| `main/notes/notes_uploader.*` | one pending note → one POST, sliced across loop passes |
| `main/notes/notes_ble.*` | the Bluetooth transfer protocol: commands in, packets out, no radio |

Durability: the audio is written as `recording.wav.part` and renamed first,
then `note.json` goes to `note.json.part`, is flushed, and is renamed.
`note.json` is the commit marker — a power cut before it leaves nothing that
looks like a complete note, so the queue never holds half a recording.
Deleting a note mirrors that order (`Store::erase`): the audio first, then
`note.json`, then whatever else the folder holds, then the folder itself, so a
power cut mid-delete leaves at most a folder with no metadata — never a
`note.json` pointing at audio that is already gone.

Sync: one note at a time, only when `app.online()` and the clock is set.
`202` and `200 duplicate` both mean the server has the recording, so the
folder moves to `sent/` (kept to 500, oldest pruned). A terminal 4xx moves it
to `failed/` for inspection and stops retrying. `5xx`, a timeout or no answer
keeps it in `pending/` under the contract's backoff (5 s doubling to 300 s),
and the retry is byte-identical because `note.json` persists the
`idempotency_key` (the note id): a retry after a lost acknowledgement is
answered `200 duplicate`, so a recording is stored once, never twice.

A read that stops before the size `stat()` reports is a read failure, not the
end of the file: `fread` answers 0 for both, and the sending phase used to take
it as EOF and close a body shorter than the `Content-Length` it had already
declared. The server waited for the rest, the request timed out at 20 s, and
the whole note went out again on the retry (measured on the board: 393,216 of
2,861,484 bytes in the first attempt). `notes::audio_read_short`
(`main/notes/notes_uploader.hpp`) is the rule both passes now apply, so a short
read is a `Retry` with the byte counts in the error line. The size, not
`ferror`, is the test: the card logged `sdmmc_read_blocks failed (0x101)` 60 ms
before one such stop with the error flag clear, because the failed read reaches
`fread` as a short file.

The body has its own clock. `cfg.timeout_ms` (20 s) reaches every write's
`select()` through `esp_transport`, so one quiet stretch mid-body used to abort
the POST and throw away everything already on the wire. On this board a body
that follows a recording can be quiet for 66-81 s before its first bytes leave
the chip — the hub's SYN-ACK goes unanswered while the WebSocket flow keeps
working, and when it clears the body runs at 1.3-1.6 MB/s — so the uploader
raises the clock to 120 s right after `esp_http_client_open` and keeps 20 s for
the connect, the head and the answer. A hub that is not there still fails in
20 s; a body that has really stopped still ends.

The card no longer depends on the internal heap being in one piece while that
runs. Every buffer the notes read into is in PSRAM (the 128 KB capture ring, the
64 KB transfer buffer) and this chip cannot DMA out of PSRAM, so the sdmmc host
used to take a temporary internal DMA buffer for each read and write and free it
again: during a Wi-Fi upload it could not get one 512-byte sector (`dma_free`
4,119 B, largest free block 368 B) and the read failed with `0x101`.
`sd_mount::ensure` (`main/notes/sd_mount.cpp`) now allocates one 2 KiB internal
DMA buffer at mount and hands it to the host as `dma_aligned_buffer` with
`unaligned_multi_block_rw_max_chunk_size` at 4 sectors, so both directions stage
through memory the card owns and no read or write can lose a transfer to a
fragmented heap. The cost is a fixed 2 KiB, which is the largest chunk the
driver's own allocation was getting during a transfer anyway (it halves from
8 KiB down to one sector); measured on the board, a 2.8 MB note uploads at the
same rate with it, 350 KB/s in the body and 580 KB/s hashing off the card.

A note whose audio is gone is terminal too, decided before the uploader is
asked: `notes::plan_upload` (`main/notes/notes_queue.cpp`) takes the
`recording.wav` size and answers `Upload` or `Quarantine` against the 45-byte
minimum the uploader itself refuses below (the WAV header and no sample is no
audio). `begin_sync` moves a quarantined note to `failed/` with one warning, so
a recording that never landed — an interrupted take, a card that ran out of
room — cannot be rescanned on every pass, which warned 30 times a second and
never moved it. A start the uploader refuses for another reason (no memory, an
unreadable file) waits out the same backoff a failed transfer gets.

The board has no RTC, so until its first clock sync `std::time()` reads 1970 and
a note taken then carries a `1970-01-01T…` stamp — the absence of a time, not a
time. Such a note is never uploaded that way: `notes::stamped_for_upload`
(`main/notes/notes_queue.cpp`) replaces any stamp earlier than 2024-01-01 with
the time the note is finally sent, and `begin_sync` writes that stamp back into
`note.json` before the request, so every retry resends the same strings. The note
id and the `idempotency_key` never move — the server derives the job from the key
— so this changes what goes on the wire and how the vault note is dated
([§5.5](voice-note-ingestion.md#55-filename) names the file from
`recorded_at`), never which job a recording belongs to, and a re-sent undated
note still answers `200 duplicate`. A note that already carries a plausible stamp
is sent untouched: its stored bytes have to stay put for the dedupe.

Bluetooth: when the board has it on, `NotesIo::ble` (port `NoteLink`) gives the
mode a second way out of the queue, for a device with no network at all. The mode
owns the state machine — `step_ble()` runs first in `tick()`, drains the commands
the radio queued, feeds them to `notes::BleSync` and hands its packets back to the
link — so all card reads stay on the app task beside the recording. The mode
implements `notes::BleSource` over the store (`CardBleSource`): pending notes only,
sequential reads, and `done <id>` is what moves a note to `sent/`. The protocol and
its failure rules are [Bluetooth note sync](ble-note-sync.md).

The console drives it headlessly: `notes status`, `record`, `stop`, `sync`,
`ble`, `list`, `mount`, `format`, `prune`, `delete`. `screen` prints the panel image
(1 bit per pixel, hex), which is how the display is checked without a camera.
`notes status` also counts the pending notes whose id says 1970 (`… | 2
undated`): they are the ones waiting for their first upload to give them a date.

`notes delete <note_id>` removes one note wherever it lives (`pending/`,
`failed/`, `sent/`) and answers `@ok deleted <id>`, or `@error unknown note
<id>` for an id that is not on the card — including a folder left behind by a
recording that died before its commit. `notes delete --failed` empties
`failed/` and answers `@ok deleted <n> failed`. A note that is recording or
uploading (including one sitting in its retry backoff) is refused with
`@error the note is recording` / `@error the note is uploading`, so the card
can be tidied without `notes format`, which still wipes every note and the log
mirror.

Where the notes go: `notes_url()` is `set notes_url <url>` when that is set
(the console writes it; the `notes_url` setting exists for a host behind a
different port or a proxy), otherwise the hub's own host from `server` with
the notes port — `http://<host>:8766/notes/v1`. With neither set the screen
and `notes status` say `no url` and `notes sync` answers "Hermes is not
reachable yet", leaving every note pending.

The wire half runs on any host: `firmware/tools/notes_request.cpp` builds the
same `notes::Request`, canonical string and HMAC the firmware sends
(`hg_notes_request`, built by the host CMake), so `plugin/notes_ingest.py` and
`plugin/notes_pipeline.py` can be exercised without the device joining Wi-Fi.

## Status screen: the resting face

`main/modes/status_screen.{hpp,cpp}` draws the 200x200 status display from the
approved design (`SPEC.md`, the design card's reference renderer); its drawing
code is that renderer, so the mockups' frames are the rendered frames. The host
tests pin all ten states by frame hash
(`firmware/tests/test_epaper_status.cpp`).

Two halves:

- **`compose_status(StatusInputs)`** turns the device's facts into three
  strings: the hero (what is happening now: `RECORDING`, `OFFLINE`,
  `LOW POWER`, `3 NOTES`), the note block (the mode's own words, the newest
  note's time and length, or the reason it cannot record), and the footer (the
  battery percentage plus one fact: `2 QUEUED`, `SYNCED`, `ON POWER`,
  `NO CARD`). It is plain data in, strings out, so a host test can walk every
  combination. The hero never says `READY`: a word that carries no information
  is a wasted panel.
- **`StatusScreen`** paints a view and remembers its signature. A view identical
  to the last one costs nothing: no canvas run, no flush, no refresh. That is
  what keeps an idle e-paper panel from flickering at the app's frame rate.

Column budgets are layout contract, not taste: the hero is 9 columns at scale
3, the note 13 at scale 2, the footer 14 at scale 2 (`layout` in
`status_screen.hpp`). A string past its budget is truncated with ASCII `..` --
the 5x7 font has no U+2026 and would print `?`.

**Ownership is a hand-off.** `App::rest_screen` (core) lets a board module own
the whole panel while the device rests: `update_model()` calls it with the model
it would draw and whether the core UI drew the frame before. Returning true
means "the panel is mine this frame" and the core UI draws nothing. So:

| Core screen | Owner |
|---|---|
| `Ready`, `Offline`, `LowBattery`, `Charging`, `Charged` | the status screen |
| boot, connecting, pairing, listening, thinking, responding, clock face, settings, prompts, choice, errors, OTA, camera, dashboard, widgets | the core UI |

`main/modes/notes_screen.cpp` is the decided half: it reads the clock from
`System::wall_time`, the links from `App` (`online()`, `network_up()`,
`phones_paired()`, `phone_connected()`), the cell from the core's own power read
(`App::displayed_battery_percent()`, `App::external_power()`: the gauge or the
voltage estimate, whichever the board's rules allow), and the notes facts from
`NotesMode::status()` (cached; no card access). Each hand-over forces a full
panel refresh (`EpdDisplay::request_full_refresh()`): a partial update between
the core's white-on-black screen and this one leaves the old image ghosted.

Edge states are copy, not special cases: no Wi-Fi is `OFFLINE` with the pending
count, a missing card is `NO CARD`, an unknown cell prints `NO BATTERY` (or
nothing, when a more useful fact takes the line), an unsynced clock prints
`--:--` and `NO TIME`, and a build without a Bluetooth stack draws the same cut
icon as an unpaired one -- the words never claim a link that cannot exist.
There is no charge-status GPIO on this board, so the bolt means external power,
never "charging".

Two device settings decide what the screen can say, and they are the user's:

- **The battery level.** The footer prints the core's own percentage
  (`App::displayed_battery_percent()`): the gauge's reading, or the voltage
  estimate when `battery_estimate` is `liion`. This board has no gauge, so
  until that setting is on the footer says `NO BATTERY` while the masthead still
  shows the level bar's absence and the bolt. The design's mockups show the
  unknown case; the fix is one console command, not different firmware.
- **The clock.** It prints `System::wall_time`, which is UTC until `timezone`
  is set (`set timezone "CST6CDT,M3.2.0,M11.1.0"` for US Central). The clock
  face and the note times follow the same setting, so the panel is never out of
  step with the rest of the device.

No new dependencies: the screen compiles against `hg::Canvas`, `hg::font` and
the board's existing `EpdDisplay`.

To check the panel without a camera: `python firmware/tools/panel_dump.py
--port /dev/ttyACM0 --out panel` walks the `screen <first> <count>` console
command (one write carries about 4,000 characters, the image is 10,000) and
writes `panel.txt` (ASCII) and `panel.pbm` (1:1 image).

## `diag`, and clearing an old crash

`diag` prints the report (parts, battery, phase, stacks, recordings, Wi-Fi) and
writes nothing; `diag log` prints the log copy. The one console command that
writes is `diag crash clear`: it erases the core dump in the `coredump`
partition (`esp_core_dump_image_erase()`), answers `@ok crash cleared`, and the
next `diag` reports `"crash":{"state":"none"}`.

Why it is needed: a dump outlives flashes, so `crash` can report
`"state":"saved"` with `"this_build":false` — a crash from firmware that is no
longer on the device. The report says so honestly, but its `pc` and `backtrace`
only decode against the ELF of the build that crashed (`crash.elf_sha256`), and
until it is cleared every later report carries it. The port answers
`@error could not clear the crash` when the partition is missing (a board
updated over the air from a release whose table had none) or the erase failed.

Which build is running is `build.elf_sha256`; `build.date` and `build.time` are
the compiler's stamp, for a human asking "how old is this image?". The stamp
lives in one object file (`esp_app_desc.c`, whose `__DATE__`/`__TIME__` freeze
when it is first compiled), so `tools/pio_app_desc.py` removes that object
before every PlatformIO build; an idf.py build in a reused build directory keeps
the first stamp. Never identify an image by the date and time.

## Buttons: who owns what

| Input | Owner | Behavior |
|---|---|---|
| `buttons` = `{-1,-1,-1,-1}`, `buttons.record = 0` | `ModeRouter` (`main/modes/mode_router.cpp`) | The core's `Button::Talk` is not wired on this board: the router owns both keys through `HomeKeys` (`main/modes/home_keys.cpp`, host cases in `firmware/tests/test_epaper_keys.cpp`). |
| BOOT tap (GPIO 0, < 1.5 s) | `ModeRouter` → `NotesMode` | Starts or stops an offline voice note (`NotesMode::on_note_button`). Never talks. BOOT is also the flash-download and deep-sleep wake pin. |
| BOOT held ≥ 2 s | `ModeRouter` → `App::on_button(Cancel)` | Fires once at the threshold, while the key is still down: it drops the note being recorded, or otherwise taps CANCEL for a live turn, an overlay, a pairing window or a question. |
| PWR short press (GPIO 18, < 1.5 s) | `ModeRouter` → `App::talk_toggle` | Starts a live Hermes turn; the next short press ends it. While a phone asks to pair, PWR refuses instead. |
| PWR hold (GPIO 18) | `LatchPower` in hardware; in the `notes` image `power_hold` (`notes_app/main.cpp`) → `NotesUi`, `Runtime` | Releases the battery latch on a continuous **2.5 s** press: power off on battery, screen dark on USB. This image's foundation reads no hold of its own — the hardware cut is the whole gesture — while the `notes` image takes the hold at 1.5 s and draws the sleeping face before the shutdown (below). |
| Touchscreen | none | This board has no touch panel and no touch input path: no tap, no swipe, no gesture. |

**A PWR hold, in the `notes` image.** An e-paper keeps its last frame with no power,
so the `notes` image (which replaced this foundation with `notes_app/`) draws a
sleeping face before it goes: `NotesUi::sleep_face()` puts paper, a crescent and
`asleep - hold PWR 2 s` on the panel as `ScreenApp::sleep_face()`'s **full** refresh
and waits out the waveform (~2.6 s bound on a 2.1 s waveform), and only then runs
`Runtime::shutdown()` — which flushes the card, closes the radio and drops the
latch. Both of its power paths do this: the `Effect::PowerOff` the screens answer a
Home hold with, and `power_hold()`'s own path for a hold on any other screen, where
the screens decline and the loop still draws the face. The latch's hardware cut
still follows a continuous 2.5 s press, so a finger kept down past it cuts the rail
mid-waveform and tears the face; the console's `poweroff` and a dead battery power
down with no frame. What the face is, and why it is not a `Screen`, is
[the UI specification, §12.2](notes-ui-spec.md).

The two keys are the whole input surface, and the settings hint names them in the
case's own words: `talk_label = "PWR"` and `cancel_label = "BOOT"` (the core's
Talk control changes a setting and its Cancel control steps to the next one), so
the face reads `PWR: change | BOOT: next`. A pairing ask runs the other way round,
and the board names its keys for that too (`pair_allow_label` / `pair_refuse_label`
in `board.cpp`): **BOOT allows, PWR refuses, no answer within 20 s refuses**
(`App::kPhoneAskMs`).

The panel returns to its resting face after 60 s without a key press
(`modes/auto_home.cpp`, `kAutoHomeSeconds`); an error face stays until a key press,
because the error is the one message that needs a person (`docs/features.md`).

The PWR key is also the latch's power key (`latch_power.key = 18`), so the
tap must survive the latch's own press detection; whether the tap arrives
cleanly next to the power circuit is a physical-check item for the
hardware-validation task (see the integration spec's open-verify list).

## Screen rules for e-paper

The notice line is the only rendering the foundation performs. A mode that
draws its own band must follow the e-paper rules in the
[integration spec, section 6.2](waveshare-epaper-154-v2.md): no animation
(the board sets `DeviceProfile::animation_default = false`, so the `animation`
setting starts off here and motion only costs refreshes if you turn it back
on), no 10 fps expectations, clear before darkening, and the
driver's refresh policy (partial by default, a full refresh every 20
partial ones, on boot and after a failed transfer) does the rest.

The panel image and its snapshot live in PSRAM (18.9 KB of an 8 MB part that
the internal heap cannot spare), and this chip's SPI cannot DMA out of PSRAM:
the driver allocates a private internal buffer the size of the write when it
must, which for the 5,000-byte frame is 5,000 bytes of internal RAM on every
update. During a Wi-Fi transfer the largest free internal block falls to a few
KiB, that request failed with `ESP_ERR_NO_MEM`, and the panel silently kept the
frame it had. `EpdDisplay::write` now copies 1 KiB at a time through one fixed
internal DMA buffer, so no panel update asks internal RAM for anything, and a
write that still fails sets the redraw + full-refresh request instead of
leaving a half-updated screen.

## Out of scope in the foundation

- **Bluetooth**: not enabled on this board (integration spec decision D2).
  Wi-Fi is the only transport; `board.bluetooth` stays false and the phone
  companion never starts. The task body's "BLE provisioning" is answered by
  the existing Wi-Fi setup portal (`wifi-setup`).
- **Offline behavior**: implemented — see `NotesMode` above; live mode too.
- **Doze screen-off**: the e-paper rail stays on in the foundation. Clearing
  the panel and dropping GPIO 6 before a doze (spec section 6.2) is follow-up
  work once the modes exist.

## Replacing a stub

1. Implement the interface in the mode task's own files (or turn the stub's
   file into the implementation; keep the header's signatures).
2. Swap the type in `main.cpp`'s globals (`hgp::StubNotesMode g_notes` → the
   real class; both modes have gone this way, `hgp::LiveModeImpl g_live` and
   `hgp::NotesModeImpl g_notes`) and, if `begin()`'s needs grow, that one
   call.
3. Keep every `#if CONFIG_HG_BOARD_WS_EPAPER_154` in place: CI selects the
   boards a pull request builds from those conditions and from
   `platformio.ini`.
