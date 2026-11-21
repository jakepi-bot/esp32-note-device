// See notes_app/ui_cache.hpp.
#include "notes_app/ui_cache.hpp"

#include <algorithm>
#include <cstdio>

#include "notes_app/strings.hpp"

namespace hgp::notesui {
namespace {

void read_stub(notes::Store& store, const std::string& id, NoteStub& stub) {
  stub = NoteStub{};
  notes::NoteState state = notes::NoteState::Pending;
  if (!store.locate(id, state)) return;
  stub.mark = state == notes::NoteState::Pending
                  ? NoteMark::Pending
                  : state == notes::NoteState::Failed ? NoteMark::Failed : NoteMark::None;

  notes::Meta meta;
  if (!store.read_meta(id, meta)) {
    // A folder with no note.json: a recording that died before its commit. It
    // is still deletable, so it keeps its row, and its time is unknown.
    std::snprintf(stub.day, sizeof stub.day, "-- ---");
    std::snprintf(stub.clock, sizeof stub.clock, "--:--");
    return;
  }
  Stamp stamp;
  const bool dated = stamp_from_iso(meta.recorded_at, stamp);
  format_day(stamp, stub.day);
  format_clock(stamp, stub.clock);
  (void)dated;
  stub.duration_s = static_cast<uint16_t>(std::min<uint32_t>(meta.duration_s, 65535));
}

}  // namespace

bool Cache::reload(notes::Store& store) {
  card_ = store.ensure();
  pending_ = sent_ = failed_ = 0;
  ids_.clear();
  if (!card_) return false;

  pending_ = store.count(notes::NoteState::Pending);
  sent_ = store.count(notes::NoteState::Sent);
  failed_ = store.count(notes::NoteState::Failed);
  ids_.reserve(pending_ + sent_ + failed_);
  for (notes::NoteState state :
       {notes::NoteState::Pending, notes::NoteState::Sent, notes::NoteState::Failed}) {
    for (std::string& id : store.list(state)) ids_.push_back(std::move(id));
  }
  std::sort(ids_.begin(), ids_.end());
  ids_.erase(std::unique(ids_.begin(), ids_.end()), ids_.end());
  std::reverse(ids_.begin(), ids_.end());  // newest first
  // The screens hold one page plus the whole cursor's worth; a card pruned at
  // 500 sent notes never shows more than the newest few hundred.
  if (ids_.size() > kMaxListRows) ids_.resize(kMaxListRows);
  return true;
}

void Cache::fill(notes::Store& store, Snapshot& snap, uint8_t first, uint8_t rows) const {
  snap.card = card_;
  snap.count_pending = pending_;
  snap.count_sent = sent_;
  snap.count_failed = failed_;
  snap.note_count = static_cast<uint16_t>(ids_.size());

  if (!card_ || rows == 0 || first >= ids_.size()) {
    snap.stub_first = 0;
    snap.stub_rows = 0;
    return;
  }
  if (first + rows > ids_.size()) rows = static_cast<uint8_t>(ids_.size() - first);
  if (rows > kMaxListRows) rows = kMaxListRows;
  snap.stub_first = first;
  snap.stub_rows = rows;
  for (uint8_t i = 0; i < rows; ++i) {
    read_stub(store, ids_[static_cast<size_t>(first) + i], snap.notes[i]);
  }
}

}  // namespace hgp::notesui
