// See notes_store.hpp.
#include "notes_store.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace hgp::notes {
namespace {

bool is_dir_name(const char* name) {
  if (!name || name[0] == '\0') return false;
  if (name[0] == '.') return false;  // ".", "..", and any dot-file an OS leaves
  return true;
}

// A note id is one directory name, and the console takes it straight from the
// user: no separators, no leading dot, so `delete ../sent` cannot walk out of
// the notes tree.
bool is_note_id(std::string_view id) {
  if (id.empty() || id.front() == '.') return false;
  for (char c : id)
    if (c == '/' || c == '\\' || c == '\0') return false;
  return true;
}

// Unlinks every regular file directly inside `dir` (never recursing into a
// subfolder). Used after the audio and the metadata are gone, so a stray file
// cannot keep a deleted note's folder on the card.
void remove_leftover_files(const std::string& dir) {
  DIR* d = opendir(dir.c_str());
  if (!d) return;
  while (struct dirent* entry = readdir(d)) {
    if (!is_dir_name(entry->d_name)) continue;
    const std::string path = dir + "/" + entry->d_name;
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) continue;
    std::remove(path.c_str());
  }
  closedir(d);
}

}  // namespace

const char* state_name(NoteState state) {
  switch (state) {
    case NoteState::Pending: return "pending";
    case NoteState::Sent: return "sent";
    case NoteState::Failed: return "failed";
  }
  return "?";
}

bool Store::make_dir(const std::string& path) {
  if (path.empty()) return false;
  struct stat st{};
  if (stat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
  return ::mkdir(path.c_str(), 0777) == 0;
}

bool Store::exists(const std::string& path) const {
  struct stat st{};
  return stat(path.c_str(), &st) == 0;
}

bool Store::ensure() {
  if (root_.empty()) return false;
  if (!make_dir(root_)) return false;
  for (NoteState s : {NoteState::Pending, NoteState::Sent, NoteState::Failed})
    if (!make_dir(state_dir(s))) return false;
  return true;
}

std::string Store::state_dir(NoteState state) const {
  std::string out = root_;
  out += '/';
  out += state_name(state);
  return out;
}

std::string Store::note_dir(NoteState state, std::string_view id) const {
  std::string out = state_dir(state);
  out += '/';
  out += id;
  return out;
}

std::string Store::audio_path(NoteState state, std::string_view id) const {
  std::string out = note_dir(state, id);
  out += "/recording.wav";
  return out;
}

std::string Store::meta_path(NoteState state, std::string_view id) const {
  std::string out = note_dir(state, id);
  out += "/note.json";
  return out;
}

bool Store::create_note(std::string_view id, std::string& error) {
  if (id.empty()) {
    error = "empty note id";
    return false;
  }
  if (!ensure()) {
    error = "no card";
    return false;
  }
  if (!make_dir(note_dir(NoteState::Pending, id))) {
    error = "cannot write to the card";
    return false;
  }
  return true;
}

bool Store::read_meta(std::string_view id, Meta& out) {
  for (NoteState s : {NoteState::Pending, NoteState::Sent, NoteState::Failed}) {
    const std::string path = meta_path(s, id);
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) continue;
    std::string text;
    char buf[256];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    std::fclose(f);
    return Meta::parse(text, out);
  }
  return false;
}

bool Store::write_meta(std::string_view id, const Meta& meta, std::string& error) {
  const std::string final_path = meta_path(NoteState::Pending, id);
  std::string part = final_path;
  part += ".part";
  FILE* f = std::fopen(part.c_str(), "wb");
  if (!f) {
    error = "cannot open note.json";
    return false;
  }
  const std::string text = meta.json();
  const bool wrote = std::fwrite(text.data(), 1, text.size(), f) == text.size();
  const bool flushed = std::fflush(f) == 0;
  std::fclose(f);
  if (!wrote || !flushed) {
    std::remove(part.c_str());
    error = "cannot write note.json";
    return false;
  }
  // The rename is the commit: until it lands the note does not exist.
  if (std::rename(part.c_str(), final_path.c_str()) != 0) {
    std::remove(part.c_str());
    error = "cannot commit note.json";
    return false;
  }
  return true;
}

std::vector<std::string> Store::list(NoteState state) const {
  std::vector<std::string> out;
  DIR* dir = opendir(state_dir(state).c_str());
  if (!dir) return out;
  while (struct dirent* entry = readdir(dir)) {
    if (!is_dir_name(entry->d_name)) continue;
    out.emplace_back(entry->d_name);
  }
  closedir(dir);
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<std::string> Store::pending_ids() const {
  std::vector<std::string> out;
  for (auto& id : list(NoteState::Pending)) {
    if (exists(meta_path(NoteState::Pending, id))) out.push_back(std::move(id));
  }
  return out;
}

uint32_t Store::count(NoteState state) const { return static_cast<uint32_t>(list(state).size()); }

uint32_t Store::pending_bytes() const {
  uint32_t total = 0;
  for (const auto& id : list(NoteState::Pending)) {
    struct stat st{};
    if (stat(audio_path(NoteState::Pending, id).c_str(), &st) == 0 && st.st_size > 0)
      total += static_cast<uint32_t>(st.st_size);
  }
  return total;
}

bool Store::move(std::string_view id, NoteState from, NoteState to) {
  if (from == to) return true;
  if (!ensure()) return false;
  std::string dst = note_dir(to, id);
  // A note already in the destination (a move that already happened, or a
  // retry that raced it) wins: the folder is identical, nothing to reconcile.
  if (exists(dst)) return true;
  const std::string src = note_dir(from, id);
  if (!exists(src)) return false;
  return std::rename(src.c_str(), dst.c_str()) == 0;
}

bool Store::discard(NoteState state, std::string_view id) {
  const std::string dir = note_dir(state, id);
  std::remove(audio_path(state, id).c_str());
  std::remove((audio_path(state, id) + ".part").c_str());
  std::remove(meta_path(state, id).c_str());
  std::remove((meta_path(state, id) + ".part").c_str());
  return ::rmdir(dir.c_str()) == 0;
}

bool Store::locate(std::string_view id, NoteState& out) const {
  if (!is_note_id(id)) return false;
  for (NoteState s : {NoteState::Pending, NoteState::Failed, NoteState::Sent}) {
    struct stat st{};
    if (stat(note_dir(s, id).c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
      out = s;
      return true;
    }
  }
  return false;
}

bool Store::erase(std::string_view id, NoteState* found) {
  NoteState state = NoteState::Pending;
  if (!locate(id, state)) return false;
  const std::string audio = audio_path(state, id);
  const std::string meta = meta_path(state, id);
  // The mirror image of the write order: the audio first, then the commit
  // marker, then whatever else the folder holds, then the folder itself. A
  // power cut between the first two leaves a folder with no note.json, which
  // reads as "not a note" everywhere; the reverse order would leave a
  // note.json whose audio is already gone.
  std::remove((audio + ".part").c_str());
  std::remove(audio.c_str());
  std::remove((meta + ".part").c_str());
  std::remove(meta.c_str());
  const std::string dir = note_dir(state, id);
  remove_leftover_files(dir);
  if (::rmdir(dir.c_str()) != 0) return false;
  if (found) *found = state;
  return true;
}

uint32_t Store::erase_failed() {
  uint32_t removed = 0;
  for (const auto& id : list(NoteState::Failed)) {
    NoteState from = NoteState::Failed;
    // locate() looks in pending/ first: an id that lives on both sides was
    // deleted from pending and has not left failed/ yet, so do not count it.
    if (erase(id, &from) && from == NoteState::Failed) ++removed;
  }
  return removed;
}

uint32_t Store::prune_sent(size_t keep) {
  auto ids = list(NoteState::Sent);
  if (ids.size() <= keep) return 0;
  uint32_t removed = 0;
  const size_t excess = ids.size() - keep;
  for (size_t i = 0; i < excess; ++i) {
    const std::string dir = note_dir(NoteState::Sent, ids[i]);
    const std::string audio = audio_path(NoteState::Sent, ids[i]);
    const std::string meta = meta_path(NoteState::Sent, ids[i]);
    std::remove(audio.c_str());
    std::remove(meta.c_str());
    if (::rmdir(dir.c_str()) == 0) ++removed;
  }
  return removed;
}

}  // namespace hgp::notes
