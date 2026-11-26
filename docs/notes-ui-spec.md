# Notes-only device: screen, key and e-paper specification

**Target:** Waveshare ESP32-S3-ePaper-1.54 (v2) — 200 × 200, 1 bit per pixel
**Status:** implementable as written. Every wireframe below is a real render of the
layout compiled against the SDK's own `hg::Canvas` and `hg::font` (§17), and every
string in §14 was width-checked against the slot that holds it.
**Keys:** as landed in `firmware/esp32/main/notes_app/ui_keys.{hpp,cpp}`. This
document adds no bindings.

---

## 0. What this covers, and the one thing it changes

This is the whole visual surface of the notes-only firmware: nine screens, one
font, one frame, one string table. It is written against the tree as it stands:

| Source of truth | What it fixes |
|---|---|
| `notes_app/ui_keys.{hpp,cpp}` | every gesture, the menu order, the screen enum |
| `port_display_epd.{hpp,cpp}` | refresh policy, waveform choice, its six constants |
| `hg/canvas.hpp`, `hg/font.hpp` | the only drawing primitives and the type metrics |
| `notes/notes_store.hpp`, `notes/notes_queue.hpp` | what a note *is* on the card |

### 0.1 The brief said five screens. The landed key map requires nine.

The brief names a status face, a menu, a notes list, a play screen and a
delete-confirm. Those are five of the nine that `ui_keys.hpp` already encodes:

| Screen | In the brief | Why it has to exist |
|---|---|---|
| `Home` | ✓ status face | the resting face: queue, links, power, mode |
| `Menu` | ✓ | the nine rows of `MenuItem` |
| `NoteList` | ✓ | `MenuItem::Notes` destination |
| `Playing` | ✓ play | transport while audio plays |
| `ConfirmDelete` | ✓ | `Action::AskDelete` destination |
| `Recording` | — | `Home`'s BOOT tap returns `StartRecording`; something must show it running |
| `NoteAction` | — | `NoteList`'s PWR returns `OpenNote`, not `Play`. Play/Delete/Back live here |
| `Pairing` | — | `Screen::Pairing` owns both keys while a phone asks |
| `About` | — | `MenuItem::About` exists; see §11.1 for the one-line gap this exposes |

All nine are specified here. §4–§8 are the five the brief names, in its order.

**The one change to landed code** is §11.1: `Screen` has no `About` value, so
`MenuItem::About` currently has no destination and no way back. The fix is three
lines of `ui_keys.cpp` and reuses the existing "hold = back" invariant.

---

## 1. Facts that dictate the design

| Fact | Value | Consequence for this spec |
|---|---|---|
| Panel | 200 × 200, 1.54″ diagonal | **183 dpi. 1 px = 0.138 mm.** |
| Controller | SSD1681-class, 1 bit | no grey, no anti-aliasing, no opacity |
| Driver threshold | `convert()`: `luma ≥ 128 → white dot` | exactly two survivable colours; see §2.1 |
| Font | 5 × 7 bitmap, **integer scale only** | type sizes are quantised: 7 / 14 / 21 / 28 px |
| Refresh | full waveform `0xC7`, partial waveform `0xCF` | see §15 |
| Partial refresh | the whole frame in one window, partial waveform, ~0.6 s | `flush(y0,y1)` narrows *conversion*; the frame's diff only decides *whether* an update is sent (§15.1) |
| BUSY wait | `kBusyTimeoutMs = 3000` | a full update is budgeted at "around a second", see §15.1 |
| Coalescing | `kQuietMs 80`, `kMaxCoalesceMs 250` | a frame's bands merge; do not reimplement |
| Rate limit | `kMinGapMs 300` | ~3 updates/s ceiling, enforced by the driver |
| Ghost reset | `kFullEvery = 10` | 10 partials then an automatic full; hand-overs keep a tighter budget of their own (§15.2) |
| Charge status | `LatchingPower.charging = -1` | **there is no charge-status pin.** A bolt can only mean *on external power* |
| Battery | ADC on GPIO 4, 2:1 divider | `std::optional<uint8_t> battery_percent` |

### 1.1 Two defects in the current build this spec removes

The generic Hermes UI runs on this panel today and gets both of these wrong. The
notes app must not inherit either:

1. **Type at 0.97 mm.** The core UI picks `min(4, min(w/160, h/120))`, which on a
   200 px panel is **scale 1** — a 7 px, 0.97 mm glyph. That is below the
   legibility floor of a handheld held at 25–30 cm. This spec never uses scale 1
   for anything a user must read (§2.2).
2. **Inverted polarity.** The core UI draws a dark background (`kBg = rgb565(10,14,20)`)
   with light text, and `convert()` maps *light* to a white dot — so the panel
   currently prints **white text on a black field**. This spec draws black ink on
   paper and needs no change to `convert()` (§2.1).

---

## 2. Type, ink and grid

### 2.1 The only two colours

```c
constexpr uint16_t kPaper = hg::rgb565(255, 255, 255);  // luma 255 → white dot
constexpr uint16_t kInk   = hg::rgb565(0, 0, 0);        // luma   0 → black dot
```

Both sit far from the 128 threshold, so the result is deterministic. Anything
between them lands unpredictably: **no greys, no tints, no dimmed text.** Where
the core UI would use `kDim`, this spec uses the same `kInk` at a smaller scale
or a shorter string — hierarchy is size and space, never value.

Do not draw through the core UI's palette. White-on-black is not a style choice
here: 1-px strokes bloom and merge when inverted, and every partial-refresh
artefact is more conspicuous on a black field.

### 2.2 Metrics, measured from `hg::font`

| Property | scale 1 | **scale 2** | **scale 3** | scale 4 |
|---|---|---|---|---|
| Glyph bitmap | 5 × 7 | **10 × 14** | **15 × 21** | 20 × 28 |
| Advance per column | 6 px | **12 px** | **18 px** | 24 px |
| Line pitch (`Canvas::line_height`) | 9 px | **18 px** | **27 px** | 36 px |
| Cap height on the glass | 0.97 mm | **1.94 mm** | **2.9 mm** | 3.87 mm |
| Columns in the 176 px content width | 29 | **14** | **9** | 7 |

`Canvas::text_width(s, scale) == columns(s) * 6 * scale - scale`.

**The rule: scale 2 is the body size, scale 3 the only display size, scale 1 is
the exception and scale 4 is not used.** Scale 1 appears only where a row set has
to close under a two-line footer — the nine-row menu (§5.1) and the unpair table
(§11.4) — and on the fourteen rows §14 measures at scale 1 (the menu's, Home's
menu hint, the unpair empty hint, the sleep face's line). Two sizes on a 200 px
screen is already a lot; a third would read as accident. Scale 2 is 1.94 mm, which at 25–30 cm viewing distance is
comfortably above the ~1.2 mm floor where 5 × 7 bitmap text stops resolving.

Fourteen columns is the hard content budget. It is the number that shapes every
string in §14 and every row in §3, and it is why the footer is one label rather
than two (§3.3).

### 2.3 The three levers

There is no colour, no weight, no opacity and no shadow. Hierarchy comes from:

1. **Size** — 14 px or 21 px, nothing else.
2. **Space** — how much paper surrounds a thing.
3. **Ink mass** — a solid bar reads heavier than any arrangement of strokes.

Everything below is built from those three. A selected row is a solid bar because
that is the largest ink mass the panel can produce in one row (lever 3); a hero is
21 px because that is the only size above body (lever 1); the action bar is
separated by 15 px of paper rather than a rule where possible (lever 2).

---

## 3. The frame every screen shares

### 3.1 The four zones

Every screen is the same 200 × 200 page with the same four horizontal zones. A
screen may leave a zone empty; nothing may move between zones or straddle a rule.

```
y=0    ┌────────────────────────────────────────────────────────┐
       │ ZONE A · FACTS        queue / links / power            │
y=22   │                                                        │
y=27   ├────────────────────────────────────────────────────────┤  1 px rule, x 12..188
       │                                                        │
       │ ZONE B · CONTENT      hero, rows, note, progress       │
       │                                                        │
y=144  ├────────────────────────────────────────────────────────┤  2-line footer only
y=162  ├────────────────────────────────────────────────────────┤  1-line footer
       │ ZONE C · KEYS         the one binding you cannot guess │
y=186  └────────────────────────────────────────────────────────┘
```

| Zone | x | first row | content budget | carries |
|---|---|---|---|---|
| A · facts | 12 … 188 | glyph top **y = 8** | band height 15 px, ink to y = 22 | queue state, the clock's unset warning, the Wi-Fi glyph, battery |
| rule | 12 … 188 | **y = 27** | 1 px | the only rule on a one-rule screen |
| B · content | 12 … 188 | **y = 32** | to y = 138 (2-line footer) or y = 156 (1-line) | everything the screen is *about* |
| rule | 12 … 188 | **y = 144** or **y = 162** | 1 px | closes the page |
| C · keys | 12 … 188 | **y = 152 + 170**, or **y = 172** | 1–2 lines, ink to y ≤ 186 | the key legend |

Nothing may be inked outside **x 4 … 196** or below **y 186**. The 4 px of
bleed beyond the 12 px margin is reserved for the selection bar, which is the one
element allowed to break the margin (§3.4).

### 3.2 Zone A is facts, and facts do not tick

The band carries four things, all of them event-driven:

| Slot | x | Size | Content |
|---|---|---|---|
| queue | 12 | text at scale 1, ≤ 21 cols | `3 TO SEND` / `ALL SENT` / `NO CARD` / blank |
| time warning | right of queue | text at scale 1, 7 cols | `NO TIME` while the clock has never been set; blank once it has |
| Wi-Fi | 136 | 16 × 15 bitmap | up (uncut) or down (2 px cut — the rune with the cut); blank when the Wi-Fi is off |
| battery | 160 | 28 × 15 procedural | fill, bolt, or `?` |

**There is no clock on the resting face; only its unset warning remains.** A
minute-accurate clock costs one panel update a minute, and every partial re-drives
the frame (§15.2): measured on the board, Home at rest moved `frames` by one every
92 s, and each frame is a CPU wake plus ~589 ms of panel driving. The time is
therefore not drawn anywhere on Home. What the band keeps is the one fact that
cannot be seen elsewhere: `NO TIME`, in the time's old slot, while the clock has
never been set (a note's time cannot be trusted). It is 7 columns and fits beside
the queue's 10; once the clock is set the slot is blank, so the band is exactly as
wide as it was without a clock. The set/unset answer is an event, not a tick:
boot, or the first SNTP sync, is what changes it.

The wall clock itself is untouched. Notes and uploads still stamp
`std::time(nullptr)`, and the `clock` console command still reads and sets it;
those consumers never needed the band to draw the minute.

**The 16 × 15 box is the Wi-Fi glyph's, and nothing else's.** The band reports the
station, not the Bluetooth radio: a phone's window is the Phone screen's own page
(§11.5), which draws no band at all, so there is no rune to keep in this slot and
no state in which the box changes owner. When the Wi-Fi is off the box is paper,
not a placeholder mark.

This is the single most useful rule in the document: **nothing redraws unless
something changed.** The only timers in the whole UI are the recording counter
and the playback position, and they run only while the user is holding a session
open.

### 3.3 The footer is one label

At scale 2 the content width holds **14 columns**. Two side-by-side key labels do
not fit: `BOOT next` (9) + `PWR open` (8) + a gap is 18 columns, 214 px. So the
footer carries **one binding**, and it carries the binding that would leave the
user stuck if they guessed wrong:

| Screen | Footer | Why this one |
|---|---|---|
| Home | `BOOT: record` | without it the device has no discoverable verb |
| Recording | `BOOT: stop` / `HOLD: discard` | two lines: the hold is destructive and invisible |
| Menu | `BOOT: next` / `PWR: select` | two lines: the row select is the one verb the glass cannot show |
| NoteList | `BOOT: next` / `PWR: open` | same reason |
| NoteAction | `BOOT: next` / `PWR: select` | same reason |
| ConfirmDelete | *(none)* | the body *is* the legend (`HOLD: delete` / `TAP: keep`) |
| Playing | `BOOT: stop` | the stop that stays on the note; PWR stops too but leaves the player |
| Pairing | `HOLD: nothing` | the question is answered on the keys; a hold is not consent |
| About | `HOLD: back` | the only way out |
| Phone | `PWR: close` / `HOLD: back` | two lines: the tap is the discoverable way out and the hold is the same exit — the window ends with the page, so both bindings must say so |

`HOLD:` always means **BOOT** held. **PWR is never a power gesture on a tap**:
a tap opens, selects, refuses, stops or leaves, and the only power-off is a PWR
*hold from Home*. The footer never has to warn that a key the user is about to
press will switch the device off.

**The footer yields to a warning.** One slot, precedence: an active warning
(`NO CARD`, `LOW POWER`, upload failure) beats the key legend. A warning that the
user cannot see because a legend is in the way is worse than an undiscovered key.

### 3.4 Rows and the selection bar

Six rows fit in zone B at the 2-line footer, seven at the 1-line footer.

| Property | Value |
|---|---|
| row pitch | **18 px** (one scale-2 line box) |
| first row glyph top | **y = 32** |
| row *n* glyph top | `32 + 18n` |
| label x | 12 |
| value | right-aligned, right edge **x = 188** |
| selection bar | `x = 8, y = row − 2, w = 184, h = 18` |
| text on a selected row | `kPaper` on the bar; `kInk` otherwise |

The bar gives the glyph 2 px of padding above and below and bleeds 4 px past each
margin, so a selected row reads as a **band across the page** rather than a box
around some text. That bleed is the only place the 12 px margin is broken, and it
is what makes the selection unmistakable at arm's length.

The bar is the largest ink mass in the UI and therefore the highest ghosting
risk — §15.3 bounds it.

---

## 4. Home — the status face

**Keys** (`ui_keys.cpp`): BOOT tap `StartRecording` · BOOT hold `OpenMenu` · PWR tap `None` · PWR hold `PowerOff` (Home only).
**Job:** answer, in one glance and without a key press — *will it record, did my
last note go, is it charged, is it connected.*
**This is the only centred screen.** Every other screen is left-aligned, so the
alignment itself is the mode signal: centred means "nothing to navigate".

### 4.1 Layout

```
y=0    ┌──────────────────────────────────────────────────────┐
       │ 3 TO SEND                       ((•))   ▰▰▰▱        │  A · facts      y=8
y=27   ├──────────────────────────────────────────────────────┤
       │                                                      │
       │                     READY                            │  hero  scale 3  (87×21)
       │                                                      │
       │                   LAST 14:02                         │  line  scale 2  (118×14)
       │                                                      │
y=162  ├──────────────────────────────────────────────────────┤
       │ BOOT: record                                         │  C · keys      y=172
y=194  └──────────────────────────────────────────────────────┘
```

| Element | x | y (glyph top) | size | scale | note |
|---|---|---|---|---|---|
| queue | 12 | 8 | ≤ 118 × 14 | 2 | `3 TO SEND` / `ALL SENT` / `NO CARD` / blank |
| Wi-Fi | 136 | 8 | 16 × 15 | — | bitmap, §4.4 |
| battery | 160 | 8 | 28 × 15 | — | procedural, §4.5 |
| rule | 12 | 27 | 176 × 1 | — | |
| hero | centred | *t* | ≤ 160 × 21 | 3 | one word, 5–9 columns |
| line | centred | *t* + 33 | ≤ 168 × 14 | 2 | the one fact under the hero |
| rule | 12 | 162 | 176 × 1 | — | |
| footer | 12 | 172 | ≤ 166 × 14 | 2 | |

**The centred group is centred between the rules, not on the panel.**

```c
const int LINE  = 18;                        // one scale-2 line box
const int group = 21 + 12 + 14;              // hero + gap + line  = 47
const int top   = 28 + ((162 - 28) - group) / 2;   // = 71
```

For `READY` that puts the hero's glyph top at **y = 71**, the line at **y = 104**,
and leaves 43 px of paper above the hero against 45 px below the line. Near-equal,
which is what stops a sparse face from looking like a layout bug: the two rules
frame it, and the space inside the frame is even.

**Exactly one line sits under the hero, never two.** An earlier draft had a `sub`
slot *and* a `last` slot, and the first render put `3 TO SEND` in the band and
`3 TO SEND` again as the sub. One slot, one precedence order (§4.2), no way to
repeat a fact.

Horizontal centring is `(200 − Canvas::text_width(s, scale)) / 2`, floored.

### 4.2 The state table

Hero, band and sub are three independent slots with their own precedence. Build
them in this order and the face is always true.

**Hero — the highest-priority true statement about the device:**

| Priority | Hero | Condition |
|---|---|---|
| 1 | `NO CARD` | the card is not mounted (`Store::ensure()` failed) |
| 2 | `LOW POWER` | `battery_percent ≤ 10` and not on external power |
| 3 | `UPLOADING` | an upload is in flight |
| 4 | `NO SYNC` | the last upload batch failed and retrying will not help |
| 5 | `OFFLINE` | there is at least one note to send and Wi-Fi is down |
| 6 | `READY` | no id. **`READY` is the fallback, never an empty face.** |

**Band queue count and time warning:**

| Condition | Text |
|---|---|
| card missing | *blank* (the hero already says it) |
| `pending > 0` | `%u TO SEND`, capped at 99 |
| `pending == 0` | `ALL SENT` |
| clock never set | `NO TIME` in the time's old slot, right of the queue (7 columns) |

The clock warning is independent of the queue slot: it is drawn whether the queue
says `3 TO SEND`, `ALL SENT` or nothing, and it disappears the moment the clock is
set. It is not a hero word (an earlier design put `NO TIME` at hero scale; Jake's
2026-11-26 compromise moved the same signal into the band so the hero can keep
answering what the device is doing).

**The line under the hero — context, most urgent first. One line only:**

| Priority | Line | Condition |
|---|---|---|
| 1 | `PLUG IN SOON` | low power |
| 2 | `NOTHING LOST` | no card — the reassurance, because the hero only reports the fault |
| 3 | `%u OF %u SENT` | uploading, `i` of `n` done |
| 4 | `NO AUDIO SAVED` | a stop just discarded a capture for holding no audio; it lapses after a few seconds and the line below returns |
| 5 | `LAST %02u:%02u` | there is at least one note; its newest local time |
| 6 | `BOOT TO RECORD` | no notes at all — the only state that needs a verb |
| — | *blank* | otherwise |

**The three slots never restate each other.** The band answers *what is queued*,
the hero answers *what mode am I in*, this line answers *what happened last or
what to do now*. That is why `UPLOADING` pairs with `2 OF 3 SENT` and not with the
count, and why `OFFLINE` pairs with `LAST 14:02` and not with `3 TO SEND` — the
band already said it.

`NO AUDIO SAVED` is the one line that does not describe a state but an event: a
stop that found nothing to keep throws the capture away, the card does not move,
so with the hero still `READY` and the band unchanged this line is the only thing
on the glass that can say *nothing was saved*. It owns the slot for five seconds
and then the line it replaced comes back (`LAST`, or `BOOT TO RECORD` on an empty
card). It never displaces a line with a higher priority above, never touches the
hero or the band, and a capture started or a note committed while it is up drops
it early — a saved note makes it a lie.

Every combination above is rendered in `01`–`07` of the wireframe set.

### 4.3 What changes on partial vs full

| Change | Refresh | Note |
|---|---|---|
| entering Home from any other screen | PARTIAL | a hand-over, bounded by §15.2 |
| first frame after a wake that dropped `EPD_PWR` | **FULL** | the panel's RAM was reset |
| hero word changes (`READY` → `UPLOADING`) | **FULL** | three slots change at once (§15.2) |
| band queue count / `ALL SENT` | PARTIAL | ≤ 10 columns, same slot |
| the line under the hero | PARTIAL | |
| battery fill | PARTIAL | fill is **growth-only in one direction per charge cycle**; see §15.3 |
| Wi-Fi icon up ↔ down | PARTIAL | 16 × 15 region |
| upload progress bar | PARTIAL | **growth-only, so it leaves no residue at all** |
| clock set/unset answer (`NO TIME`) | PARTIAL | the warning appearing or clearing, Home only; a minute rollover is not a change |

The one transition on this screen that must be full is `UPLOADING → READY`,
because it changes three slots at once and a partial leaves the hero's word
ghosted under the new one. Exact rule: **when the hero string changes, the frame
is full.** It is one line of code and it removes the ugliest artefact this screen
can produce.

### 4.4 Wi-Fi — 16 × 15

Two concentric arcs at r ≈ 8.6 and 5.4 px, ±46° from vertical, plus a 3 px source
dot. Three arcs were tried and rejected: at 15 px the innermost degrades to a
3 px stub that reads as dirt between the middle arc and the dot.

```c
// connected
static const uint8_t kWifiUp[] = {
    0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x03,0xc0, 0x0e,0x70,
    0x30,0x0c, 0x20,0x04, 0x07,0xe0, 0x08,0x10, 0x00,0x00, 0x00,0x00,
    0x01,0x80, 0x01,0x80, 0x01,0x80,
};
// not connected: the same glyph, 2 px cut on a cleared halo
static const uint8_t kWifiCut[] = {
    0x80,0x00, 0xc0,0x00, 0xe0,0x00, 0x78,0x00, 0x3d,0xc0, 0x1e,0x70,
    0x2f,0x0c, 0x23,0x84, 0x05,0xc0, 0x08,0xf0, 0x00,0x78, 0x00,0x3c,
    0x01,0x9e, 0x01,0x87, 0x01,0x83,
};
```

Draw the down state as **cut, not omitted.** An empty slot is indistinguishable
from a rendering fault; a struck-through icon plus the `OFFLINE` hero says the
same thing twice, which is what a 16 px mark needs.

### 4.5 Battery — 28 × 15, procedural

The fill must track an arbitrary percentage, so it is drawn, not blitted: a 26 × 15
1-px shell, a 2 × 5 nub at x + 26, y + 5, then:

| State | Drawing |
|---|---|
| level known | fill `x+3 … x+3+21·pct/100`, 11 px tall at y + 2 |
| on external power | the bolt `(13,2)→(7,8)→(12,8)→(9,13)` in local coords |
| unknown (`pct < 0`) | a `?` glyph inside an empty shell |

A blank box reads as **0 %**, so an unknown level must never draw one. 21 px of
travel is ~5 % resolution, which is right for an icon; the exact number lives in
`About` (§11).

**The bolt means ON EXTERNAL POWER, never "charging".** `LatchingPower` has
`charging = -1`: there is no charge-status pin on this board, and a full device on
USB is indistinguishable from an empty one. Never label it `CHARGING`.

---

## 5. Menu

**Keys:** PWR tap `MenuSelect` · BOOT tap `MenuNext` (wraps) · BOOT hold `Back` to Home.
**Job:** nine destinations, and a summary of the device's state in the right column
so the menu answers questions as well as asking them.

### 5.1 Layout

Nine rows since 2026-11-22 (wipe notes, format card, unpair phones). They do not
fit at the list's 18 px pitch — at nine rows the last one would start at 176, under
the footer's rule at 144 — so the menu draws its own rows: **scale 1 labels at a
12 px pitch**, with an 11 px selection bar. The footer keeps both bindings, because
the menu is how a person moves; the label is the one thing that gave way, and 5 × 7
pixels is still a word at reading distance on this panel.

```
y=0    ┌──────────────────────────────────────────────────────┐
       │ MENU                                                 │  y=8
y=27   ├──────────────────────────────────────────────────────┤
       │ ████ Notes                                      12   │  y=32   selected
       │      Sync now                                    3   │  y=44
       │      Phone                                           │  y=56
       │      Wi-Fi                                     on    │  y=68
       │      About                                           │  y=80
       │      Wipe notes                                      │  y=92
       │      Format card                                     │  y=104
       │      Unpair phones                                   │  y=116
       │      Back                                            │  y=128
y=144  ├──────────────────────────────────────────────────────┤
       │ BOOT: next                                           │  y=152
       │ PWR: select                                          │  y=170
y=194  └──────────────────────────────────────────────────────┘
```

| Element | x | y | size | note |
|---|---|---|---|---|
| title `MENU` | 12 | 8 | 58 × 14 | scale 2 |
| rule | 12 | 27 | 176 × 1 | |
| rows 0–8 | 12 | 32 + 12n | label ≤ 106, value right to 188 | scale 1, §3.4 |
| rule | 12 | 144 | 176 × 1 | |
| footer | 12 | 152, 170 | ≤ 166 × 14 each | two lines |

Nine rows at 12 px occupy y 32 … 134 and the bar reaches y 137; the rule is at 144,
so the column closes with 6 px of paper — the same margin the six-row menu had. A
`static_assert` in `ui_render.cpp` refuses a tenth row rather than letting one run
under the footer.

### 5.2 The right column

The value column is what makes a nine-row list worth 176 px of width. It is
right-aligned to x = 188 and at the same scale 1 as the labels (§5.1), in the same
`kInk`:

| Row | Value | Source |
|---|---|---|
| `Notes` | `Store::count(Pending) + count(Sent) + count(Failed)` | the card |
| `Sync now` | `Store::count(Pending)` — what it would upload | the card |
| `Phone` | *blank* | it opens its own page (§11.5); the row itself has no state to show |
| `Wi-Fi` | `on` / `off` | the Wi-Fi port's link state |
| `About` | *blank* | |
| `Wipe notes` | *blank* | what it would take is on its own confirm |
| `Format card` | *blank* | likewise |
| `Unpair phones` | *blank* | likewise |
| `Back` | *blank* | |

A blank value is a deliberate empty column, not a missing one: three rows with
values and three without reads as a table, whereas six without reads as a list of
words. Do not substitute a dash.

### 5.3 What changes on partial vs full

| Change | Refresh |
|---|---|
| entering Menu | PARTIAL, a hand-over (§15.2) |
| bar moves (BOOT tap) | PARTIAL, bounded by §15.3 |
| a value changes (a note was sent) | PARTIAL |
| leaving Menu → Notes, About, Unpair, a confirm, Back | PARTIAL, a hand-over |
| leaving Menu → Phone, Wi-Fi | **FULL** — the port owns that window or that panel |

The value column changes while the menu is open only after `Sync now`, which
returns to Home (§6.4) rather than to the menu, so in practice the menu is static
until a key is pressed.

---

## 6. Notes list

**Keys:** BOOT tap `NextNote` (wraps) · BOOT hold `Back` to Menu · PWR tap `OpenNote` (nothing at zero notes).
**Job:** pick one note out of N, and show at a glance which ones have not gone yet.

### 6.1 Layout

```
y=0    ┌──────────────────────────────────────────────────────┐
       │ NOTES                                     * TO SEND  │  y=8
y=27   ├──────────────────────────────────────────────────────┤
       │ ████ 21 Nov 14:02                                  * │  y=32   selected
       │      20 Nov 09:11                                  * │  y=50
       │      20 Nov 07:48                                    │  y=68
       │      19 Nov 18:40                                    │  y=86
       │      18 Nov 21:05                                    │  y=104
       │      18 Nov 16:22                                    │  y=122
y=144  ├──────────────────────────────────────────────────────┤
       │ BOOT: next                                           │  y=152
       │ PWR: open                                            │  y=170
y=194  └──────────────────────────────────────────────────────┘
```

| Element | x | y | size | note |
|---|---|---|---|---|
| title `NOTES` | 12 | 8 | 58 × 14 | scale 2 |
| key label | right to 188 | 8 | ≤ 106 × 14 | `* TO SEND` / `! FAILED` / the count |
| rule | 12 | 27 | 176 × 1 | |
| rows 0–5 | 12 | 32 + 18n | date ≤ 154, mark right to 188 | §3.4 |
| rule | 12 | 144 | 176 × 1 | |
| footer | 12 | 152, 170 | | two lines |

### 6.2 The row is a ledger line

One line per note, because the arithmetic forbids two:

- `21 Nov 14:02` is **12 columns** (142 px) and is the whole left side.
- `0:14  pending` is 13 more columns. With the date that is 25 columns against a
  budget of 14, which is why **duration and state do not appear in the list.**
  They appear on the note's own screen (§7), which is one press away.
- What is left of the 176 px after the date is a **status column** at the right
  edge, 1 column wide.

| State | Mark | Why |
|---|---|---|
| `pending` | `*` | the queue, the thing that will move |
| `failed` | `!` | needs a decision |
| `sent` | *blank* | **mark the exception, not the rule** |

Sent rows carry nothing. That is the right polarity: a blank margin is the normal
state of a settled ledger, and printing a `v` or a dot on every row would cost 12 px
of the 14-column budget on every line to say "nothing to do".

The marks are keyed on the glass, not in documentation: when the list holds any
unsent note the title band reads `* TO SEND` on the right; when any note has failed
it reads `! FAILED` (failure wins, being the more urgent). When everything is sent
the right side shows the plain count. **The key is never off-screen**, and it costs
no vertical space because it shares the title line.

### 6.3 Ordering and the empty list

Newest first, as the plan specifies: sort by `note_id` **descending**. Note ids are
`<device_id>-<YYYYMMDDTHHMMSS>-<seq:04u>` and therefore sort chronologically as
plain strings — do not parse dates to order them. Notes recorded before the clock
was ever set carry a 1970 stamp in the id; `note_id_undated()` identifies them, and
they sort to the bottom, which is honest: they are the oldest and least trusted.

**Empty state** — two centred lines, `NO NOTES YET` at y = 78 and `BOOT TO RECORD`
at y = 100, with `PWR: open` suppressed from the footer (nothing can open) and
`BOOT: next` replaced by `BOOT: record`. An empty list that still advertises
"next" is a lie the user tests once.

### 6.4 Scrolling is a cursor, not a viewport

`State::note` is a single `uint16_t` index into the whole list, and BOOT wraps it.
There is no scroll offset and no paging: **the bar walks down the page and, at row
5, the list rotates so the selection stays at the bottom row.**

| `note` index | rows shown | bar at row |
|---|---|---|
| 0 … 5 | 0 … 5 | `note` |
| 6 … N−1 | `note − 5` … `note` | 5 |

So `visible_first = note < 6 ? 0 : note − 5`. That is the whole scroll model, it
is five lines of code, and it means the bar never leaves the glass — which matters
because a bar that disappears off the bottom edge while the user holds a key reads
as a dropped press.

### 6.5 What changes on partial vs full

| Change | Refresh |
|---|---|
| entering the list (from Menu, from a note, after a delete) | PARTIAL, a hand-over (§15.2) |
| bar moves | PARTIAL, bounded by §15.3 |
| the row set changes (the list rotates) | **FULL** — six rows of text repaint at once |
| a mark changes (`*` → blank after an upload) | PARTIAL |
| the title-band key line changes | PARTIAL |

The rotation is full because it moves six text rows, and a partial would leave the
current dates ghosted under the new ones — the classic e-paper smear. One extra
flash per sixth press is a fair price for never showing two dates stacked.

---

## 7. NoteAction — a note's own screen

**Keys:** BOOT tap `NextNoteAction` (Play → Delete → Back, wraps) · BOOT hold `Back` to the list · PWR tap `RunNoteAction`.
**Job:** identify the note beyond doubt, and offer three verbs in a fixed order.

### 7.1 Layout

```
y=0    ┌──────────────────────────────────────────────────────┐
       │ NOTE                                                 │  y=8
y=27   ├──────────────────────────────────────────────────────┤
       │ 21 Nov                                               │  y=34  hero, scale 3
       │                                                      │
       │ 14:02 · 0:14                                         │  y=62
       │ to send                                              │  y=80
y=98   ├──────────────────────────────────────────────────────┤
       │ ████ Play                                            │  y=106  selected
       │      Delete                                          │  y=124
       │      Back                                            │  y=142
y=162  ├──────────────────────────────────────────────────────┤
       │ PWR: select                                          │  y=172
y=194  └──────────────────────────────────────────────────────┘
```

| Element | x | y | size | note |
|---|---|---|---|---|
| title `NOTE` | 12 | 8 | 58 × 14 | |
| rule | 12 | 27 | | |
| hero | 12 | 34 | ≤ 106 × 21 | scale 3, `DD MMM`, 6 cols |
| meta | 12 | 62 | ≤ 154 × 14 | `HH:MM · M:SS`, 13 cols |
| state | 12 | 80 | ≤ 106 × 14 | `to send` / `sent` / `failed` |
| rule | 12 | 98 | 176 × 1 | separates identity from verbs |
| rows 0–2 | 12 | 106 + 18n | | §3.4 |
| rule | 12 | 162 | | |
| footer | 12 | 172 | | one line |

The date is the hero because a note has no title — it has a time. Two rules make
the screen read as *this is the note* / *these are your choices*, which is worth
more than the 24 px they cost.

`·` is U+00B7, inside the font's Latin-1 range, and renders as a real glyph.
**Never use U+2026 (`…`)**: the font has no glyph for it and every occurrence
draws as `?`. Mark overflow with ASCII `..`.

### 7.2 The three verbs

| Row | Label | PWR tap does |
|---|---|---|
| 0 | `Play` | `Action::Play` → `Playing` |
| 1 | `Delete` | `Action::AskDelete` → `ConfirmDelete` |
| 2 | `Back` | `Action::Back` → `NoteList` |

Labels come from `action_label()` in `ui_keys.cpp` — **do not duplicate these
strings in the app.** `Delete` does not delete: it opens the confirm. That is why
the confirm screen names the hold (§8).

### 7.3 What changes on partial vs full

| Change | Refresh |
|---|---|
| entering from the list | PARTIAL, a hand-over (§15.2) |
| bar moves | PARTIAL, bounded by §15.3 |
| returning from ConfirmDelete | PARTIAL, a hand-over |
| returning from Playing | **FULL** — the player is the runtime's, and it stops the codec on the way out |

Nothing else on this screen moves. State (`to send` → `sent`) cannot change while
it is open, because `Playing` and `ConfirmDelete` are the only exits and both
return here (§13).

---

## 8. ConfirmDelete

**Keys:** BOOT hold `DeleteNote` · BOOT tap `KeepNote` · PWR tap `KeepNote`.
**Job:** make the destructive action deliberate and the safe action free.

### 8.1 Layout

```
y=0    ┌──────────────────────────────────────────────────────┐
       │                                                      │
       │                    DELETE?                           │  y=46  hero, scale 3
       │                                                      │
       │                 21 Nov 14:02                         │  y=78
       │                                                      │
y=104  ├──────────────────────────────────────────────────────┤
       │                  HOLD: delete                        │  y=118
       │                                                      │
       │                    TAP: keep                         │  y=140
y=190  └──────────────────────────────────────────────────────┘
```

| Element | x | y | size | note |
|---|---|---|---|---|
| hero `DELETE?` | centred | 46 | 123 × 21 | scale 3, 7 cols |
| date | centred | 78 | ≤ 154 × 14 | scale 2 |
| rule | 12 | 104 | 176 × 1 | |
| body 1 `HOLD: delete` | centred | 118 | 154 × 14 | |
| body 2 `TAP: keep` | centred | 140 | 118 × 14 | |

### 8.2 Why this screen is built the way it is

- **It has no footer and no rows.** Every other screen is a page you navigate;
  this one is a question you answer. Removing the frame's bottom rule and the row
  grammar is what makes it read as an interruption — the only interruption in the
  UI — rather than another list.
- **Nothing is preselected.** There is no bar, because there is nothing to walk to.
  The user cannot tab onto `delete` and press; they must hold.
- **Both cancel paths are free.** BOOT tap and PWR tap both `KeepNote`, so a
  fumbled press is always the safe answer, and the *only* way to delete is the
  deliberate 2 s hold the key map already defines.
- **Both key names are on the glass.** This is the one screen where the footer's
  budget is met by the body instead: `HOLD:` / `TAP:` is the legend, and it is
  where the decision is.
- **The date is restated.** Deletion is irreversible on the card, so the screen
  repeats which note is about to go.

**Known limitation:** if two notes share a minute, `21 Nov 14:02` does not
distinguish them, and there is no room for the duration as well (19 columns). The
mitigation is structural rather than visual: the list order is deterministic, so
the user can leave and re-check, and the hold makes an accidental delete
impossible. Adding an id fragment to disambiguate would cost the hero and is not
worth it for a collision that requires two notes in the same minute.

### 8.3 What changes on partial vs full

| Change | Refresh |
|---|---|
| entering from the note screen | PARTIAL, a hand-over (§15.2) |
| **anything while it is open** | **nothing at all** |
| leaving (delete or keep) → the list | PARTIAL, a hand-over |

This screen is **immobile by design**: the note identity and the two key verbs are
fixed for as long as the question is on the glass. It is the one place in the UI
where nothing can move while it is open, which also means its two frames are the
only ones it ever draws.

### 8.4 ConfirmWipe and ConfirmFormat

The menu's two destructive rows use the same face, the same key map (PWR tap,
BOOT tap → cancel; BOOT hold → the act) and the same "no footer, no rows" rule:

| Screen | Hero (scale 3) | Line 1 (scale 2) | Line 2 (scale 2) | Hold legend |
|---|---|---|---|---|
| `ConfirmWipe` | `WIPE NOTES` | `113 NOTES GO` | `THE CARD STAYS` | `HOLD: wipe` |
| `ConfirmFormat` | `FORMAT` | `THE SD CARD` | `NOTES AND LOG GO` | `HOLD: format` |

The hero is ten columns at most because 11 columns at scale 3 is 195 px and the ink
rule keeps 4 px of margin; the rest of each sentence is on the body lines, where
the count (from `Snapshot::notes_total()`) and what is lost are stated plainly. A
wipe keeps the filesystem and the settings (they are NVS); a format takes the volume
and everything on it.

Both screens carry the **busy guard**: while the runtime is not idle (recording or
uploading) `Snapshot::busy` holds its own words — `BUSY RECORDING`,
`BUSY UPLOADING` — drawn at scale 1 above the rule, and `ScreenApp::press()` refuses
the hold before the key map can move the screen. The confirm stays up; a recording
or an upload cannot be formatted out from under itself. The guard is re-evaluated on
every key and tick, so a capture that starts while the question is open puts the
line on the glass.

The layout shifts up from §8.1 to make room for the second body line and the
reason: hero y=42, line 1 y=72, line 2 y=90, reason y=108, rule y=118, legend
y=126 and y=144.

### 8.5 Wiping — the work behind the two confirms

The hold does not do the work on the key's path. A wipe of 114 note folders was
measured at ~90 s on this card (four `remove`/`rmdir` a note on a 1-bit SD link)
and a 64 GB format at 22.8 s; run inside the effect, either one kept the confirm
on the glass for its whole length and left the console answering `@error device
busy` (the console waits 10 s for the app task's reply). So `Action::WipeNotes` /
`Action::FormatCard` hand the screen to `Screen::Wiping` with the effect, the
runtime starts a worker task and returns in milliseconds, and `ScreenWiping` is
what the person watches:

| Screen | Hero (scale 3) | Line 1 (scale 2) | Line 2 (scale 2) | Bar | Footer |
|---|---|---|---|---|---|
| `Wiping` (wipe) | `WIPING` | `%u OF %u` (then `READING CARD` before the first count) | `THE CARD STAYS` | yes, from the real count | `PLEASE WAIT` |
| `Wiping` (format) | `FORMATTING` | `THE SD CARD` | `DO NOT UNPLUG` | no — a format has no honest number | `PLEASE WAIT` |

**Keys:** none. All four gestures are `None` on this screen; a press in the middle
of a 20-90 s wipe can only confuse, and on this board a 2.5 s PWR hold cuts the
rail in hardware whatever the firmware decides.

The counts are the store's own (`Store::wipe` lists the three folders first, so
the total is known before the first removal, and reports `(done, total)` once per
note). The bar redraws on a **5 % step**, never per note: a partial waveform is
~0.6 s on this unit and 114 of them would cost more than the wipe. The last note
always draws. When the job ends - either way - the machine goes back to Home and
the publish that follows re-reads the card, so the frame says `0 NOTES`.

The runtime's side: `mode` is `wiping`/`formatting` for the length of it (which
`notes status` prints), the count scan, the retention pass, the Bluetooth source
and the upload planner wait for the job (the worker is inside the same volume),
and a recording asked for during it is refused in the log. The console stays
answerable throughout - `notes status` and `ui` took 0.2 s on the bench - because
the app task only polls the worker's counts. A key press during the job takes no
card read either (`UiState::press` skips the rescan on `Screen::Wiping`).

---

## 9. Playing

**Keys:** BOOT tap `StopPlay` · BOOT hold `StopPlay` · PWR tap `StopPlayToList` · PWR hold `None`.
**Job:** show that audio is coming out, of what, and how far in.

### 9.1 Layout

```
y=0    ┌──────────────────────────────────────────────────────┐
       │ 2 TO SEND                        ((•))   ▰▰▰▱       │  y=8
y=27   ├──────────────────────────────────────────────────────┤
       │ 21 Nov 14:02                                         │  y=40
       │                                                      │
       │                    0:06                              │  y=60  hero, scale 3
       │                                                      │
       │                  OF 0:14                             │  y=92
       │                                                      │
       │ ▐███████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░    │  y=132 bar
y=162  ├──────────────────────────────────────────────────────┤
       │ BOOT: stop                                           │  y=172
y=194  └──────────────────────────────────────────────────────┘
```

| Element | x | y | size | note |
|---|---|---|---|---|
| band | — | 8 | — | facts continue to be true while audio plays |
| rule | 12 | 27 | | |
| title | 12 | 40 | ≤ 154 × 14 | the note's `DD MMM HH:MM` |
| elapsed | centred | 60 | ≤ 141 × 21 | scale 3, `M:SS`, `H:MM:SS` past an hour |
| total | centred | 92 | ≤ 130 × 14 | `OF M:SS`, `OF H:MM:SS` past an hour |
| bar | 12 | 132 | 176 × 9 | track; fill inset 2 px, 5 px tall |
| rule | 12 | 162 | | |
| footer | 12 | 172 | | one line |

The elapsed time is the hero because during playback it is the only number that
answers "is it working". The total sits below as `OF M:SS` so the bar has a
denominator on the glass.

### 9.2 Transport timing

**Update at most once every 5 s**, and never per second. A 14 s note refreshed at
1 Hz would cost 14 partial refreshes for a bar that gains 12 px per second; at 5 s
it costs 3 and the bar still reads as live. The recording counter is slower still
(30 s, §10.3): it has no bar that has to look live, and a note whose length is
read once a minute is the same note.

```c
if (millis() - last_draw < 5000 && !state_changed) return;   // no flush at all
```

End of audio and `StopPlay` are state changes and draw immediately.

### 9.3 A PWR tap stops the audio and goes back to the list

`press_power()` on `Screen::Playing` returns `Action::StopPlayToList`: the codec
stops, the file closes, and the list the player was opened from is the screen
after the tap. It used to return `Action::PowerOff`, so a person browsing notes
switched the device off mid-playback with one key (t_dec6f9ef). Stopping is BOOT's
job too, and BOOT's stop stays on the note's own screen; PWR is the way out. The
footer says `BOOT: stop` because that is the verb that keeps the note open, not
because PWR is dangerous.

A PWR *hold* on the player is not a power-off either: only Home is (13.2 row 4b).

### 9.4 What changes on partial vs full

| Change | Refresh |
|---|---|
| entering from the note screen | **FULL** — the runtime opens the codec on the way in |
| elapsed digits + bar | PARTIAL every 5 s |
| stop / end of audio → back to the note screen or the list | **FULL** — the runtime closes the codec on the way out |

The bar grows only, so it is ghost-free (§15.3). The elapsed digits are the one
region on this screen that can leave residue, bounded by `kFullEvery` and by the
5 s cadence.

---

## 10. Recording

**Keys:** BOOT tap `StopKeep` · BOOT hold `Discard` · PWR tap `None` · PWR hold `None`.
**Job:** prove it is recording, show how long, and show how to keep or throw it away.

### 10.1 Layout

```
y=0    ┌──────────────────────────────────────────────────────┐
       │ 3 TO SEND                        ((•))   ▰▰▰▱       │  y=8
y=27   ├──────────────────────────────────────────────────────┤
       │ RECORDING                                            │  y=38
       │                                                      │
       │                    0:42                              │  y=62  hero, scale 3
       │                                                      │
       │                  OF 5:00                             │  y=94
       │                                                      │
       │ ▐█████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░    │  y=118 bar
y=144  ├──────────────────────────────────────────────────────┤
       │ BOOT: stop                                           │  y=152
       │ HOLD: discard                                        │  y=170
y=194  └──────────────────────────────────────────────────────┘
```

| Element | x | y | size | note |
|---|---|---|---|---|
| band | — | 8 | — | queue count of the *previous* notes |
| rule | 12 | 27 | | |
| label `RECORDING` | 12 | 38 | 106 × 14 | scale 2, sentence case is wrong here — uppercase |
| elapsed | centred | 62 | ≤ 141 × 21 | scale 3, `M:SS` and `H:MM:SS` past an hour — the hero, the real length, never a clamp (`24:00:00` at the ceiling is 8 columns of 18 px) |
| cap | centred | 94 | ≤ 154 × 14 | `OF M:SS` and `OF H:MM` past an hour, plus the room behind it: `OF 1:30·2.2G`. The cap is the capture's own budget — the setting, the 2 GiB ceiling and the card's free space less its 256 MB reserve, whichever is smallest — not the setting alone |
| bar | 12 | 118 | 176 × 9 | `elapsed / cap` |
| rule | 12 | 144 | | |
| footer | 12 | 152, 170 | | **two lines** |

The elapsed time is the hero and the state word is the label: after one second the
timer answers "did it start" better than the word does, and it keeps answering it.

**No audio level meter.** The panel updates at most every 30 s while recording, so
a VU meter would be a half-minute average of a 1-second signal — it would lie
convincingly. A missing meter is better than a misleading one.

### 10.2 The two-line footer is the safety rail

`HOLD: discard` is the only destructive gesture in the UI that has no confirmation
step, and it is invisible. It earns a dedicated line. This is the one screen where
both bindings are shown, and it is the reason the footer is allowed to be two
lines at all.

### 10.3 What changes on partial vs full

| Change | Refresh |
|---|---|
| entering (`StartRecording`) | **FULL** — the frame says `0:00` |
| elapsed + bar, every **30 s** | PARTIAL |
| `StopKeep` → Home, `Discard` → Home, `PowerOff` | **FULL** |

At 30 s cadence a 5-minute note costs 10 partials and 3 fulls. At 1 Hz it would
cost 300 partials and 15 full-screen flashes — the difference between a counter
and a strobe; at 5 s it still cost 60 partials for a number nobody reads that
often. **The counter is driven by a tick count, never by a blocking delay in the
draw path** (§16).

### 10.4 A tap during an upload: queued, not dropped

The runtime has one enum for capture and upload, so a note cannot start while a
note is uploading. A tap in that window is kept, not refused: the runtime arms a
pending recording and starts it on the pass the note in flight finishes (one
transfer, not the whole queue). On the glass the recording face keeps its label
and its footer but replaces the counter with the wait, so the press visibly
landed:

```
       │ RECORDING                                            │  y=38
       │                     QUEUED                           │  y=62  hero, scale 3
       │                  FOR THE UPLOAD                      │  y=94
       │ BOOT: cancel                                         │  y=152
       │ HOLD: cancel                                         │  y=170
```

No counter and no bar, because nothing has been recorded yet; a counter at `0:00`
over a note that is not being made is the same lie the runtime-follows rule
exists to prevent. When the upload ends and the capture starts, the counter
appears at the runtime's own second — the same full refresh an entry takes. A tap
on this face cancels the wait (`StopKeep` on the runtime's arm), and the glass
goes Home with no note written. The reason the capture waits rather than
overlapping the transfer is measured: the capture's PCM reaches the app task, and
that task is inside the uploader's socket writes for seconds at a time, so a
capture begun during an upload stores a fraction of what the microphone
delivered.

---

## 11. Pairing, and About

### 11.1 Pairing — the phone's ask

**Keys:** BOOT tap `AllowPhone` · PWR tap `RefusePhone` · BOOT hold does nothing.
**Job:** show the six-digit passkey so the user can compare it with the phone.

```
y=0    ┌──────────────────────────────────────────────────────┐
       │ PAIRING                                              │  y=8
y=27   ├──────────────────────────────────────────────────────┤
       │                                                      │
       │                  418 207                             │  y=44  hero, scale 3
       │                                                      │
       │               Same as phone?                         │  y=86
       │                                                      │
       │               BOOT: allow                            │  y=108
       │                                                      │
       │               PWR: refuse                            │  y=130
y=162  ├──────────────────────────────────────────────────────┤
       │ HOLD: nothing                                        │  y=172
y=194  └──────────────────────────────────────────────────────┘
```

| Element | x | y | size | note |
|---|---|---|---|---|
| title `PAIRING` | 12 | 8 | 82 × 14 | |
| rule | 12 | 27 | | |
| passkey | centred | 44 | ≤ 123 × 21 | scale 3, `NNN NNN` |
| body 1 | centred | 86 | 166 × 14 | `Same as phone?` |
| body 2 | centred | 108 | 130 × 14 | `BOOT: allow` |
| body 3 | centred | 130 | 142 × 14 | `PWR: refuse` |
| rule | 12 | 162 | | |
| footer | 12 | 172 | | `HOLD: nothing` |

Three things make this screen different from the rest:

- **It is an overlay, not a destination.** `show_pairing_ask()` records where it
  interrupted, and `answer_pairing()` returns there, so the menu keeps its
  selection and the list keeps its note. The spec must not "go home" afterwards.
- **The passkey is the highest-value string in the UI** and gets scale 3. The space
  in `418 207` is a grouping, not a separator: the phone shows six digits and the
  eye compares three at a time.
- **The footer says the hold does nothing**, because `hold_boot()` returns
  `Action::None` on this screen and a user panicking at a pairing prompt is
  exactly the user who holds the button. "A hold is never consent" is worth one
  line of glass.

The passkey is drawn in `kInk`. Do not box it, do not invert it: a code the user
must read aloud should be the plainest thing on the screen.

### 11.2 About

**Keys:** BOOT hold `Back` to Menu · BOOT tap does nothing · PWR tap `None`.
**Job:** the four facts the plan asks for — battery, firmware, device id, card free
— plus the note count.

```
y=0    ┌──────────────────────────────────────────────────────┐
       │ ABOUT                                                │  y=8
y=27   ├──────────────────────────────────────────────────────┤
       │ Battery                                       84%   │  y=40
       │ Firmware                                    2.0.1    │  y=62
       │ Device                                   hg-a1b2     │  y=84
       │ Card free                                  62.9G     │  y=106
       │ Notes                                         12     │  y=128
y=162  ├──────────────────────────────────────────────────────┤
       │ HOLD: back                                           │  y=172
y=194  └──────────────────────────────────────────────────────┘
```

| Element | x | y | size | note |
|---|---|---|---|---|
| title `ABOUT` | 12 | 8 | 70 × 14 | |
| rule | 12 | 27 | | |
| rows 0–4 | 12 | **40 + 22n** | label ≤ 106, value right to 188 | pitch 22, not 18 |
| rule | 12 | 162 | | |
| footer | 12 | 172 | | |

The pitch is 22 rather than 18 because five rows at 18 would leave 55 px of dead
paper above the rule; a reference page can afford to breathe where a list cannot.
Labels are sentence case, values right-aligned — the same grammar as the menu, so
the two screens read as a family.

**11.2.1 The gap in `ui_keys.hpp` this screen exposes.** The landed `Screen` enum
has no `About`, so `MenuItem::About` has nowhere to go and no way back. The fix is
three lines and invents no binding — it reuses the invariant every non-menu screen
already follows (`hold_boot` = back):

```cpp
// ui_keys.hpp, in enum class Screen, after Pairing:
  About,

// ui_keys.cpp, press_power():  About is one of the taps that does nothing
    case Screen::About:
      return Action::None;          // a tap is never a power-off (t_dec6f9ef)

// ui_keys.cpp, hold_boot():
    case Screen::About:
      state.go(Screen::Menu);
      return Action::Back;
```

`press_boot()` returns `Action::None` for `About` — the page is complete on one
screen and there is nothing to advance to. Add one host case to
`firmware/tests/test_notes_ui.cpp`:

```cpp
TEST("about: a hold goes back to the menu, a tap does nothing, and a tap is not power") {
  State s = at(Screen::About);
  CHECK(press_boot(s) == Action::None);
  CHECK(hold_boot(s) == Action::Back);
  CHECK(s.screen == Screen::Menu);
  CHECK(press_power(s) == Action::None);
}
```

### 11.3 What changes on partial vs full

| Screen | Change | Refresh |
|---|---|---|
| Pairing | entering / answering | **FULL** both ways |
| Pairing | anything while open | nothing — the ask is immobile |
| About | entering / leaving | PARTIAL, a hand-over (§15.2) |
| About | anything while open | nothing — the page is a snapshot at entry |
| Phone | entering / leaving | **FULL** both ways |
| Phone | the state word, a refusal or the bond count moves | **FULL** — a fact changed, not a tick |
| Phone | the countdown crosses a 5 s boundary | **P** |
| Phone | a poll finds nothing new | nothing — no flush at all |

Pairing and About are static pages. About is deliberately **not live**: a battery
percentage that ticks while you watch it would cost a panel refresh to tell you
something you can already see in the band of every other screen. The Phone page is
the one live page here, and only in one line: its countdown is a deadline, so it
moves, and it moves at most once per 5 s (§11.5.2).

### 11.4 Unpair — the bonded phones

**Keys:** BOOT tap next phone · PWR tap ask about the selected one · BOOT hold
`Back` to Menu. On the question: BOOT hold removes the bond, BOOT tap and PWR tap
keep it (the delete confirm's map, §9).

**Job:** let a person remove a phone from the device's own glass, so a borrowed or
resold unit is not still bonded to somebody else's handset. The pairing table
holds three (`CONFIG_BT_NIMBLE_MAX_BONDS`), so the list is short enough that every
phone is on the page: no scroll, and the bar's wrap is the only movement.

```
y=0    ┌──────────────────────────────────────────────────────┐
       │ UNPAIR                                    2 PAIRED   │  y=8
y=27   ├──────────────────────────────────────────────────────┤
       │ A4:C1:38:7F:12:9B                                   │  y=32  (selected)
       │ E0:5A:1B:22:9C:04       Pixel 8 Pro                 │  y=48
y=144  ├──────────────────────────────────────────────────────┤
       │ BOOT: next                                           │  y=152
       │ PWR: unpair                                          │  y=170
y=194  └──────────────────────────────────────────────────────┘
```

| Element | x | y | size | note |
|---|---|---|---|---|
| title `UNPAIR` | 12 | 8 | 70 × 14 | |
| count `2 PAIRED` | right to 188 | 8 | | empty when the table is empty |
| rule | 12 | 27 | | |
| rows | 12 | **32 + 16n** | address ≤ 101, name right to 188 | scale 1, pitch 16, bar 15 |
| rule / footers | 12 | 144 / 152 / 170 | | the two-line footer |

**11.4.1 Why the address, at that size.** The address is what a phone's own
settings show, so it is the one thing a person can compare with the handset in
their hand. It is 17 columns (`A4:C1:38:7F:12:9B`), which is 203 px at the body
size — off the 176 px column — so the list rows and the question's two lines are
scale 1, the size the menu's rows already use. A name is drawn beside the address
when the radio knew one; the bond store keeps keys, not names, so in practice the
address is the whole row, and the layout leaves the room for the day something
does know one (12 columns, no collision with the 101 px address).

**11.4.2 The question.** `UNPAIR?`, the address, the name (or nothing), and the
runtime's one-line reason when the hold would be refused:

```
y=42   UNPAIR?                        scale 3
y=72   E0:5A:1B:22:9C:04              scale 1
y=90   Pixel 8 Pro                    scale 1, empty when unknown
y=108  BUSY UPLOADING                 scale 1, only while the device is busy
y=118  ───────────────────────────────
y=126  HOLD: unpair                   scale 2
y=144  TAP: keep                      scale 2
```

The guard is the wipe's (§9.3): while `mode_ != Mode::Idle` — a note recording, an
upload in flight — the hold is refused and the reason is already on the glass. A
phone losing its keys mid-upload is the case that would strand the note.

**11.4.3 What the hold does.** One `ble_store_util_delete_peer` for the address the
question named - the store's key entries and that phone's CCCDs and RPA records -
then the table is read again and the list republished. If the phone is connected,
its link is terminated first: it keeps its own half of the bond until it notices,
and a link held over keys this device no longer has can never encrypt again. The
phone then shows the device as not paired and has to pair again (BOOT hold, Phone,
the passkey) before it can write a note. The delete is asked once per hold: a
second phone needs a second question.

The store's API (`ble_store_util_*`) is the Bluetooth host task's, and the host is
deinitialised when its window closes. The screens ask from the app task, so
`port_ble` hands the work to the host's event queue and waits a few milliseconds
for the answer; a store call with the host down is refused rather than run on the
app task, because the host's own lock lives in memory `nimble_port_deinit()` has
freed. Opening the screen (or `ui peers`) therefore wakes the radio first when it
has gone to rest, and the window it opens is the plain wake, not a pairing one:
the person asked to see the table, so the stack it lives in comes up for a minute.

**11.4.4 An empty table.** `NOTHING PAIRED` at the body size, `open PHONE to pair`
under it, and a footer that offers only `HOLD: back` — a list that advertises
"next" with nothing in it is a lie the user tests once (the note list's own rule).
PWR does nothing there.

### 11.5 Phone — the Bluetooth window's own page

**Keys:** PWR tap `ClosePhone` to Menu · BOOT hold `ClosePhone` to Menu · BOOT tap
does nothing.
**Job:** show that a window is open, what the radio is doing, how long it has left
and how many phones are already bonded — so a window that failed to open says why
on the glass instead of looking like a flash and a step back.

```
y=0    ┌──────────────────────────────────────────────────────┐
       │ PHONE                                                │  y=8
y=27   ├──────────────────────────────────────────────────────┤
       │                   Hermes Gadget                      │  y=36   name, scale 2
       │                      WAITING                         │  y=54   state, scale 2
       │                       4:57                           │  y=74   countdown, scale 3
       │                    1 OF 3 PAIRED                     │  y=104  bond table
       │                                                      │  y=124  reason (empty here)
y=144  ├──────────────────────────────────────────────────────┤
       │ PWR: close                                           │  y=152
       │ HOLD: back                                           │  y=170
y=194  └──────────────────────────────────────────────────────┘
```

| Element | x | y | size | note |
|---|---|---|---|---|
| title `PHONE` | 12 | 8 | 58 × 14 | scale 2 |
| rule | 12 | 27 | 176 × 1 | |
| device name | centred | 36 | ≤ 154 × 14 | `snap.device`: the name a phone's chooser lists |
| state | centred | 54 | ≤ 154 × 14 | one of seven states, §11.5.1 |
| countdown | centred | 74 | ≤ 87 × 21 | scale 3, `M:SS`, only while a deadline runs |
| bond table | centred | 104 | ≤ 154 × 14 | `%u OF %u PAIRED`, empty when `slots` is 0 |
| reason | centred | 124 | ≤ 106 × 14 | `Held open` / `Radio off`, empty otherwise |
| rule | 12 | 144 | 176 × 1 | the two-line footer's rule |
| footer | 12 | 152, 170 | ≤ 118 × 14 each | `PWR: close` / `HOLD: back` |

The page has **no band**. Pairing and About do the same: a page that owns the whole
panel puts its title in the band's slot at y = 8, draws the rule at 27, and spends
the rest on its own content. Nothing here reports the queue, the station or the
battery, so a glance at this page tells you about the window and nothing else.

**11.5.1 Five lines, and the three that come and go.** The name is the configured device
name, the string the pairing chooser shows on the phone (`Hermes Gadget` by
default; the line is simply absent when the snapshot carries no name). The state
line is the port's phase, uppercase:

| Phase | State line | Countdown | Bond table | Reason line |
|---|---|---|---|---|
| controller still coming up | `ADVERTISING` | the deadline | when `slots` is known | — |
| advertising, no link | `WAITING` | the deadline | when `slots` is known | — |
| a phone is on it | `CONNECTED` | never — a link holds the window open | when `slots` is known | `Held open` |
| refused: no bond slot free | `PAIRING FULL` | never | `3 OF 3 PAIRED` | — |
| refused: controller would not start | `RADIO BUSY` | never | `n OF 3 PAIRED` | — |
| refused: this build has no Bluetooth | `BLE OFF` | never | — (`slots` = 0) | — |
| the deadline passed | `WINDOW CLOSED` | never | when `slots` is known | `Radio off` |

The countdown is `M:SS`, rounded **up**, so a window that has just opened reads
`5:00` rather than `4:59`. It is drawn only while `left_ms` is not zero and the
phone is not connected: a connected phone keeps the window open, so a clock there
would count down to nothing. The bond table is drawn only when the port knows the
slots (`slots != 0`); `0` means the build has no Bluetooth, and `0 OF 0 PAIRED`
would be a lie about a table that does not exist. The reason line is the same
discipline: it carries only the two states that need a second sentence, so the
countdown at scale 3 ends at y = 95 with 9 px of paper above the table.

**11.5.2 The countdown is the only thing that moves.** The app task polls the port
every 250 ms while this screen is up (`poll_phone()`), and `set_phone()` asks three
questions of the answer: did the phase or refusal move, did the bond count move, did
the countdown cross a 5 s boundary. The first two are facts, so they take a full
refresh; the countdown takes a partial; if none of the three moved there is no
flush at all. A five-minute window is therefore about sixty partial refreshes, not
three hundred. Applying the port's answer when the screen is not up changes the
stored status and draws nothing.

**11.5.3 The port owns the radio, not the page.** `Effect::OpenPhoneWindow` asks
`port_ble` for the window and `PhoneStatus` is the port's answer; `set_phone()`
"turns the port's answer into a frame and nothing else". The window is five minutes
(`RadioWindow::Timing::phone_ms = 300000`). Leaving the page closes it
(`close_phone_window()`), so the radio is never left advertising behind a page
nobody is on. A window that runs out on its own does **not** close the page: the
word becomes `WINDOW CLOSED`, the reason `Radio off`, and the page waits for PWR or
the hold, so the person still sees why their phone stopped finding the device.

**11.5.4 A refusal is the whole point.** The three refusals are the reasons a window
can fail to open: the bond table is full, the controller would not start, or this
build has no Bluetooth. Each is the state word itself, and where the table exists
the row under it says how full it is. That is the defect this page removed: the row
used to hand the panel to the port and stay on the menu, so a refusal was a flash
and a step back, indistinguishable from a key that missed (t_9972e2a1).

The name is the one variable-length line. The default is 13 columns (154 px); the
snapshot carries up to 15, which is 178 px, one pixel past the column — the open
item in §17.2 item 9.


---

## 12. Frames outside the screen machine

One of these is drawn by code this spec only hands off to; the other is drawn by
the notes app itself, on the way to a power cut, and no key can leave it.

### 12.1 The port-owned frame

One menu row hands the panel to code that already exists and is proven. It is
**out of scope**, it must not be re-implemented, and the notes app owns only the
hand-off:

| Row | Screen | Owner |
|---|---|---|
| `Wi-Fi` | network list and the phone-based setup AP | `port_wifi_setup` |

The contract at the boundary: the app calls `display->request_full_refresh()`
before the hand-off and again on return, because both directions change the whole
page. Nothing else about that screen changes.

The `Phone` row is **not** one of these. It opens `Screen::Phone` and the page in
§11.5; `port_ble` owns the radio behind it, and `set_phone()` turns the port's
answer into a frame and nothing else. It used to be a hand-off, and the difference
is exactly what a refused window looked like: a flash and a step back rather than
the reason (t_9972e2a1).

### 12.2 The sleeping face — the last frame, and not a screen

An e-paper keeps whatever it was holding when the rail drops. Without a last frame
an off device looks exactly like an on one: after a PWR hold the panel went on
showing Home with its counts, so the one fact a person needs from a dead device —
that it is dead — was the one it could not state. The hold therefore draws a face
of its own before the power goes (`ui_render.cpp`'s `SleepFace`, reached as
`draw_sleep`):

```
y=0    ┌──────────────────────────────────────────────────────┐
       │                                                      │
y=28   │                       zzz                            │  x=96, scale 3 (hero)
y=40   │             ◗▘                                       │  x=54, the 32 px crescent
       │                                                      │
y=116  │          asleep - hold PWR 2 s                       │  scale 1, centred
y=194  └──────────────────────────────────────────────────────┘
```

| Element | x | y | size | note |
|---|---|---|---|---|
| the whole panel | 0 | 0 | 200 × 200 | filled to `kPaper` first. The face is drawn over whatever the person was looking at, and a stale Home frame behind the moon would read as a device that is still on |
| crescent | 54 | 40 | 32 × 32 | `bitmap1` of a 1-bit rune in `kInk`, opening up and to the right, where the zzz climbs out |
| `zzz` | 96 | 28 | 51 × 21 | scale 3 (`grid::kHero`): a mark, not a caption |
| line `asleep - hold PWR 2 s` | 37 | 116 | 125 × 7 | scale 1, centred: what the panel is, and the gesture that leaves it (a hold of about two seconds is also how the board comes up on battery, [using the device](notes-device.md)) |
| band, rule, footer, counts | — | — | — | **not drawn.** Nothing on this face may read as a live device, so it has no zone A, no queue count and no key legend |

**When it is drawn: every firmware-initiated power-off.** Both power paths go
through it — the `Effect::PowerOff` the screens answer a PWR hold with on Home
(`main.cpp`'s `run_effect`), and `power_hold()`'s own path for a hold on any other
screen, where the screens decline and the loop still draws the face before it shuts
down. Two ways down do not: the console's `poweroff`, which runs `shutdown()`
directly so a host can time the flush without a finger, and a battery that dies.
Neither draws a frame, and a dead battery could not.

**It is a full refresh, and it waits for one.** `ScreenApp::sleep_face()` answers
`Refresh::Full`, the only class that leaves no trace of the page it replaces;
`NotesUi::sleep_face()` calls `request_full_refresh()`, flushes, and waits for the
waveform with a 2.6 s bound (`kSleepPanelMs`). The full waveform measured **2,122 ms**
on this unit, and the wait is load-bearing: the rail goes the moment the call
returns, and on battery an unfinished update is not "off", it is a broken panel.
This is the one place the app task does wait on the panel (§15.4's rule has this
single exception), and the bound exists only so a panel that never answers cannot
hold the power-off for ever. On USB the board stays up after the shutdown, because
VBUS keeps it alive, so the frame is the half of the gesture a bench can see; the
rail actually going is a battery observation.

**The hold's timing is why a long hold tears it.** The screens take a hold at
**1.5 s** (`kPowerHoldMs`; the gesture is the release at or past it), and the latch
cuts on its own after a continuous **2.5 s** press, in hardware
([hardware and wiring](hardware.md#waveshare-esp32-s3-epaper-154)). Release inside
that window and the firmware's `shutdown()` is the only cut: the waveform completes
and the panel keeps the face. Keep the finger down past 2.5 s and the hardware cuts
the rail mid-waveform, leaving a part-written frame — the tear the 2.6 s wait exists
to avoid and cannot prevent, because the cut is not the firmware's.

**It is not a `Screen`.** `Screen` has no `Asleep` case and nothing enters one:
`ScreenApp::sleep_face()` moves no state (the machine is still on the screen it was
showing, `app.screen() == Screen::Home` afterwards), so no key comes back from the
face and nothing in the state machine has to know it exists. The frame is drawn
straight into the framebuffer rather than through `frame()`, and
`NotesUi::sleep_face()` sets `asleep_` **before** the flush while `render()` returns
early once it is set — so a publish the runtime makes on its way out (the counts
after a shutdown, a late callback) cannot repaint Home over the face. That is what
makes this the last frame rather than merely the next one. Its two strings are the
only ones in the UI that belong to no screen, so they live beside the others in
`strings.hpp` (`sleep::kZzz`, `sleep::kHint`).

**Host cases.** `firmware/tests/test_notes_screens.cpp`:
`sleep: the frame is the sleeping face, and the panel is cleared` draws it over a
stale Home page and checks the page is gone (the band and the footer ink nothing,
the whole face is under 1,200 px and less than half of Home's, the crescent, the
`zzz` and the line where the table above puts them), and
`sleep: the face draws with a full refresh` checks the class and that it is not a
screen (`Refresh::Full`, `draw`, `Effect::None`, the screen still `Home`, and the
hold that leads to it asking for no frame of its own).


---

## 13. States and transitions

### 13.1 The machine

Two layers, deliberately separate. `ui_keys.*` is the **pure** layer: it turns one
press into exactly one `Action` and mutates only `State`. The app is the
**impure** layer: it performs the action (SD, radio, codec) and decides which
screen is drawn. Everything in the diagram below that touches hardware lives in
the app; everything that is a key rule lives in `ui_keys.cpp` and is already
covered by eight host cases.

```
                        ┌──────────────┐
        power on ──────►│     BOOT     │  panel_init()  · FULL, unavoidable
                        └──────┬───────┘
                               │ first frame
                               ▼
   ┌───────────────────────────────────────────────────────────┐
   │                          HOME                             │
   │  substates: IDLE · SYNCING · NO_CARD · LOW_POWER          │◄────────┐
   │             OFFLINE · NO_SYNC · NO_TIME                   │         │
   └───┬───────────┬───────────────┬───────────────────────────┘         │
       │BOOT tap   │BOOT hold      │PWR hold                             │
       ▼           ▼               ▼                                     │
 ┌──────────┐  ┌────────┐      ┌───────┐                                 │
 │RECORDING │  │  MENU  │      │  OFF  │  graceful power off (Home only)  │
 └────┬─────┘  └─┬──┬──┬┘      └───────┘                                 │
      │          │  │  └──────────────► PHONE ──► PWR/HOLD: back to MENU
 BOOT │          │  └───────────────► WIFI_SETUP ─────► (port) ─────────┤
 tap  │          │                                                        │
 stop │       PWR│ BOOT tap = next row, BOOT hold = back                  │
 keep │          ▼                                                        │
      │     ┌─────────┐   PWR(About)   ┌────────┐                        │
      │     │  ABOUT  │◄───────────────│  MENU  │                        │
      │     └────┬────┘                └────────┘                        │
      │          │ HOLD: back                                            │
      │          └──────────────────────────────────────────────────────►┤
      │                                                                   │
      │          PWR(Notes)                                               │
      │          ┌──────────┐  BOOT tap = next note (wraps, rotates)      │
      └─────────►│   LIST   │◄──────────────────────────┐                 │
                 └────┬─────┘                           │                 │
            PWR(open) │  BOOT hold = back to MENU      │                 │
                      ▼                                 │                 │
                 ┌──────────┐  BOOT tap = next action   │                 │
                 │   NOTE   │                          │                 │
                 └─┬──┬──┬──┘                          │                 │
        PWR(Play)  │  │  │  PWR(Back)                  │                 │
                   │  │  └─────────────────────────────┘                 │
                   │  │  PWR(Delete)                                     │
                   │  ▼                                                   │
                   │ ┌──────────┐  HOLD = delete ──────────────────────► │
                   │ │ CONFIRM  │  TAP or PWR = keep ───────────────────► │
                   │ └──────────┘                                        │
                   ▼                                                     │
              ┌──────────┐  BOOT tap/hold = stop, end of audio           │
              │ PLAYING  │──────────────────────────────────────────────┘
              └──────────┘  PWR tap = stop, back to LIST ──► LIST

  PAIRING is an overlay: it can arrive from any screen, owns both keys, and
  returns to State::after_pairing when answered.
```

### 13.2 Every transition, spelled out

`F` = call `display->request_full_refresh()` before the flush. `P` = let the
driver's partial path handle it. `—` = no flush at all.

`P` is the class of every hand-over (§15.2): the driver's diff band means the
panel is only driven over the rows either page inked. The one exception in a run
of hand-overs is the machine's ghost budget - **the fourth hand-over in a row is
`F`** - so a walk that deep sees one flash, and anything else that draws `F` in
between (a hero change, the sleeping face, `epd full`) starts the count again.
The rows that stay `F` here are the ones the panel has to be certain of: boot,
the sleeping face, the busy face of a wipe or a format, the recording face, the
player, the pairing ask, and the pages a port owns.

| # | From | Event | `ui_keys` result | App does | To | Refresh |
|---|---|---|---|---|---|---|
| 1 | — | power on | — | `EpdDisplay::begin()`, `panel_init()`, mount SD, draw Home | `HOME` | `F` (unavoidable) |
| 2 | `HOME` | BOOT tap | `StartRecording` | open a new `pending/<id>/`, start `port_codec` capture | `RECORDING` | `F` |
| 3 | `HOME` | BOOT hold | `OpenMenu` | reset `state.menu = 0` | `MENU` | | `P` |
| 4a | `HOME` | PWR tap | `None` | nothing at all — a tap is never a power-off | `HOME` | — |
| 4b | `HOME` | PWR hold | `PowerOff` | draw the sleeping face and wait for the full waveform (§12.2), then flush the card, close the radio, drop the latch | `OFF` | `F` (the face) |
| 5 | `HOME` | *(auto)* pending > 0 and a link is up | — | connect, `notes::Queue::plan_upload()`, upload | `HOME`/SYNCING | `P` unless the hero word changes (`F`) |
| 6 | `HOME` | *(auto)* upload batch fails | — | record the failure, no retry storm | `HOME`/NO_SYNC | `F` (hero changes) |
| 7 | `RECORDING` | BOOT tap | `StopKeep` | stop capture, `write_meta()` (the commit marker), enqueue | `HOME` | `F` |
| 8 | `RECORDING` | BOOT hold | `Discard` | stop capture, `Store::discard(pending, id)` | `HOME` | `F` |
| 9 | `RECORDING` | PWR tap | `None` | nothing at all; the capture is BOOT's to stop or discard | `RECORDING` | — |
| 10 | `RECORDING` | *(auto)* `note_max_s` reached | `StopKeep` | same as 7, then draw a one-line "cap reached" note | `HOME` | `F` |
| 11 | `MENU` | BOOT tap | `MenuNext` | — | `MENU` | `P` (bar) |
| 12 | `MENU` | PWR tap, row `Notes` | `MenuSelect` | read the store's three folders once | `LIST` | | `P` |
| 13 | `MENU` | PWR tap, row `Sync now` | `MenuSelect` | connect, upload every pending note | `HOME`/SYNCING | `F` |
| 14 | `MENU` | PWR tap, row `Phone` | `MenuSelect` | `state.go(Screen::Phone)`, `phone_ = PhoneStatus{}`, `Effect::OpenPhoneWindow`; the port's answer becomes the frame | `PHONE` | `F` |
| 15 | `MENU` | PWR tap, row `Wi-Fi` | `MenuSelect` | `port_wifi_setup` | `WIFI_SETUP` | `F` |
| 16 | `MENU` | PWR tap, row `About` | `MenuSelect` | gather 5 values | `ABOUT` | | `P` |
| 17 | `MENU` | PWR tap, row `Back` | `MenuSelect` | — | `HOME` | | `P` |
| 18 | `MENU` | BOOT hold | `Back` | — | `HOME` | | `P` |
| 19 | `LIST` | BOOT tap | `NextNote` | rotate the window if `note ≥ 6` | `LIST` | `P`; `F` when it rotates |
| 20 | `LIST` | PWR tap | `OpenNote` (`None` at zero notes) | read the one `note.json` | `NOTE` | | `P` |
| 21 | `LIST` | BOOT hold | `Back` | — | `MENU` | | `P` |
| 22 | `NOTE` | BOOT tap | `NextNoteAction` | — | `NOTE` | `P` (bar) |
| 23 | `NOTE` | PWR tap, row `Play` | `RunNoteAction` → `Play` | open the WAV, start playback | `PLAYING` | `F` |
| 24 | `NOTE` | PWR tap, row `Delete` | `RunNoteAction` → `AskDelete` | — | `CONFIRM` | | `P` |
| 25 | `NOTE` | PWR tap, row `Back` | `RunNoteAction` → `Back` | — | `LIST` | | `P` |
| 26 | `NOTE` | BOOT hold | `Back` | — | `LIST` | | `P` |
| 27 | `CONFIRM` | BOOT hold | `DeleteNote` | `Store::erase(id)`, drop it from the cached list | `LIST` | | `P` |
| 28 | `CONFIRM` | BOOT tap | `KeepNote` | — | `LIST` | | `P` |
| 29 | `CONFIRM` | PWR tap | `KeepNote` | — | `LIST` | | `P` |
| 30 | `PLAYING` | BOOT tap | `StopPlay` | stop the codec, close the file | `NOTE` | `F` |
| 31 | `PLAYING` | BOOT hold | `StopPlay` | same as 30 | `NOTE` | `F` |
| 32 | `PLAYING` | PWR tap | `StopPlayToList` | stop the codec, close the file, back where the player was opened | `LIST` | `F` |
| 33 | `PLAYING` | *(auto)* end of audio | — | close the file | `NOTE` | `F` |
| 34 | *any* | a phone asks to pair | `show_pairing_ask()` | remember `previous` | `PAIRING` | `F` |
| 35 | `PAIRING` | BOOT tap | `AllowPhone` | `port_ble` accepts, then `answer_pairing()` | `after_pairing` | `F` |
| 36 | `PAIRING` | PWR tap | `RefusePhone` | reject, then `answer_pairing()` | `after_pairing` | `F` |
| 37 | `PAIRING` | BOOT hold | `None` | nothing at all — no redraw, no state change | `PAIRING` | `—` |
| 38 | `ABOUT` | BOOT hold | `Back` | — | `MENU` | | `P` |
| 39 | `ABOUT` | PWR tap | `None` | nothing at all | `ABOUT` | — |
| 40 | `ABOUT` | BOOT tap | `None` | nothing | `ABOUT` | `—` |
| 41 | `WIFI_SETUP` | the port returns | — | — | `MENU` | `F` |
| 42 | *any* | a display transfer fails (`take_redraw_request()`) | — | rebuild the view model and redraw | same | `F` (driver already forced it) |
| 43 | `MENU` | PWR tap, row `Unpair phones` | `MenuSelect` | read the bond store once | `UNPAIR` | | `P` |
| 44 | `UNPAIR` | BOOT tap | `NextPeer` | — | `UNPAIR` | `P` (bar) |
| 45 | `UNPAIR` | PWR tap | `OpenUnpairConfirm` (`None` on an empty table) | — | `CONFIRM_UNPAIR` | | `P` |
| 46 | `UNPAIR` | BOOT hold | `Back` | — | `MENU` | | `P` |
| 47 | `CONFIRM_UNPAIR` | BOOT hold | `UnpairPeer` | `ble_store_util_delete_peer(addr)`, read the table, republish | `UNPAIR` | | `P` |
| 48 | `CONFIRM_UNPAIR` | BOOT tap / PWR tap | `Back` | nothing outside the glass | `UNPAIR` | | `P` |
| 49 | `CONFIRM_UNPAIR` | BOOT hold while `mode_ != Idle` | `None` | nothing at all; the reason is already on the glass | `CONFIRM_UNPAIR` | `—` |
| 50 | `PHONE` | PWR tap | `ClosePhone` | `port_ble` closes the window (`Effect::ClosePhoneWindow`) | `MENU` | `F` |
| 51 | `PHONE` | BOOT hold | `ClosePhone` | same as 50: out the way the row came in | `MENU` | `F` |
| 52 | `PHONE` | BOOT tap | `None` | nothing at all; the window is PWR's to close | `PHONE` | `—` |
| 53 | `PHONE` | *(auto)* `set_phone()` gets the port's answer | — | phase/refusal or bond count moved: `F`; countdown crossed 5 s: `P`; nothing moved: no draw | `PHONE` | `F` / `P` / `—` |

**Rows 4a, 9, 37, 39, 40 and 52 are the gestures in the whole UI that produce no
panel activity at all.** A PWR hold outside Home is the nearest thing to one: the
screens answer it with nothing, and the only frame it draws is the sleeping face the
loop puts up on its way to the shutdown (§12.2) — never a screen change. They
matter: a hold at a pairing prompt is a common panic response, and a tap on Home,
About or Phone must not look like it did something — least of all switch the device
off.

### 13.3 Upload progress is not a screen

There is no `Screen::Uploading`. Upload state is a **Home substate**, because the
upload is a background consequence of recording rather than a place the user goes:

| Phase | Hero | Band | Sub | Bar |
|---|---|---|---|---|
| queued, not started | `READY` or `OFFLINE` | `%u TO SEND` | `LAST …` | — |
| in flight | `UPLOADING` | `%u TO SEND` | `%u OF %u SENT` | yes, determinate |
| all done | `READY` | `ALL SENT` | `LAST …` | — |
| batch failed | `NO SYNC` | `%u TO SENT` | `LAST …` | — |

Two consequences worth stating because they are easy to get wrong:

- **`Sync now` returns to Home, not to the menu.** The menu has no room for
  progress and Home is where the result belongs; after a sync the user is done
  thinking about the menu.
- **The progress bar is determinate and growth-only.** `i of n` is known, so there
  is no indeterminate animation — which on this panel would be a flicker, not
  feedback (§15.4).

### 13.4 Errors are Home faces, not dialogs

There is no modal error. Every fault is one of the seven Home faces in §4.2, which
means an error never hides the queue count, never blocks a key, and never needs to
be dismissed. A notes device fails by *not recording*, so a blocking dialog that
requires a press to clear would be the worst possible design: it would consume the
one gesture the user wanted to record with.

---

## 14. UI strings

**These replace the i18n table.** There is no locale layer, no `i18n.cpp`, no
table of translated pairs. The whole surface is the **104 strings** below: 92
`constexpr const char*` in `strings.hpp`, and twelve that already exist in
`ui_keys.cpp` (`menu_label()`'s nine row labels, `action_label()`'s three) — those
twelve must **not** be duplicated in the app.

Every width below was measured with `Canvas::text_width()` against the slot in
§3–§12 that holds it, at the stated scale. `cols` is display columns; at scale 2
one column is 12 px, at scale 3 it is 18 px. A `%u` row is measured at the widest
value the screen can put in it (a three-digit count reads `999`; a bond count stops
at the pairing table's three). A row that sits at the edge of its slot — the widest
string that slot can hold, or one past it — is marked **←**.

| Key | English | Scale | Cols | px | Slot | Screen | Source |
|---|---|---:|---:|---:|---:|---|---|
| `home.hero.ready` | `READY` | 3 | 5 | 87 | 160 | Home | new |
| `home.hero.uploading` | `UPLOADING` | 3 | 9 | 159 | 160 **←** | Home | new |
| `home.hero.offline` | `OFFLINE` | 3 | 7 | 123 | 160 | Home | new |
| `home.hero.no_card` | `NO CARD` | 3 | 7 | 123 | 160 | Home | new |
| `home.hero.low_power` | `LOW POWER` | 3 | 9 | 159 | 160 **←** | Home | new |
| `home.hero.no_sync` | `NO SYNC` | 3 | 7 | 123 | 160 | Home | new |
| `home.band.to_send` | `%u TO SEND` | 2 | 10 | 118 | 118 **←** | Home | new |
| `home.band.all_sent` | `ALL SENT` | 2 | 8 | 94 | 118 | Home | new |
| `home.band.no_time` | `NO TIME` | 1 | 7 | 42 | 58 | Home | new |
| `home.sub.of_n` | `%u OF %u SENT` | 2 | 11 | 130 | 168 | Home | new |
| `home.sub.last` | `LAST %02u:%02u` | 2 | 10 | 118 | 168 | Home | new |
| `home.sub.boot_to_record` | `BOOT TO RECORD` | 2 | 14 | 166 | 168 **←** | Home | new |
| `home.sub.nothing_lost` | `NOTHING LOST` | 2 | 12 | 142 | 168 | Home | new |
| `home.sub.plug_in` | `PLUG IN SOON` | 2 | 12 | 142 | 168 | Home | new |
| `home.sub.no_audio` | `NO AUDIO SAVED` | 2 | 14 | 166 | 168 **←** | Home | new |
| `home.label.recording` | `RECORDING` | 2 | 9 | 106 | 106 **←** | Recording | new |
| `recording.hero.queued` | `QUEUED` | 3 | 6 | 105 | 176 | Recording | new |
| `recording.sub.waiting` | `FOR THE UPLOAD` | 2 | 14 | 166 | 176 | Recording | new |
| `fmt.of_cap` | `OF %u:%02u`, `OF %u:%02u:%02u` past an hour, `OF %u:%02u·%s` with the size | 2 | 7..13 | 82..154 | 176 | Recording, Playing | new |
| `fmt.clock` | `%u:%02u` | 3 | 8 | 141 | 176 | Recording, Playing | new |
| `key.boot.record` | `BOOT: record` | 2 | 12 | 142 | 166 | Home | new |
| `key.boot.next` | `BOOT: next` | 2 | 10 | 118 | 166 | Menu, List, Note | new |
| `key.boot.stop` | `BOOT: stop` | 2 | 10 | 118 | 166 | Recording, Playing | new |
| `key.pwr.select` | `PWR: select` | 2 | 11 | 130 | 166 | Menu, Note | new |
| `key.pwr.open` | `PWR: open` | 2 | 9 | 106 | 166 | List | new |
| `key.boot.allow` | `BOOT: allow` | 2 | 11 | 130 | 154 | Pairing | new |
| `key.pwr.refuse` | `PWR: refuse` | 2 | 11 | 130 | 154 | Pairing | new |
| `key.hold.discard` | `HOLD: discard` | 2 | 13 | 154 | 166 | Recording | new |
| `key.hold.delete` | `HOLD: delete` | 2 | 12 | 142 | 154 | Confirm | new |
| `key.hold.back` | `HOLD: back` | 2 | 10 | 118 | 166 | About | new |
| `key.hold.nothing` | `HOLD: nothing` | 2 | 13 | 154 | 166 | Pairing | new |
| `key.tap.keep` | `TAP: keep` | 2 | 9 | 106 | 118 | Confirm | new |
| `key.hold.wipe` | `HOLD: wipe` | 2 | 10 | 118 | 176 | ConfirmWipe | new |
| `key.hold.format` | `HOLD: format` | 2 | 12 | 142 | 176 | ConfirmFormat | new |
| `key.please_wait` | `PLEASE WAIT` | 2 | 11 | 130 | 166 | Wiping | new |
| `key.boot.cancel` | `BOOT: cancel` | 2 | 12 | 142 | 166 | Recording | new |
| `key.hold.cancel` | `HOLD: cancel` | 2 | 12 | 142 | 166 | Recording | new |
| `key.pwr.close` | `PWR: close` | 2 | 10 | 118 | 166 | Phone | new |
| `key.pwr.unpair` | `PWR: unpair` | 2 | 11 | 130 | 166 | Unpair | new |
| `key.hold.menu_hint` | `hold BOOT for menu` | 1 | 18 | 107 | 176 | Home | new |
| `menu.title` | `MENU` | 2 | 4 | 46 | 58 | Menu | new |
| `menu.notes` | `Notes` | 1 | 5 | 29 | 106 | Menu | ui_keys.cpp |
| `menu.sync_now` | `Sync now` | 1 | 8 | 47 | 106 | Menu | ui_keys.cpp |
| `menu.phone` | `Phone` | 1 | 5 | 29 | 106 | Menu | ui_keys.cpp |
| `menu.wifi` | `Wi-Fi` | 1 | 5 | 29 | 106 | Menu | ui_keys.cpp |
| `menu.about` | `About` | 1 | 5 | 29 | 106 | Menu | ui_keys.cpp |
| `menu.wipe_notes` | `Wipe notes` | 1 | 10 | 59 | 106 | Menu | ui_keys.cpp |
| `menu.format_card` | `Format card` | 1 | 11 | 65 | 106 | Menu | ui_keys.cpp |
| `menu.unpair_phones` | `Unpair phones` | 1 | 13 | 77 | 106 | Menu | ui_keys.cpp |
| `menu.back` | `Back` | 1 | 4 | 23 | 106 | Menu | ui_keys.cpp |
| `menu.value.wifi_on` | `on` | 1 | 2 | 11 | 60 | Menu | new |
| `menu.value.wifi_off` | `off` | 1 | 3 | 17 | 60 | Menu | new |
| `list.title` | `NOTES` | 2 | 5 | 58 | 58 **←** | List | new |
| `list.key.to_send` | `* TO SEND` | 2 | 9 | 106 | 106 **←** | List | new |
| `list.key.failed` | `! FAILED` | 2 | 8 | 94 | 106 | List | new |
| `list.empty` | `NO NOTES YET` | 2 | 12 | 142 | 168 | List | new |
| `note.title` | `NOTE` | 2 | 4 | 46 | 58 | Note | new |
| `note.state.pending` | `to send` | 2 | 7 | 82 | 106 | Note | new |
| `note.state.sent` | `sent` | 2 | 4 | 46 | 106 | Note | new |
| `note.state.failed` | `failed` | 2 | 6 | 70 | 106 | Note | new |
| `note.action.play` | `Play` | 2 | 4 | 46 | 106 | Note | ui_keys.cpp |
| `note.action.delete` | `Delete` | 2 | 6 | 70 | 106 | Note | ui_keys.cpp |
| `note.action.back` | `Back` | 2 | 4 | 46 | 106 | Note | ui_keys.cpp |
| `confirm.hero` | `DELETE?` | 3 | 7 | 123 | 176 | Confirm | new |
| `confirm.wipe.hero` | `WIPE NOTES` | 3 | 10 | 177 | 176 **←** | ConfirmWipe | new |
| `confirm.wipe.count` | `%u NOTES GO` | 2 | 12 | 142 | 176 | ConfirmWipe | new |
| `confirm.wipe.keeps` | `THE CARD STAYS` | 2 | 14 | 166 | 176 | ConfirmWipe | new |
| `confirm.format.hero` | `FORMAT` | 3 | 6 | 105 | 176 | ConfirmFormat | new |
| `confirm.format.target` | `THE SD CARD` | 2 | 11 | 130 | 176 | ConfirmFormat | new |
| `confirm.format.loss` | `NOTES AND LOG GO` | 2 | 16 | 190 | 176 **←** | ConfirmFormat | new |
| `busy.wipe.hero` | `WIPING` | 3 | 6 | 105 | 176 | Wiping | new |
| `busy.format.hero` | `FORMATTING` | 3 | 10 | 177 | 176 **←** | Wiping | new |
| `busy.count` | `%u OF %u` | 2 | 10 | 118 | 176 | Wiping | new |
| `busy.reading` | `READING CARD` | 2 | 12 | 142 | 176 | Wiping | new |
| `busy.card_stays` | `THE CARD STAYS` | 2 | 14 | 166 | 176 | Wiping | new |
| `busy.format_target` | `THE SD CARD` | 2 | 11 | 130 | 176 | Wiping | new |
| `busy.dont_unplug` | `DO NOT UNPLUG` | 2 | 13 | 154 | 176 | Wiping | new |
| `pair.title` | `PAIRING` | 2 | 7 | 82 | 82 **←** | Pairing | new |
| `pair.same` | `Same as phone?` | 2 | 14 | 166 | 176 | Pairing | new |
| `unpair.title` | `UNPAIR` | 2 | 6 | 70 | 70 **←** | Unpair | new |
| `unpair.count` | `%u PAIRED` | 2 | 8 | 94 | 106 | Unpair | new |
| `unpair.empty` | `NOTHING PAIRED` | 2 | 14 | 166 | 176 | Unpair | new |
| `unpair.empty_hint` | `open PHONE to pair` | 1 | 18 | 107 | 176 | Unpair | new |
| `unpair.confirm_hero` | `UNPAIR?` | 3 | 7 | 123 | 176 | ConfirmUnpair | new |
| `unpair.hold` | `HOLD: unpair` | 2 | 12 | 142 | 176 | ConfirmUnpair | new |
| `phone.title` | `PHONE` | 2 | 5 | 58 | 176 | Phone | new |
| `phone.advertising` | `ADVERTISING` | 2 | 11 | 130 | 176 | Phone | new |
| `phone.waiting` | `WAITING` | 2 | 7 | 82 | 176 | Phone | new |
| `phone.connected` | `CONNECTED` | 2 | 9 | 106 | 176 | Phone | new |
| `phone.closed` | `WINDOW CLOSED` | 2 | 13 | 154 | 176 | Phone | new |
| `phone.table_full` | `PAIRING FULL` | 2 | 12 | 142 | 176 | Phone | new |
| `phone.radio_busy` | `RADIO BUSY` | 2 | 10 | 118 | 176 | Phone | new |
| `phone.ble_off` | `BLE OFF` | 2 | 7 | 82 | 176 | Phone | new |
| `phone.holding` | `Held open` | 2 | 9 | 106 | 176 | Phone | new |
| `phone.off` | `Radio off` | 2 | 9 | 106 | 176 | Phone | new |
| `phone.table` | `%u OF %u PAIRED` | 2 | 13 | 154 | 176 | Phone | new |
| `about.title` | `ABOUT` | 2 | 5 | 58 | 70 | About | new |
| `about.battery` | `Battery` | 2 | 7 | 82 | 106 | About | new |
| `about.firmware` | `Firmware` | 2 | 8 | 94 | 106 | About | new |
| `about.device` | `Device` | 2 | 6 | 70 | 106 | About | new |
| `about.card` | `Card free` | 2 | 9 | 106 | 106 **←** | About | new |
| `about.notes` | `Notes` | 2 | 5 | 58 | 106 | About | new |
| `sleep.zzz` | `zzz` | 3 | 3 | 51 | 51 | Sleep | new |
| `sleep.hint` | `asleep - hold PWR 2 s` | 1 | 21 | 125 | 200 | Sleep | new |

Three rows run past the 176 px column. `confirm.wipe.hero` and `busy.format.hero`
are ten columns at scale 3 — 177 px — which centred puts at x = 11, one pixel
outside the 12 px margin and well inside the 4 px ink box (§3.1); §8.4 accepts that
so a wipe's hero can be two words. `confirm.format.loss` is the one body string
over the fourteen-column budget: sixteen columns, 190 px, centred at x = 5, which
breaks the margin by 7 px a side where §3.1 lets only the selection bar break it.
`build_format_confirm()` copies it whole and the confirm draws it as it is, so this
is an open item (§17.2) rather than something the draw path fits.

The nine menu labels are scale 1 (§5.1) — the menu is the one screen whose labels
gave way to a smaller size when the rows went from six to nine, which is why their
`px` column is half what the same words cost on About.

### 14.1 The four formatting rules

1. **A string is never truncated with `…`.** The font has no U+2026 and draws `?`.
   If a value can exceed its slot, shorten the value at the source (`99 TO SEND`
   caps at 99) or use ASCII `..` via `hg::fit()`. On this UI nothing is expected to
   truncate, and §17's checker is how that stays true.
2. **Uppercase means state; sentence case means content.** Hero words, error words
   and key legends are uppercase. Menu rows, action rows, note states and About
   labels are sentence case. The mixed case is the signal for "this is a thing you
   can act on" versus "this is what the device is doing".
3. **No plurals machinery.** `%u TO SEND` is correct at 0, 1 and 40. Where a plural
   would be needed the string is phrased to avoid it (`ALL SENT`, not `0 NOTES`).
4. **The only punctuation is `:`, `.`, `·`, `!`, `?`, `*` and `-`.** All render.
   `-` appears only in the sleep face's line (`asleep - hold PWR 2 s`). `…`, em
   dashes, curly quotes and arrows do not — do not introduce them.

### 14.2 Where each string is sourced

| Group | Source |
|---|---|
| the nine menu row labels, `note.action.*` | `ui_keys.cpp` (`menu_label()`, `action_label()`) — already implemented, do not copy |
| `menu.title`, `menu.value.*`, `home.*`, `key.*`, `list.*`, `note.state.*`, `fmt.*`, `recording.*`, `pair.*`, `about.*`, `confirm.*` | new; one `strings.hpp` of `constexpr const char*` |
| `busy.*` | new, `strings.hpp`: §8.5's work behind the two confirms — the same count and the same warning, in the present tense |
| `unpair.*` | new, `strings.hpp`: §11.4 — the bonded table, its empty page, and the question one phone gets |
| `phone.*` | new, `strings.hpp`: the Bluetooth window's own face, `ui_render.cpp`'s `phone_face()` |
| `sleep.*` | new, `strings.hpp`: §12.2 — the two that belong to no screen, and the only two drawn outside `frame()` |
| `%u` values | `Store::count()`, `Store::pending_ids()`, the uploader's `i/n`, `Meta::duration_s`, `Store::wipe()`'s `(done, total)`, the bond table's count |
| `LAST %02u:%02u` | the newest note's `Meta::recorded_at`, parsed with `notes::parse_iso8601()`, rendered in local time |

No string is built at draw time with `std::string` concatenation in a loop. Format
into a fixed `char buf[24]` first and pass a `string_view`; the draw path runs on
the app task and must not allocate (§16).

---

## 15. E-paper timing

### 15.1 What the driver actually does

| | full | partial |
|---|---|---|
| LUT | `kLutFull`, `0xC7` | `kLutPartial`, `0xCF` |
| sequence | `panel_init()` → `0x24` + `0x26` write of the whole frame → `0x22 0xC7` → `0x20` → BUSY → `panel_partial_init()` | window (`0x44`/`0x45`) → `0x24` + `0x26` write of the whole frame → `0x22 0xCF` → `0x20` → BUSY |
| visible | a full inversion flash, ~1.4-2.1 s of drawing here | a settle with no flash, **589 ms measured** on the unit (200 rows or one, same) |
| payload | **5,000 bytes** (200 × 200 ÷ 8) | 5,000 bytes into each plane, ~4 ms on the wire at 10 MHz |
| chosen when | first frame, `force_full_`, `!partial_`, or `partials_ >= kFullEvery` (10) | otherwise |

**Three facts that decide this whole section:**

1. **A partial is the whole frame in one window, in the panel's own addressing.**
   `update_once()` compares the frame it is about to send with the one it sent
   last and sends nothing if no pixel moved, but what it sends is the whole frame:
   the window is written start-row 199 → end-row 0 with the cursor on row 199
   (`panel_init()`'s own form), `0x24` carries the new frame, `0x26` the frame on
   the glass, and `0x22 0xCF` drives one transition per pixel. Narrowing the
   window is not the saving it looks like, and on this unit it is not an option:
   the same trigger with the window expressed 0 → 199 paints nothing, nor does any
   band taller than half the page in that form (measured 2026-11-23), and the
   partial waveform's duration does not depend on how many rows it drives (589 ms
   for a 27-row bar move and for a whole page). `flush(y0, y1)` is a *different*
   narrowing: it limits the RGB565 → 1-bit conversion, which is 40,000 luma
   computations over the full panel.
2. **Updating is off the app task.** `EpdDisplay::task()` owns the BUSY wait; a
   refresh that outlasts the frame budget does not stall the key loop. The app must
   never wait for it (§16) — the sleeping face is the one exception, and it waits
   because the rail goes the moment it returns (§12.2).
3. **A partial cannot finish a black → white transition.** Each one leaves a
   little residue where ink goes back to paper, which is what `kFullEvery` bounds:
   ten partials, then one full refresh that clears the glass. The budget is a
   property of the panel, not of the UI, and it is why a page that keeps changing
   in place is safe to draw partially.

The **full-update duration on this unit measured 2,122 ms**, and the one place the
firmware has to know it is the sleeping face's wait (§12.2). The driver only
guarantees it will not wait longer than `kBusyTimeoutMs = 3000`, and
`docs/waveshare-epaper-154-v2.md` §12 keeps it as open item 1 for panels nobody has
timed. Treat a full refresh as a visible flash of about two seconds, and measure it
before tuning `kFullEvery`:

```
hg> screen            # dumps the 1-bit image, proves *what* is on the glass
hg> status            # duty cycle: awake ms, radio ms per hour
```

Time `panel_init()` + a full write with `esp_timer_get_time()` across ten
transitions and log the median. Until a panel is timed, treat every `F` in §13.2 as a
user-visible pause and keep them where a pause is expected: on a key press.

### 15.2 Hand-overs are partial; certainty is full

**Rule: a hand-over between the UI's own pages is a partial, and the machine
keeps a tighter ghost budget for them. Everything the panel has to be certain of
is full.**

A hand-over is one page replacing another as the person walks the device:
`HOME → MENU`, `MENU → LIST`, `LIST → NOTE`, and every step back out — plus the
questions the menu and the list ask on the way (`ABOUT`, `UNPAIR`, the two
confirms) and the note screen's `CONFIRM`. They are the moves in §13.2 that carry
`P`, and they are listed in `ui_app.cpp`'s `handover()`.

This used to be the one place that was always full. The reason was sound on the
old driver: a partial re-drove the whole page from what was already in the
panel's RAM, so two layouts that differ everywhere left the old one legible
through the new one, and `kFullEvery = 20` was far too lazy to hide it. The
driver works differently now (§15.1): it hands the panel both frames — the new one
in `0x24`, the one on the glass in `0x26` — and the partial waveform drives each
pixel from where it was to where it is. So where the new page is white, the ink
the old page left is driven back to white in the same pass, at the partial
waveform's 589 ms instead of the full refresh's 1.4-2.1 s, and with no inversion.

What a partial still cannot do is *finish* those black → white transitions, so a
run of hand-overs does leave residue, and the driver's ten-band cadence is not
the right bound for it: a hand-over leaves a whole layout's worth, not a bar's.
`ScreenApp::handover_frame()` therefore keeps its own budget,
`kHandoverFullEvery = 4`: **the fourth hand-over in a row is a full refresh.** A
menu session of any depth carries at most three pages of ghost and sees one
flash, on the fourth move rather than on every key. The count is reset by any
full frame, because that flash already cleaned the glass.

Still full, each for its own reason:

| Also full | Why |
|---|---|
| the first frame after power on | `panel_init()` is part of the full path; `NotesUi::render()` forces it on `first_frame_`, and the driver's `!partial_` agrees |
| the first frame after a wake that dropped `EPD_PWR` | the reset cleared the panel's RAM; a partial would have nothing to diff against |
| **any change to the Home hero string** | `READY → UPLOADING` changes three slots at once; a partial leaves the old word under the new |
| **a hero that moved while another page was up** | the word on the glass is Home's old one, so the hand-over that returns Home is full (`hero_stale_`), or the new word lands over the old |
| a list rotation (§6.4) | six text rows move at once |
| `take_redraw_request()` returns true | a transfer failed; the driver has already forced full, the app only has to redraw |
| the busy face of a wipe or a format | it stays up for 20-90 s, so one clean flash costs nothing next to the job |
| the frame that ends a wipe or a format | the card changed under it, counts and all |
| the recording face | the runtime starts and stops the capture, and a commit moves the counts under the frame |
| the player | the runtime opens and closes the codec on the way in and out |
| the pairing ask | an interrupt from the radio, drawn over whatever page the person was on |
| the Phone screen and Wi-Fi setup | the port owns that window or that panel (§12.1) |
| the sleeping face (§12.2) | it is drawn over the page it replaces, and a partial would leave that page ghosting through the moon |
| `epd full` | the console's clean-up: the driver forces the next update full on its own, whatever the screens asked |

Everything else — bar moves, counters, progress bars, a mark appearing — is
partial, and the driver's `kFullEvery` bounds the accumulation regardless.

### 15.3 Ghosting: where it comes from and how it is bounded

The ghost sources in this UI, with the mitigation that actually applies:

| Source | Mechanism | Mitigation |
|---|---|---|
| **the selection bar moving** | a 184 × 18 solid block (3,312 black px) must go white on the row it left. Black → white is the transition the partial waveform handles worst | **after 8 consecutive bar moves with no other change, force a full refresh.** 8 moves is 7 vacated rows; one flash per 8 presses is rare enough not to annoy and frequent enough that a list never looks dirty |
| **the elapsed-time digits** | glyphs change in place; the old strokes have to be erased | the region is repainted to `kPaper` first, and the counter ticks at 30 s (§10.3), so a handful of digit changes before the driver's own full |
| **progress bars** | **none** — the fill only ever grows | nothing to mitigate, *provided the bar never shrinks*. A bar that resets (e.g. on a retry) must force a full refresh |
| **a hand-over** | one page's ink has to go white where the next page is paper — the largest black → white delta in the UI | the machine's own budget: **every fourth hand-over in a row is a full refresh** (`kHandoverFullEvery`), which is one flash per deep menu session. The driver's `kFullEvery = 10` bounds the rest (§15.2) |

The general principle: **ghosting is a function of pixels that go from black to
white, so design transitions that only add ink.** That is why the bar grows rather
than sweeps, why the queue count changes value in place rather than the band
reflowing, and why a selected row is a solid block rather than an outline (residue
inside a black block is invisible; residue inside an outline is conspicuous).

Two things not to do, both of which make ghosting worse:

- **No dithered greys.** `convert()` hard-thresholds at luma 128, so a "50 % grey"
  becomes a checkerboard that shimmers differently on every refresh.
- **No animations.** A moving element at ≤ 3 updates/s (the `kMinGapMs` ceiling)
  is not an animation, it is a stutter, and each frame leaves residue for the next.

### 15.4 Where to avoid blocking the render loop

The render loop runs on the app task and is the only owner of the panel. Six rules,
each with the specific hazard it prevents:

| Rule | Hazard it prevents |
|---|---|
| **Never call `Store::list()`, `count()`, `read_meta()` or `audio_bytes()` from the draw function.** Snapshot the view model first. | An SD directory walk is tens to hundreds of ms. Drawn inline, it delays the key loop by that much per frame, and a 52-note card makes the list feel broken. |
| **Never wait on `BUSY`, never poll `partials_`, never sleep after `flush()`.** | The driver's task owns that wait. Waiting in the draw path converts a background refresh into a stalled key press. The sleeping face is the one exception (§12.2): it waits for its own full waveform because the rail goes the moment it returns, and there is no key loop left to stall. |
| **Never draw from the BLE task, the upload task, or the panel's own task.** | Two owners of one framebuffer is a torn frame at best. The uploader publishes `i/n` into an atomic; the app task reads it and draws. |
| **Do not render on a timer for its own sake.** Gate on a hash of the view model. | A blind 100 ms redraw tick costs 3 flushes/s forever, and `kMinGapMs 300` turns it into a constant flicker at the panel's maximum rate. |
| **Do not `vTaskDelay()` inside the draw path** (including the recording counter). | It blocks the key poll, so the stop gesture lands late and the note runs long. Drive the counter from a tick count compared against `millis()`. |
| **Keep the flush band honest.** `flush(y0, y1)` over the rows you touched. | It is the only cheap lever left: it narrows the RGB565 → 1-bit conversion, which is 40,000 luma computations for the full panel. |

The view-model snapshot is the load-bearing one. The shape:

```cpp
struct HomeView {           // built by the app task, from cached state only
  const char* hero;          // a string from strings.hpp, never formatted here
  char band[12];             // "3 TO SEND"
  char sub[16];              // "LAST 14:02"
  uint8_t  wifi  : 1;
  uint8_t  wifi_up : 1;
  int8_t   battery_pct;      // -1 unknown
  uint8_t  external : 1;
  uint8_t  bar_pct;          // 0 = no bar
};
```

The cache is refreshed on **events**, never on a tick: after a recording commits,
after a delete, after an upload batch, after a link change, after a card insert.
Between events the draw path touches no hardware at all.

---

## 16. Verification

You do not need a camera, and you do not need to trust this document.

**On the device**, `port_console` already answers:

```
hg> screen            # the 1-bit panel image as hex, rows MSB-first
                     # (4,000 characters per call; walk it with a slice)
hg> status            # awake ms and radio ms per hour
```

`screen` proves *what the glass is showing*, text and all. Diff two dumps across a
transition and you have the exact ghosting delta.

**On the host**, the renderer that produced every wireframe in this document
compiles the layout against the SDK's own `hg::Canvas` and `hg::font` and checks
each string against its slot:

```
$ make -C docs/notes-ui
$ docs/notes-ui/render out/
FAIL hero: "UPLOADING" is 176 px in a 160 px slot (9 cols)   <- never happens now
```

It prints the ink bounding box per state and fails if anything leaves the page
(x 4 … 196, y ≤ 186). Four defects in this spec were found that way before any of
it was written down: a progress bar overlapping a text line, three strings 2–50 px
over their column budget, a hero that needed 11 columns, and a PBM whose polarity
was inverted. The last one is worth repeating: **PBM `P4` encodes 1 as black, while
this panel's image encodes 1 as white.** Every renderer written for this board gets
that wrong once.

**Acceptance test for the spec itself** — a reviewer signs off if:

1. All nine screens are reachable with the keys in §13.2 and every row's refresh
   class is stated.
2. Every string in §14 fits the slot beside it, measured with `Canvas::text_width()`
   at the stated scale, except the three §14's note under the table names — the two
   heroes at 177 px and `NOTES AND LOG GO` at 190 px (§17.2 item 8).
3. No screen uses scale 4, and scale 1 appears only where §14 marks it: the menu's
   nine rows (§5.1), the unpair table, the sleep face's line, the unpair empty
   hint, and Home's menu hint.
4. Nothing is inked outside x 4 … 196 or below y 186.
5. Every screen change is `F`; no in-screen transition is `F` except the seven in
   §15.2.

---

## 17. Decisions, and what is left open

### 17.1 Decisions this document makes

| # | Decision | Why |
|---|---|---|
| 1 | Black ink on white paper; `convert()` untouched | inverted 1-px strokes bloom; a black field makes every partial artefact conspicuous |
| 2 | Scale 2 is the floor, scale 3 the only display size | 1.94 mm is the smallest that resolves at 25–30 cm; two sizes is already a lot |
| 3 | **No clock on the resting face; only `NO TIME` while it is unset** | a minute tick costs 60 whole-panel refreshes an hour against a 1 mA idle target; time appears where it is a timestamp, and the unset warning is an event, not a tick |
| 4 | Home is centred, everything else is left-aligned | the alignment *is* the mode signal: centred = nothing to navigate |
| 5 | One key legend per footer, two only where both keys are load-bearing | two side-by-side labels need 18 columns; the budget is 14 |
| 6 | The footer yields to a warning | an undiscovered key is cheaper than an unseen fault |
| 7 | A selected row is a solid bar that bleeds 4 px past the margin | the largest ink mass the panel can make; residue inside it is invisible |
| 8 | One line per list row; no duration, no state word | date + duration + state is 25 columns against a budget of 14 |
| 9 | Mark the exception (`*`, `!`), leave sent rows blank | a blank margin is a settled ledger; a mark on every row costs 12 px to say "nothing to do" |
| 10 | The mark key lives on the list's title line | it costs no vertical space and is never off-screen |
| 11 | Scrolling is a cursor that rotates the window | the bar never leaves the glass, so a wrap never looks like a dropped press |
| 12 | Delete confirm has no footer, no rows, no selection | it is a question, not a page you navigate; the body is the legend |
| 13 | No audio level meter while recording | at a 30 s update it would be a half-minute average of a 1-second signal |
| 14 | No splash screen on boot | a splash costs one extra full refresh before the first useful frame |
| 15 | Upload progress is a Home substate, not a screen | it is a consequence of recording, not a place you go |
| 16 | Errors are Home faces, never modals | a modal that needs a press to clear steals the gesture the user wanted to record with |
| 17 | About is a static snapshot | a live battery percentage would cost panel refreshes to say what the band already says |

### 17.2 Gaps and open items

| # | Item | Status |
|---|---|---|
| 1 | `Screen` has no `About` | **gap**; three-line fix in §11.2.1, plus one host case |
| 2 | Full-update duration on a panel nobody has timed | measured at **2,122 ms** here, from the sleeping face (§12.2); another unit needs the procedure in §15.1 |
| 3 | The bolt cannot distinguish charging from charged | hardware limit (`charging = -1`); labelled ON EXTERNAL POWER, never `CHARGING` |
| 4 | Two notes in the same minute are indistinguishable on the confirm | accepted, §8.2; mitigated by the hold and deterministic order |
| 5 | 8 consecutive bar moves before a forced full | a starting value, not a measurement. Tune against the real panel; the constant belongs next to `kFullEvery` |
| 6 | The `Wi-Fi` frame is owned by `port_wifi_setup` | out of scope by design (§12.1); only the `F` hand-off is specified. The `Phone` row is not a hand-off: it has its own page (§11.5), and `port_ble` owns only the radio behind it |
| 7 | `note_max_s` cap shown as `OF 5:00` | read it from the setting, do not hard-code the default; past 99:59 it reads `OF 24:00` |
| 8 | `confirm.format.loss` is 16 columns | **gap**: 190 px centred at x = 5, so the format confirm breaks the 12 px margin by 7 px a side where §3.1 lets only the selection bar break it (§14). Shorten the line, or give §3.1 the exception and the host test |
| 9 | The Phone page's device name is up to 15 columns | **gap**: 178 px centred at x = 11, one pixel past the 176 px column and the 12 px margin, when the configured name is that long. The default `Hermes Gadget` is 13 (154 px). Shorten the stored name, or clip it to 14 columns on this page (§11.5) |

---
---

## Appendix A · Wireframes

One character per **2 × 2 pixels** of the 200 × 200 panel, so a block is 100 × 100.
The ramp shows how much ink a cell holds, which is what keeps 1 px glyph strokes
visible at this scale:

| Char | Ink in the 2 × 2 cell |
|---|---|
| `#` | 4 px — solid |
| `*` | 3 px |
| `+` | 2 px |
| `.` | 1 px — a stroke edge |
| ` ` | 0 — paper |

So `#` is black ink and ` ` is white paper, exactly as the panel prints them.
Nothing here is drawn by hand: every block is the output of the renderer in
`docs/notes-ui/render.cpp` (§16), which compiles the layout against `hg::Canvas`
and `hg::font`. Native 200 × 200 one-char-per-pixel text for all 17 states is in
`docs/notes-ui/wireframes/`, with exact 1-bit PBMs and `@4x` PNGs beside them.

The nine screens the spec defines are open; the eight alternate states are collapsed.

<details open>
<summary><b>Home — READY (3 notes to send)</b> — status face, the resting state · <code>01-home-ready</code></summary>

```text




      #####       #####  ###         #### ##### #   # ###                       *+++++++++++*
         #          #   #   #       #     #     #   # #  #                      ++########  +
        #           #   #   #       #     #     ##  # #   #           +**+      ++########  ++
         #          #   #   #        ###  ####  # # # #   #          *    *     ++########  +#
          #         #   #   #           # #     #  ## #   #           ++++      ++########  +#
      #   #         #   #   #           # #     #   # #  #                      ++########  +
       ###          #    ###        ####  ##### #   # ###              ++       +.++++++++  +
                                                                       ..       +++++++++++++

      ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++





















                            ++++++   +++++++.  .++++   ++++.    +.    +.
                            ######   #######+  +####   ####+    #+    #+
                            #+    #+ #+       #+    #+ #+  +#   #+    #+
                            #+    #+ #+       #+    #+ #+  .++. #+    #+
                            #+    #+ #+       #+    #+ #+    #+ #+    #+
                            ######   ######   #######+ #+    #+  +# +#
                            #*+#*+   #*++++   #*++++#+ #+    #+  .++++
                            #+ #+    #+       #+    #+ #+    #+    #+
                            #+  +#   #+       #+    #+ #+  +#      #+
                            #+  .++. #*+++++. #+    #+ #*++++      #+
                            #+    #+ #######+ #+    #+ ####+       #+






                    ++     +##+  +###++####+        ++     ++        +##+  +##+
                    ++    ++  ++++      ++         +#+    +#+  +#+  ++  ++++  ++
                    ++    ++  ++++      ++          ++   ++++  +#+  ++ +#+    ++
                    ++    +####+ +##+   ++          ++  ++ ++       ++++++   ++
                    ++    ++  ++    ++  ++          ++  +####+ +#+  +#+ ++  ++
                    ++    ++  ++    ++  ++          ++     ++  +#+  ++  ++ ++
                    +####+++  +++###+   ++         +##+    ++        +##+ +####+






















      ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++




      ####   ###   ###  #####                                               #
      #   # #   # #   #   #    ##                                           #
      #   # #   # #   #   #    ##         # ##   ###   ###   ###  # ##   ## #
      ####  #   # #   #   #               ##  # #   # #     #   # ##  # #  ##
      #   # #   # #   #   #    ##         #     ##### #     #   # #     #   #
      #   # #   # #   #   #    ##         #     #     #   # #   # #     #   #
      ####   ###   ###    #               #      ###   ###   ###  #      ####







```

</details>

<details>
<summary><b>Home — READY (nothing recorded yet)</b> — the one state that needs a verb · <code>02-home-empty</code></summary>

```text




       ###  #     #            #### ##### #   # #####                           *+++++++++++*
      #   # #     #           #     #     #   #   #                             ++########  +
      #   # #     #           #     #     ##  #   #                   +**+      ++########  ++
      ##### #     #            ###  ####  # # #   #                  *    *     ++########  +#
      #   # #     #               # #     #  ##   #                   ++++      ++########  +#
      #   # #     #               # #     #   #   #                             ++########  +
      #   # ##### #####       ####  ##### #   #   #                    ++       +.++++++++  +
                                                                       ..       +++++++++++++

      ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++





















                            ++++++   +++++++.  .++++   ++++.    +.    +.
                            ######   #######+  +####   ####+    #+    #+
                            #+    #+ #+       #+    #+ #+  +#   #+    #+
                            #+    #+ #+       #+    #+ #+  .++. #+    #+
                            #+    #+ #+       #+    #+ #+    #+ #+    #+
                            ######   ######   #######+ #+    #+  +# +#
                            #*+#*+   #*++++   #*++++#+ #+    #+  .++++
                            #+ #+    #+       #+    #+ #+    #+    #+
                            #+  +#   #+       #+    #+ #+  +#      #+
                            #+  .++. #*+++++. #+    #+ #*++++      #+
                            #+    #+ #######+ #+    #+ ####+       #+






        +###+  +##+  +##+ +####+      +####+ +##+       +###+ +####+ +##+  +##+ +###+ +##+
        ++  ++++  ++++  ++  ++          ++  ++  ++      ++  ++++    ++  ++++  ++++  ++++ ++
        ++  ++++  ++++  ++  ++          ++  ++  ++      ++  ++++    ++    ++  ++++  ++++  ++
        +###+ ++  ++++  ++  ++          ++  ++  ++      +###+ +###+ ++    ++  +++###+ ++  ++
        ++  ++++  ++++  ++  ++          ++  ++  ++      ++++  ++    ++    ++  ++++++  ++  ++
        ++  ++++  ++++  ++  ++          ++  ++  ++      ++ ++ ++    ++  ++++  ++++ ++ ++ ++
        +###+  +##+  +##+   ++          ++   +##+       ++  +++####+ +##+  +##+ ++  +++##+






















      ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++




      ####   ###   ###  #####                                               #
      #   # #   # #   #   #    ##                                           #
      #   # #   # #   #   #    ##         # ##   ###   ###   ###  # ##   ## #
      ####  #   # #   #   #               ##  # #   # #     #   # ##  # #  ##
      #   # #   # #   #   #    ##         #     ##### #     #   # #     #   #
      #   # #   # #   #   #    ##         #     #     #   # #   # #     #   #
      ####   ###   ###    #               #      ###   ###   ###  #      ####







```

</details>

<details>
<summary><b>Home — OFFLINE</b> — queue present, link down · <code>03-home-offline</code></summary>

```text




      #####       #####  ###         #### ##### #   # ###           *           *+++++++++++*
         #          #   #   #       #     #     #   # #  #          **.         ++########  +
        #           #   #   #       #     #     ##  # #   #          *#+*+      ++########  ++
         #          #   #   #        ###  ####  # # # #   #          ++#. *     ++########  +#
          #         #   #   #           # #     #  ## #   #           +.#+      ++########  +#
      #   #         #   #   #           # #     #   # #  #              .#*     ++########  +
       ###          #    ###        ####  ##### #   # ###              ++.**    +.++++++++  +
                                                                       ..  +    +++++++++++++

      ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++





















                    .++++   +++++++. +++++++. +.        .++++   +.    +. +++++++.
                    +####   #######+ #######+ #+        +####   #+    #+ #######+
                   #+    #+ #+       #+       #+          #+    #+    #+ #+
                   #+    #+ #+       #+       #+          #+    #*+   #+ #+
                   #+    #+ #+       #+       #+          #+    ###   #+ #+
                   #+    #+ ######   ######   #+          #+    #+ #+ #+ ######
                   #+    #+ #*++++   #*++++   #+          #+    #+ +++#+ #*++++
                   #+    #+ #+       #+       #+          #+    #+  +##+ #+
                   #+    #+ #+       #+       #+          #+    #+    #+ #+
                   +++++++. #+       #+       #*+++++.  .+#*+   #+    #+ #*+++++.
                    +####   #+       #+       #######+  +####   #+    #+ #######+






                    ++     +##+  +###++####+        ++     ++        +##+  +##+
                    ++    ++  ++++      ++         +#+    +#+  +#+  ++  ++++  ++
                    ++    ++  ++++      ++          ++   ++++  +#+  ++ +#+    ++
                    ++    +####+ +##+   ++          ++  ++ ++       ++++++   ++
                    ++    ++  ++    ++  ++          ++  +####+ +#+  +#+ ++  ++
                    ++    ++  ++    ++  ++          ++     ++  +#+  ++  ++ ++
                    +####+++  +++###+   ++         +##+    ++        +##+ +####+






















      ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++




      ####   ###   ###  #####                                               #
      #   # #   # #   #   #    ##                                           #
      #   # #   # #   #   #    ##         # ##   ###   ###   ###  # ##   ## #
      ####  #   # #   #   #               ##  # #   # #     #   # ##  # #  ##
      #   # #   # #   #   #    ##         #     ##### #     #   # #     #   #
      #   # #   # #   #   #    ##         #     #     #   # #   # #     #   #
      ####   ###   ###    #               #      ###   ###   ###  #      ####







```

</details>

<details>
<summary><b>Home — UPLOADING</b> — upload in flight, determinate bar · <code>04-home-uploading</code></summary>

```text




      #####       #####  ###         #### ##### #   # ###                       *+++++++++++*
         #          #   #   #       #     #     #   # #  #                      ++########  +
        #           #   #   #       #     #     ##  # #   #           +**+      ++########  ++
         #          #   #   #        ###  ####  # # # #   #          *    *     ++########  +#
          #         #   #   #           # #     #  ## #   #           ++++      ++########  +#
      #   #         #   #   #           # #     #   # #  #                      ++########  +
       ###          #    ###        ####  ##### #   # ###              ++       +.++++++++  +
                                                                       ..       +++++++++++++

      ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++





















          +.    +. ++++++   +.        .++++    .++++   ++++.     .++++   +.    +.  .++++
          #+    #+ ######   #+        +####    +####   ####+     +####   #+    #+  +####
          #+    #+ #+    #+ #+       #+    #+ #+    #+ #+  +#      #+    #+    #+ #+    #+
          #+    #+ #+    #+ #+       #+    #+ #+    #+ #+  .++.    #+    #*+   #+ #+    +.
          #+    #+ #+    #+ #+       #+    #+ #+    #+ #+    #+    #+    ###   #+ #+
          #+    #+ ######   #+       #+    #+ #######+ #+    #+    #+    #+ #+ #+ #+ ####+
          #+    #+ #*++++   #+       #+    #+ #*++++#+ #+    #+    #+    #+ +++#+ #+ +++#+
          #+    #+ #+       #+       #+    #+ #+    #+ #+    #+    #+    #+  +##+ #+    #+
          #+    #+ #+       #+       #+    #+ #+    #+ #+  +#      #+    #+    #+ #+    #+
          +++++++. #+       #*+++++. +++++++. #+    #+ #*++++    .+#*+   #+    #+ ++++++#+
           +####   #+       #######+  +####   #+    #+ ####+     +####   #+    #+  +#####+






                  +##+        +##+ +####+      +####+       +###++####+++  +++####+
                 ++  ++      ++  ++++             ++       ++    ++    ++  ++  ++
                     ++      ++  ++++            ++        ++    ++    +#+ ++  ++
                    ++       ++  +++###+          ++        +##+ +###+ ++++++  ++
                   ++        ++  ++++              ++          ++++    ++ +#+  ++
                  ++         ++  ++++          ++  ++          ++++    ++  ++  ++
                 +####+       +##+ ++           +##+       +###+ +####+++  ++  ++








      *++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++*
      +########################################################+                             +
      +########################################################+                             +
      +++++++++++++++++++++++++++++++++++++++++++++++++++++++++.                             +
      ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++









      ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++




      ####   ###   ###  #####                                               #
      #   # #   # #   #   #    ##                                           #
      #   # #   # #   #   #    ##         # ##   ###   ###   ###  # ##   ## #
      ####  #   # #   #   #               ##  # #   # #     #   # ##  # #  ##
      #   # #   # #   #   #    ##         #     ##### #     #   # #     #   #
      #   # #   # #   #   #    ##         #     #     #   # #   # #     #   #
      ####   ###   ###    #               #      ###   ###   ###  #      ####







```

</details>

<details>
<summary><b>Home — NO CARD</b> — the fault the device cannot recover from alone · <code>05-home-nocard</code></summary>

```text




                                                                                *+++++++++++*
                                                                                ++########  +
                                                                      +**+      ++########  ++
                                                                     *    *     ++########  +#
                                                                      ++++      ++########  +#
                                                                                ++########  +
                                                                       ++       +.++++++++  +
                                                                       ..       +++++++++++++

      ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++





















                   +.    +.  .++++             .++++    .++++   ++++++   ++++.
                   #+    #+  +####             +####    +####   ######   ####+
                   #+    #+ #+    #+          #+    #+ #+    #+ #+    #+ #+  +#
                   #*+   #+ #+    #+          #+    +. #+    #+ #+    #+ #+  .++.
                   ###   #+ #+    #+          #+       #+    #+ #+    #+ #+    #+
                   #+ #+ #+ #+    #+          #+       #######+ ######   #+    #+
                   #+ +++#+ #+    #+          #+       #*++++#+ #*+#*+   #+    #+
                   #+  +##+ #+    #+          #+       #+    #+ #+ #+    #+    #+
                   #+    #+ #+    #+          #+    #+ #+    #+ #+  +#   #+  +#
                   #+    #+ +++++++.          +++++++. #+    #+ #+  .++. #*++++
                   #+    #+  +####             +####   #+    #+ #+    #+ ####+






              ++  ++ +##+ +####+++  ++ +##+ ++  ++ +##+       ++     +##+  +###++####+
              ++  ++++  ++  ++  ++  ++  ++  ++  ++++  ++      ++    ++  ++++      ++
              +#+ ++++  ++  ++  ++  ++  ++  +#+ ++++          ++    ++  ++++      ++
              ++++++++  ++  ++  +####+  ++  +++++++++##+      ++    ++  ++ +##+   ++
              ++ +#+++  ++  ++  ++  ++  ++  ++ +#+++  ++      ++    ++  ++    ++  ++
              ++  ++++  ++  ++  ++  ++  ++  ++  ++++  ++      ++    ++  ++    ++  ++
              ++  ++ +##+   ++  ++  ++ +##+ ++  ++ +###+      +####+ +##+ +###+   ++






















      ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++




      ####   ###   ###  #####                                               #
      #   # #   # #   #   #    ##                                           #
      #   # #   # #   #   #    ##         # ##   ###   ###   ###  # ##   ## #
      ####  #   # #   #   #               ##  # #   # #     #   # ##  # #  ##
      #   # #   # #   #   #    ##         #     ##### #     #   # #     #   #
      #   # #   # #   #   #    ##         #     #     #   # #   # #     #   #
      ####   ###   ###    #               #      ###   ###   ###  #      ####







```

</details>

<details>
<summary><b>Home — LOW POWER</b> — battery at 9 %, on battery · <code>06-home-lowpower</code></summary>

```text




      #####       #####  ###         #### ##### #   # ###                       *+++++++++++*
         #          #   #   #       #     #     #   # #  #                      ++          +
        #           #   #   #       #     #     ##  # #   #           +**+      ++          ++
         #          #   #   #        ###  ####  # # # #   #          *    *     ++          +#
          #         #   #   #           # #     #  ## #   #           ++++      ++          +#
      #   #         #   #   #           # #     #   # #  #                      ++          +
       ###          #    ###        ####  ##### #   # ###              ++       +.          +
                                                                       ..       +++++++++++++

      ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++





















          +.        .++++   +.    +.          ++++++    .++++   +.    +. +++++++. ++++++
          #+        +####   #+    #+          ######    +####   #+    #+ #######+ ######
          #+       #+    #+ #+    #+          #+    #+ #+    #+ #+    #+ #+       #+    #+
          #+       #+    #+ #+    #+          #+    #+ #+    #+ #+    #+ #+       #+    #+
          #+       #+    #+ #+    #+          #+    #+ #+    #+ #+    #+ #+       #+    #+
          #+       #+    #+ #+ #+ #+          ######   #+    #+ #+ #+ #+ ######   ######
          #+       #+    #+ #+ #+ #+          #*++++   #+    #+ #+ #+ #+ #*++++   #*+#*+
          #+       #+    #+ #+ #+ #+          #+       #+    #+ #+ #+ #+ #+       #+ #+
          #+       #+    #+ #+ #+ #+          #+       #+    #+ #+ #+ #+ #+       #+  +#
          #*+++++. +++++++. +++++++.          #+       +++++++. +++++++. #*+++++. #+  .++.
          #######+  +####    +# +#            #+        +####    +# +#   #######+ #+    #+






              +###+ ++    ++  ++ +##+        +##+ ++  ++       +###+ +##+  +##+ ++  ++
              ++  ++++    ++  ++++  ++        ++  ++  ++      ++    ++  ++++  ++++  ++
              ++  ++++    ++  ++++            ++  +#+ ++      ++    ++  ++++  +++#+ ++
              +###+ ++    ++  +++++##+        ++  ++++++       +##+ ++  ++++  ++++++++
              ++    ++    ++  ++++  ++        ++  ++ +#+          ++++  ++++  ++++ +#+
              ++    ++    ++  ++++  ++        ++  ++  ++          ++++  ++++  ++++  ++
              ++    +####+ +##+  +###+       +##+ ++  ++      +###+  +##+  +##+ ++  ++






















      ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++




      ####   ###   ###  #####                                               #
      #   # #   # #   #   #    ##                                           #
      #   # #   # #   #   #    ##         # ##   ###   ###   ###  # ##   ## #
      ####  #   # #   #   #               ##  # #   # #     #   # ##  # #  ##
      #   # #   # #   #   #    ##         #     ##### #     #   # #     #   #
      #   # #   # #   #   #    ##         #     #     #   # #   # #     #   #
      ####   ###   ###    #               #      ###   ###   ###  #      ####







```

</details>

<details>
<summary><b>Home — NO SYNC</b> — the last batch failed · <code>07-home-nosync</code></summary>

```text




      #####       #####  ###         #### ##### #   # ###                       *+++++++++++*
         #          #   #   #       #     #     #   # #  #                      ++########  +
        #           #   #   #       #     #     ##  # #   #           +**+      ++########  ++
         #          #   #   #        ###  ####  # # # #   #          *    *     ++########  +#
          #         #   #   #           # #     #  ## #   #           ++++      ++########  +#
      #   #         #   #   #           # #     #   # #  #                      ++########  +
       ###          #    ###        ####  ##### #   # ###              ++       +.++++++++  +
                                                                       ..       +++++++++++++

      ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++





















                   +.    +.  .++++             .+++++. +.    +. +.    +.  .++++
                   #+    #+  +####             +#####+ #+    #+ #+    #+  +####
                   #+    #+ #+    #+          #+       #+    #+ #+    #+ #+    #+
                   #*+   #+ #+    #+          #+       #+    #+ #*+   #+ #+    +.
                   ###   #+ #+    #+          #+       #+    #+ ###   #+ #+
                   #+ #+ #+ #+    #+           +####    +# +#   #+ #+ #+ #+
                   #+ +++#+ #+    #+           .+++++.  .++++   #+ +++#+ #+
                   #+  +##+ #+    #+                #+    #+    #+  +##+ #+
                   #+    #+ #+    #+                #+    #+    #+    #+ #+    #+
                   #+    #+ +++++++.          +++++++.    #+    #+    #+ +++++++.
                   #+    #+  +####            ######      #+    #+    #+  +####






                    ++     +##+  +###++####+        ++     ++        +##+  +##+
                    ++    ++  ++++      ++         +#+    +#+  +#+  ++  ++++  ++
                    ++    ++  ++++      ++          ++   ++++  +#+  ++ +#+    ++
                    ++    +####+ +##+   ++          ++  ++ ++       ++++++   ++
                    ++    ++  ++    ++  ++          ++  +####+ +#+  +#+ ++  ++
                    ++    ++  ++    ++  ++          ++     ++  +#+  ++  ++ ++
                    +####+++  +++###+   ++         +##+    ++        +##+ +####+






















      ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++




      ####   ###   ###  #####                                               #
      #   # #   # #   #   #    ##                                           #
      #   # #   # #   #   #    ##         # ##   ###   ###   ###  # ##   ## #
      ####  #   # #   #   #               ##  # #   # #     #   # ##  # #  ##
      #   # #   # #   #   #    ##         #     ##### #     #   # #     #   #
      #   # #   # #   #   #    ##         #     #     #   # #   # #     #   #
      ####   ###   ###    #               #      ###   ###   ###  #      ####







```

</details>

<details open>
<summary><b>Menu</b> — selection on row 0, value column on the right · <code>08-menu</code></summary>

```text




      #   # ##### #   # #   #
      ## ## #     #   # #   #
      # # # #     ##  # #   #
      # # # ####  # # # #   #
      #   # #     #  ## #   #
      #   # #     #   # #   #
      #   # ##### #   #  ###


      ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

    ############################################################################################
    ## ### ######## ################################################################# ####   ###
    ## ### ######## ################################################################  ### ### ##
    ##  ## ##   ##   ####   ###    ################################################## ####### ##
    ## # # # ### ## #### ### # ###################################################### ###### ###
    ## ##  # ### ## ####     ##   ################################################### ##### ####
    ## ### # ### ## ## # ######### ################################################## #### #####
    ## ### ##   ####  ###   ##    ##################################################   ##     ##
    ############################################################################################

       ####                                                                              #####
      #                                                                                     #
      #     #   # # ##   ###        # ##   ###  #   #                                      #
       ###  #   # ##  # #           ##  # #   # #   #                                       #
          #  #### #   # #           #   # #   # # # #                                        #
          #     # #   # #   #       #   # #   # # # #                                    #   #
      ####   ###  #   #  ###        #   #  ###   # #                                      ###


      ####  #
      #   # #
      #   # # ##   ###  # ##   ###
      ####  ##  # #   # ##  # #   #
      #     #   # #   # #   # #####
      #     #   # #   # #   # #
      #     #   #  ###  #   #  ###


      #   #   #         #####   #
      #   #             #
      #   #  ##         #      ##                                                   ###  # ##
      # # #   #   ##### ####    #                                                  #   # ##  #
      # # #   #         #       #                                                  #   # #   #
      # # #   #         #       #                                                  #   # #   #
       # #   ###        #      ###                                                  ###  #   #


       ###  #                  #
      #   # #                  #
      #   # # ##   ###  #   # ###
      ##### ##  # #   # #   #  #
      #   # #   # #   # #   #  #
      #   # #   # #   # #  ##  #  #
      #   # ####   ###   ## #   ##


      ####              #
      #   #             #
      #   #  ###   ###  #  #
      ####      # #     # #
      #   #  #### #     ##
      #   # #   # #   # # #
      ####   ####  ###  #  #




      ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++



      ####   ###   ###  #####                                #
      #   # #   # #   #   #    ##                            #
      #   # #   # #   #   #    ##         # ##   ###  #   # ###
      ####  #   # #   #   #               ##  # #   #  # #   #
      #   # #   # #   #   #    ##         #   # #####   #    #
      #   # #   # #   #   #    ##         #   # #      # #   #  #
      ####   ###   ###    #               #   #  ###  #   #   ##


      ####  #   # ####                           ##                #
      #   # #   # #   #  ##                       #                #
      #   # #   # #   #  ##          ####  ###    #    ###   ###  ###
      ####  # # # ####              #     #   #   #   #   # #      #
      #     # # # # #    ##          ###  #####   #   ##### #      #
      #     # # # #  #   ##             # #       #   #     #   #  #  #
      #      # #  #   #             ####   ###   ###   ###   ###    ##








```

</details>

<details open>
<summary><b>Notes list</b> — 6 of 12 rows, two still to send · <code>09-list</code></summary>

```text




      #   #  ###  ##### #####  ####                  #####  ###         #### ##### #   # ###
      #   # #   #   #   #     #            #           #   #   #       #     #     #   # #  #
      ##  # #   #   #   #     #          # # #         #   #   #       #     #     ##  # #   #
      # # # #   #   #   ####   ###        ###          #   #   #        ###  ####  # # # #   #
      #  ## #   #   #   #         #      # # #         #   #   #           # #     #  ## #   #
      #   # #   #   #   #         #        #           #   #   #           # #     #   # #  #
      #   #  ###    #   ##### ####                     #    ###        ####  ##### #   # ###


      ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

    ############################################################################################
    ###   #### ######### ### ##################### ###### #########   ###   ####################
    ## ### ##  ######### ### ####################  #####  ###  ### ### # ### ############## ####
    ###### ### #########  ## ##   ## ### ######### #### # ###  ### ##  ##### ############ # # ##
    ##### #### ######### # # # ### # ### ######### ### ## ######## # # #### ##############   ###
    #### ##### ######### ##  # ### # ### ######### ###     ##  ###  ## ### ############## # # ##
    ### ###### ######### ### # ### ## # ########## ###### ###  ### ### ## ################# ####
    ##     ##   ######## ### ##   #### ##########   ##### #########   ##     ###################
    ############################################################################################

       ###   ###        #   #                    ###   ###          #     #
      #   # #   #       #   #                   #   # #   #  ##    ##    ##                #
          # #  ##       ##  #  ###  #   #       #  ## #   #  ##     #     #              # # #
         #  # # #       # # # #   # #   #       # # #  ####         #     #               ###
        #   ##  #       #  ## #   # #   #       ##  #     #  ##     #     #              # # #
       #    #   #       #   # #   #  # #        #   #    #   ##     #     #                #
      #####  ###        #   #  ###    #          ###   ##          ###   ###


       ###   ###        #   #                    ###  #####          #   ###
      #   # #   #       #   #                   #   #     #  ##     ##  #   #
          # #  ##       ##  #  ###  #   #       #  ##    #   ##    # #  #   #
         #  # # #       # # # #   # #   #       # # #   #         #  #   ###
        #   ##  #       #  ## #   # #   #       ##  #  #     ##   ##### #   #
       #    #   #       #   # #   #  # #        #   #  #     ##      #  #   #
      #####  ###        #   #  ###    #          ###   #             #   ###


        #    ###        #   #                     #    ###           #   ###
       ##   #   #       #   #                    ##   #   #  ##     ##  #   #
        #   #   #       ##  #  ###  #   #         #   #   #  ##    # #  #  ##
        #    ####       # # # #   # #   #         #    ###        #  #  # # #
        #       #       #  ## #   # #   #         #   #   #  ##   ##### ##  #
        #      #        #   # #   #  # #          #   #   #  ##      #  #   #
       ###   ##         #   #  ###    #          ###   ###           #   ###


        #    ###        #   #                    ###    #          ###  #####
       ##   #   #       #   #                   #   #  ##    ##   #   # #
        #   #   #       ##  #  ###  #   #           #   #    ##   #  ## ####
        #    ###        # # # #   # #   #          #    #         # # #     #
        #   #   #       #  ## #   # #   #         #     #    ##   ##  #     #
        #   #   #       #   # #   #  # #         #      #    ##   #   # #   #
       ###   ###        #   #  ###    #         #####  ###         ###   ###


        #    ###        #   #                     #     ##         ###   ###
       ##   #   #       #   #                    ##    #     ##   #   # #   #
        #   #   #       ##  #  ###  #   #         #   #      ##       #     #
        #    ###        # # # #   # #   #         #   ####           #     #
        #   #   #       #  ## #   # #   #         #   #   #  ##     #     #
        #   #   #       #   # #   #  # #          #   #   #  ##    #     #
       ###   ###        #   #  ###    #          ###   ###        ##### #####




      ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++



      ####   ###   ###  #####                                #
      #   # #   # #   #   #    ##                            #
      #   # #   # #   #   #    ##         # ##   ###  #   # ###
      ####  #   # #   #   #               ##  # #   #  # #   #
      #   # #   # #   #   #    ##         #   # #####   #    #
      #   # #   # #   #   #    ##         #   # #      # #   #  #
      ####   ###   ###    #               #   #  ###  #   #   ##


      ####  #   # ####
      #   # #   # #   #  ##
      #   # #   # #   #  ##          ###  ####   ###  # ##
      ####  # # # ####              #   # #   # #   # ##  #
      #     # # # # #    ##         #   # ####  ##### #   #
      #     # # # #  #   ##         #   # #     #     #   #
      #      # #  #   #              ###  #      ###  #   #








```

</details>

<details>
<summary><b>Notes list — empty</b> — no notes on the card · <code>10-list-empty</code></summary>

```text




      #   #  ###  ##### #####  ####                                                       ###
      #   # #   #   #   #     #                                                          #   #
      ##  # #   #   #   #     #                                                          #  ##
      # # # #   #   #   ####   ###                                                       # # #
      #  ## #   #   #   #         #                                                      ##  #
      #   # #   #   #   #         #                                                      #   #
      #   #  ###    #   ##### ####                                                        ###


      ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

























              ++  ++ +##+       ++  ++ +##+ +####++####+ +###+      ++  +++####++####+
              ++  ++++  ++      ++  ++++  ++  ++  ++    ++          ++  ++++      ++
              +#+ ++++  ++      +#+ ++++  ++  ++  ++    ++          ++  ++++      ++
              ++++++++  ++      ++++++++  ++  ++  +###+  +##+        ++++ +###+   ++
              ++ +#+++  ++      ++ +#+++  ++  ++  ++        ++        ++  ++      ++
              ++  ++++  ++      ++  ++++  ++  ++  ++        ++        ++  ++      ++
              ++  ++ +##+       ++  ++ +##+   ++  +####++###+         ++  +####+  ++




        +###+  +##+  +##+ +####+      +####+ +##+       +###+ +####+ +##+  +##+ +###+ +##+
        ++  ++++  ++++  ++  ++          ++  ++  ++      ++  ++++    ++  ++++  ++++  ++++ ++
        ++  ++++  ++++  ++  ++          ++  ++  ++      ++  ++++    ++    ++  ++++  ++++  ++
        +###+ ++  ++++  ++  ++          ++  ++  ++      +###+ +###+ ++    ++  +++###+ ++  ++
        ++  ++++  ++++  ++  ++          ++  ++  ++      ++++  ++    ++    ++  ++++++  ++  ++
        ++  ++++  ++++  ++  ++          ++  ++  ++      ++ ++ ++    ++  ++++  ++++ ++ ++ ++
        +###+  +##+  +##+   ++          ++   +##+       ++  +++####+ +##+  +##+ ++  +++##+
























      ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++




      ####   ###   ###  #####                                               #
      #   # #   # #   #   #    ##                                           #
      #   # #   # #   #   #    ##         # ##   ###   ###   ###  # ##   ## #
      ####  #   # #   #   #               ##  # #   # #     #   # ##  # #  ##
      #   # #   # #   #   #    ##         #     ##### #     #   # #     #   #
      #   # #   # #   #   #    ##         #     #     #   # #   # #     #   #
      ####   ###   ###    #               #      ###   ###   ###  #      ####







```

</details>

<details open>
<summary><b>Note — Play selected</b> — identity above, verbs below · <code>11-note-play</code></summary>

```text




      #   #  ###  ##### #####
      #   # #   #   #   #
      ##  # #   #   #   #
      # # # #   #   #   ####
      #  ## #   #   #   #
      #   # #   #   #   #
      #   #  ###    #   #####


      ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++



       +####      #+             #+    #+
      +++++++.  .+#+             #+    #+
      #+    #+  +##+             #+    #+
            #+    #+             ###   #+  +####   #+    #+
          .++.    #+             #*++. #+ +++++++. #+    #+
          +#      #+             #+ #+ #+ #+    #+ #+    #+
         #+       #+             #+  +##+ #+    #+ #+    #+
       .++.       #+             #+  .+#+ #+    #+ +++ .++.
       +#         #+             #+    #+ #+    #+  +# +#
      #######+  +####            #+    #+  +####      #+
      +++++++.  .++++            +.    +.  .++++      +.



        #      #         ###   ###                     ###          #      #
       ##     ##   ##   #   # #   #                   #   #  ##    ##     ##
        #    # #   ##   #  ##     #                   #  ##  ##     #    # #
        #   #  #        # # #    #          #         # # #         #   #  #
        #   #####  ##   ##  #   #                     ##  #  ##     #   #####
        #      #   ##   #   #  #                      #   #  ##     #      #
       ###     #         ###  #####                    ###         ###     #


       #                                      #
       #                                      #
      ###    ###         ####  ###  # ##   ## #
       #    #   #       #     #   # ##  # #  ##
       #    #   #        ###  ##### #   # #   #
       #  # #   #           # #     #   # #   #
        ##   ###        ####   ###  #   #  ####


      ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++


    ############################################################################################
    ##    ###  #################################################################################
    ## ### ### #################################################################################
    ## ### ### ####   ## ### ###################################################################
    ##    #### ####### # ### ###################################################################
    ## ####### ####    ##    ###################################################################
    ## ####### ### ### ##### ###################################################################
    ## ######   ###    ##   ####################################################################
    ############################################################################################

      ###          ##          #
      #  #          #          #
      #   #  ###    #    ###  ###    ###
      #   # #   #   #   #   #  #    #   #
      #   # #####   #   #####  #    #####
      #  #  #       #   #      #  # #
      ###    ###   ###   ###    ##   ###


      ####              #
      #   #             #
      #   #  ###   ###  #  #
      ####      # #     # #
      #   #  #### #     ##
      #   # #   # #   # # #
      ####   ####  ###  #  #



      ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++




      ####   ###   ###  #####                                #
      #   # #   # #   #   #    ##                            #
      #   # #   # #   #   #    ##         # ##   ###  #   # ###
      ####  #   # #   #   #               ##  # #   #  # #   #
      #   # #   # #   #   #    ##         #   # #####   #    #
      #   # #   # #   #   #    ##         #   # #      # #   #  #
      ####   ###   ###    #               #   #  ###  #   #   ##







```

</details>

<details>
<summary><b>Note — Delete selected</b> — the same screen, one row down · <code>12-note-delete</code></summary>

```text




      #   #  ###  ##### #####
      #   # #   #   #   #
      ##  # #   #   #   #
      # # # #   #   #   ####
      #  ## #   #   #   #
      #   # #   #   #   #
      #   #  ###    #   #####


      ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++



       +####      #+             #+    #+
      +++++++.  .+#+             #+    #+
      #+    #+  +##+             #+    #+
            #+    #+             ###   #+  +####   #+    #+
          .++.    #+             #*++. #+ +++++++. #+    #+
          +#      #+             #+ #+ #+ #+    #+ #+    #+
         #+       #+             #+  +##+ #+    #+ #+    #+
       .++.       #+             #+  .+#+ #+    #+ +++ .++.
       +#         #+             #+    #+ #+    #+  +# +#
      #######+  +####            #+    #+  +####      #+
      +++++++.  .++++            +.    +.  .++++      +.



        #      #         ###   ###                     ###          #      #
       ##     ##   ##   #   # #   #                   #   #  ##    ##     ##
        #    # #   ##   #  ##     #                   #  ##  ##     #    # #
        #   #  #        # # #    #          #         # # #         #   #  #
        #   #####  ##   ##  #   #                     ##  #  ##     #   #####
        #      #   ##   #   #  #                      #   #  ##     #      #
       ###     #         ###  #####                    ###         ###     #


       #                                      #
       #                                      #
      ###    ###         ####  ###  # ##   ## #
       #    #   #       #     #   # ##  # #  ##
       #    #   #        ###  ##### #   # #   #
       #  # #   #           # #     #   # #   #
        ##   ###        ####   ###  #   #  ####


      ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++



      ####   ##
      #   #   #
      #   #   #    ###  #   #
      ####    #       # #   #
      #       #    ####  ####
      #       #   #   #     #
      #      ###   ####  ###

    ############################################################################################
    ##   ##########  ########## ################################################################
    ## ## ########## ########## ################################################################
    ## ### ##   #### ####   ##   ####   ########################################################
    ## ### # ### ### ### ### ## #### ### #######################################################
    ## ### #     ### ###     ## ####     #######################################################
    ## ## ## ####### ### ###### ## # ###########################################################
    ##   ####   ###   ###   ####  ###   ########################################################
    ############################################################################################

      ####              #
      #   #             #
      #   #  ###   ###  #  #
      ####      # #     # #
      #   #  #### #     ##
      #   # #   # #   # # #
      ####   ####  ###  #  #



      ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++




      ####   ###   ###  #####                                #
      #   # #   # #   #   #    ##                            #
      #   # #   # #   #   #    ##         # ##   ###  #   # ###
      ####  #   # #   #   #               ##  # #   #  # #   #
      #   # #   # #   #   #    ##         #   # #####   #    #
      #   # #   # #   #   #    ##         #   # #      # #   #  #
      ####   ###   ###    #               #   #  ###  #   #   ##







```

</details>

<details open>
<summary><b>Confirm delete</b> — a question, not a page · <code>13-confirm</code></summary>

```text























                   ####+    #######+ #+       #######+ #######+ #######+  +####
                   #*++++   #*+++++. #+       #*+++++. +++#*++. #*+++++. +++++++.
                   #+  +#   #+       #+       #+          #+    #+       #+    #+
                   #+    #+ #+       #+       #+          #+    #+             #+
                   #+    #+ #*++++   #+       #*++++      #+    #*++++       .++.
                   #+    #+ ######   #+       ######      #+    ######       +#
                   #+    #+ #+       #+       #+          #+    #+          #+
                   #+  .++. #+       #+       #+          #+    #+          +.
                   #+  +#   #+       #+       #+          #+    #+
                   ####+    #######+ #######+ #######+    #+    #######+    #+
                   ++++.    +++++++. +++++++. +++++++.    +.    +++++++.    +.





               +##+   ++        ++  ++                    ++     ++        +##+  +##+
              ++  ++ +#+        ++  ++                   +#+    +#+  +#+  ++  ++++  ++
                  ++  ++        +#+ ++ +##+ ++  ++        ++   ++++  +#+  ++ +#+    ++
                 ++   ++        ++++++++  ++++  ++        ++  ++ ++       ++++++   ++
                ++    ++        ++ +#+++  ++++  ++        ++  +####+ +#+  +#+ ++  ++
               ++     ++        ++  ++++  ++ ++++         ++     ++  +#+  ++  ++ ++
              +####+ +##+       ++  ++ +##+   ++         +##+    ++        +##+ +####+






      ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++






              ++  ++ +##+ ++    +##+                  ++       +#+         ++
              ++  ++++  ++++    ++ ++  +#+            ++        ++         ++
              ++  ++++  ++++    ++  ++ +#+         +#+++ +##+   ++   +##+ +##+   +##+
              +####+++  ++++    ++  ++            ++ +#+++  ++  ++  ++  ++ ++   ++  ++
              ++  ++++  ++++    ++  ++ +#+        ++  +++####+  ++  +####+ ++   +####+
              ++  ++++  ++++    ++ ++  +#+        ++  ++++      ++  ++     ++ ++++
              ++  ++ +##+ +####++##+               +###+ +##+  +##+  +##+   +#+  +##+




                       +####+ +##+ +###+             ++
                         ++  ++  ++++  ++ +#+        ++
                         ++  ++  ++++  ++ +#+        ++ ++  +##+  +##+ +###+
                         ++  +####++###+             ++++  ++  ++++  ++++  ++
                         ++  ++  ++++     +#+        +#+   +####++####++###+
                         ++  ++  ++++     +#+        ++++  ++    ++    ++
                         ++  ++  ++++                ++ ++  +##+  +##+ ++























```

</details>

<details open>
<summary><b>Playing</b> — transport with a determinate position bar · <code>14-playing</code></summary>

```text




       ###        #####  ###         #### ##### #   # ###                       *+++++++++++*
      #   #         #   #   #       #     #     #   # #  #                      ++########  +
          #         #   #   #       #     #     ##  # #   #           +**+      ++########  ++
         #          #   #   #        ###  ####  # # # #   #          *    *     ++########  +#
        #           #   #   #           # #     #  ## #   #           ++++      ++########  +#
       #            #   #   #           # #     #   # #  #                      ++########  +
      #####         #    ###        ####  ##### #   # ###              ++       +.++++++++  +
                                                                       ..       +++++++++++++

      ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++






       ###    #         #   #                     #      #         ###   ###
      #   #  ##         #   #                    ##     ##   ##   #   # #   #
          #   #         ##  #  ###  #   #         #    # #   ##   #  ##     #
         #    #         # # # #   # #   #         #   #  #        # # #    #
        #     #         #  ## #   # #   #         #   #####  ##   ##  #   #
       #      #         #   # #   #  # #          #      #   ##   #   #  #
      #####  ###        #   #  ###    #          ###     #         ###  #####



                                  ####+             ####+     +##+
                                .+++++++   +++    .+++++++   ++++.
                                +#    +#   ###    +#    +#   #+
                                +#   ###   ###    +#   ### +#
                                +# .++*#   +++    +# .++*# +#++++.
                                +# +# +#          +# +# +# +#####+
                                +##+  +#   ###    +##+  +# +#    +#
                                +#+.  +#   ###    +#+.  +# +#    +#
                                +#    +#   ###    +#    +# +#    +#
                                  ####+             ####+    ####+
                                  ++++.             ++++.    ++++.





                              +##+ +####+       +##+         ++     ++
                             ++  ++++          ++  ++ +#+   +#+    +#+
                             ++  ++++          ++ +#+ +#+    ++   ++++
                             ++  +++###+       ++++++        ++  ++ ++
                             ++  ++++          +#+ ++ +#+    ++  +####+
                             ++  ++++          ++  ++ +#+    ++     ++
                              +##+ ++           +##+        +##+    ++













      *++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++*
      +####################################                                                  +
      +####################################                                                  +
      +++++++++++++++++++++++++++++++++++++                                                  +
      ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++










      ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++




      ####   ###   ###  #####                    #
      #   # #   # #   #   #    ##                #
      #   # #   # #   #   #    ##          #### ###    ###  ####
      ####  #   # #   #   #               #      #    #   # #   #
      #   # #   # #   #   #    ##          ###   #    #   # ####
      #   # #   # #   #   #    ##             #  #  # #   # #
      ####   ###   ###    #               ####    ##   ###  #







```

</details>

<details open>
<summary><b>Pairing</b> — the passkey, and both answers · <code>15-pairing</code></summary>

```text




      ####   ###   ###  ####   ###  #   #  ###
      #   # #   #   #   #   #   #   #   # #   #
      #   # #   #   #   #   #   #   ##  # #
      ####  #####   #   ####    #   # # # # ###
      #     #   #   #   # #     #   #  ## #   #
      #     #   #   #   #  #    #   #   # #   #
      #     #   #  ###  #   #  ###  #   #  ####


      ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++








                       +#      #+     +####             +####    +####   #######+
                      +*#    .+#+    +++++++.          +++++++. +++++++. ++++++#+
                      ###    +##+    #+    #+          #+    #+ #+    #+       #+
                    +# +#      #+    #+    #+                #+ #+  +##+     +#
                   +++ +#      #+    +++++++.              .++. #+ +++#+    +++
                   #+  +#      #+     +####                +#   #+ #+ #+    #+
                   #######+    #+    #+    #+             #+    ###   #+  +#
                   ++++*#+.    #+    #+    #+           .++.    #*+   #+  +#
                       +#      #+    #+    #+           +#      #+    #+  +#
                       +#    +####    +####            #######+  +####    +#
                       .+    .++++    .++++            +++++++.  .++++    .+










         +###+                                                ++                       +##+
        ++                                                    ++                      ++  ++
        ++     +##+ +#+++  +##+        +##+  +###+      +###+ +++#+  +##+ +++#+  +##+     ++
         +##+     ++++++++++  ++          ++++          ++  +++#+ ++++  +++#+ ++++  ++   ++
            ++ +###++++++++####+       +###+ +##+       +###+ ++  ++++  ++++  +++####+  ++
            ++++  ++++  ++++          ++  ++    ++      ++    ++  ++++  ++++  ++++
        +###+  +###+++  ++ +##+        +###++###+       ++    ++  ++ +##+ ++  ++ +##+   ++




                 +###+  +##+  +##+ +####+                   +#+   +#+
                 ++  ++++  ++++  ++  ++   +#+                ++    ++
                 ++  ++++  ++++  ++  ++   +#+         +##+   ++    ++   +##+ ++  ++
                 +###+ ++  ++++  ++  ++                  ++  ++    ++  ++  ++++  ++
                 ++  ++++  ++++  ++  ++   +#+         +###+  ++    ++  ++  ++++++++
                 ++  ++++  ++++  ++  ++   +#+        ++  ++  ++    ++  ++  ++++++++
                 +###+  +##+  +##+   ++               +###+ +##+  +##+  +##+  ++++




                 +###+ ++  +++###+                           +#+
                 ++  ++++  ++++  ++ +#+                     ++ ++
                 ++  ++++  ++++  ++ +#+        +++#+  +##+  ++   ++  ++ +###+ +##+
                 +###+ +++++++###+             +#+ ++++  +++##+  ++  ++++    ++  ++
                 ++    ++++++++++   +#+        ++    +####+ ++   ++  ++ +##+ +####+
                 ++    ++++++++ ++  +#+        ++    ++     ++   ++ +#+    ++++
                 ++     ++++ ++  ++            ++     +##+  ++    +#++++###+  +##+









      ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++




      #   #  ###  #     ###                            #    #       #
      #   # #   # #     #  #   ##                      #    #                  ####
      #   # #   # #     #   #  ##         # ##   ###  ###   # ##   ##   # ##  #   #
      ##### #   # #     #   #             ##  # #   #  #    ##  #   #   ##  # #   #
      #   # #   # #     #   #  ##         #   # #   #  #    #   #   #   #   #  ####
      #   # #   # #     #  #   ##         #   # #   #  #  # #   #   #   #   #     #
      #   #  ###  ##### ###               #   #  ###    ##  #   #  ###  #   #  ###







```

</details>

<details open>
<summary><b>Recording</b> — 42 s of a 300 s cap · <code>16-recording</code></summary>

```text




      #####       #####  ###         #### ##### #   # ###                       *+++++++++++*
         #          #   #   #       #     #     #   # #  #                      ++########  +
        #           #   #   #       #     #     ##  # #   #           +**+      ++########  ++
         #          #   #   #        ###  ####  # # # #   #          *    *     ++########  +#
          #         #   #   #           # #     #  ## #   #           ++++      ++########  +#
      #   #         #   #   #           # #     #   # #  #                      ++########  +
       ###          #    ###        ####  ##### #   # ###              ++       +.++++++++  +
                                                                       ..       +++++++++++++

      ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++





      ####  #####  ###   ###  ####  ###    ###  #   #  ###
      #   # #     #   # #   # #   # #  #    #   #   # #   #
      #   # #     #     #   # #   # #   #   #   ##  # #
      ####  ####  #     #   # ####  #   #   #   # # # # ###
      # #   #     #     #   # # #   #   #   #   #  ## #   #
      #  #  #     #   # #   # #  #  #  #    #   #   # #   #
      #   # #####  ###   ###  #   # ###    ###  #   #  ####





                                  ####+                #+    ####+
                                .+++++++   +++       .+#+  .+++++++
                                +#    +#   ###       +##+  +#    +#
                                +#   ###   ###      #+ #+        +#
                                +# .++*#   +++    .++. #+       +++
                                +# +# +#          +#   #+       #+
                                +##+  +#   ###    +#######    +#
                                +#+.  +#   ###    .++++#*+   +++
                                +#    +#   ###         #+    #+
                                  ####+                #+  +#######
                                  ++++.                +.  .+++++++





                              +##+ +####+      +####+       +##+  +##+
                             ++  ++++          ++     +#+  ++  ++++  ++
                             ++  ++++          +###+  +#+  ++ +#+++ +#+
                             ++  +++###+           ++      ++++++++++++
                             ++  ++++              ++ +#+  +#+ +++#+ ++
                             ++  ++++          ++  ++ +#+  ++  ++++  ++
                              +##+ ++           +##+        +##+  +##+





      *++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++*
      +############                                                                          +
      +############                                                                          +
      +++++++++++++                                                                          +
      ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++








      ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++



      ####   ###   ###  #####                    #
      #   # #   # #   #   #    ##                #
      #   # #   # #   #   #    ##          #### ###    ###  ####
      ####  #   # #   #   #               #      #    #   # #   #
      #   # #   # #   #   #    ##          ###   #    #   # ####
      #   # #   # #   #   #    ##             #  #  # #   # #
      ####   ###   ###    #               ####    ##   ###  #


      #   #  ###  #     ###                   #   #                               #
      #   # #   # #     #  #   ##             #                                   #
      #   # #   # #     #   #  ##          ## #  ##    ####  ###   ###  # ##   ## #
      ##### #   # #     #   #             #  ##   #   #     #         # ##  # #  ##
      #   # #   # #     #   #  ##         #   #   #    ###  #      #### #     #   #
      #   # #   # #     #  #   ##         #   #   #       # #   # #   # #     #   #
      #   #  ###  ##### ###                ####  ###  ####   ###   #### #      ####








```

</details>

<details open>
<summary><b>About</b> — the read-only reference page · <code>17-about</code></summary>

```text




       ###  ####   ###  #   # #####
      #   # #   # #   # #   #   #
      #   # #   # #   # #   #   #
      ##### ####  #   # #   #   #
      #   # #   # #   # #   #   #
      #   # #   # #   # #   #   #
      #   # ####   ###   ###    #


      ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++






      ####         #     #                                                    ###     #  ##
      #   #        #     #                                                   #   #   ##  ##  #
      #   #  ###  ###   ###    ###  # ##  #   #                              #   #  # #     #
      ####      #  #     #    #   # ##  # #   #                               ###  #  #    #
      #   #  ####  #     #    ##### #      ####                              #   # #####  #
      #   # #   #  #  #  #  # #     #         #                              #   #    #  #  ##
      ####   ####   ##    ##   ###  #      ###                                ###     #     ##




      #####   #                                                   ###         ###          #
      #                                                          #   #       #   #        ##
      #      ##   # ##  ## #  #   #  ###  # ##   ###                 #       #  ##         #
      ####    #   ##  # # # # #   #     # ##  # #   #               #        # # #         #
      #       #   #     # # # # # #  #### #     #####              #         ##  #         #
      #       #   #     #   # # # # #   # #     #                 #     ##   #   #  ##     #
      #      ###  #     #   #  # #   #### #      ###             #####  ##    ###   ##    ###




      ###                 #                          #                         #   #      ###
      #  #                                           #      ####              ##   #     #   #
      #   #  ###  #   #  ##    ###   ###             # ##  #   #        ###    #   # ##      #
      #   # #   # #   #   #   #     #   #            ##  # #   # #####     #   #   ##  #    #
      #   # ##### #   #   #   #     #####            #   #  ####        ####   #   #   #   #
      #  #  #      # #    #   #   # #                #   #     #       #   #   #   #   #  #
      ###    ###    #    ###   ###   ###             #   #  ###         ####  ###  ####  #####




       ###                  #         ##                           ##   ###         ###   ###
      #   #                 #        #  #                         #    #   #       #   # #   #
      #      ###  # ##   ## #        #    # ##   ###   ###       #         #       #   # #
      #         # ##  # #  ##       ###   ##  # #   # #   #      ####     #         #### # ###
      #      #### #     #   #        #    #     ##### #####      #   #   #             # #   #
      #   # #   # #     #   #        #    #     #     #          #   #  #     ##      #  #   #
       ###   #### #      ####        #    #      ###   ###        ###  #####  ##    ##    ####




      #   #        #                                                                 #    ###
      #   #        #                                                                ##   #   #
      ##  #  ###  ###    ###   ####                                                  #       #
      # # # #   #  #    #   # #                                                      #      #
      #  ## #   #  #    #####  ###                                                   #     #
      #   # #   #  #  # #         #                                                  #    #
      #   #  ###    ##   ###  ####                                                  ###  #####










      ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++




      #   #  ###  #     ###               #                 #
      #   # #   # #     #  #   ##         #                 #
      #   # #   # #     #   #  ##         # ##   ###   ###  #  #
      ##### #   # #     #   #             ##  #     # #     # #
      #   # #   # #     #   #  ##         #   #  #### #     ##
      #   # #   # #     #  #   ##         #   # #   # #   # # #
      #   #  ###  ##### ###               ####   ####  ###  #  #







```

</details>
