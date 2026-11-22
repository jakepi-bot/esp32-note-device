// hg/radio_window.hpp: when the Bluetooth controller may run. The e-paper
// board has no 32 kHz crystal, so a controller that is up forbids light sleep:
// the radio has to rest, and every reason to run it is a bounded window.
#include "check.hpp"

#include "hg/radio_window.hpp"

using hg::RadioWindow;

namespace {

// A clock small enough that every deadline stays below 2^31 and the wrap-safe
// comparisons in the policy never see a wrapped value in these cases.
constexpr uint32_t kStart = 1000;

RadioWindow::Timing timing() {
  RadioWindow::Timing t;
  t.boot_ms = 120000;
  t.pairing_ms = 120000;
  t.post_note_ms = 180000;
  t.retry_ms = 300000;
  t.retry_hold_ms = 30000;
  t.trigger_ms = 60000;
  t.phone_ms = 300000;
  t.phone_link_ms = 60000;
  t.grace_ms = 20000;
  return t;
}

}  // namespace

TEST("a radio window rests until something asks for it") {
  RadioWindow w(timing());
  CHECK(!w.warm(kStart));
  CHECK_EQ(RadioWindow::name(w.why(kStart)), std::string("rest"));
  CHECK(!w.tick(kStart));
  CHECK(!w.tick(kStart + 1000));
}

TEST("boot opens a window, and the grace ends it") {
  RadioWindow w(timing());
  w.boot(kStart);
  CHECK(w.warm(kStart));
  CHECK(w.warm(kStart + 119000));
  CHECK_EQ(RadioWindow::name(w.why(kStart + 119000)), std::string("boot"));
  // The window is over but the grace keeps it up, then it rests.
  CHECK(w.tick(kStart + 120001));
  CHECK(!w.tick(kStart + 140001));
  CHECK_EQ(RadioWindow::name(w.why(kStart + 140001)), std::string("rest"));
}

TEST("the pairing window lasts two minutes, and closing it early does not") {
  RadioWindow w(timing());
  w.pair(kStart);
  CHECK(w.warm(kStart));
  CHECK_EQ(RadioWindow::name(w.why(kStart + 1000)), std::string("pair"));
  CHECK(w.tick(kStart + 119000));
  // 120 s window + 20 s grace, then it rests.
  CHECK(w.tick(kStart + 121000));
  CHECK(!w.tick(kStart + 145000));
}

TEST("a recorded note keeps the radio up for three minutes") {
  RadioWindow w(timing());
  w.note_saved(kStart);
  CHECK(w.warm(kStart));
  CHECK_EQ(RadioWindow::name(w.why(kStart)), std::string("note"));
  CHECK(w.tick(kStart + 179000));
  CHECK(!w.tick(kStart + 201000));
}

TEST("an explicit wake is the shortest window") {
  RadioWindow w(timing());
  w.trigger(kStart);
  CHECK(w.warm(kStart));
  CHECK_EQ(RadioWindow::name(w.why(kStart)), std::string("wake"));
  // 60 s window + 20 s grace.
  CHECK(!w.tick(kStart + 85000));
}

TEST("a link keeps the radio up whatever else expired") {
  RadioWindow w(timing());
  w.trigger(kStart);
  w.connected(true, kStart + 1000);
  // Long past every window: the link is the reason, not a deadline.
  CHECK(w.tick(kStart + 600000));
  CHECK_EQ(RadioWindow::name(w.why(kStart + 600000)), std::string("link"));
  w.connected(false, kStart + 600000);
  // The disconnect gets a short grace, then the radio may rest.
  CHECK(w.tick(kStart + 600000 + 19000));
  CHECK(!w.tick(kStart + 600000 + 21000));
}

TEST("a waiting note advertises now, then retries on its own clock") {
  RadioWindow w(timing());
  w.set_pending(1, kStart);
  CHECK(w.warm(kStart));
  CHECK_EQ(RadioWindow::name(w.why(kStart)), std::string("pending"));
  // The first listen is 30 s + grace; then silence until the 5-minute retry.
  CHECK(w.tick(kStart + 29000));
  CHECK(!w.tick(kStart + 60000));
  CHECK(!w.tick(kStart + 240000));
  // The retry slot opens on its own, without another note.
  CHECK(w.tick(kStart + 300000));
  CHECK_EQ(RadioWindow::name(w.why(kStart + 300000)), std::string("pending"));
  // This listen is 30 s + grace as well.
  CHECK(!w.tick(kStart + 360000));
}

TEST("the last note taken stops the retries at once") {
  RadioWindow w(timing());
  w.set_pending(1, kStart);
  CHECK(w.warm(kStart));
  w.set_pending(0, kStart + 5000);
  // The window already open finishes (it is a phone's chance to stay), and no
  // new one opens after it.
  CHECK(w.tick(kStart + 31000));
  CHECK(!w.tick(kStart + 55000));
  CHECK(!w.tick(kStart + 400000));
}

TEST("the reasons stack: the longest one wins and the rest still end") {
  RadioWindow w(timing());
  w.pair(kStart);
  w.note_saved(kStart + 10000);
  CHECK_EQ(RadioWindow::name(w.why(kStart + 50000)), std::string("pair"));
  CHECK_EQ(RadioWindow::name(w.why(kStart + 130000)), std::string("note"));
  CHECK(w.tick(kStart + 200000));  // the note window's grace
  CHECK(!w.tick(kStart + 215000));
}

// The Phone screen's window (t_9972e2a1): five minutes, held open by a link, and
// over on its own - the three things the report could not tell apart.
TEST("the phone window is five minutes, then the grace, then rest") {
  RadioWindow w(timing());
  w.phone(kStart);
  CHECK(w.warm(kStart));
  CHECK_EQ(RadioWindow::name(w.why(kStart)), std::string("phone"));
  CHECK_EQ(w.phone_left_ms(kStart), 300000u);
  CHECK_EQ(w.phone_left_ms(kStart + 299000), 1000u);
  CHECK(w.pairing_open(kStart + 299000));  // a phone may pair inside it
  CHECK_EQ(w.phone_left_ms(kStart + 301000), 0u);
  CHECK(!w.pairing_open(kStart + 301000));
  CHECK(w.tick(kStart + 301000));  // the grace the policy adds to every window
  CHECK(!w.tick(kStart + 321000));
  CHECK_EQ(RadioWindow::name(w.why(kStart + 321000)), std::string("rest"));
}

TEST("a connected phone holds the phone window open, and it ends a minute after it leaves") {
  RadioWindow w(timing());
  w.phone(kStart);
  // Four minutes in a phone connects: every pass pushes the deadline a minute
  // out, so the window never expires under the link.
  w.connected(true, kStart + 240000);
  CHECK(w.tick(kStart + 240000));
  CHECK(w.tick(kStart + 290000));  // past the original five minutes
  CHECK_EQ(w.phone_left_ms(kStart + 290000), 60000u);
  CHECK(w.tick(kStart + 600000));  // ten minutes, still connected, still open
  CHECK(w.phone_left_ms(kStart + 600000) > 0);
  // The phone leaves: at most a minute of window is left, then it is over.
  w.connected(false, kStart + 600000);
  CHECK(w.tick(kStart + 630000));
  CHECK(w.phone_left_ms(kStart + 630000) > 0);
  CHECK_EQ(w.phone_left_ms(kStart + 661000), 0u);
  CHECK(w.tick(kStart + 661000));  // the closing grace
  CHECK(!w.tick(kStart + 682000));
}

TEST("closing the phone window by hand ends it at once, with no grace") {
  RadioWindow w(timing());
  w.phone(kStart);
  w.phone_closed(kStart + 1000);
  CHECK_EQ(w.phone_left_ms(kStart + 1000), 0u);
  CHECK(!w.pairing_open(kStart + 1000));
  CHECK(!w.tick(kStart + 1000));
}
