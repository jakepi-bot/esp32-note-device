// The e-paper board's status screen: a 200x200 face that answers "what is this
// device doing, and can it do it?" in one glance. SPEC.md in the design card is
// the source of every number here; this file is the implementation of it, and
// the copy that fills it in.
//
// Two halves, both hardware-free so the host tests drive them
// (firmware/tests/test_epaper_status.cpp):
//   compose_status()  the device's state as words: hero, note, footer
//   StatusScreen      the pixels, and the change check that keeps an e-paper
//                     panel from flashing a frame nobody can see
//
// Panel facts that shape every number below:
//   * 200x200 px, 183 dpi, 1.54" diagonal. 1 px = 0.138 mm.
//   * The SSD1681 driver thresholds the RGB565 framebuffer to 1 bit: the UI's
//     dark palette prints as black ink on white paper, so this screen draws
//     black on white and never uses a mid-tone.
//   * The font is a 5x7 bitmap at integer scale. Scale 1 is 7 px = 0.97 mm,
//     which is below the legibility floor, so this screen never uses it.
//
// The screen owns the whole panel while the notes device rests: the core UI
// gives it the frame through App::rest_screen (modes/notes_screen.cpp), so the
// two never paint the same glass in one frame.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "hg/canvas.hpp"
#include "hg/font.hpp"
#include "hg/hal.hpp"

namespace hgp {

// The grid, in panel pixels. Public because the copy budgets below and the
// tests measure against them.
namespace layout {
constexpr int kW = 200;
constexpr int kH = 200;
constexpr int kMarginX = 12;
constexpr int kRightEdge = kW - kMarginX;  // 188
constexpr int kMastheadTop = 8;            // glyph top of the clock
constexpr int kRuleY = 36;
constexpr int kHeroTop = 46;
constexpr int kNoteTop = 74;
// Three wrapped lines: the note block gives one line back to the footer's own
// row so the card's free space always has a place, and the touch strip below it
// has room for a finger.
constexpr int kNoteMaxLines = 3;
constexpr int kFootTop = 134;
// The touch strip: the home screen's TALK button, or the pairing question's YES
// and NO. 44 px is 6.1 mm, above the 5 mm a fingertip needs and inside the 4 px
// bottom margin.
constexpr int kPanelTop = 154;
constexpr int kPanelH = 44;
constexpr int kPanelGap = 10;
constexpr int kPanelBorder = 2;
constexpr int kSpineX = kMarginX, kSpineW = 3;
constexpr int kNoteTextX = 22;  // clears the spine, with a real gutter
// Type scale. 2 = 14 px (1.93 mm), 3 = 21 px (2.90 mm). Scale 2 is the floor,
// scale 3 is reserved for the clock and the hero.
constexpr int kScaleBody = 2;
constexpr int kScaleHero = 3;
constexpr int kScaleTime = 3;
// Column budgets: the widest string each slot can hold without truncation.
constexpr int kHeroCols = (kRightEdge - kMarginX) / (hg::font::kCellWidth * kScaleHero);
constexpr int kNoteCols = (kRightEdge - kNoteTextX) / (hg::font::kCellWidth * kScaleBody);
constexpr int kFootCols = (kRightEdge - kMarginX) / (hg::font::kCellWidth * kScaleBody);
}  // namespace layout

// Both link slots always draw: a missing icon reads as a rendering fault, a cut
// mark reads as "this link is not up". Bluetooth has no stack on this build, so
// Unsupported draws the same cut mark and the words carry the difference.
enum class WifiState : uint8_t { Offline, Connecting, Online };
enum class BtState : uint8_t { Unsupported, Off, Disconnected, Connected };

// What the notes mode is doing, as the screen words it.
enum class NotesPhase : uint8_t { Idle, Recording, Uploading, Error };

// What the touch strip along the bottom of the panel offers. None on a unit whose
// glass has no touch controller: the strip then draws nothing and the keys carry
// every action. Ask is the pairing question: two boxes, NO on the left and YES on
// the right, because a new phone must be allowed on this glass and nowhere else.
enum class TouchPanel : uint8_t { None, Talk, Ask };

struct StatusView {
  bool time_valid = false;
  int hour = 0, minute = 0;  // local time, 24 h
  WifiState wifi = WifiState::Offline;
  BtState bt = BtState::Unsupported;
  int battery_pct = -1;         // -1 = unknown
  bool external_power = false;  // the board has no charge-status line: bolt = USB in
  std::string hero;             // <= kHeroCols, drawn at scale 3
  std::string note;             // free text, wrapped by the renderer
  std::string foot;             // one line, <= kFootCols
  bool error = false;           // draws the hero in a box
  // The touch strip. `button` is the button's WORD ("TALK", "STOP"), drawn
  // centred in its box.
  TouchPanel panel = TouchPanel::None;
  std::string button;
};

// Everything the copy needs, gathered by the board. Plain data: no clock, no
// card, no radio, so the same facts produce the same screen on a host.
struct StatusInputs {
  bool time_valid = false;
  int hour = 0, minute = 0;

  bool network_up = false;   // Wi-Fi associated: the device has an address
  bool link_online = false;  // the Hermes link answered
  bool bt_supported = false; // the build carries a Bluetooth stack at all
  bool bt_paired = false;
  bool bt_connected = false;

  int battery_pct = -1;
  bool external_power = false;

  // The glass has a touch controller (the Touch SKU); the home screen then asks
  // through its own TALK button instead of through a key hold.
  bool touch = false;
  // A live turn is in progress: the home screen's button reads STOP.
  bool talk_live = false;
  // From the card: free space for the footer, and the card's size for the copy.
  // sd_free_known is false until the card answers, so the footer never prints a
  // zero it did not measure.
  uint64_t sd_free_bytes = 0;
  bool sd_free_known = false;

  // From NotesMode::status().
  bool sd_ready = true;  // false: no card, or one the device cannot write
  NotesPhase notes = NotesPhase::Idle;
  uint32_t recording_s = 0;  // whole seconds recorded so far
  uint32_t pending = 0, synced = 0, failed = 0, stored = 0;
  uint32_t uploading_bytes = 0;  // size of the note being sent
  bool uploading_valid = false;
  int uploading_hour = 0, uploading_minute = 0;  // its local record time
  bool has_newest = false;                       // a note is on the card
  bool newest_valid = false;                     // and carries a real timestamp
  int newest_hour = 0, newest_minute = 0;
  uint32_t newest_duration_s = 0;

  std::string error;  // the notice line's error, empty when there is none
};

// The device's state as the screen's three strings. The hero always answers
// "what is happening now", never a content-free READY; the footer carries the
// one number that does not fit in the masthead.
StatusView compose_status(const StatusInputs& in);

// The core UI's pairing screen, drawn in this screen's paper style instead of
// the core's white-on-black (the hardware QA's D3: the panel flashed inverted
// exactly when a phone was paired). The words are the core model's own, already
// in the device's language, so nothing is retranslated here; the code sits at
// hero scale because it is what the user types into the host.
struct PairingCopy {
  std::string headline;  // "Pairing code 123456" / "Pair this device"
  std::string detail;    // "On the Hermes host, run: ..."
  std::string hint;      // "waiting for approval"
  std::string code;      // the digits alone, when the core has them
};
StatusView compose_pairing(const StatusInputs& in, const PairingCopy& copy);

// A phone asking to pair, on the touch SKU: the address's last two octets (all
// that thirteen columns hold), the question, and the YES/NO strip. It is its own
// face rather than compose_pairing's because the answer is the only thing the
// screen is for, and a tap off the strip must answer nothing.
StatusView compose_ask(const StatusInputs& in, std::string_view peer);

// What a touch at (x, y) hit on the strip, for the screen that owns the panel.
// Computed from the same constants the renderer draws with, so the target and the
// box can never drift apart.
enum class PanelHit : uint8_t { None, Talk, Yes, No };
PanelHit status_panel_hit(const StatusView& v, int x, int y);

// `s` in at most `cols` display columns, ending in ".." when it had to cut.
// (The font has no ellipsis glyph: U+2026 draws as '?'.)
std::string status_fit(std::string_view s, int cols);

// Paints a view into a 200x200 canvas. Exposed for the tests and for the
// screen briefly above; the screen itself goes through StatusScreen::draw.
void render_status(hg::Canvas& c, const StatusView& v);

// Owns the panel's copy of the frame: draws only when the view changed, so an
// e-paper update costs one refresh per real change and none for a repaint of
// identical pixels.
class StatusScreen {
 public:
  // Draws `v` unless an identical view is already on the glass (and `force` is
  // false, which a caller sets when something else may have painted over it).
  // Returns true when the panel holds this view afterwards.
  bool draw(hg::Display& display, const StatusView& v, bool force);

  // The next draw sends the image even if the view has not changed: the core
  // UI owned the panel in between, so the glass is someone else's image.
  void invalidate() { valid_ = false; }

  // A hash of everything that reaches the panel: the change check's input.
  static uint32_t signature(const StatusView& v);

 private:
  bool valid_ = false;
  uint32_t sig_ = 0;
};

}  // namespace hgp
