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
| Display driver | `main/port_display_epd.{hpp,cpp}` | `EpdDisplay`: the whole panel in one RGB565 framebuffer (PSRAM), thresholded to 1 bit on flush, refreshed full or partial by a task off the app loop |
| UI state machine | `main/ui_state.{hpp,cpp}` | `UiState`: idle / recording / uploading / error, published on the core's notice line |
| Durable config | `main/device_config.{hpp,cpp}` | `DeviceConfig`: NVS-backed typed settings that survive reboots |
| Mode interfaces | `main/modes/live_mode.hpp`, `main/modes/notes_mode.hpp` | `LiveMode` (implemented: `LiveModeImpl` over the hardware-free `LiveState`) and `NotesMode`, still the stub the offline task replaces |
| Button router | `main/modes/mode_router.{hpp,cpp}` | `ModeRouter`: a PWR tap reaches `NotesMode`, and every loop pass ticks both modes |

`main.cpp` wires them, all under `#if CONFIG_HG_BOARD_WS_EPAPER_154`:

- `g_config.begin(g_storage)` right after NVS opens (app_main);
- `g_epd.begin(board.epd)` in the display block, next to the LCD branch;
- after `app.begin()`: `g_ui.bind(app)`,
  `g_modes.begin(board.latch_power.key, &g_live, &g_notes)`,
  `g_live.begin(app, g_ui)`, `g_notes.begin(g_ui, g_config)`;
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
int  note_max_s() const;                 // 300 by default, clamped 10..3600
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
  virtual void on_note_button() = 0;      // a PWR tap
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
| `main/notes/notes_uploader.*` | one pending note → one POST, sliced across loop passes |

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

The console drives it headlessly: `notes status`, `record`, `stop`, `sync`,
`list`, `mount`, `format`, `prune`, `delete`. `screen` prints the panel image
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
| `Button::Talk` | `buttons.talk = 0` (GPIO 0) | The `talk_mode` setting: hold to talk (default) or tap to talk. No cancel button is wired (`buttons.cancel = -1`), so holding BOOT for 2 s taps CANCEL instead (`main/talk_hold.hpp`, board opt-in `ButtonConfig.talk_hold_cancel_ms`). The press arms it only when it found a turn in flight or an overlay on screen: a press from the idle screen is talk for as long as it is held. |
| PWR tap (GPIO 18, < 1.5 s) | `ModeRouter` → `NotesMode` | Starts or stops a voice note. |
| PWR hold (GPIO 18, 2.5 s) | `LatchPower` | Releases the battery latch: power off on battery, screen dark on USB. |
| Touchscreen | none | This board has no touch panel. |

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
