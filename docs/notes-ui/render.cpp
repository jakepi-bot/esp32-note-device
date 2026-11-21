// Reference renderer for the notes-only device UI spec.
//
// Compiles against the SDK's own hg::Canvas and hg::font, so every wireframe in
// docs/notes-ui-spec.md is the real thing rather than a sketch. Emits one PBM
// (exact 1-bit panel image), one 200x200 ASCII and one 100x100 ASCII per state,
// and checks every string against the column budget that slots it.
//
// Build and run: make -C docs/notes-ui && docs/notes-ui/render out/
//
// Writes, per state: <name>.pbm (exact 1-bit panel image, 5,000 bytes),
// <name>.txt (100x100, one char per 2x2 px) and <name>.full.txt (200x200,
// one char per pixel).
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "hg/canvas.hpp"
#include "hg/font.hpp"

namespace {

// ---------------------------------------------------------------- panel facts
constexpr int kW = 200, kH = 200;
// 1 px = 0.138 mm on this panel (183 dpi).
constexpr uint16_t kInk = hg::rgb565(0, 0, 0);
constexpr uint16_t kPaper = hg::rgb565(255, 255, 255);

// The driver thresholds luma >= 128 to a white dot, so only these two survive.
// Measured from hg::font: advance 6*s, glyph 5*s x 7*s, pitch 9*s.
constexpr int S2 = 2, S3 = 3;  // the only two scales this UI uses

// ---------------------------------------------------------------- grid
constexpr int kML = 12;          // left margin
constexpr int kMR = 188;         // right edge of content (exclusive-ish)
constexpr int kContent = kMR - kML;  // 176 px
constexpr int kFootRule1 = 162;  // rule above a one-line footer
constexpr int kFootY1 = 172;     // glyph top, one-line footer
constexpr int kFootRule2 = 144;  // rule above a two-line footer
constexpr int kFootY2a = 152;    // glyph top, two-line footer, line 1
constexpr int kFootY2b = 170;    // glyph top, two-line footer, line 2
constexpr int kInkBottom = 186;  // nothing may be inked below this row

// Column budgets, from Canvas::text_width == cols*6*s - s.
int cols_for(int px, int s) { return (px + s) / (6 * s); }

// ---------------------------------------------------------------- text
int tw(const std::string& s, int sc) { return hg::Canvas::text_width(s, sc); }
int cols(const std::string& s) { return static_cast<int>(hg::font::length(s)); }

void text(hg::Canvas& c, int x, int y, const std::string& s, int sc, uint16_t col) {
  c.text(x, y, s, sc, col);
}
void ctext(hg::Canvas& c, int y, const std::string& s, int sc, uint16_t col) {
  c.text((kW - tw(s, sc)) / 2, y, s, sc, col);
}
// Left at x, snapped right to kMR.
void rtext(hg::Canvas& c, int y, const std::string& s, int sc, uint16_t col) {
  c.text(kMR - tw(s, sc), y, s, sc, col);
}

// ---------------------------------------------------------------- icons
// 16x15 Wi-Fi, two arcs and a source dot. 1 px strokes. From the panel's own
// driver work: three arcs degrade into noise below 20 px.
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

void draw_battery(hg::Canvas& c, int x, int y, int pct, bool external) {
  // 28 x 15 shell: 26 px body + 2 px nub.
  c.rect(x, y, 26, 15, kInk);
  c.fill_rect(x + 26, y + 5, 2, 5, kInk);
  if (external) {
    // A bolt, in local coords, means ON EXTERNAL POWER. The board has no
    // charge-status pin, so it can never mean "charging".
    const int bx = x + 3, by = y + 2;
    c.fill_rect(bx + 11, by, 3, 4, kInk);
    c.fill_rect(bx + 9, by + 4, 3, 3, kInk);
    c.fill_rect(bx + 7, by + 7, 4, 3, kInk);
    c.fill_rect(bx + 6, by + 10, 3, 2, kInk);
    return;
  }
  if (pct < 0) {
    c.text(x + 10, y + 4, "?", S2, kInk);  // never a blank box: that reads as 0%
    return;
  }
  const int w = 21 * pct / 100;
  if (w > 0) c.fill_rect(x + 3, y + 2, w, 11, kInk);
}

// ---------------------------------------------------------------- frame
struct Frame {
  bool show_band = true;
  std::string queue = "3 TO SEND";  // pending count, or ALL SENT / NO CARD
  int wifi = 1;   // 0 off, 1 up
  int batt = 84;  // -1 unknown
  bool external = false;
  std::vector<std::string> footer;
};

void draw_band(hg::Canvas& c, const Frame& f) {
  if (!f.show_band) return;
  if (!f.queue.empty()) text(c, kML, 8, f.queue, S2, kInk);
  c.bitmap1(136, 8, 16, 15, f.wifi ? kWifiUp : kWifiCut, kInk);
  draw_battery(c, 160, 8, f.batt, f.external);
}

void draw_footer(hg::Canvas& c, const Frame& f) {
  if (f.footer.empty()) return;
  const bool two = f.footer.size() >= 2;
  const int rule = two ? kFootRule2 : kFootRule1;
  c.fill_rect(kML, rule, kContent, 1, kInk);
  text(c, kML, two ? kFootY2a : kFootY1, f.footer[0], S2, kInk);
  if (two) text(c, kML, kFootY2b, f.footer[1], S2, kInk);
}

// A selected row is a solid bar with paper-coloured text: the strongest cue a
// 1-bit panel can give, and residue inside a black block is invisible.
void draw_row(hg::Canvas& c, int y, const std::string& label, const std::string& right, bool sel) {
  if (sel) c.fill_rect(kML - 4, y - 2, kContent + 8, 18, kInk);
  const uint16_t col = sel ? kPaper : kInk;
  text(c, kML, y, label, S2, col);
  if (!right.empty()) {
    c.text(kMR - tw(right, S2), y, right, S2, col);
  }
}

// ---------------------------------------------------------------- screens
struct HomeView {
  std::string hero;
  std::string sub;
  std::string label;  // optional line above the hero (the recording face)
  std::string last;   // newest note: "LAST 14:02 0:14"
  bool bar = false;          // upload progress bar
  int bar_pct = 0;
  Frame frame;
};

void draw_home(hg::Canvas& c, const HomeView& v) {
  c.fill_rect(0, 0, kW, kH, kPaper);
  draw_band(c, v.frame);
  c.fill_rect(kML, 27, kContent, 1, kInk);
  const bool two = v.frame.footer.size() >= 2;
  const int bottom = two ? kFootRule2 : kFootRule1;
  if (!v.label.empty()) {
    // The recording face is a page, not a centred face: a label, the elapsed
    // time as the one hero, the cap, and the bar.
    text(c, kML, 38, v.label, S2, kInk);
    ctext(c, 62, v.hero, S3, kInk);
    ctext(c, 94, v.sub, S2, kInk);
    if (v.bar) {
      const int by = 118;
      c.rect(kML, by, kContent, 9, kInk);
      const int w = (kContent - 4) * v.bar_pct / 100;
      if (w > 0) c.fill_rect(kML + 2, by + 2, w, 5, kInk);
    }
    draw_footer(c, v.frame);
    return;
  }
  const int group_h = 21 + 12 + 14;
  const int top = 28 + ((bottom - 28) - group_h) / 2;
  ctext(c, top, v.hero, S3, kInk);
  if (!v.sub.empty()) ctext(c, top + 33, v.sub, S2, kInk);
  if (v.bar) {
    const int by = top + group_h + 16;
    c.rect(kML, by, kContent, 9, kInk);
    const int w = (kContent - 4) * v.bar_pct / 100;
    if (w > 0) c.fill_rect(kML + 2, by + 2, w, 5, kInk);
  }
  draw_footer(c, v.frame);
}

struct MenuView {
  std::vector<std::string> labels;
  std::vector<std::string> values;
  int sel = 0;
  Frame frame;
};

void draw_menu(hg::Canvas& c, const MenuView& v) {
  c.fill_rect(0, 0, kW, kH, kPaper);
  text(c, kML, 8, "MENU", S2, kInk);
  c.fill_rect(kML, 27, kContent, 1, kInk);
  for (size_t i = 0; i < v.labels.size(); ++i) {
    draw_row(c, 32 + 18 * static_cast<int>(i), v.labels[i], v.values[i], static_cast<int>(i) == v.sel);
  }
  draw_footer(c, v.frame);
}

struct ListRow {
  std::string when;
  std::string mark;  // "*" to send, "!" failed, "" sent
};

struct ListView {
  std::string title;
  std::string right;
  std::vector<ListRow> rows;
  int sel = 0;
  std::string empty_line;
  Frame frame;
};

void draw_list(hg::Canvas& c, const ListView& v) {
  c.fill_rect(0, 0, kW, kH, kPaper);
  text(c, kML, 8, v.title, S2, kInk);
  if (!v.right.empty()) rtext(c, 8, v.right, S2, kInk);
  c.fill_rect(kML, 27, kContent, 1, kInk);
  if (v.rows.empty()) {
    ctext(c, 78, v.empty_line, S2, kInk);
    ctext(c, 100, "BOOT TO RECORD", S2, kInk);
  }
  for (size_t i = 0; i < v.rows.size(); ++i) {
    const int y = 32 + 18 * static_cast<int>(i);
    draw_row(c, y, v.rows[i].when, v.rows[i].mark, static_cast<int>(i) == v.sel);
  }
  draw_footer(c, v.frame);
}

struct ActionView {
  std::string title;
  std::string date;
  std::string meta;
  std::string state;
  std::vector<std::string> actions;
  int sel = 0;
  Frame frame;
};

void draw_action(hg::Canvas& c, const ActionView& v) {
  c.fill_rect(0, 0, kW, kH, kPaper);
  text(c, kML, 8, v.title, S2, kInk);
  c.fill_rect(kML, 27, kContent, 1, kInk);
  text(c, kML, 34, v.date, S3, kInk);
  text(c, kML, 62, v.meta, S2, kInk);
  text(c, kML, 80, v.state, S2, kInk);
  c.fill_rect(kML, 98, kContent, 1, kInk);
  for (size_t i = 0; i < v.actions.size(); ++i) {
    draw_row(c, 106 + 18 * static_cast<int>(i), v.actions[i], "", static_cast<int>(i) == v.sel);
  }
  draw_footer(c, v.frame);
}

struct ConfirmView {
  std::string date;
  std::vector<std::string> body;
  Frame frame;
};

void draw_confirm(hg::Canvas& c, const ConfirmView& v) {
  c.fill_rect(0, 0, kW, kH, kPaper);
  ctext(c, 46, "DELETE?", S3, kInk);
  ctext(c, 78, v.date, S2, kInk);
  c.fill_rect(kML, 104, kContent, 1, kInk);
  int y = 118;
  for (const auto& line : v.body) {
    ctext(c, y, line, S2, kInk);
    y += 22;
  }
  draw_footer(c, v.frame);
}

struct PlayView {
  std::string title;
  std::string elapsed;
  std::string total;
  int pct = 0;
  Frame frame;
};

void draw_playing(hg::Canvas& c, const PlayView& v) {
  c.fill_rect(0, 0, kW, kH, kPaper);
  draw_band(c, v.frame);
  c.fill_rect(kML, 27, kContent, 1, kInk);
  text(c, kML, 40, v.title, S2, kInk);
  ctext(c, 60, v.elapsed, S3, kInk);
  ctext(c, 92, "OF " + v.total, S2, kInk);
  c.rect(kML, 132, kContent, 9, kInk);
  const int w = (kContent - 4) * v.pct / 100;
  if (w > 0) c.fill_rect(kML + 2, 134, w, 5, kInk);
  draw_footer(c, v.frame);
}

struct PairView {
  std::string code;
  std::vector<std::string> body;
  Frame frame;
};

void draw_pairing(hg::Canvas& c, const PairView& v) {
  c.fill_rect(0, 0, kW, kH, kPaper);
  text(c, kML, 8, "PAIRING", S2, kInk);
  c.fill_rect(kML, 27, kContent, 1, kInk);
  ctext(c, 44, v.code, S3, kInk);
  int y = 86;
  for (const auto& line : v.body) {
    ctext(c, y, line, S2, kInk);
    y += 22;
  }
  draw_footer(c, v.frame);
}

struct AboutView {
  std::vector<std::string> labels;
  std::vector<std::string> values;
  Frame frame;
};

void draw_about(hg::Canvas& c, const AboutView& v) {
  c.fill_rect(0, 0, kW, kH, kPaper);
  text(c, kML, 8, "ABOUT", S2, kInk);
  c.fill_rect(kML, 27, kContent, 1, kInk);
  for (size_t i = 0; i < v.labels.size(); ++i) {
    const int y = 40 + 22 * static_cast<int>(i);
    text(c, kML, y, v.labels[i], S2, kInk);
    rtext(c, y, v.values[i], S2, kInk);
  }
  draw_footer(c, v.frame);
}

// ---------------------------------------------------------------- output
struct State {
  std::string name;
  std::vector<std::string> budget;  // labelled strings, checked against slots
};

// Every string that has to fit a slot, with the slot width in pixels.
struct Check {
  std::string name;
  int cols;
  int px;
};

void write_pbm(const char* path, const uint8_t* bits) {
  FILE* f = std::fopen(path, "wb");
  if (!f) return;
  std::fprintf(f, "P4\n%d %d\n", kW, kH);
  // bits[] is the panel image: 1 = white dot. PBM P4 is 1 = black. Invert.
  uint8_t line[kW * kH / 8];
  for (int i = 0; i < kW * kH / 8; ++i) line[i] = static_cast<uint8_t>(~bits[i]);
  std::fwrite(line, 1, kW * kH / 8, f);
  std::fclose(f);
}

// 2 px per character: still shows glyph shapes and exact block edges at a size
// a terminal can hold.
void write_ascii2(const char* path, const uint8_t* bits) {
  FILE* f = std::fopen(path, "wb");
  if (!f) return;
  for (int y = 0; y < kH; y += 2) {
    std::string line;
    for (int x = 0; x < kW; x += 2) {
      int on = 0;
      for (int dy = 0; dy < 2; ++dy) {
        for (int dx = 0; dx < 2; ++dx) {
          const int xx = x + dx, yy = y + dy;
          const int bit = (bits[(yy * kW + xx) / 8] >> (7 - ((yy * kW + xx) & 7))) & 1;
          on += bit ? 0 : 1;  // panel bit 1 = white; ink is 0
        }
      }
      line += on >= 4 ? '#' : on == 3 ? '*' : on == 2 ? '+' : on == 1 ? '.' : ' ';
    }
    std::fprintf(f, "%s\n", line.c_str());
  }
  std::fclose(f);
}

void write_ascii1(const char* path, const uint8_t* bits) {
  FILE* f = std::fopen(path, "wb");
  if (!f) return;
  for (int y = 0; y < kH; ++y) {
    std::string line;
    for (int x = 0; x < kW; ++x) {
      const int bit = (bits[(y * kW + x) / 8] >> (7 - ((y * kW + x) & 7))) & 1;
      line += bit ? '.' : '#';
    }
    std::fprintf(f, "%s\n", line.c_str());
  }
  std::fclose(f);
}

uint16_t g_fb[kW * kH];

// The panel bit is 1 for white, exactly as EpdDisplay::convert() decides it.
void flatten(uint8_t* bits) {
  for (int i = 0; i < kW * kH; ++i) {
    const uint16_t px = g_fb[i];
    const int r = ((px >> 11) & 0x1f) * 255 / 31;
    const int g = ((px >> 5) & 0x3f) * 255 / 63;
    const int b = (px & 0x1f) * 255 / 31;
    const bool white = r * 299 + g * 587 + b * 114 >= 128 * 1000;
    for (int k = 0; k < 1; ++k) {}
    if (white) bits[i / 8] |= static_cast<uint8_t>(1u << (7 - (i & 7)));
  }
}

std::vector<Check> g_checks;

void check(const std::string& name, const std::string& s, int slot_px, int sc) {
  g_checks.push_back({name + " [" + s + "]", cols(s), tw(s, sc)});
  if (tw(s, sc) > slot_px) {
    std::fprintf(stderr, "FAIL %s: \"%s\" is %d px in a %d px slot (%d cols)\n", name.c_str(), s.c_str(),
                 tw(s, sc), slot_px, cols(s));
  }
}

void emit(const char* out, const std::string& name, void (*draw)(hg::Canvas&, const void*), const void* v) {
  std::memset(g_fb, 0xff, sizeof g_fb);  // paper
  hg::Canvas c(g_fb, kW, kH, /*swap_bytes=*/false);
  draw(c, v);
  uint8_t bits[kW * kH / 8] = {0};
  flatten(bits);
  // ink bounds
  int minx = kW, maxx = -1, miny = kH, maxy = -1;
  for (int y = 0; y < kH; ++y) {
    for (int x = 0; x < kW; ++x) {
      const int bit = (bits[(y * kW + x) / 8] >> (7 - ((y * kW + x) & 7))) & 1;
      if (!bit) {
        minx = std::min(minx, x); maxx = std::max(maxx, x);
        miny = std::min(miny, y); maxy = std::max(maxy, y);
      }
    }
  }
  if (maxx >= 0 && (maxy > kInkBottom || minx < 4 || maxx > 196)) {
    std::fprintf(stderr, "FAIL %s: ink box x %d..%d y %d..%d escapes the page (x 4..196, y <= %d)\n",
                 name.c_str(), minx, maxx, miny, maxy, kInkBottom);
  }
  std::string p = std::string(out) + name;
  write_pbm((p + ".pbm").c_str(), bits);
  write_ascii2((p + ".txt").c_str(), bits);
  write_ascii1((p + ".full.txt").c_str(), bits);
  std::printf("%-22s ink x %3d..%3d  y %3d..%3d\n", name.c_str(), minx, maxx, miny, maxy);
}

// ---------------------------------------------------------------- the catalog
// A tiny shim so emit() can take a typed view through void*.
template <typename V>
void draw_thunk(hg::Canvas& c, const void* v) {
  if constexpr (std::is_same_v<V, HomeView>) draw_home(c, *static_cast<const V*>(v));
  else if constexpr (std::is_same_v<V, MenuView>) draw_menu(c, *static_cast<const V*>(v));
  else if constexpr (std::is_same_v<V, ListView>) draw_list(c, *static_cast<const V*>(v));
  else if constexpr (std::is_same_v<V, ActionView>) draw_action(c, *static_cast<const V*>(v));
  else if constexpr (std::is_same_v<V, ConfirmView>) draw_confirm(c, *static_cast<const V*>(v));
  else if constexpr (std::is_same_v<V, PlayView>) draw_playing(c, *static_cast<const V*>(v));
  else if constexpr (std::is_same_v<V, PairView>) draw_pairing(c, *static_cast<const V*>(v));
  else if constexpr (std::is_same_v<V, AboutView>) draw_about(c, *static_cast<const V*>(v));
}

}  // namespace

int main(int argc, char** argv) {
  const std::string out = argc > 1 ? argv[1] : "out/";

  // ---- 1. HOME: ready, three notes to send
  Frame band;
  Frame foot1;
  foot1.footer = {"BOOT: record"};

  HomeView h_ready;
  h_ready.hero = "READY";
  h_ready.sub = "LAST 14:02";
  h_ready.frame = foot1;
  h_ready.frame.show_band = true;
  emit(out.c_str(), "01-home-ready", draw_thunk<HomeView>, &h_ready);

  // ---- 2. HOME: nothing recorded yet
  HomeView h_empty = h_ready;
  h_empty.sub = "BOOT TO RECORD";
  h_empty.frame.queue = "ALL SENT";
  emit(out.c_str(), "02-home-empty", draw_thunk<HomeView>, &h_empty);

  // ---- 3. HOME: offline
  HomeView h_off = h_ready;
  h_off.hero = "OFFLINE";
  h_off.sub = "LAST 14:02";
  h_off.frame.wifi = 0;
  h_off.frame.footer = {"BOOT: record"};
  emit(out.c_str(), "03-home-offline", draw_thunk<HomeView>, &h_off);

  // ---- 4. HOME: uploading
  HomeView h_up = h_ready;
  h_up.hero = "UPLOADING";
  h_up.sub = "2 OF 3 SENT";
  h_up.frame.queue = "3 TO SEND";
  h_up.bar = true;
  h_up.bar_pct = 66;
  emit(out.c_str(), "04-home-uploading", draw_thunk<HomeView>, &h_up);

  // ---- 5. HOME: no card
  HomeView h_nocard = h_ready;
  h_nocard.hero = "NO CARD";
  h_nocard.sub = "NOTHING LOST";
  h_nocard.frame.queue = "";
  emit(out.c_str(), "05-home-nocard", draw_thunk<HomeView>, &h_nocard);

  // ---- 6. HOME: low power
  HomeView h_low = h_ready;
  h_low.hero = "LOW POWER";
  h_low.sub = "PLUG IN SOON";
  h_low.frame.batt = 9;
  h_low.sub = "PLUG IN SOON";
  emit(out.c_str(), "06-home-lowpower", draw_thunk<HomeView>, &h_low);

  // ---- 7. HOME: error, sync failed
  HomeView h_fail = h_ready;
  h_fail.hero = "NO SYNC";
  h_fail.sub = "LAST 14:02";
  emit(out.c_str(), "07-home-nosync", draw_thunk<HomeView>, &h_fail);

  // ---- 8. MENU
  MenuView mv;
  mv.labels = {"Notes", "Sync now", "Phone", "Wi-Fi", "About", "Back"};
  mv.values = {"12", "3", "", "on", "", ""};
  mv.sel = 0;
  mv.frame.footer = {"BOOT: next", "PWR: select"};
  emit(out.c_str(), "08-menu", draw_thunk<MenuView>, &mv);

  // ---- 9. NOTES LIST
  ListView lv;
  lv.title = "NOTES";
  lv.right = "* TO SEND";
  lv.rows = {{"21 Nov 14:02", "*"}, {"20 Nov 09:11", "*"}, {"20 Nov 07:48", ""},
             {"19 Nov 18:40", ""},  {"18 Nov 21:05", ""}, {"18 Nov 16:22", ""}};
  lv.sel = 0;
  lv.frame.footer = {"BOOT: next", "PWR: open"};
  emit(out.c_str(), "09-list", draw_thunk<ListView>, &lv);

  // ---- 10. NOTES LIST: empty
  ListView lv0;
  lv0.title = "NOTES";
  lv0.right = "0";
  lv0.empty_line = "NO NOTES YET";
  lv0.frame.footer = {"BOOT: record"};
  emit(out.c_str(), "10-list-empty", draw_thunk<ListView>, &lv0);

  // ---- 11. NOTE (its own screen), Play selected
  ActionView av;
  av.title = "NOTE";
  av.date = "21 Nov";
  av.meta = "14:02 · 0:14";
  av.state = "to send";
  av.actions = {"Play", "Delete", "Back"};
  av.sel = 0;
  av.frame.footer = {"BOOT: next"};
  emit(out.c_str(), "11-note-play", draw_thunk<ActionView>, &av);

  // ---- 12. NOTE, Delete selected
  ActionView av2 = av;
  av2.sel = 1;
  av2.state = "to send";
  av2.frame.footer = {"BOOT: next"};
  emit(out.c_str(), "12-note-delete", draw_thunk<ActionView>, &av2);

  // ---- 13. CONFIRM DELETE
  ConfirmView cv;
  cv.date = "21 Nov 14:02";
  cv.body = {"HOLD: delete", "TAP: keep"};
  cv.frame.footer = {};
  emit(out.c_str(), "13-confirm", draw_thunk<ConfirmView>, &cv);

  // ---- 14. PLAYING
  PlayView pv;
  pv.title = "21 Nov 14:02";
  pv.elapsed = "0:06";
  pv.total = "0:14";
  pv.pct = 42;
  pv.frame.footer = {"BOOT: stop"};
  pv.frame.queue = "2 TO SEND";
  emit(out.c_str(), "14-playing", draw_thunk<PlayView>, &pv);

  // ---- 15. PAIRING ASK
  PairView pas;
  pas.code = "418 207";
  pas.body = {"Same as phone?", "BOOT: allow", "PWR: refuse"};
  pas.frame.footer = {"HOLD: nothing"};
  emit(out.c_str(), "15-pairing", draw_thunk<PairView>, &pas);

  // ---- 16. RECORDING
  HomeView h_rec;
  h_rec.hero = "0:42";
  h_rec.sub = "OF 5:00";
  h_rec.label = "RECORDING";
  h_rec.bar = true;
  h_rec.bar_pct = 14;  // 42 of 300 s
  h_rec.frame.footer = {"BOOT: stop", "HOLD: discard"};
  h_rec.frame.queue = "3 TO SEND";
  emit(out.c_str(), "16-recording", draw_thunk<HomeView>, &h_rec);

  // ---- 17. ABOUT
  AboutView ab;
  ab.labels = {"Battery", "Firmware", "Device", "Card free", "Notes"};
  ab.values = {"84%", "2.0.1", "hg-a1b2", "62.9G", "12"};
  ab.frame.footer = {"HOLD: back"};
  emit(out.c_str(), "17-about", draw_thunk<AboutView>, &ab);

  // ---- budgets
  check("hero", "READY", 160, S3);
  check("hero", "OFFLINE", 160, S3);
  check("hero", "NO CARD", 160, S3);
  check("hero", "LOW POWER", 160, S3);
  check("hero", "UPLOADING", 160, S3);
  check("hero", "NO SYNC", 160, S3);
  check("title", "PAIRING", 100, S2);
  check("title", "NOTES", 58, S2);
  check("title", "NOTE", 58, S2);
  check("title", "MENU", 58, S2);
  check("title", "ABOUT", 58, S2);
  // Two-column rows: the label must clear the right-aligned value by >= 12 px.
  const char* ab_l[] = {"Battery", "Firmware", "Device", "Card free", "Notes"};
  const char* ab_v[] = {"84%", "2.0.1", "hg-a1b2", "62.9G", "12"};
  for (int i = 0; i < 5; ++i) {
    const int lr = kML + tw(ab_l[i], S2);
    const int vl = kMR - tw(ab_v[i], S2);
    if (vl - lr < 12) std::fprintf(stderr, "FAIL about row %d: label ends %d, value starts %d\n", i, lr, vl);
  }
  check("foot", "HOLD: back", 176, S2);
  check("hero3", "DELETE?", 176, S3);
    check("hero3", "418 207", 176, S3);
  check("hero3", "0:42", 176, S3);
  check("hero3", "21 Nov", 176, S3);
  check("sub", "3 TO SEND", 176, S2);
  check("sub", "BOOT TO RECORD", 176, S2);
  check("sub", "NOTHING LOST", 176, S2);
  check("sub", "PLUG IN SOON", 176, S2);
  check("sub", "RECORDING", 176, S2);
  check("sub", "2 OF 3 SENT", 176, S2);
  check("sub", "OF 0:14", 176, S2);
  check("foot", "BOOT: record", 176, S2);
  check("foot", "BOOT: stop", 176, S2);
  check("foot", "HOLD: discard", 176, S2);
  check("foot", "HOLD: nothing", 176, S2);
  check("foot", "BOOT: next", 176, S2);
  check("foot", "PWR: select", 176, S2);
  check("foot", "PWR: open", 176, S2);
  check("foot", "PWR: delete", 176, S2);
  check("foot", "PWR: refuse", 176, S2);
  check("foot", "PWR: play", 176, S2);
  check("row", "Sync now", 100, S2);
  check("row", "21 Nov 14:02", 154, S2);
  check("meta", "14:02 · 0:14", 176, S2);
  check("meta", "to send", 176, S2);
  check("body", "HOLD: delete", 176, S2);
  check("body", "TAP: keep", 176, S2);
  check("body", "Same as phone?", 176, S2);
  check("label", "RECORDING", 176, S2);
  check("sub", "OF 5:00", 176, S2);
  check("sub", "LAST 14:02", 176, S2);
  check("band", "99 TO SEND", 118, S2);
  check("band", "ALL SENT", 118, S2);
  check("band", "NO CARD", 118, S2);

  std::printf("\n--- %zu strings checked ---\n", g_checks.size());
  std::printf("cols at scale 2 for 176 px: %d ; at scale 3: %d\n", cols_for(176, 2), cols_for(176, 3));
  return 0;
}
