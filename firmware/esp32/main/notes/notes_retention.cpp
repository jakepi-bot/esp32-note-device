// See notes_retention.hpp.
#include "notes_retention.hpp"

#include <algorithm>
#include <sys/stat.h>

#include "notes_queue.hpp"

namespace hgp::notes {
namespace {

// The part of a note that takes the room: its WAV. A note whose audio is
// already gone frees nothing, but its folder and metadata still go.
uint64_t audio_bytes(const Store& store, std::string_view id) {
  struct stat st{};
  if (stat(store.audio_path(NoteState::Sent, id).c_str(), &st) != 0) return 0;
  return st.st_size > 0 ? static_cast<uint64_t>(st.st_size) : 0;
}

// When the note was recorded, as far as the card can say cheaply: the note id
// carries the time in its name, which costs nothing to read; only an id that
// says 1970 (recorded before the clock was ever set) needs its note.json,
// because the upload re-stamps that one.
bool recorded_at_unix(Store& store, std::string_view id, int64_t& out) {
  int64_t from_id = 0;
  if (note_id_stamp(id, from_id) && from_id >= kMinTrustedStampUnix) {
    out = from_id;
    return true;
  }
  Meta meta;
  if (!store.read_meta(id, meta)) return false;
  if (!parse_iso8601(meta.recorded_at, out)) return false;
  return out >= kMinTrustedStampUnix;
}

int free_percent(uint64_t free_bytes, uint64_t total_bytes) {
  if (total_bytes == 0) return -1;  // the card did not say
  if (free_bytes > total_bytes) free_bytes = total_bytes;
  // 100 TB of bytes × 100 still fits in 64 bits, so this cannot overflow.
  return static_cast<int>(free_bytes * 100ull / total_bytes);
}

}  // namespace

RetentionResult enforce(Store& store, const CardSpace& space, int64_t now_s, const RetentionPolicy& policy,
                        std::string_view active_id) {
  RetentionResult out;
  out.evaluated = true;
  out.clock_set = now_s >= kMinTrustedStampUnix;
  out.unsynced_left =
      store.count(NoteState::Pending) + store.count(NoteState::Failed);

  const uint64_t total = space.total;
  uint64_t free_bytes = std::min(space.free, space.total);
  out.free_pct_before = free_percent(free_bytes, total);
  out.free_pct_after = out.free_pct_before;
  out.low_space = total > 0 && out.free_pct_before < policy.min_free_pct;

  // Nothing to do when the age rule has no clock and the card is not tight.
  const bool space_rule = total > 0 && out.free_pct_before < policy.min_free_pct;
  const bool age_rule = out.clock_set;
  if (!space_rule && !age_rule) return out;

  // sent/ sorts by id, and an id starts with its recording time: oldest first.
  for (const auto& id : store.list(NoteState::Sent)) {
    if (!active_id.empty() && id == active_id) continue;  // in use this moment

    const bool tight = space_rule && free_percent(free_bytes, total) < policy.min_free_pct;
    bool too_old = false;
    if (age_rule) {
      int64_t stamp = 0;
      if (recorded_at_unix(store, id, stamp)) {
        const int64_t age = now_s - stamp;
        too_old = age > static_cast<int64_t>(policy.max_age_s);
      }
    }
    if (!too_old && !tight) continue;

    const uint64_t bytes = audio_bytes(store, id);
    NoteState from = NoteState::Sent;
    if (!store.erase(id, &from) || from != NoteState::Sent) continue;  // not ours to remove

    ++out.deleted;
    if (too_old)
      ++out.deleted_for_age;
    else
      ++out.deleted_for_space;
    out.deleted_bytes += bytes;
    free_bytes += bytes;
    out.free_pct_after = free_percent(free_bytes, total);
  }

  out.low_space = total > 0 && free_percent(free_bytes, total) < policy.min_free_pct;
  return out;
}

}  // namespace hgp::notes
