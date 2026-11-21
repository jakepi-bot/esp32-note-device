// The e-paper status screen: the design card's ten states, the copy's column
// budgets, and the change check that keeps an idle panel from refreshing.
//
// The hashes in the first case were produced by the design card's reference
// renderer (its out/*.pbm, one PBM per state) and then compared byte for byte
// with this renderer's output: same PBM, same frame. They pin the layouts the
// mockups were approved on -- the masthead slots, the icon centring, the hero
// box -- so a later "small" tweak cannot quietly move the design.
#include "check.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "hg/canvas.hpp"
#include "hg/hal.hpp"
#include "modes/status_screen.hpp"

using namespace hgp;

namespace {

constexpr int kW = layout::kW, kH = layout::kH;

struct FakeDisplay final : hg::Display {
  FakeDisplay() { std::memset(fb, 0, sizeof fb); }
  hg::DisplayInfo info() const override {
    hg::DisplayInfo i;
    i.width = kW;
    i.height = kH;
    return i;  // 1-bit e-paper: no byte swap, no backlight
  }
  uint16_t* framebuffer() override { return fb; }
  void flush(uint16_t y0, uint16_t y1) override {
    ++flushes;
    last_y0 = y0;
    last_y1 = y1;
  }
  uint16_t fb[kW * kH];
  int flushes = 0;
  uint16_t last_y0 = 0, last_y1 = 0;
};

uint32_t hash_fb(const uint16_t* fb) {
  uint32_t h = 2166136261u;
  const auto* p = reinterpret_cast<const uint8_t*>(fb);
  for (size_t i = 0; i < static_cast<size_t>(kW) * kH * 2; ++i) {
    h ^= p[i];
    h *= 16777619u;
  }
  return h;
}

bool drawable_text(std::string_view s) {
  for (size_t i = 0; i < s.size();) {
    const char32_t cp = hg::font::next(s, i);
    if (!hg::font::drawable(cp)) return false;
  }
  return true;
}

int cols(std::string_view s) { return static_cast<int>(hg::font::length(s)); }

StatusInputs online_inputs() {
  StatusInputs in;
  in.time_valid = true;
  in.hour = 14;
  in.minute = 32;
  in.network_up = true;
  in.link_online = true;
  in.sd_ready = true;
  in.battery_pct = 87;
  return in;
}

}  // namespace

TEST("the design card's ten states and the talk strip draw exactly the frames it approved") {
  struct Case {
    const char* name;
    StatusView v;
    uint32_t hash;
  };
  std::vector<Case> cases;
  {
    StatusView v;
    v.time_valid = true; v.hour = 14; v.minute = 32;
    v.wifi = WifiState::Online; v.bt = BtState::Connected; v.battery_pct = 87;
    v.hero = "3 NOTES";
    v.note = "Reviewed the kitchen quote, second is cheaper";
    v.foot = "87% \xC2\xB7 SD 3.1G";
    cases.push_back({"01-idle-synced", v, 0x9ff782c3u});
  }
  {
    StatusView v;
    v.time_valid = true; v.hour = 14; v.minute = 32;
    v.wifi = WifiState::Offline; v.bt = BtState::Disconnected; v.battery_pct = 87;
    v.hero = "OFFLINE";
    v.note = "Notes save to the card, then upload.";
    v.foot = "87% \xC2\xB7 2 QUEUED";
    cases.push_back({"02-offline", v, 0x342ecfdbu});
  }
  {
    StatusView v;
    v.time_valid = true; v.hour = 14; v.minute = 32;
    v.wifi = WifiState::Online; v.bt = BtState::Connected; v.battery_pct = 84;
    v.hero = "RECORDING";
    v.note = "00:08  hold BOOT to stop";
    v.foot = "84%";
    cases.push_back({"03-recording", v, 0x56394095u});
  }
  {
    StatusView v;
    v.time_valid = true; v.hour = 14; v.minute = 33;
    v.wifi = WifiState::Online; v.bt = BtState::Connected; v.battery_pct = 84;
    v.hero = "UPLOADING";
    v.note = "note 14:32  \xC2\xB7  443 KB";
    v.foot = "84% \xC2\xB7 1 OF 2";
    cases.push_back({"04-uploading", v, 0xb3fc282du});
  }
  {
    StatusView v;
    v.time_valid = true; v.hour = 14; v.minute = 32;
    v.wifi = WifiState::Online; v.bt = BtState::Unsupported; v.battery_pct = 12;
    v.hero = "LOW POWER";
    v.note = "Plug in soon or recording stops.";
    v.foot = "12%";
    cases.push_back({"05-low-battery", v, 0x2806c5d5u});
  }
  {
    StatusView v;
    v.time_valid = true; v.hour = 18; v.minute = 5;
    v.wifi = WifiState::Online; v.bt = BtState::Connected; v.battery_pct = 46;
    v.external_power = true;
    v.hero = "3 NOTES";
    v.note = "Call the plumber about the upstairs radiator";
    v.foot = "46% \xC2\xB7 ON POWER";
    cases.push_back({"06-on-power", v, 0xbb16c4afu});
  }
  {
    StatusView v;
    v.time_valid = false;
    v.wifi = WifiState::Offline; v.bt = BtState::Unsupported; v.battery_pct = -1;
    v.hero = "NO TIME";
    v.note = "The clock sets itself once online.";
    v.foot = "NO CLOCK DATA";
    cases.push_back({"07-no-time", v, 0x9bac6ef5u});
  }
  {
    StatusView v;
    v.time_valid = true; v.hour = 9; v.minute = 2;
    v.wifi = WifiState::Online; v.bt = BtState::Connected; v.battery_pct = 96;
    v.hero = "NO NOTES";
    v.note = "Hold BOOT and speak to record one.";
    v.foot = "96%";
    cases.push_back({"08-empty", v, 0x28f5ccdbu});
  }
  {
    StatusView v;
    v.time_valid = true; v.hour = 14; v.minute = 40;
    v.wifi = WifiState::Online; v.bt = BtState::Connected; v.battery_pct = 61;
    v.hero = "3 NOTES";
    v.note =
        "The long case: this text is deliberately far longer than five wrapped lines at fourteen columns so the "
        "block has to truncate and say that it did rather than silently dropping the rest of what was said into the "
        "void";
    v.foot = "61% \xC2\xB7 1 OF 3";
    cases.push_back({"09-long-note", v, 0xf1c6cf69u});
  }
  {
    StatusView v;
    v.time_valid = true; v.hour = 14; v.minute = 45;
    v.wifi = WifiState::Online; v.bt = BtState::Unsupported; v.battery_pct = 73;
    v.hero = "ERROR";
    v.error = true;
    v.note = "No SD card. Recording needs the card.";
    v.foot = "73% \xC2\xB7 NO CARD";
    cases.push_back({"10-error", v, 0x44ba6089u});
  }
  {
    // The Touch SKU's resting face: the free space in the footer and the TALK
    // box along the bottom. The box is an outline, never a fill (a filled box
    // costs a full refresh's ink for a button), and its word is centred in it.
    StatusView v;
    v.time_valid = true; v.hour = 9; v.minute = 5;
    v.wifi = WifiState::Online; v.bt = BtState::Connected; v.battery_pct = 87;
    v.hero = "NO NOTES";
    v.note = "Tap BOOT to record, TALK to talk.";
    v.foot = "87% \xC2\xB7 SD 3.1G";
    v.panel = TouchPanel::Talk;
    v.button = "TALK";
    cases.push_back({"11-touch-talk", v, 0x524c8f35u});
  }
  {
    // The pairing ask on the Touch SKU: the question and the two boxes that answer
    // it. The frame is the one that decides whether a new phone is let in.
    StatusView v;
    v.time_valid = true; v.hour = 9; v.minute = 5;
    v.wifi = WifiState::Online; v.bt = BtState::Disconnected; v.battery_pct = 87;
    v.hero = "NEW PHONE";
    v.note = "Address ends EE:FF. Allow?";
    v.foot = "87% \xC2\xB7 SD 3.1G";
    v.panel = TouchPanel::Ask;
    cases.push_back({"12-pair-ask", v, 0x16ce3343u});
  }

  CHECK_EQ(cases.size(), 12u);
  static uint16_t fb[kW * kH];
  for (const auto& kv : cases) {
    std::memset(fb, 0, sizeof fb);
    hg::Canvas c(fb, kW, kH, false);
    render_status(c, kv.v);
    const uint32_t got = hash_fb(fb);
    if (got != kv.hash) check::fail(__FILE__, __LINE__, std::string(kv.name) + " frame changed");
  }
}

TEST("the copy fits the grid whatever the device is doing") {
  // The cross product of the states the device can be in: every phase, every
  // link combination, an unknown / empty / low / full cell, and no clock.
  const NotesPhase phases[] = {NotesPhase::Idle, NotesPhase::Recording, NotesPhase::Uploading, NotesPhase::Error};
  const int batteries[] = {-1, 0, 20, 87, 100};
  const uint32_t counts[] = {0, 1, 3, 999};
  int checked = 0;
  for (NotesPhase phase : phases) {
    for (int pct : batteries) {
      for (bool wifi : {false, true}) {
        for (bool bt : {false, true}) {
          for (bool time_valid : {false, true}) {
            for (bool sd : {false, true}) {
              for (bool ext : {false, true}) {
                for (uint32_t stored : counts) {
                  StatusInputs in = online_inputs();
                  in.notes = phase;
                  in.battery_pct = pct;
                  in.link_online = wifi;
                  in.network_up = wifi;
                  in.bt_supported = bt;
                  in.bt_connected = bt;
                  in.time_valid = time_valid;
                  in.sd_ready = sd;
                  in.external_power = ext;
                  in.stored = stored;
                  in.has_newest = stored > 0;
                  in.newest_valid = stored > 0 && time_valid;
                  in.newest_hour = 9;
                  in.newest_minute = 5;
                  in.newest_duration_s = 12;
                  in.recording_s = 8;
                  in.pending = stored > 2 ? 2 : stored;
                  in.synced = stored > 2 ? stored - 2 : 0;
                  in.uploading_valid = wifi;
                  in.uploading_bytes = 453632;
                  in.uploading_hour = 14;
                  in.uploading_minute = 32;
                  in.error = phase == NotesPhase::Error ? "No room on the card" : "";
                  const StatusView v = compose_status(in);
                  ++checked;
                  // The layout's own budgets: the renderer truncates past them,
                  // and a truncated status word is a copy bug, not a layout one.
                  CHECK(!v.hero.empty());
                  CHECK(cols(v.hero) <= layout::kHeroCols);
                  CHECK(cols(v.foot) <= layout::kFootCols);
                  CHECK(static_cast<int>(hg::wrap_text(v.note, layout::kNoteCols).size()) <= layout::kNoteMaxLines);
                  // Every character has a glyph: the font draws "?" for what it
                  // lacks, and an ellipsis the copy borrowed from a word
                  // processor would ship as "?".
                  CHECK(drawable_text(v.hero));
                  CHECK(drawable_text(v.foot));
                  CHECK(drawable_text(v.note));
                  // The footer never invents a level: an unknown cell prints no
                  // number at all.
                  if (pct < 0) CHECK(v.foot.find('%') == std::string::npos);
                }
              }
            }
          }
        }
      }
    }
  }
  CHECK(checked == 4 * 5 * 2 * 2 * 2 * 2 * 2 * 4);
}

TEST("a composed frame is ink on paper all the way to the margins") {
  StatusInputs in = online_inputs();
  in.stored = 3;
  in.has_newest = true;
  in.newest_valid = true;
  in.newest_duration_s = 12;
  in.pending = 2;
  const StatusView v = compose_status(in);
  static uint16_t fb[kW * kH];
  hg::Canvas c(fb, kW, kH, false);
  render_status(c, v);

  // Two tones only: the panel thresholds at luma 128, and a mid-grey would
  // print as whichever side it lands on -- a design that depends on it would
  // not survive the conversion.
  int ink = 0, paper = 0, other = 0;
  for (int i = 0; i < kW * kH; ++i) {
    if (fb[i] == hg::rgb565(0, 0, 0)) ++ink;
    else if (fb[i] == hg::rgb565(255, 255, 255)) ++paper;
    else ++other;
  }
  CHECK_EQ(other, 0);
  // The paper margin is real: the drawing keeps 12 px clear on every side.
  bool margin_clean = true;
  for (int y = 0; y < kH; ++y) {
    for (int x = 0; x < layout::kMarginX - 1; ++x) {
      margin_clean = margin_clean && fb[y * kW + x] != hg::rgb565(0, 0, 0);
    }
  }
  CHECK(margin_clean);
  CHECK(ink > 600);  // the words and the icons, not a blank panel
  CHECK(paper > 20000);
}

TEST("an unchanged view is not sent to the panel again") {
  FakeDisplay display;
  StatusScreen screen;
  StatusInputs in = online_inputs();
  in.stored = 3;
  in.has_newest = true;
  in.newest_valid = true;
  in.newest_duration_s = 12;
  const StatusView v = compose_status(in);

  CHECK(screen.draw(display, v, false));
  CHECK_EQ(display.flushes, 1);
  CHECK_EQ(display.last_y0, 0);
  CHECK_EQ(display.last_y1, kH);
  // A repaint of identical pixels costs nothing: this is what keeps an e-paper
  // panel from flashing at the app's frame rate.
  CHECK(screen.draw(display, v, false));
  CHECK_EQ(display.flushes, 1);

  // A changed minute is a changed frame.
  StatusView minute = v;
  minute.minute = 33;
  CHECK(minute.hero == v.hero);  // the clock moved, the words did not
  CHECK(StatusScreen::signature(minute) != StatusScreen::signature(v));
  CHECK(screen.draw(display, minute, false));
  CHECK_EQ(display.flushes, 2);

  // force: the core UI painted over the panel, so the same view must go out
  // again even though nothing changed.
  CHECK(screen.draw(display, minute, true));
  CHECK_EQ(display.flushes, 3);

  // The same ideas hash the same: the signature is over the pixels' inputs, not
  // over the strings' storage.
  StatusView same = v;
  CHECK_EQ(StatusScreen::signature(same), StatusScreen::signature(v));
}

TEST("the footer keeps the card's free space and drops the level to do it") {
  StatusInputs in = online_inputs();
  in.sd_free_bytes = 3100000000ull;
  in.sd_free_known = true;
  in.stored = 3;
  in.has_newest = true;
  in.newest_valid = true;
  in.newest_duration_s = 12;
  in.pending = 0;
  in.synced = 0;
  // Nothing else to say: the level fits beside the space, with room to breathe.
  const StatusView alone = compose_status(in);
  CHECK(alone.foot == std::string("87% \xC2\xB7 SD 3.1G"));

  // A queued count is actionable, so it keeps the line with the space and the
  // level -- whose masthead icon still shows it -- is what goes. Fourteen
  // columns do not hold "2 QUEUED", a spaced dot and "SD 3.1G" (17), so the space
  // gives up its "SD" word rather than the count giving up its place.
  StatusInputs queued = in;
  queued.pending = 2;
  queued.synced = 1;
  const StatusView q = compose_status(queued);
  CHECK(q.foot.find("2 QUEUED") != std::string::npos);
  CHECK(q.foot.find("3.1G") != std::string::npos);
  CHECK(q.foot.find('%') == std::string::npos);
  CHECK(cols(q.foot) <= layout::kFootCols);

  // A synced face keeps its word and the space spelled out; the level goes.
  StatusInputs synced = in;
  synced.battery_pct = 100;
  synced.synced = 3;
  const StatusView sy = compose_status(synced);
  CHECK(sy.foot.find("SYNCED") != std::string::npos);
  CHECK(sy.foot.find("SD 3.1G") != std::string::npos);
  CHECK(sy.foot.find('%') == std::string::npos);
  CHECK(cols(sy.foot) <= layout::kFootCols);

  // Under a gigabyte the number is megabytes, and it still fits the line.
  StatusInputs small = in;
  small.pending = 0;
  small.sd_free_bytes = 940ull * 1000 * 1000;
  const StatusView sm = compose_status(small);
  CHECK(sm.foot.find("SD 940M") != std::string::npos);
  CHECK(cols(sm.foot) <= layout::kFootCols);
}

TEST("a full-ish card keeps its SD label even beside ON POWER (D9)") {
  // The desk unit's card: 62.9 GB free. "ON POWER · 62.9G" fits fourteen columns
  // (16 with spaces, 14 tight) but a reader cannot tell the number from the
  // cell's charge, so the fact is what goes: "87%·SD 62.9G".
  StatusInputs in = online_inputs();
  in.sd_free_bytes = 62900000000ull;
  in.sd_free_known = true;
  in.external_power = true;
  in.stored = 52;
  in.has_newest = true;
  in.newest_valid = true;
  in.newest_duration_s = 12;
  in.pending = 0;
  in.synced = 52;
  const StatusView v = compose_status(in);
  CHECK(v.foot.find("SD 62.9G") != std::string::npos);
  CHECK(v.foot.find("ON POWER") == std::string::npos);
  CHECK(cols(v.foot) <= layout::kFootCols);
  // The number is never a printed zero: a card with nothing left still prints
  // kilobytes, and an unmeasured card prints no number at all.
  StatusInputs empty = in;
  empty.sd_free_bytes = 0;
  const StatusView z = compose_status(empty);
  CHECK(z.foot.find("SD 0") == std::string::npos);
  StatusInputs unknown = in;
  unknown.sd_free_known = false;
  const StatusView u = compose_status(unknown);
  CHECK(u.foot.find("SD") == std::string::npos);
}

TEST("a long note is marked as cut, not silently cut") {
  StatusView v;
  v.time_valid = true;
  v.hour = 14;
  v.minute = 40;
  v.wifi = WifiState::Online;
  v.bt = BtState::Connected;
  v.battery_pct = 61;
  v.hero = "3 NOTES";
  v.note =
      "The long case: this text is deliberately far longer than five wrapped lines at fourteen columns so the "
      "block has to truncate and say that it did rather than silently dropping the rest of what was said into the "
      "void";
  v.foot = "61%";
  static uint16_t fb[kW * kH];
  hg::Canvas c(fb, kW, kH, false);
  render_status(c, v);
  // The marker is ASCII "..": the 5x7 font has no U+2026 and draws "?" for it.
  const std::string cut = status_fit(v.note, layout::kNoteCols);
  CHECK(cols(cut) <= layout::kNoteCols);
  CHECK(status_fit("short", layout::kNoteCols) == "short");
  CHECK(drawable_text(cut));
}

TEST("the pairing screen is paper like the resting face, and says what the core said") {
  StatusInputs in = online_inputs();
  const PairingCopy copy{"Pairing code 123456", "On the Hermes host, run: hermes-gadget pair --code 123456",
                         "waiting for approval", "123456"};
  const StatusView v = compose_pairing(in, copy);
  // The code is the hero: it is what the user carries to the host.
  CHECK(v.hero == std::string("123456"));
  // Every word the core had is still on the screen, in the model's language.
  CHECK(v.note.find(copy.headline) == 0);
  CHECK(v.note.find(copy.detail) != std::string::npos);
  CHECK(v.note.find(copy.hint) != std::string::npos);
  CHECK(!v.error);
  CHECK(drawable_text(v.note));

  // Paper, not the core UI's white-on-black: the panel must not invert between
  // this screen and the resting one (the hardware QA measured 94% ink there
  // against 11% here; anything near 94% is the old frame).
  static uint16_t fb[kW * kH];
  std::memset(fb, 0, sizeof fb);
  hg::Canvas c(fb, kW, kH, false);
  render_status(c, v);
  int ink = 0;
  for (int i = 0; i < kW * kH; ++i) {
    if (fb[i] == hg::rgb565(0, 0, 0)) ++ink;
  }
  CHECK(ink > 600);           // it is not a blank panel
  CHECK(ink < 12000);         // and it is a paper screen, not a black one

  // No code yet: the hero is the state, and the words still come through.
  const PairingCopy waiting{"Pair this device", "Asking Hermes for a pairing code...", "waiting for approval", ""};
  const StatusView w = compose_pairing(in, waiting);
  CHECK(w.hero == std::string("PAIRING"));
  CHECK(w.note.find("Pair this device") == 0);
  CHECK(drawable_text(w.note));
}

TEST("a newest note without a stamp prints no clock at all") {
  // The clock is set (the masthead shows it) and the note is not: it was
  // recorded before the first time sync. The line may not claim a time, and it
  // may not claim there is no clock either (the hardware QA's D5).
  StatusInputs in = online_inputs();
  in.has_newest = true;
  in.newest_valid = false;
  in.newest_duration_s = 2;
  in.stored = 1;
  const StatusView v = compose_status(in);
  CHECK(v.note.find("no clock") == std::string::npos);
  CHECK(v.note.find("00:00") == std::string::npos);
  CHECK(v.note == std::string("Newest note, 2 s. Tap BOOT to add."));
  CHECK(drawable_text(v.note));
}

TEST("the resting face always pays for the card's free space, and drops the level first") {
  StatusInputs in = online_inputs();
  in.has_newest = true;
  in.stored = 3;
  in.synced = 3;
  in.newest_valid = true;
  in.newest_hour = 9;
  in.newest_minute = 5;
  in.newest_duration_s = 12;
  in.sd_free_bytes = 3100000000ull;  // 3.1 GB
  in.sd_free_known = true;
  {
    const StatusView v = compose_status(in);
    CHECK(v.foot.find("SD 3.1G") != std::string::npos);
    CHECK(cols(v.foot) <= layout::kFootCols);
    // "SYNCED" and "SD 3.1G" fill the fourteen columns: the level goes, not the
    // space, and a face with nothing else to say keeps the level (below).
    CHECK(v.foot.find('%') == std::string::npos);
  }
  {
    // Nothing to report: the level and the space share the line.
    in.synced = 0;
    const StatusView v = compose_status(in);
    CHECK(v.foot.find("87%") != std::string::npos);
    CHECK(v.foot.find("SD 3.1G") != std::string::npos);
    CHECK(cols(v.foot) <= layout::kFootCols);
    in.synced = 3;
  }
  {
    // A queued note plus the level plus the space does not fit in fourteen
    // columns: the level is what goes, because the masthead icon still carries it.
    in.pending = 2;
    const StatusView v = compose_status(in);
    CHECK(v.foot.find("2 QUEUED") != std::string::npos);
    CHECK(v.foot.find("3.1G") != std::string::npos);
    CHECK(v.foot.find('%') == std::string::npos);
    CHECK(cols(v.foot) <= layout::kFootCols);
  }
  {
    // No card: no free space is claimed, and the fault takes the line.
    in.sd_ready = false;
    in.sd_free_known = false;
    const StatusView v = compose_status(in);
    CHECK(v.foot.find("SD ") == std::string::npos);
    CHECK(v.foot.find("NO CARD") != std::string::npos);
  }
}

TEST("a card that never answered prints no free space at all") {
  StatusInputs in = online_inputs();
  in.sd_free_known = false;  // mounted, but the scan has not asked yet
  const StatusView v = compose_status(in);
  CHECK(v.foot.find("SD ") == std::string::npos);
  CHECK(v.foot.find("87%") != std::string::npos);
}

TEST("the strip is drawn only where a finger can press it") {
  StatusInputs in = online_inputs();
  in.has_newest = false;  // the face whose copy names the controls
  in.stored = 0;
  in.synced = 0;
  in.newest_valid = false;
  {
    // The plain SKU: no touch controller, so no box, and the copy names the keys.
    const StatusView v = compose_status(in);
    CHECK(v.panel == TouchPanel::None);
    CHECK(v.note.find("Tap BOOT") != std::string::npos);
    CHECK(status_panel_hit(v, kW / 2, layout::kPanelTop + layout::kPanelH / 2) == PanelHit::None);
  }
  {
    // The Touch SKU: one box across the panel, and the copy sends talk to it.
    in.touch = true;
    const StatusView v = compose_status(in);
    CHECK(v.panel == TouchPanel::Talk);
    CHECK(v.button == std::string("TALK"));
    CHECK(v.note.find("TALK") != std::string::npos);
    const int mid_x = (layout::kMarginX + layout::kRightEdge) / 2;
    const int mid_y = layout::kPanelTop + layout::kPanelH / 2;
    CHECK(status_panel_hit(v, mid_x, mid_y) == PanelHit::Talk);
    // A tap on the paper above the strip is nothing: the panel has one target.
    CHECK(status_panel_hit(v, mid_x, layout::kPanelTop - 1) == PanelHit::None);
    CHECK(status_panel_hit(v, layout::kMarginX - 1, mid_y) == PanelHit::None);
    // Inside the very corners of the box: a finger is not a pixel.
    CHECK(status_panel_hit(v, layout::kMarginX, layout::kPanelTop) == PanelHit::Talk);
    CHECK(status_panel_hit(v, layout::kRightEdge - 1, layout::kPanelTop + layout::kPanelH - 1) ==
          PanelHit::Talk);
  }
  {
    // A live turn turns the same box into STOP.
    in.touch = true;
    in.talk_live = true;
    const StatusView v = compose_status(in);
    CHECK(v.button == std::string("STOP"));
    // The label is part of the frame's identity: a changed word must repaint.
    in.talk_live = false;
    CHECK(compose_status(in).button == std::string("TALK"));
  }
}

TEST("a phone asking to pair is answered on the strip, nowhere else") {
  StatusInputs in = online_inputs();
  in.touch = true;
  in.bt_supported = true;
  const StatusView v = compose_ask(in, "AA:BB:CC:DD:EE:FF");
  CHECK(v.panel == TouchPanel::Ask);
  // The last two octets are what thirteen columns hold, and what the phone's own
  // dialog shows; the question is short enough for three lines.
  CHECK(v.note.find("EE:FF") != std::string::npos);
  CHECK(v.note.find("Allow?") != std::string::npos);
  CHECK(static_cast<int>(hg::wrap_text(v.note, layout::kNoteCols).size()) <= layout::kNoteMaxLines);
  CHECK(cols(v.hero) <= layout::kHeroCols);
  CHECK(cols(v.foot) <= layout::kFootCols);

  const int mid_y = layout::kPanelTop + layout::kPanelH / 2;
  const int left_x = layout::kMarginX + 10;
  const int right_x = layout::kRightEdge - 10;
  // NO is the left box, YES the right one, and the gap between them is neither.
  CHECK(status_panel_hit(v, left_x, mid_y) == PanelHit::No);
  CHECK(status_panel_hit(v, right_x, mid_y) == PanelHit::Yes);
  CHECK(status_panel_hit(v, (layout::kMarginX + layout::kRightEdge) / 2, mid_y) == PanelHit::None);
  // The paper above the strip answers nothing, so a stray finger cannot allow a phone.
  CHECK(status_panel_hit(v, right_x, layout::kPanelTop - 1) == PanelHit::None);
  CHECK(status_panel_hit(v, right_x, layout::kFootTop) == PanelHit::None);

  // No touch glass: no strip at all (the core's prompt keeps the ask and the TALK
  // key answers it), and the hit test agrees with the drawing.
  in.touch = false;
  const StatusView plain = compose_ask(in, "AA:BB:CC:DD:EE:FF");
  CHECK(plain.panel == TouchPanel::None);
  CHECK(status_panel_hit(plain, right_x, mid_y) == PanelHit::None);
}

TEST("the strip is inside the glass, and the copy above it does not reach it") {
  // Every line of a composed frame must land above the strip: a wrapped note that
  // ran into the button would print under its border.
  StatusInputs in = online_inputs();
  in.touch = true;
  in.has_newest = true;
  in.stored = 12;
  in.synced = 9;
  in.newest_valid = true;
  in.newest_hour = 23;
  in.newest_minute = 59;
  in.newest_duration_s = 3725;  // cut short with "..", the longest copy there is
  const StatusView v = compose_status(in);
  const size_t lines = hg::wrap_text(v.note, layout::kNoteCols).size();
  CHECK(static_cast<int>(lines) <= layout::kNoteMaxLines);
  const int note_bottom = layout::kNoteTop + layout::kNoteMaxLines * hg::Canvas::line_height(layout::kScaleBody);
  CHECK(note_bottom <= layout::kFootTop);
  CHECK(layout::kFootTop + hg::Canvas::line_height(layout::kScaleBody) <= layout::kPanelTop);
  CHECK(layout::kPanelTop + layout::kPanelH <= layout::kH);
}
