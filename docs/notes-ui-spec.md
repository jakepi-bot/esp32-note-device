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
| `Menu` | ✓ | the six rows of `MenuItem` |
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
| Partial refresh | **whole-panel waveform**, not a rectangle | `flush(y0,y1)` narrows *conversion* only, not transmission |
| BUSY wait | `kBusyTimeoutMs = 3000` | a full update is budgeted at "around a second", see §15.1 |
| Coalescing | `kQuietMs 80`, `kMaxCoalesceMs 250` | a frame's bands merge; do not reimplement |
| Rate limit | `kMinGapMs 300` | ~3 updates/s ceiling, enforced by the driver |
| Ghost reset | `kFullEvery = 20` | 20 partials then an automatic full |
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

**The rule: scale 2 is the floor, scale 3 is the only display size, scale 1 and 4
are not used.** Two sizes on a 200 px screen is already a lot; a third would read
as accident. Scale 2 is 1.94 mm, which at 25–30 cm viewing distance is
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
| A · facts | 12 … 188 | glyph top **y = 8** | band height 15 px, ink to y = 22 | queue state, the clock, Wi-Fi / Bluetooth, battery |
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
| clock | right of queue | text at scale 1 | `14:02`, blank until the clock has been set once |
| Wi-Fi / Bluetooth | 136 | 16 × 15 bitmap | Wi-Fi up (uncut) or down (2 px cut); the Bluetooth rune while the radio has a window open (thin = advertising, 2 px stem = a phone connected) |
| battery | 160 | 28 × 15 procedural | fill, bolt, or `?` |

**The clock is the one exception, and it is bounded.** The original rule was no
clock at all: a minute-accurate clock costs 60 panel updates an hour and each
partial refresh re-drives every pixel of the whole panel (§15.2). The device now
carries a clock anyway, because a notes device with no way to read the time is a
notes device you cannot date a note with. The cost is bounded by four rules: it
is drawn on **Home only** (a minute that turns over in a menu or on the
recording face is adopted into the snapshot and drawn by the next frame that
returns Home, not by a refresh of its own); it repaints **only when the HH:MM
text changes**, so the poll that reads the clock costs nothing; that poll runs
every **10 s**, not every second; and a battery that moves by one percent — the
ADC's own noise — is not a change. Measured on the board: two minutes on Home
produced exactly two refreshes, one per minute; two minutes in a menu produced
none from the poll.

**Bluetooth is drawn in the Wi-Fi glyph's box, never beside it.** The mark is the
radio's answer, the same three states `radio` prints (off / advertising /
connected), and the box is the reason the band does not shift when it appears.
The Wi-Fi glyph keeps the slot whenever the radio is off, which is every state
except the boot window, a note's window and the Phone screen's window.

This is the single most useful rule in the document: **nothing redraws unless
something changed.** The only timers in the whole UI are the recording counter,
the playback position and the minute of the clock, and the first two run only
while the user is holding a session open.

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
| 6 | `NO TIME` | the clock has never been set (a note's time cannot be trusted) |
| 7 | `READY` | no id. **`READY` is the fallback, never an empty face.** |

**Band queue count:**

| Condition | Text |
|---|---|
| card missing | *blank* (the hero already says it) |
| `pending > 0` | `%u TO SEND`, capped at 99 |
| `pending == 0` | `ALL SENT` |

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
| entering Home from any other screen | **FULL** | §15.2 |
| first frame after a wake that dropped `EPD_PWR` | **FULL** | the panel's RAM was reset |
| hero word changes (`READY` → `UPLOADING`) | PARTIAL | same geometry, different ink |
| band queue count / `ALL SENT` | PARTIAL | ≤ 10 columns, same slot |
| the line under the hero | PARTIAL | |
| battery fill | PARTIAL | fill is **growth-only in one direction per charge cycle**; see §15.3 |
| Wi-Fi icon up ↔ down | PARTIAL | 16 × 15 region |
| upload progress bar | PARTIAL | **growth-only, so it leaves no residue at all** |
| *the clock* | minutes *n* × 1, Home only | the HH:MM text changed; a minute that turns over elsewhere is drawn by the next Home frame |

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

The value column is what makes a six-row list worth 176 px of width. It is
right-aligned to x = 188 and always at scale 2, in the same `kInk`:

| Row | Value | Source |
|---|---|---|
| `Notes` | `Store::count(Pending) + count(Sent) + count(Failed)` | the card |
| `Sync now` | `Store::count(Pending)` — what it would upload | the card |
| `Phone` | *blank* | it opens a window; there is no state to show |
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
| entering Menu | **FULL** |
| bar moves (BOOT tap) | PARTIAL, bounded by §15.3 |
| a value changes (a note was sent) | PARTIAL |
| leaving Menu → any row's destination | **FULL** |

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
| entering the list (from Menu, from a note, after a delete) | **FULL** |
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
| entering from the list | **FULL** |
| bar moves | PARTIAL, bounded by §15.3 |
| returning from Playing or ConfirmDelete | **FULL** |

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
| entering from the note screen | **FULL** |
| **anything while it is open** | **nothing at all** |
| leaving (delete or keep) → the list | **FULL** |

This screen is **immobile by design**: the note identity and the two key verbs are
fixed for as long as the question is on the glass. It is the one place in the UI
where a partial refresh never happens, which also means it never ghosts.

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
| elapsed | centred | 60 | ≤ 88 × 21 | scale 3, `M:SS` |
| total | centred | 92 | ≤ 106 × 14 | `OF M:SS` |
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
| entering from the note screen | **FULL** |
| elapsed digits + bar | PARTIAL every 5 s |
| stop / end of audio → back to the note screen | **FULL** |

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
| elapsed | centred | 62 | ≤ 88 × 21 | scale 3, `M:SS` and `H:MM:SS` past an hour — the hero, the real length, never a clamp |
| cap | centred | 94 | ≤ 88 × 14 | `OF M:SS` and `OF H:MM` past an hour, plus the room behind it: `OF 1:30·2.2G`. The cap is the capture's own budget — the setting, the 2 GiB ceiling and the card's free space less its 256 MB reserve, whichever is smallest — not the setting alone |
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
| body 1 | centred | 86 | 154 × 14 | `Same as phone?` |
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
| About | entering / leaving | **FULL** |
| About | anything while open | nothing — the page is a snapshot at entry |

Both are static pages. About is deliberately **not live**: a battery percentage
that ticks while you watch it would cost a panel refresh to tell you something you
can already see in the band of every other screen.

---

## 12. Frames this spec does not own

Two menu rows hand the panel to code that already exists and is proven. They are
**out of scope**, they must not be re-implemented, and the notes app owns only the
hand-off:

| Row | Screen | Owner |
|---|---|---|
| `Wi-Fi` | network list and the phone-based setup AP | `port_wifi_setup` |
| `Phone` | the 30 s advertising window | `port_ble` (`radio_window`) |

The contract at the boundary: the app calls `display->request_full_refresh()`
before the hand-off and again on return, because both directions change the whole
page. Nothing else about those screens changes.


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
      │          │  │  └──────────────► PHONE_WINDOW ──► (port) ─────────┤
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

| # | From | Event | `ui_keys` result | App does | To | Refresh |
|---|---|---|---|---|---|---|
| 1 | — | power on | — | `EpdDisplay::begin()`, `panel_init()`, mount SD, draw Home | `HOME` | `F` (unavoidable) |
| 2 | `HOME` | BOOT tap | `StartRecording` | open a new `pending/<id>/`, start `port_codec` capture | `RECORDING` | `F` |
| 3 | `HOME` | BOOT hold | `OpenMenu` | reset `state.menu = 0` | `MENU` | `F` |
| 4a | `HOME` | PWR tap | `None` | nothing at all — a tap is never a power-off | `HOME` | — |
| 4b | `HOME` | PWR hold | `PowerOff` | flush the card, close the radio, `LatchingPower::power_off()` | `OFF` | — |
| 5 | `HOME` | *(auto)* pending > 0 and a link is up | — | connect, `notes::Queue::plan_upload()`, upload | `HOME`/SYNCING | `P` unless the hero word changes (`F`) |
| 6 | `HOME` | *(auto)* upload batch fails | — | record the failure, no retry storm | `HOME`/NO_SYNC | `F` (hero changes) |
| 7 | `RECORDING` | BOOT tap | `StopKeep` | stop capture, `write_meta()` (the commit marker), enqueue | `HOME` | `F` |
| 8 | `RECORDING` | BOOT hold | `Discard` | stop capture, `Store::discard(pending, id)` | `HOME` | `F` |
| 9 | `RECORDING` | PWR tap | `None` | nothing at all; the capture is BOOT's to stop or discard | `RECORDING` | — |
| 10 | `RECORDING` | *(auto)* `note_max_s` reached | `StopKeep` | same as 7, then draw a one-line "cap reached" note | `HOME` | `F` |
| 11 | `MENU` | BOOT tap | `MenuNext` | — | `MENU` | `P` (bar) |
| 12 | `MENU` | PWR tap, row `Notes` | `MenuSelect` | read the store's three folders once | `LIST` | `F` |
| 13 | `MENU` | PWR tap, row `Sync now` | `MenuSelect` | connect, upload every pending note | `HOME`/SYNCING | `F` |
| 14 | `MENU` | PWR tap, row `Phone` | `MenuSelect` | `port_ble` opens a 30 s window | `PHONE_WINDOW` | `F` |
| 15 | `MENU` | PWR tap, row `Wi-Fi` | `MenuSelect` | `port_wifi_setup` | `WIFI_SETUP` | `F` |
| 16 | `MENU` | PWR tap, row `About` | `MenuSelect` | gather 5 values | `ABOUT` | `F` |
| 17 | `MENU` | PWR tap, row `Back` | `MenuSelect` | — | `HOME` | `F` |
| 18 | `MENU` | BOOT hold | `Back` | — | `HOME` | `F` |
| 19 | `LIST` | BOOT tap | `NextNote` | rotate the window if `note ≥ 6` | `LIST` | `P`; `F` when it rotates |
| 20 | `LIST` | PWR tap | `OpenNote` (`None` at zero notes) | read the one `note.json` | `NOTE` | `F` |
| 21 | `LIST` | BOOT hold | `Back` | — | `MENU` | `F` |
| 22 | `NOTE` | BOOT tap | `NextNoteAction` | — | `NOTE` | `P` (bar) |
| 23 | `NOTE` | PWR tap, row `Play` | `RunNoteAction` → `Play` | open the WAV, start playback | `PLAYING` | `F` |
| 24 | `NOTE` | PWR tap, row `Delete` | `RunNoteAction` → `AskDelete` | — | `CONFIRM` | `F` |
| 25 | `NOTE` | PWR tap, row `Back` | `RunNoteAction` → `Back` | — | `LIST` | `F` |
| 26 | `NOTE` | BOOT hold | `Back` | — | `LIST` | `F` |
| 27 | `CONFIRM` | BOOT hold | `DeleteNote` | `Store::erase(id)`, drop it from the cached list | `LIST` | `F` |
| 28 | `CONFIRM` | BOOT tap | `KeepNote` | — | `LIST` | `F` |
| 29 | `CONFIRM` | PWR tap | `KeepNote` | — | `LIST` | `F` |
| 30 | `PLAYING` | BOOT tap | `StopPlay` | stop the codec, close the file | `NOTE` | `F` |
| 31 | `PLAYING` | BOOT hold | `StopPlay` | same as 30 | `NOTE` | `F` |
| 32 | `PLAYING` | PWR tap | `StopPlayToList` | stop the codec, close the file, back where the player was opened | `LIST` | `F` |
| 33 | `PLAYING` | *(auto)* end of audio | — | close the file | `NOTE` | `F` |
| 34 | *any* | a phone asks to pair | `show_pairing_ask()` | remember `previous` | `PAIRING` | `F` |
| 35 | `PAIRING` | BOOT tap | `AllowPhone` | `port_ble` accepts, then `answer_pairing()` | `after_pairing` | `F` |
| 36 | `PAIRING` | PWR tap | `RefusePhone` | reject, then `answer_pairing()` | `after_pairing` | `F` |
| 37 | `PAIRING` | BOOT hold | `None` | nothing at all — no redraw, no state change | `PAIRING` | `—` |
| 38 | `ABOUT` | BOOT hold | `Back` | — | `MENU` | `F` |
| 39 | `ABOUT` | PWR tap | `None` | nothing at all | `ABOUT` | — |
| 40 | `ABOUT` | BOOT tap | `None` | nothing | `ABOUT` | `—` |
| 41 | `WIFI_SETUP` / `PHONE_WINDOW` | the port returns | — | — | `MENU` | `F` |
| 42 | *any* | a display transfer fails (`take_redraw_request()`) | — | rebuild the view model and redraw | same | `F` (driver already forced it) |

**Rows 4a, 9, 37, 39 and 40 are the gestures in the whole UI that produce no panel
activity at all**, and a PWR hold outside Home adds to them. They matter: a hold
at a pairing prompt is a common panic response, and a tap on Home or About must
not look like it did something — least of all switch the device off.

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
table of translated pairs. The whole surface is the **57 strings** below, and nine of
them already exist in `ui_keys.cpp` (`menu_label()`, `action_label()`) — those must
**not** be duplicated in the app.

Every width below was computed with `Canvas::text_width()` against the slot in
§3–§11 that holds it, at the stated scale. `cols` is display columns; at scale 2
one column is 12 px, at scale 3 it is 18 px.

| Key | English | Scale | Cols | px | Slot | Screen | Source |
|---|---|---:|---:|---:|---:|---|---|
| `home.hero.ready` | `READY` | 3 | 5 | 87 | 160 | Home | new |
| `home.hero.uploading` | `UPLOADING` | 3 | 9 | 159 | 160 **←** | Home | new |
| `home.hero.offline` | `OFFLINE` | 3 | 7 | 123 | 160 | Home | new |
| `home.hero.no_card` | `NO CARD` | 3 | 7 | 123 | 160 | Home | new |
| `home.hero.low_power` | `LOW POWER` | 3 | 9 | 159 | 160 **←** | Home | new |
| `home.hero.no_sync` | `NO SYNC` | 3 | 7 | 123 | 160 | Home | new |
| `home.hero.no_time` | `NO TIME` | 3 | 7 | 123 | 160 | Home | new |
| `home.band.to_send` | `%u TO SEND` | 2 | 10 | 118 | 118 **←** | Home | new |
| `home.band.all_sent` | `ALL SENT` | 2 | 8 | 94 | 118 | Home | new |
| `home.sub.of_n` | `%u OF %u SENT` | 2 | 11 | 130 | 168 | Home | new |
| `home.sub.last` | `LAST %02u:%02u` | 2 | 10 | 118 | 168 | Home | new |
| `home.sub.boot_to_record` | `BOOT TO RECORD` | 2 | 14 | 166 | 168 **←** | Home | new |
| `home.sub.nothing_lost` | `NOTHING LOST` | 2 | 12 | 142 | 168 | Home | new |
| `home.sub.plug_in` | `PLUG IN SOON` | 2 | 12 | 142 | 168 | Home | new |
| `home.sub.no_audio` | `NO AUDIO SAVED` | 2 | 14 | 166 | 168 **←** | Home | new |
| `home.label.recording` | `RECORDING` | 2 | 9 | 106 | 106 **←** | Recording | new |
| `fmt.of_cap` | `OF %u:%02u`, `OF %u:%02u:%02u` past an hour, `OF %u:%02u·%s` with the size | 2 | 7..13 | 156 | 106 | Recording, Playing | new |
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
| `menu.notes` | `Notes` | 2 | 5 | 58 | 106 | Menu | ui_keys.cpp |
| `menu.sync_now` | `Sync now` | 2 | 8 | 94 | 106 | Menu | ui_keys.cpp |
| `menu.phone` | `Phone` | 2 | 5 | 58 | 106 | Menu | ui_keys.cpp |
| `menu.wifi` | `Wi-Fi` | 2 | 5 | 58 | 106 | Menu | ui_keys.cpp |
| `menu.about` | `About` | 2 | 5 | 58 | 106 | Menu | ui_keys.cpp |
| `menu.back` | `Back` | 2 | 4 | 46 | 106 | Menu | ui_keys.cpp |
| `menu.value.wifi_on` | `on` | 2 | 2 | 22 | 60 | Menu | new |
| `menu.value.wifi_off` | `off` | 2 | 3 | 34 | 60 | Menu | new |
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
| `pair.title` | `PAIRING` | 2 | 7 | 82 | 82 **←** | Pairing | new |
| `pair.same` | `Same as phone?` | 2 | 15 | 178 | 176 **←** | Pairing | new |
| `about.title` | `ABOUT` | 2 | 5 | 58 | 70 | About | new |
| `about.battery` | `Battery` | 2 | 7 | 82 | 106 | About | new |
| `about.firmware` | `Firmware` | 2 | 8 | 94 | 106 | About | new |
| `about.device` | `Device` | 2 | 6 | 70 | 106 | About | new |
| `about.card` | `Card free` | 2 | 9 | 106 | 106 **←** | About | new |
| `about.notes` | `Notes` | 2 | 5 | 58 | 106 | About | new |

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
4. **The only punctuation is `:`, `.`, `·`, `!`, `?` and `*`.** All render. `…`,
   em dashes, curly quotes and arrows do not — do not introduce them.

### 14.2 Where each string is sourced

| Group | Source |
|---|---|
| `menu.*`, `note.action.*` | `ui_keys.cpp` — already implemented, do not copy |
| `home.*`, `key.*`, `list.*`, `confirm.*`, `play.*`, `pair.*`, `about.*` | new; one `strings.hpp` of `constexpr const char*` |
| `%u` values | `Store::count()`, `Store::pending_ids()`, the uploader's `i/n`, `Meta::duration_s` |
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
| sequence | `panel_init()` → `0x24` write → `0x26` write → `0x22 0xC7` → `0x20` → BUSY | `0x24` write → `0x22 0xCF` → `0x20` → BUSY |
| visible | a full inversion flash | a quick settle, no flash |
| payload | **5,000 bytes** (200 × 200 ÷ 8) | **5,000 bytes** |
| chosen when | first frame, `force_full_`, `!partial_`, or `partials_ >= 20` | otherwise |

**Two facts that decide this whole section:**

1. **A partial refresh is a whole-panel waveform, not a rectangle update.** The
   driver writes all 5,000 bytes either way. `flush(y0, y1)` narrows only the
   RGB565 → 1-bit *conversion* band, never the transmission. So there is no such
   thing as "refresh just the 18 rows that changed" — every partial re-drives every
   pixel of the glass.
2. **Updating is off the app task.** `EpdDisplay::task()` owns the BUSY wait; a
   refresh that outlasts the frame budget does not stall the key loop. The app must
   never wait for it (§16).

The **full-update duration on this unit is unmeasured** — `docs/waveshare-epaper-154-v2.md`
§12 lists it as open item 1, and the driver only guarantees it will not wait longer
than `kBusyTimeoutMs = 3000`. Budget for "a visible flash of the order of a second",
and measure it before tuning `kFullEvery`:

```
hg> screen            # dumps the 1-bit image, proves *what* is on the glass
hg> status            # duty cycle: awake ms, radio ms per hour
```

Time `panel_init()` + a full write with `esp_timer_get_time()` across ten
transitions and log the median. Until then, treat every `F` in §13.2 as a
user-visible pause and keep them where a pause is expected: on a key press.

### 15.2 Which transitions must full-refresh

**Rule: entering or leaving a screen is always full. Within a screen, partial is
the default.**

The reason is not superstition. A partial waveform moves pixels to their new value
*relative to what is already in the panel's RAM*, so where the previous frame had
ink and the new one does not, the residue is proportional to the difference. Two
different screens differ almost everywhere — a menu is six text rows, a note screen
is a 21 px hero — so a partial between them leaves the old layout legible through
the new one. This is already the established rule in the tree
(`modes/notes_screen.hpp`: *"a partial update … to this one leaves the previous
image ghosted, and the panel's own 'full every 20 partials' rule is too slow to
hide it"*), and it applies with more force here because the screens differ more.

Concretely, `F` on every row of §13.2 that changes the screen, plus:

| Also full | Why |
|---|---|
| the first frame after power on | `panel_init()` is part of the full path |
| the first frame after a wake that dropped `EPD_PWR` | the reset cleared the panel's RAM; a partial would have nothing to diff against |
| **any change to the Home hero string** | `READY → UPLOADING` changes three slots at once; a partial leaves the old word under the new |
| a list rotation (§6.4) | six text rows move at once |
| `take_redraw_request()` returns true | a transfer failed; the driver has already forced full, the app only has to redraw |
| returning from a port-owned screen (§12) | both directions change the whole page |

Everything else — bar moves, counters, progress bars, a mark appearing — is
partial, and the driver's `kFullEvery = 20` bounds the accumulation regardless.

### 15.3 Ghosting: where it comes from and how it is bounded

All four ghost sources in this UI, with the mitigation that actually applies:

| Source | Mechanism | Mitigation |
|---|---|---|
| **the selection bar moving** | a 184 × 18 solid block (3,312 black px) must go white on the row it left. Black → white is the transition the partial waveform handles worst | **after 8 consecutive bar moves with no other change, force a full refresh.** 8 moves is 7 vacated rows; one flash per 8 presses is rare enough not to annoy and frequent enough that a list never looks dirty |
| **the elapsed-time digits** | glyphs change in place; the old strokes have to be erased | the region is repainted to `kPaper` first, and the counter ticks at 30 s (§10.3), so a handful of digit changes before the driver's own full |
| **progress bars** | **none** — the fill only ever grows | nothing to mitigate, *provided the bar never shrinks*. A bar that resets (e.g. on a retry) must force a full refresh |
| **screen changes** | §15.2 | full by rule |

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
| **Never wait on `BUSY`, never poll `partials_`, never sleep after `flush()`.** | The driver's task owns that wait. Waiting in the draw path converts a background refresh into a stalled key press. |
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
2. Every string in §14 has a slot width ≥ its computed width, at the stated scale.
3. No screen uses scale 1 or scale 4.
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
| 3 | **No clock on the resting face** | a minute tick costs 60 whole-panel refreshes an hour against a 1 mA idle target; time appears where it is a timestamp |
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
| 2 | Full-update duration unmeasured on this unit | open; bench procedure in §15.1. Until measured, every `F` is "about a second" |
| 3 | The bolt cannot distinguish charging from charged | hardware limit (`charging = -1`); labelled ON EXTERNAL POWER, never `CHARGING` |
| 4 | Two notes in the same minute are indistinguishable on the confirm | accepted, §8.2; mitigated by the hold and deterministic order |
| 5 | 8 consecutive bar moves before a forced full | a starting value, not a measurement. Tune against the real panel; the constant belongs next to `kFullEvery` |
| 6 | `Phone` and `Wi-Fi` frames are owned by the ports | out of scope by design (§12); only the `F` hand-off is specified |
| 7 | `note_max_s` cap shown as `OF 5:00` | read it from the setting, do not hard-code the default; past 99:59 it reads `OF 24:00` |

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
