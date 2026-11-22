// The nine screens, drawn. Layout only: every coordinate here is a row of
// docs/notes-ui-spec.md §3-§11, and every string comes from a view built by
// ui_view.cpp or from ui_keys.cpp. There is no state in this file, so it can be
// (and is) rendered into a framebuffer on the host.
//
// Two rules that are easy to break and expensive to debug:
//   * black ink on white paper only (§2.1) - the caller clears the framebuffer
//     to paper first; this file never paints a background;
//   * nothing is inked outside x 4..196 or below y 186 (§3.1). The host test
//     checks the ink bounding box of every screen.
#pragma once

#include <cstdint>

#include "hg/canvas.hpp"
#include "notes_app/ui_view.hpp"

namespace hgp::notesui {

constexpr uint16_t kPaper = hg::rgb565(255, 255, 255);
constexpr uint16_t kInk = hg::rgb565(0, 0, 0);

// The frame every screen shares (§3.1). One place, because "the same page"
// is the whole visual system.
namespace grid {
inline constexpr int kLeft = 12;         // left margin
inline constexpr int kRight = 188;       // right edge of content
inline constexpr int kWidth = kRight - kLeft;  // 176 px, 14 columns at scale 2
inline constexpr int kBandY = 8;         // zone A, glyph top
inline constexpr int kGlyphX = 136;      // the Wi-Fi / Bluetooth box (§3.2)
inline constexpr int kRuleY = 27;        // the rule under the band
inline constexpr int kRowTop = 32;       // zone B, first row
inline constexpr int kRowPitch = 18;
inline constexpr int kRuleTwoLine = 144;  // rule above a two-line footer
inline constexpr int kRuleOneLine = 162;  // rule above a one-line footer
inline constexpr int kFooterTwoA = 152;
inline constexpr int kFooterTwoB = 170;
inline constexpr int kFooterOne = 172;
inline constexpr int kScale = 2;         // the floor and the body size
inline constexpr int kHero = 3;          // the only display size
}  // namespace grid

// Everything one frame needs. A caller fills only the member its screen names.
struct ScreenDraw {
  Screen screen = Screen::Home;
  HomeView home;
  MenuView menu;
  ListView list;
  NoteView note;
  ConfirmView confirm;
  RecordingView recording;
  PlayingView playing;
  PairingView pairing;
  PhoneView phone;
  AboutView about;
};

// Paper first, then the screen. The caller owns the framebuffer.
void draw(hg::Canvas& canvas, const ScreenDraw& draw);

// The pieces, exposed for the host test's per-screen checks.
// Zone A. `queue` and `clock` share the left slot at scale 1; the glyphs keep
// the slots §3.2 gives them, so a band with a clock looks like a band without
// one. `bt` takes the Wi-Fi box whenever the radio's window is up.
void draw_band(hg::Canvas& canvas, const char* queue, const char* clock, bool wifi_up, BtState bt,
               int8_t battery_pct, bool external);
void draw_selection(hg::Canvas& canvas, int row_y);
void draw_panel_text_on(hg::Canvas& canvas, int x, int y, const char* text, bool on_bar);
void draw_progress(hg::Canvas& canvas, int y, uint8_t pct);
void draw_footer(hg::Canvas& canvas, const char* line1, const char* line2 = nullptr);
void draw_centred(hg::Canvas& canvas, int y, const char* text, int scale);

}  // namespace hgp::notesui
