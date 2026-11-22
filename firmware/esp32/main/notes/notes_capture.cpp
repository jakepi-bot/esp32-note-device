// See notes_capture.hpp.
#include "notes_capture.hpp"

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sys/stat.h>

namespace hgp::notes {
namespace {

constexpr uint32_t kBytesPerSecond = kSampleRate * kChannels * (kBitsPerSample / 8);

void put_le32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v & 0xff);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xff);
  p[2] = static_cast<uint8_t>((v >> 16) & 0xff);
  p[3] = static_cast<uint8_t>((v >> 24) & 0xff);
}

void put_le16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xff);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xff);
}

}  // namespace

void wav_header(uint8_t out[44], uint32_t data_bytes, uint32_t sample_rate) {
  std::memcpy(out + 0, "RIFF", 4);
  put_le32(out + 4, 36 + data_bytes);  // the rest of the file after this field
  std::memcpy(out + 8, "WAVE", 4);
  std::memcpy(out + 12, "fmt ", 4);
  put_le32(out + 16, 16);                                       // fmt chunk size
  put_le16(out + 20, 1);                                        // PCM
  put_le16(out + 22, static_cast<uint16_t>(kChannels));         // mono
  put_le32(out + 24, sample_rate);
  put_le32(out + 28, sample_rate * kChannels * (kBitsPerSample / 8));  // byte rate
  put_le16(out + 32, static_cast<uint16_t>(kChannels * (kBitsPerSample / 8)));  // block align
  put_le16(out + 34, static_cast<uint16_t>(kBitsPerSample));
  std::memcpy(out + 36, "data", 4);
  put_le32(out + 40, data_bytes);
}

uint32_t wav_seconds(uint32_t data_bytes, uint32_t sample_rate) {
  const uint32_t bytes_per_second = sample_rate * kChannels * (kBitsPerSample / 8);
  if (data_bytes == 0) return 0;
  const uint32_t whole = data_bytes / bytes_per_second;
  return whole == 0 ? 1 : whole;  // a short note still says "1"
}

uint64_t note_budget_bytes(uint64_t free_bytes) {
  // The reserve is what a note may not spend: a card with less than that free
  // has no room for a recording, and one with more may use the difference.
  uint64_t budget = free_bytes > kNoteReserveBytes ? free_bytes - kNoteReserveBytes : 0;
  // Never past the per-note ceiling, and never past what the upload can declare
  // (see kMaxNoteFileBytes), and always a whole sample.
  if (budget > kMaxNoteFileBytes) budget = kMaxNoteFileBytes;
  return budget - (budget % (kChannels * (kBitsPerSample / 8)));
}

Capture::~Capture() { close(); }

void Capture::close() {
  if (file_) {
    std::fclose(file_);
    file_ = nullptr;
  }
}

bool Capture::start(Store& store, std::string_view id, uint32_t max_seconds, std::string& error) {
  return start(store, id, max_seconds, 0, error);
}

bool Capture::start(Store& store, std::string_view id, uint32_t max_seconds, uint64_t free_bytes,
                    std::string& error) {
  abort();
  if (id.empty()) {
    error = "empty note id";
    return false;
  }
  // The card's room, before anything is created: a note the volume cannot hold
  // is refused here rather than written up to the point where it fails. A volume
  // that will not say (no card, or the tree unmounted mid-press) is not a full
  // one: the note starts on the per-note ceiling and `create_note` below is what
  // reports a card that really is not there.
  uint64_t free_now = 0;
  bool room_known = false;
  if (free_bytes != 0) {
    free_now = free_bytes;
    room_known = true;
  } else {
    room_known = store.free_bytes(free_now);
  }
  const uint64_t budget = room_known ? note_budget_bytes(free_now) : kMaxNoteFileBytes;
  if (budget == 0) {
    error = "SD almost full, no synced notes to delete";
    return false;
  }
  if (!store.create_note(id, error)) return false;
  store_ = &store;
  id_.assign(id);
  part_path_ = store.audio_path(NoteState::Pending, id_) + ".part";
  final_path_ = store.audio_path(NoteState::Pending, id_);
  file_ = std::fopen(part_path_.c_str(), "wb");
  if (!file_) {
    error = "cannot open recording.wav";
    store_ = nullptr;
    return false;
  }
  // The cap is the smaller of the setting's seconds and what the card can take.
  // The byte budget is computed in 64 bits: 24 h at 48 KB/s is 4.1 GB, past a
  // 32-bit product, and the result is then held to kMaxNoteFileBytes and to the
  // card's own free space less the reserve.
  const uint64_t capped_s = std::min<uint64_t>(max_seconds, kMaxDurationS);
  const uint64_t wanted = capped_s * kBytesPerSecond;
  max_bytes_ = std::min<uint64_t>(wanted, budget);
  max_bytes_ -= max_bytes_ % (kChannels * (kBitsPerSample / 8));
  bytes_ = 0;
  full_ = false;
  space_out_ = false;
  failed_ = false;
  discarded_ = false;
  uint8_t header[44];
  wav_header(header, 0);
  if (std::fwrite(header, 1, sizeof(header), file_) != sizeof(header)) {
    error = "cannot write to the card";
    abort();
    return false;
  }
  return true;
}

void Capture::feed(const int16_t* samples, size_t count) {
  if (!file_ || !samples || !count || full_ || failed_) return;
  size_t want = count * sizeof(int16_t);
  if (bytes_ + want >= max_bytes_) {
    want = static_cast<size_t>(max_bytes_ - bytes_);  // land exactly on the cap
    want -= want % sizeof(int16_t);
    full_ = true;
  }
  if (want == 0) {
    full_ = true;
    return;
  }
  const size_t wrote = std::fwrite(samples, 1, want, file_);
  bytes_ += wrote;
  if (wrote == want) return;
  // A short write that is the card running out of room is not a failure: the
  // bytes it took are the note, and finish() commits them. ENOSPC is the volume,
  // EFBIG is what FAT32 answers at its own 4 GiB wall - both mean this file
  // cannot grow. Anything else (the card pulled, the mount gone) is a failure,
  // and finish() says so instead of committing a file whose tail nobody wrote.
  if (errno == ENOSPC || errno == EFBIG) {
    space_out_ = true;
    full_ = true;  // the caller's stop, through full()
    return;
  }
  failed_ = true;
}

uint32_t Capture::file_data_bytes() const {
  if (!file_) return static_cast<uint32_t>(bytes_);
  struct stat st{};
  if (fstat(fileno(file_), &st) != 0 || st.st_size < 44) return static_cast<uint32_t>(bytes_);
  uint64_t data = static_cast<uint64_t>(st.st_size) - 44;
  data -= data % (kChannels * (kBitsPerSample / 8));  // whole samples only
  return static_cast<uint32_t>(data);
}

bool Capture::finish(Meta& meta, std::string& error) {
  if (!file_) {
    error = "no recording";
    return false;
  }
  // What the card really holds, not what the buffer asked for: a card that
  // filled mid-note keeps whatever the last short write accepted, and that is
  // the note's length. The flush that fails on a full volume is expected here,
  // so the stream's error flag is cleared before the header is patched or it
  // would refuse the write that makes the file whole.
  const bool failed = failed_;
  uint32_t data_bytes = static_cast<uint32_t>(bytes_);
  if (!failed) {
    std::fflush(file_);
    std::clearerr(file_);
    data_bytes = file_data_bytes();
  }
  // A capture with less than a quarter second in it is not a note: it is a tap
  // let go, or a microphone that never delivered. Nothing is committed - the
  // partial file and the folder go, and no note.json is ever written - so no
  // empty row can reach pending/, the queue's no-recording sweep never sees a
  // fresh one, and the counts on the glass stay honest. abort() closes the file
  // and clears the member state; the discard flag is set after it because that
  // is this object's answer to the caller, not the file tree's.
  if (data_bytes < kMinNoteDataBytes) {
    abort();
    discarded_ = true;
    error = "no audio";
    return false;
  }
  uint8_t header[44];
  wav_header(header, data_bytes);
  // Patching the header overwrites bytes already on the card, so it costs no
  // space even on a full volume: the note a full card cut short still gets a
  // header that matches its bytes.
  bool patched = false;
  if (std::fseek(file_, 0, SEEK_SET) == 0 &&
      std::fwrite(header, 1, sizeof(header), file_) == sizeof(header) && std::fflush(file_) == 0) {
    patched = true;
  }
  close();
  if (failed || !patched) {
    error = "the card stopped accepting writes";
    abort();
    return false;
  }
  if (std::rename(part_path_.c_str(), final_path_.c_str()) != 0) {
    error = "cannot commit recording.wav";
    abort();
    return false;
  }
  meta.bytes = data_bytes;
  meta.duration_s = wav_seconds(data_bytes);
  // The contract's ceiling, in lockstep with kMaxDurationS: a note that ran to
  // the byte cap (12.4 h) or to the 24 h time cap still reports a storable
  // duration_seconds, and nothing here truncates it at 15 min any more.
  if (meta.duration_s > kMaxDurationS) meta.duration_s = kMaxDurationS;
  // The note is the card's now, not this object's. finish() used to leave the
  // committed paths behind, and start() opens with abort() - so the next
  // recording deleted the note recorded before it, which is a note already on
  // the card and, when a tap lands during a sync, the exact file the uploader is
  // reading. An abandoned capture still owns its files; a committed one does not.
  part_path_.clear();
  final_path_.clear();
  id_.clear();
  store_ = nullptr;
  bytes_ = data_bytes;
  return true;
}

void Capture::abort() {
  close();
  if (!part_path_.empty()) {
    std::remove(part_path_.c_str());
    std::remove(final_path_.c_str());
    if (store_) ::rmdir(store_->note_dir(NoteState::Pending, id_).c_str());
  }
  part_path_.clear();
  final_path_.clear();
  id_.clear();
  store_ = nullptr;
  bytes_ = 0;
  max_bytes_ = 0;
  full_ = false;
  space_out_ = false;
  failed_ = false;
  discarded_ = false;
}

}  // namespace hgp::notes
