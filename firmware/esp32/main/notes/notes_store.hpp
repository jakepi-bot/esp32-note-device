// The note tree on the card: `notes/{pending,sent,failed}/<note_id>/` with a
// `recording.wav` and a `note.json` (spec §7.2). The class is plain POSIX file
// work, so the host tests drive it against a temp directory and the device
// against `/sdcard`; the mount itself belongs to sd_mount.
//
// Durability rules the caller relies on:
//   - a note exists only once its `note.json` is renamed into place, so a
//     power cut never leaves a half-written note that looks complete;
//   - the audio is written as `recording.wav.part` and renamed before the
//     metadata, in that order.
//   - deleting a note removes the audio before `note.json`, so a power cut
//     mid-delete leaves at most a folder with no metadata, never a note.json
//     whose audio is already gone.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "notes_queue.hpp"

namespace hgp::notes {

enum class NoteState : uint8_t { Pending, Sent, Failed };

const char* state_name(NoteState state);

class Store {
 public:
  // `root` is "/sdcard/notes" on the device; tests point it at a temp dir.
  explicit Store(std::string root = "/sdcard/notes") : root_(std::move(root)) {}

  const std::string& root() const { return root_; }
  void set_root(std::string root) { root_ = std::move(root); }

  // Creates the three state folders. False when the card is not mounted or
  // not writable: the caller reports "no card" and tries again later.
  bool ensure();

  std::string state_dir(NoteState state) const;
  std::string note_dir(NoteState state, std::string_view id) const;
  std::string audio_path(NoteState state, std::string_view id) const;
  std::string meta_path(NoteState state, std::string_view id) const;

  // pending/<id>/ — created empty before the first sample is written.
  bool create_note(std::string_view id, std::string& error);

  bool read_meta(std::string_view id, Meta& out);
  // Written to note.json.part, flushed, then renamed: the commit marker.
  bool write_meta(std::string_view id, const Meta& meta, std::string& error);

  // Ascending directory names (note ids are time-sortable).
  std::vector<std::string> list(NoteState state) const;
  // Pending notes that carry a note.json, oldest first.
  std::vector<std::string> pending_ids() const;
  uint32_t count(NoteState state) const;
  // Bytes of the audio waiting under pending/ (about what the queue weighs).
  uint32_t pending_bytes() const;
  // Bytes of one note's `recording.wav`, 0 when the file is missing or empty.
  // `plan_upload()` decides what that means for the queue.
  uint64_t audio_bytes(NoteState state, std::string_view id) const;

  bool move(std::string_view id, NoteState from, NoteState to);
  // Removes a note's files and folder (a commit that failed halfway).
  bool discard(NoteState state, std::string_view id);
  // Where `id` lives right now: the first of pending/, failed/, sent/ whose
  // folder exists (a folder counts even without its note.json: a recording
  // that died before the commit still has to be deletable).
  bool locate(std::string_view id, NoteState& out) const;
  // `notes delete <id>`: removes the note wherever locate() finds it. The
  // audio goes first, then `note.json`, then the folder, mirroring the write
  // order, so a power cut cannot leave a note.json pointing at missing audio.
  // False for an id that is not on the card (or one that is not a plain
  // directory name): the console then answers "unknown note".
  bool erase(std::string_view id, NoteState* found = nullptr);
  // `notes delete --failed`: empties failed/. Returns how many notes went.
  uint32_t erase_failed();
  // Keeps the newest `keep` entries under sent/ (spec §7.2: prune over 500).
  uint32_t prune_sent(size_t keep = 500);

 private:
  bool make_dir(const std::string& path);
  bool exists(const std::string& path) const;

  std::string root_;
};

}  // namespace hgp::notes
