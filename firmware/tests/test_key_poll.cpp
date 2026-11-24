// Key timing across a light-sleep gap.
//
// This is the bug Jake hit on battery: a tap on BOOT opened the menu instead of
// recording. The keys are polled; while the chip is in light sleep nobody polls,
// and the wall clock keeps running, so the release after a sleep looked like a two
// second hold - and a BOOT hold is the menu.
#include "key_poll.hpp"

#include "check.hpp"

TEST("keys: a tap whose release lands after a light-sleep gap is a tap") {
  hgp::KeyPoll k;
  k.armed = true;
  uint32_t now = 1000;
  // The press is seen, then the chip sleeps through the whole finger.
  CHECK(hgp::feed(k, true, now, 2000) == hgp::KeyEdge::None);
  k.polled_at = now;
  now += 2000;  // asleep: the clock ran, nobody sampled
  // Without the discount this is the bug: a 2 s "hold" out of a 150 ms tap.
  CHECK(now - k.since >= 2000);
  hgp::discount_sleep(k, 2000);  // the chip measured two seconds of sleep
  CHECK(hgp::feed(k, false, now, 2000) == hgp::KeyEdge::Tap);
}

TEST("keys: a real hold is still a hold when the loop keeps sampling") {
  hgp::KeyPoll k;
  k.armed = true;
  uint32_t now = 500;
  CHECK(hgp::feed(k, true, now, 2000) == hgp::KeyEdge::None);
  k.polled_at = now;
  while (now - k.since < 2500) {
    now += hgp::kKeyPollMs;
    CHECK(hgp::feed(k, true, now, 2000) == hgp::KeyEdge::None);
    k.polled_at = now;
  }
  CHECK(hgp::feed(k, false, now, 2000) == hgp::KeyEdge::Hold);
}

TEST("keys: a press that began before the lock is not a tap") {
  hgp::KeyPoll k;  // armed is false: seen down since boot
  uint32_t now = 100;
  CHECK(hgp::feed(k, true, now, 2000) == hgp::KeyEdge::None);
  k.polled_at = now;
  k.ignored = true;
  now += 300;
  CHECK(hgp::feed(k, false, now, 2000) == hgp::KeyEdge::None);
  CHECK(!k.ignored);
}

TEST("keys: sleep only counts against a press that is down") {
  hgp::KeyPoll k;
  k.armed = true;
  k.since = 0;
  hgp::discount_sleep(k, 5000);  // nothing down: no press to shorten
  CHECK(k.since == 0);
}

TEST("keys: a short tap with steady sampling never reaches the hold") {
  hgp::KeyPoll k;
  k.armed = true;
  uint32_t now = 7000;
  CHECK(hgp::feed(k, true, now, 2000) == hgp::KeyEdge::None);
  k.polled_at = now;
  for (int i = 0; i < 12; ++i) {  // 120 ms
    now += hgp::kKeyPollMs;
    CHECK(hgp::feed(k, true, now, 2000) == hgp::KeyEdge::None);
    k.polled_at = now;
  }
  CHECK(hgp::feed(k, false, now, 2000) == hgp::KeyEdge::Tap);
}
