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

TEST("the design card's ten states draw exactly the frames it approved") {
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
    v.foot = "87% \xC2\xB7 SYNCED";
    cases.push_back({"01-idle-synced", v, 0xb8b9bdcfu});
  }
  {
    StatusView v;
    v.time_valid = true; v.hour = 14; v.minute = 32;
    v.wifi = WifiState::Offline; v.bt = BtState::Disconnected; v.battery_pct = 87;
    v.hero = "OFFLINE";
    v.note = "Notes save to the card, upload when back";
    v.foot = "87% \xC2\xB7 2 QUEUED";
    cases.push_back({"02-offline", v, 0x3466973fu});
  }
  {
    StatusView v;
    v.time_valid = true; v.hour = 14; v.minute = 32;
    v.wifi = WifiState::Online; v.bt = BtState::Connected; v.battery_pct = 84;
    v.hero = "RECORDING";
    v.note = "00:08  hold BOOT to stop";
    v.foot = "84%";
    cases.push_back({"03-recording", v, 0xf1cbfa95u});
  }
  {
    StatusView v;
    v.time_valid = true; v.hour = 14; v.minute = 33;
    v.wifi = WifiState::Online; v.bt = BtState::Connected; v.battery_pct = 84;
    v.hero = "UPLOADING";
    v.note = "note 14:32  \xC2\xB7  443 KB";
    v.foot = "84% \xC2\xB7 1 OF 2";
    cases.push_back({"04-uploading", v, 0x562c382du});
  }
  {
    StatusView v;
    v.time_valid = true; v.hour = 14; v.minute = 32;
    v.wifi = WifiState::Online; v.bt = BtState::Unsupported; v.battery_pct = 12;
    v.hero = "LOW POWER";
    v.note = "Plug in soon. Recording stops if it dies.";
    v.foot = "12%";
    cases.push_back({"05-low-battery", v, 0xa36f1c91u});
  }
  {
    StatusView v;
    v.time_valid = true; v.hour = 18; v.minute = 5;
    v.wifi = WifiState::Online; v.bt = BtState::Connected; v.battery_pct = 46;
    v.external_power = true;
    v.hero = "3 NOTES";
    v.note = "Call the plumber about the upstairs radiator";
    v.foot = "46% \xC2\xB7 ON POWER";
    cases.push_back({"06-on-power", v, 0x351ff223u});
  }
  {
    StatusView v;
    v.time_valid = false;
    v.wifi = WifiState::Offline; v.bt = BtState::Unsupported; v.battery_pct = -1;
    v.hero = "NO TIME";
    v.note = "The clock sets itself once online.";
    v.foot = "NO CLOCK DATA";
    cases.push_back({"07-no-time", v, 0xfdecb2f5u});
  }
  {
    StatusView v;
    v.time_valid = true; v.hour = 9; v.minute = 2;
    v.wifi = WifiState::Online; v.bt = BtState::Connected; v.battery_pct = 96;
    v.hero = "NO NOTES";
    v.note = "Hold BOOT and speak to record one.";
    v.foot = "96%";
    cases.push_back({"08-empty", v, 0x0df562dbu});
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
    cases.push_back({"09-long-note", v, 0x3331d4a1u});
  }
  {
    StatusView v;
    v.time_valid = true; v.hour = 14; v.minute = 45;
    v.wifi = WifiState::Online; v.bt = BtState::Unsupported; v.battery_pct = 73;
    v.hero = "ERROR";
    v.error = true;
    v.note = "No SD card. Recording needs the card.";
    v.foot = "73% \xC2\xB7 NO CARD";
    cases.push_back({"10-error", v, 0xef51e8cdu});
  }

  CHECK_EQ(cases.size(), 10u);
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

TEST("the footer prefers a count over a level that does not fit with it") {
  StatusInputs in = online_inputs();
  // 100% plus a word is one column too long for the 14 the footer has.
  in.battery_pct = 100;
  in.external_power = true;
  in.stored = 3;
  in.has_newest = true;
  in.newest_valid = true;
  in.newest_duration_s = 12;
  in.pending = 0;
  in.synced = 3;
  const StatusView powered = compose_status(in);
  CHECK(powered.foot == std::string("100%"));  // the masthead bolt carries "on power"

  // A count is actionable, so it keeps the line and the level gives way.
  StatusInputs queued = in;
  queued.pending = 2;
  queued.synced = 1;
  const StatusView q = compose_status(queued);
  CHECK(q.foot == std::string("2 QUEUED"));

  // Under 100% both facts fit, which is the design's own example.
  StatusInputs both = in;
  both.battery_pct = 87;
  both.pending = 2;
  const StatusView b = compose_status(both);
  CHECK(b.foot == std::string("87% \xC2\xB7 2 QUEUED"));
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
