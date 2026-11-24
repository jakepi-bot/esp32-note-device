// See notes_app/ui_render.hpp: the layout, one screen at a time.
#include "notes_app/ui_render.hpp"

#include <cstdio>

#include "notes_app/strings.hpp"

namespace hgp::notesui {
namespace {

int text_width(const char* text, int scale) { return hg::Canvas::text_width(text ? text : "", scale); }

void text_at(hg::Canvas& c, int x, int y, const char* s, int scale, uint16_t colour) {
  if (s != nullptr && s[0] != '\0') c.text(x, y, s, scale, colour);
}

void text_right(hg::Canvas& c, int y, const char* s, uint16_t colour) {
  if (s == nullptr || s[0] == '\0') return;
  c.text(grid::kRight - text_width(s, grid::kScale), y, s, grid::kScale, colour);
}

// The 16x15 Wi-Fi mark, two arcs and a source dot (§4.4). The down state is the
// same glyph cut on a cleared halo: an empty slot reads as a rendering fault.
const uint8_t kWifiUp[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0xc0, 0x0e, 0x70,
    0x30, 0x0c, 0x20, 0x04, 0x07, 0xe0, 0x08, 0x10, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x80, 0x01, 0x80, 0x01, 0x80,
};
const uint8_t kWifiCut[] = {
    0x80, 0x00, 0xc0, 0x00, 0xe0, 0x00, 0x78, 0x00, 0x3d, 0xc0, 0x1e, 0x70,
    0x2f, 0x0c, 0x23, 0x84, 0x05, 0xc0, 0x08, 0xf0, 0x00, 0x78, 0x00, 0x3c,
    0x01, 0x9e, 0x01, 0x87, 0x01, 0x83,
};

// The band's Wi-Fi glyph box, redrawn for Bluetooth (t_3beb371a §4): a stem with
// the two lobes the mark is known by, thin while the window advertises and a
// 2 px stem while a phone is on it. The Wi-Fi glyph keeps the slot whenever the
// radio is off, so the band never shifts.
const uint8_t kRuneAdvertising[] = {
    0x00, 0x00, 0x06, 0x00, 0x05, 0x80, 0x04, 0x60, 0x04, 0x18,
    0x04, 0x60, 0x05, 0x80, 0x06, 0x00, 0x05, 0x80, 0x04, 0x60,
    0x04, 0x18, 0x04, 0x60, 0x05, 0x80, 0x06, 0x00, 0x00, 0x00,
};
const uint8_t kRuneConnected[] = {
    0x00, 0x00, 0x06, 0x00, 0x07, 0x80, 0x06, 0x60, 0x06, 0x18,
    0x06, 0x60, 0x07, 0x80, 0x06, 0x00, 0x07, 0x80, 0x06, 0x60,
    0x06, 0x18, 0x06, 0x60, 0x07, 0x80, 0x06, 0x00, 0x00, 0x00,
};

// The sleeping face's moon: a 32x32 crescent open at the upper right, thickest
// at the left, so the shape reads as "asleep" without a word of English.
const uint8_t kMoon[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x80, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x07, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00,
    0x1f, 0x00, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0x3f, 0x00, 0x00, 0x00,
    0x3f, 0x00, 0x00, 0x00, 0x3f, 0x00, 0x00, 0x00, 0x3f, 0x00, 0x00, 0x00,
    0x3f, 0x00, 0x00, 0x00, 0x7f, 0x80, 0x00, 0x00, 0x3f, 0x80, 0x00, 0x00,
    0x3f, 0xc0, 0x00, 0x00, 0x3f, 0xc0, 0x00, 0x00, 0x3f, 0xe0, 0x00, 0x00,
    0x3f, 0xf0, 0x00, 0x00, 0x1f, 0xf8, 0x00, 0x00, 0x1f, 0xfe, 0x00, 0x00,
    0x0f, 0xff, 0x80, 0x08, 0x0f, 0xff, 0xfd, 0xf8, 0x07, 0xff, 0xff, 0xf0,
    0x03, 0xff, 0xff, 0xe0, 0x01, 0xff, 0xff, 0xc0, 0x00, 0x7f, 0xff, 0x00,
    0x00, 0x1f, 0xfc, 0x00, 0x00, 0x00, 0x80, 0x00,
};

// 28 x 15: a 26 px shell, a 2 px nub, then fill, bolt or '?' (§4.5). A blank
// box would read as 0 %, so an unknown level is always drawn.
void draw_battery(hg::Canvas& c, int x, int y, int8_t pct, bool external) {
  c.rect(x, y, 26, 15, kInk);
  c.fill_rect(x + 26, y + 5, 2, 5, kInk);
  if (external) {
    const int bx = x + 3, by = y + 2;
    c.fill_rect(bx + 11, by, 3, 4, kInk);
    c.fill_rect(bx + 9, by + 4, 3, 3, kInk);
    c.fill_rect(bx + 7, by + 7, 4, 3, kInk);
    c.fill_rect(bx + 6, by + 10, 3, 2, kInk);
    return;
  }
  if (pct < 0) {
    c.text(x + 10, y + 4, "?", grid::kScale, kInk);
    return;
  }
  const int w = 21 * pct / 100;
  if (w > 0) c.fill_rect(x + 3, y + 2, w, 11, kInk);
}

void rule(hg::Canvas& c, int y) { c.fill_rect(grid::kLeft, y, grid::kWidth, 1, kInk); }

// The Unpair screen's rows: an address is 17 columns at scale 1, and its bar is
// 15 px, so the pitch is 16. Six of them (the panel's row count) close at 123 -
// above the two-line footer's rule - which is why the list needs no scroll.
constexpr int kPeerPitch = 16;
static_assert(grid::kRowTop + kPeerPitch * (kMaxPeerRows - 1) + 12 < grid::kRuleTwoLine,
              "unpair rows must end above the two-line footer rule");

// One row of a list: the selection bar bleeds 4 px past each margin, because a
// solid block's residue is invisible and an outline's is not (§3.4).
void row(hg::Canvas& c, int y, const char* label, const char* value, bool selected) {
  if (selected) c.fill_rect(grid::kLeft - 4, y - 2, grid::kWidth + 8, 18, kInk);
  const uint16_t colour = selected ? kPaper : kInk;
  text_at(c, grid::kLeft, y, label, grid::kScale, colour);
  if (value != nullptr && value[0] != '\0') {
    c.text(grid::kRight - text_width(value, grid::kScale), y, value, grid::kScale, colour);
  }
}

void home_face(hg::Canvas& c, const ScreenDraw& d) {
  const HomeView& v = d.home;
  draw_band(c, v.band, v.clock, v.wifi_on, v.wifi_up, v.battery_pct, v.external);
  rule(c, grid::kRuleY);
  // Home is a two-line footer now: the key legend at the body size, both keys,
  // where a person reads them. The 6 px hint that used to sit above the rule was
  // legible to nobody (Jake, 2026-11-23).
  const int bottom = grid::kRuleTwoLine;
  const int group_h = 21 + 12 + 14;
  const int top = 28 + ((bottom - 28) - group_h) / 2;
  draw_centred(c, top, v.hero, grid::kHero);
  if (v.sub[0] != '\0') draw_centred(c, top + 33, v.sub, grid::kScale);
  if (v.bar_pct > 0) {
    // Growth-only, so the bar leaves no residue at all (§15.3).
    draw_progress(c, top + group_h + 16, v.bar_pct);
  }
  draw_footer(c, key::kBootRecord, key::kHoldMenu);
}

void recording_face(hg::Canvas& c, const ScreenDraw& d) {
  const HomeView& band = d.home;  // the queue count of the previous notes
  draw_band(c, band.band, band.clock, band.wifi_on, band.wifi_up, band.battery_pct, band.external);
  rule(c, grid::kRuleY);
  text_at(c, grid::kLeft, 38, recording::kLabel, grid::kScale, kInk);
  if (d.recording.queued) {
    // The tap was kept, not honoured: the capture starts when the upload ends.
    // No counter and no bar, because nothing has been recorded yet - and a
    // footer that says what the press does now (cancel the wait).
    draw_centred(c, 62, recording::kQueued, grid::kHero);
    draw_centred(c, 94, recording::kWaiting, grid::kScale);
    draw_footer(c, key::kBootCancel, key::kHoldCancel);
    return;
  }
  char elapsed[10], cap[24];
  format_duration(d.recording.elapsed_s, elapsed);
  format_cap(d.recording.cap_s, d.recording.cap_bytes, cap);
  draw_centred(c, 62, elapsed, grid::kHero);
  draw_centred(c, 94, cap, grid::kScale);
  const uint32_t pct = d.recording.cap_s == 0
                           ? 0
                           : static_cast<uint32_t>(d.recording.elapsed_s) * 100u / d.recording.cap_s;
  draw_progress(c, 118, static_cast<uint8_t>(pct > 100 ? 100 : pct));
  draw_footer(c, key::kBootStop, key::kHoldDiscard);
}

// Nine rows do not fit at the list's pitch: at 18 px the last row would start at
// 176, under the two-line footer's rule and past the 186 px ink limit. The menu
// therefore draws its own rows (scale 1, a 12 px pitch, an 11 px bar) and this
// is the check that the column still closes above the footer's rule when a row
// is added or the pitch moves.
static_assert(grid::kRowTop + grid::kMenuPitch * (kMenuCount - 1) + grid::kMenuBar - 1 < grid::kRuleTwoLine,
              "menu rows must end above the two-line footer rule");

void menu_face(hg::Canvas& c, const ScreenDraw& d) {
  text_at(c, grid::kLeft, grid::kBandY, menu::kTitle, grid::kScale, kInk);
  rule(c, grid::kRuleY);
  for (uint8_t i = 0; i < kMenuCount; ++i) {
    const int y = grid::kRowTop + grid::kMenuPitch * i;
    const bool selected = i == d.menu.selected;
    if (selected) c.fill_rect(grid::kLeft - 4, y - 1, grid::kWidth + 8, grid::kMenuBar, kInk);
    const uint16_t colour = selected ? kPaper : kInk;
    text_at(c, grid::kLeft, y, d.menu.rows[i].label, grid::kMenuLabelScale, colour);
    if (d.menu.rows[i].value[0] != '\0') {
      c.text(grid::kRight - text_width(d.menu.rows[i].value, grid::kMenuLabelScale), y,
             d.menu.rows[i].value, grid::kMenuLabelScale, colour);
    }
  }
  draw_footer(c, key::kBootNext, key::kPwrSelect);
}

void list_face(hg::Canvas& c, const ScreenDraw& d) {
  text_at(c, grid::kLeft, grid::kBandY, list::kTitle, grid::kScale, kInk);
  text_right(c, grid::kBandY, d.list.key_line ? d.list.key_line
                                              : (d.list.empty ? nullptr : d.list.key_count),
             kInk);
  rule(c, grid::kRuleY);
  if (d.list.empty) {
    draw_centred(c, 78, list::kEmpty, grid::kScale);
    draw_centred(c, 100, sub::kBootToRecord, grid::kScale);
    // An empty list that still advertises "next" is a lie the user tests once.
    draw_footer(c, key::kBootRecord);
    return;
  }
  for (uint8_t i = 0; i < d.list.rows; ++i) {
    char mark[2] = {d.list.row[i].mark, '\0'};
    row(c, grid::kRowTop + grid::kRowPitch * i, d.list.row[i].when,
        d.list.row[i].mark == ' ' ? nullptr : mark, d.list.first + i == d.list.selected);
  }
  draw_footer(c, key::kBootNext, key::kPwrOpen);
}

void note_face(hg::Canvas& c, const ScreenDraw& d) {
  text_at(c, grid::kLeft, grid::kBandY, note::kTitle, grid::kScale, kInk);
  rule(c, grid::kRuleY);
  text_at(c, grid::kLeft, 34, d.note.day, grid::kHero, kInk);
  text_at(c, grid::kLeft, 62, d.note.when, grid::kScale, kInk);
  text_at(c, grid::kLeft, 80, d.note.state, grid::kScale, kInk);
  rule(c, 98);
  for (uint8_t i = 0; i < static_cast<uint8_t>(NoteActionItem::Count); ++i) {
    row(c, 106 + grid::kRowPitch * i, action_label(static_cast<NoteActionItem>(i)), nullptr,
        i == d.note.selected);
  }
  draw_footer(c, key::kPwrSelect);
}

void confirm_face(hg::Canvas& c, const ScreenDraw& d) {
  const ConfirmView& v = d.confirm;
  // The question, then what it is about: one line for a note's stamp, two for a
  // wipe (the count, and that the card stays) or a format (the card, and what
  // goes with it). An unpair states a 17-column address, so its two lines drop to
  // scale 1 - the body size would run past the margin. The refusal sits above the
  // rule at the small size - it is a reason, not part of the question.
  draw_centred(c, 42, v.hero, grid::kHero);
  draw_centred(c, 72, v.line1, v.line_scale);
  draw_centred(c, 90, v.line2, v.line_scale);
  if (v.why != nullptr) draw_centred(c, 108, v.why, 1);
  rule(c, 118);
  // The body is the legend: this screen has no footer and no rows (§8.2).
  draw_centred(c, 126, v.hold != nullptr ? v.hold : key::kHoldDelete, grid::kScale);
  draw_centred(c, 144, key::kTapKeep, grid::kScale);
}

// The busy face (t_0070a731): one word, what is happening and - for a wipe - how
// far it has got. No footer bindings: there are none to give, and the line says
// so rather than leaving the slot empty.
void busy_face(hg::Canvas& c, const ScreenDraw& d) {
  const BusyView& v = d.busy;
  draw_centred(c, 42, v.hero, grid::kHero);
  draw_centred(c, 72, v.line1, grid::kScale);
  draw_centred(c, 90, v.line2, grid::kScale);
  // The bar is real progress, so it is drawn only where the count is real (a
  // wipe). A format would only be able to fill it from a timer, which is a
  // spinner wearing a ruler's clothes.
  if (v.bar) draw_progress(c, 116, v.pct);
  rule(c, grid::kRuleOneLine);
  draw_centred(c, grid::kFooterOne, key::kPleaseWait, grid::kScale);
}

// The Unpair screen (§11.4): one bonded phone a row, its address in the form the
// phone's own settings show. The address is 17 columns, which the body size
// (12 px a column) would run off the page, so the rows are the menu's size - and
// the name, when the table knew one, takes the room beside it.
void unpair_face(hg::Canvas& c, const ScreenDraw& d) {
  const UnpairView& v = d.unpair;
  text_at(c, grid::kLeft, grid::kBandY, unpair::kTitle, grid::kScale, kInk);
  text_right(c, grid::kBandY, v.count_line, kInk);
  rule(c, grid::kRuleY);
  if (v.empty) {
    // A table with nothing in it says so, and says where a phone is paired. An
    // empty page under a title reads as a screen that failed to load.
    draw_centred(c, 74, unpair::kEmpty, grid::kScale);
    draw_centred(c, 96, unpair::kEmptyHint, 1);
    draw_footer(c, key::kHoldBack);
    return;
  }
  for (uint8_t i = 0; i < v.count; ++i) {
    const int y = grid::kRowTop + kPeerPitch * i;
    const bool selected = i == v.selected;
    // The bar is 15 px for a 7 px glyph: a solid block's residue is invisible and
    // an outline's is not (§3.4).
    if (selected) c.fill_rect(grid::kLeft - 4, y - 3, grid::kWidth + 8, 15, kInk);
    const uint16_t colour = selected ? kPaper : kInk;
    text_at(c, grid::kLeft, y, v.row[i].addr, 1, colour);
    if (v.row[i].name[0] != '\0') {
      c.text(grid::kRight - text_width(v.row[i].name, 1), y, v.row[i].name, 1, colour);
    }
  }
  draw_footer(c, key::kBootNext, key::kPwrUnpair);
}

void playing_face(hg::Canvas& c, const ScreenDraw& d) {
  const HomeView& band = d.home;
  draw_band(c, band.band, band.clock, band.wifi_on, band.wifi_up, band.battery_pct, band.external);
  rule(c, grid::kRuleY);
  text_at(c, grid::kLeft, 40, d.playing.when, grid::kScale, kInk);
  char elapsed[10], total[13];
  format_duration(d.playing.elapsed_s, elapsed);
  format_of(d.playing.total_s, total);
  draw_centred(c, 60, elapsed, grid::kHero);
  draw_centred(c, 92, total, grid::kScale);
  c.rect(grid::kLeft, 132, grid::kWidth, 9, kInk);
  const int w = (grid::kWidth - 4) * d.playing.bar_pct / 100;
  if (w > 0) c.fill_rect(grid::kLeft + 2, 134, w, 5, kInk);
  draw_footer(c, key::kBootStop);
}

void pairing_face(hg::Canvas& c, const ScreenDraw& d) {
  text_at(c, grid::kLeft, grid::kBandY, pairing::kTitle, grid::kScale, kInk);
  rule(c, grid::kRuleY);
  // The passkey is the highest-value string in the UI: scale 3, plain ink,
  // never boxed or inverted.
  draw_centred(c, 44, d.pairing.digits, grid::kHero);
  draw_centred(c, 86, pairing::kSame, grid::kScale);
  draw_centred(c, 108, key::kBootAllow, grid::kScale);
  draw_centred(c, 130, key::kPwrRefuse, grid::kScale);
  draw_footer(c, key::kHoldNothing);
}

// The Phone screen: what the Bluetooth window is doing, the device's name (what
// the phone's chooser lists), the countdown, and the pairing table. A window
// that could not open says why in the state slot - never a blank page.
void phone_face(hg::Canvas& c, const ScreenDraw& d) {
  const PhoneView& v = d.phone;
  text_at(c, grid::kLeft, grid::kBandY, phone::kTitle, grid::kScale, kInk);
  rule(c, grid::kRuleY);
  draw_centred(c, 36, v.name[0] != '\0' ? v.name : nullptr, grid::kScale);
  draw_centred(c, 54, v.state, grid::kScale);
  draw_centred(c, 74, v.countdown[0] != '\0' ? v.countdown : nullptr, grid::kHero);
  draw_centred(c, 104, v.table[0] != '\0' ? v.table : nullptr, grid::kScale);
  draw_centred(c, 124, v.note, grid::kScale);
  draw_footer(c, key::kPwrClose, key::kHoldBack);
}

void about_face(hg::Canvas& c, const ScreenDraw& d) {
  text_at(c, grid::kLeft, grid::kBandY, about::kTitle, grid::kScale, kInk);
  rule(c, grid::kRuleY);
  for (uint8_t i = 0; i < AboutView::kRows; ++i) {
    const int y = 40 + 22 * i;
    text_at(c, grid::kLeft, y, d.about.label[i], grid::kScale, kInk);
    text_right(c, y, d.about.value[i], kInk);
  }
  draw_footer(c, key::kHoldBack);
}

// The sleeping face: the last image the panel is given before the power goes,
// and the only one that is not a screen. The e-paper keeps whatever it was
// holding when the rail drops, so without this an off device looks exactly like
// an on one (Jake, 2026-11-22). Paper first, because it is drawn over whatever
// the person was looking at: a stale Home frame behind the moon would read as a
// device that is still on. No band, no footer, no counts - nothing that could
// be read as a live device. The moon and the 'zzz' are the motif; the one line
// says what the panel is and how the device got here.
void SleepFace(hg::Canvas& c) {
  c.fill_rect(0, 0, c.width(), c.height(), kPaper);
  const int moon_x = 54;   // 32 px wide, left of the zzz
  const int moon_y = 40;
  c.bitmap1(moon_x, moon_y, 32, 32, kMoon, kInk);
  // The zzz climbs out of the crescent's opening, at the hero size so the
  // motif is a drawing and not a caption.
  c.text(moon_x + 42, moon_y - 12, sleep::kZzz, grid::kHero, kInk);
  // The body size, not the 6 px caption: this is the line that tells a person
  // the device is off and how to bring it back, and it was unreadable
  // (Jake, 2026-11-23).
  draw_centred(c, 108, sleep::kHint, grid::kScale);
}

}  // namespace

void draw_band(hg::Canvas& c, const char* queue, const char* clock, bool wifi_on, bool wifi_up,
               int8_t battery_pct, bool external) {
  // The band is full at scale 2: the queue slot runs to 130 px, then a 16 px
  // glyph and a 28 px battery to the right margin. Adding a clock meant choosing
  // between a second line and smaller type, and §3.2's band is one line, so the
  // text drops to scale 1 - 6 px a column, which fits "3 TO SEND" and "14:02"
  // together with the glyph and battery exactly where they were.
  text_at(c, grid::kLeft, grid::kBandY + 3, queue, 1, kInk);
  if (clock != nullptr && clock[0] != '\0') {
    c.text(grid::kGlyphX - 6 - text_width(clock, 1), grid::kBandY + 3, clock, 1, kInk);
  }
  // Wi-Fi owns this slot, in its three states: associated, up but not
  // associated (the rune with the cut), and off (no rune at all - the slot is
  // cleared, so "off" is a blank there rather than a different little icon).
  // Bluetooth is the Phone screen's business and never appears on Home
  // (Jake, 2026-11-22).
  if (wifi_up) {
    c.bitmap1(grid::kGlyphX, grid::kBandY, 16, 15, kWifiUp, kInk);
  } else if (wifi_on) {
    c.bitmap1(grid::kGlyphX, grid::kBandY, 16, 15, kWifiCut, kInk);
  } else {
    c.fill_rect(grid::kGlyphX, grid::kBandY, 16, 15, kPaper);
  }
  draw_battery(c, 160, grid::kBandY, battery_pct, external);
}

void draw_selection(hg::Canvas& c, int row_y) {
  c.fill_rect(grid::kLeft - 4, row_y - 2, grid::kWidth + 8, 18, kInk);
}

void draw_panel_text_on(hg::Canvas& c, int x, int y, const char* text, bool on_bar) {
  text_at(c, x, y, text, grid::kScale, on_bar ? kPaper : kInk);
}

void draw_progress(hg::Canvas& c, int y, uint8_t pct) {
  if (pct > 100) pct = 100;
  c.rect(grid::kLeft, y, grid::kWidth, 9, kInk);
  const int w = (grid::kWidth - 4) * pct / 100;
  if (w > 0) c.fill_rect(grid::kLeft + 2, y + 2, w, 5, kInk);
}

void draw_footer(hg::Canvas& c, const char* line1, const char* line2) {
  const bool two = line2 != nullptr && line2[0] != '\0';
  rule(c, two ? grid::kRuleTwoLine : grid::kRuleOneLine);
  text_at(c, grid::kLeft, two ? grid::kFooterTwoA : grid::kFooterOne, line1, grid::kScale, kInk);
  if (two) text_at(c, grid::kLeft, grid::kFooterTwoB, line2, grid::kScale, kInk);
}

void draw_centred(hg::Canvas& c, int y, const char* text, int scale) {
  if (text == nullptr || text[0] == '\0') return;
  c.text((c.width() - hg::Canvas::text_width(text, scale)) / 2, y, text, scale, kInk);
}

void draw(hg::Canvas& c, const ScreenDraw& d) {
  c.fill_rect(0, 0, c.width(), c.height(), kPaper);
  switch (d.screen) {
    case Screen::Home: home_face(c, d); break;
    case Screen::Recording: recording_face(c, d); break;
    case Screen::Menu: menu_face(c, d); break;
    case Screen::NoteList: list_face(c, d); break;
    case Screen::NoteAction: note_face(c, d); break;
    case Screen::ConfirmDelete:
    case Screen::ConfirmWipe:
    case Screen::ConfirmFormat: confirm_face(c, d); break;
    case Screen::Wiping: busy_face(c, d); break;
    case Screen::ConfirmUnpair: confirm_face(c, d); break;
    case Screen::UnpairList: unpair_face(c, d); break;
    case Screen::Playing: playing_face(c, d); break;
    case Screen::Pairing: pairing_face(c, d); break;
    case Screen::Phone: phone_face(c, d); break;
    case Screen::About: about_face(c, d); break;
    case Screen::Back: home_face(c, d); break;
  }
}

void draw_sleep(hg::Canvas& c) { SleepFace(c); }

}  // namespace hgp::notesui
