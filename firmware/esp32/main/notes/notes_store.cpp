// See notes_store.hpp.
#include "notes_store.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#if defined(ESP_PLATFORM)
#include <esp_vfs_fat.h>  // esp_vfs_fat_info: the FAT's own count (no statvfs here)
#endif

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

uint64_t Store::audio_bytes(NoteState state, std::string_view id) const {
  struct stat st{};
  if (stat(audio_path(state, id).c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) return 0;
  return static_cast<uint64_t>(st.st_size);
}

bool Store::free_bytes(uint64_t& out) const {
  out = 0;
  if (root_.empty()) return false;
#if defined(ESP_PLATFORM)
  // ESP-IDF's FAT VFS does not implement statvfs (the POSIX call answers
  // -1, which would read as "no room" and refuse every note on a card with
  // 62 GB free), and `esp_vfs_fat_info` matches the mount point by exact
  // string. So the tree's own mount is asked: the first component of the root,
  // which is `/sdcard` on this board. That is the same call `sd` and
  // `sd_mount::space()` make, and the answer is the FAT's own free count - kept
  // current by the mount, so it does not rescan the table.
  const size_t slash = root_.find('/', 1);
  const std::string mount = slash == std::string::npos ? root_ : root_.substr(0, slash);
  uint64_t total = 0, free_now = 0;
  if (esp_vfs_fat_info(mount.c_str(), &total, &free_now) != ESP_OK) return false;
  out = free_now;
  return true;
#else
  struct statvfs vfs{};
  if (::statvfs(root_.c_str(), &vfs) != 0) return false;
  // f_bavail is what an unprivileged writer may use, which is the number the
  // recording may spend; f_frsize is the block the count is in (FAT: clusters).
  out = static_cast<uint64_t>(vfs.f_bavail) * static_cast<uint64_t>(vfs.f_frsize);
  return true;
#endif
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

Store::PruneCounts Store::prune_notes(size_t keep_sent) {
  PruneCounts out;
  out.sent = prune_sent(keep_sent);
  for (NoteState state : {NoteState::Pending, NoteState::Failed}) {
    for (const auto& id : list(state)) {
      // locate() looks in pending/ first: an id that also lives there is the
      // pending row's to sweep, not this state's, and deleting it from the
      // wrong folder is how a note goes missing.
      NoteState where = state;
      if (!locate(id, where) || where != state) continue;
      // The uploader's own no-audio line: a missing recording, a zero-length
      // one, and a bare 44-byte header are all empty. A note with audio is
      // never touched, however short.
      if (audio_bytes(state, id) >= kMinAudioBytes) continue;
      if (!erase(id)) continue;
      if (state == NoteState::Pending) {
        ++out.pending;
      } else {
        ++out.failed;
      }
    }
  }
  return out;
}

}  // namespace hgp::notes
