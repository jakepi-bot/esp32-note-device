// See radio_window.hpp.
#include "hg/radio_window.hpp"

namespace hg {

void RadioWindow::open(uint32_t& until, uint32_t now_ms, uint32_t duration_ms, Reason reason) {
  until = now_ms + duration_ms;
  tail_ = reason;
  const uint32_t grace = until + timing_.grace_ms;
  if (before(grace_until_, grace)) grace_until_ = grace;
}

void RadioWindow::boot(uint32_t now_ms) { open(boot_until_, now_ms, timing_.boot_ms, Reason::Boot); }

void RadioWindow::pair(uint32_t now_ms) { open(pair_until_, now_ms, timing_.pairing_ms, Reason::Pairing); }

void RadioWindow::note_saved(uint32_t now_ms) { open(note_until_, now_ms, timing_.post_note_ms, Reason::Note); }

void RadioWindow::trigger(uint32_t now_ms) { open(trigger_until_, now_ms, timing_.trigger_ms, Reason::Wake); }

void RadioWindow::connected(bool up, uint32_t now_ms) {
  linked_ = up;
  if (!up) {
    // The link was the reason; the grace keeps the radio up for a beat in case
    // the phone reconnects straight away.
    tail_ = Reason::Link;
    const uint32_t grace = now_ms + timing_.grace_ms;
    if (before(grace_until_, grace)) grace_until_ = grace;
  }
}

void RadioWindow::set_pending(uint32_t count, uint32_t now_ms) {
  if (count == pending_) return;
  const bool was_empty = pending_ == 0;
  pending_ = count;
  if (pending_ == 0) {
    // Nothing left to hand over: the next window is the next recording's.
    retry_until_ = 0;
    next_retry_ms_ = 0;
    return;
  }
  if (was_empty) {
    // A note just became available: take it now rather than at the next slot.
    open(retry_until_, now_ms, timing_.retry_hold_ms, Reason::Pending);
    next_retry_ms_ = now_ms + timing_.retry_ms;
  }
}

bool RadioWindow::tick(uint32_t now_ms) {
  if (pending_ > 0 && !before(now_ms, retry_until_) && !before(now_ms, next_retry_ms_)) {
    open(retry_until_, now_ms, timing_.retry_hold_ms, Reason::Pending);
    next_retry_ms_ = now_ms + timing_.retry_ms;
  }
  return warm(now_ms);
}

RadioWindow::Reason RadioWindow::why(uint32_t now_ms) const {
  if (linked_) return Reason::Link;
  if (before(now_ms, pair_until_)) return Reason::Pairing;
  if (before(now_ms, note_until_)) return Reason::Note;
  if (before(now_ms, trigger_until_)) return Reason::Wake;
  if (pending_ > 0 && before(now_ms, retry_until_)) return Reason::Pending;
  if (before(now_ms, boot_until_)) return Reason::Boot;
  if (before(now_ms, grace_until_)) return tail_;
  return Reason::Rest;
}

const char* RadioWindow::name(Reason reason) {
  switch (reason) {
    case Reason::Rest: return "rest";
    case Reason::Boot: return "boot";
    case Reason::Pairing: return "pair";
    case Reason::Note: return "note";
    case Reason::Pending: return "pending";
    case Reason::Link: return "link";
    case Reason::Wake: return "wake";
  }
  return "?";
}

}  // namespace hg
