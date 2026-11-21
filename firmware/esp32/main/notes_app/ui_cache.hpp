// The card as the screens see it, cached on events.
//
// docs/notes-ui-spec.md §15.4 is blunt about the draw path: a directory walk is
// tens to hundreds of ms, so `Store::list()`/`count()`/`read_meta()` must never
// run while a frame is drawn. This is the one place that reads them, and the
// app calls it when something happened (a recording committed, a note was
// deleted, an upload finished, a card was pushed in) - never per frame.
//
// It is plain C++ over notes::Store, so the host tests drive the same code
// against a temp directory: ordering, the marks and the window are checked
// against real files, not a hand-built snapshot.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "notes/notes_store.hpp"
#include "notes_app/ui_view.hpp"

namespace hgp::notesui {

class Cache {
 public:
  // Counts every folder and rebuilds the id list: newest first, because note
  // ids are time-sortable as plain strings (§6.3). A missing card empties it.
  // Returns true when the card is usable.
  bool reload(notes::Store& store);

  bool card() const { return card_; }
  uint32_t pending() const { return pending_; }
  uint32_t sent() const { return sent_; }
  uint32_t failed() const { return failed_; }
  uint32_t note_count() const { return static_cast<uint32_t>(ids_.size()); }
  // True when the card holds more notes than the list keeps: the list is the
  // newest kMaxListRows and the screens have to say so (QA D7 - it used to
  // truncate silently).
  bool truncated() const { return truncated_; }
  const std::vector<std::string>& ids() const { return ids_; }
  const std::string* id_at(uint16_t index) const {
    return index < ids_.size() ? &ids_[index] : nullptr;
  }

  // Fills `snap`'s window - [first, first + rows) of the newest-first order -
  // and publishes the counts and the total into it. `rows` is six for the list
  // and one for a note's own screen or Home's LAST line.
  void fill(notes::Store& store, Snapshot& snap, uint8_t first, uint8_t rows) const;

 private:
  bool card_ = false;
  bool truncated_ = false;
  uint32_t pending_ = 0, sent_ = 0, failed_ = 0;
  std::vector<std::string> ids_;
};

}  // namespace hgp::notesui
