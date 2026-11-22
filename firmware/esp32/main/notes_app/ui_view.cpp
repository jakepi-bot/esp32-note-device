// See notes_app/ui_view.hpp: the screens' copy, decided from a snapshot.
#include "notes_app/ui_view.hpp"

#include <cstdio>
#include <cstring>
#include <ctime>

#include "notes/notes_queue.hpp"  // parse_iso8601, the one ISO-8601 reader
#include "notes_app/strings.hpp"

namespace hgp::notesui {
namespace {

void copy_out(const char* text, char* out, size_t size) {
  std::snprintf(out, size, "%s", text ? text : "");
}

}  // namespace

const char* month_name(unsigned month) {
  static const char* kMonths[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  return month >= 1 && month <= 12 ? kMonths[month - 1] : "---";
}

void format_day(const Stamp& stamp, char out[7]) {
  if (!stamp.valid) {
    copy_out("no date", out, 7);
    return;
  }
  std::snprintf(out, 7, "%u %s", static_cast<unsigned>(stamp.day), month_name(stamp.month));
}

void format_when(const Stamp& stamp, char out[13]) {
  if (!stamp.valid) {
    copy_out("no date", out, 13);
    return;
  }
  std::snprintf(out, 13, "%u %s %02u:%02u", static_cast<unsigned>(stamp.day),
                month_name(stamp.month), static_cast<unsigned>(stamp.hour),
                static_cast<unsigned>(stamp.minute));
}

void format_clock(const Stamp& stamp, char out[6]) {
  const unsigned hour = stamp.hour % 24, minute = stamp.minute % 60;
  std::snprintf(out, 6, "%02u:%02u", hour, minute);
}

void format_duration(uint32_t seconds, char out[8]) {
  // 99:59 is the ceiling the slot takes, and a note may now run for hours, so a
  // longer time is clamped here for the glass only (the note keeps its real
  // length; the cap line, format_of, does show hours).
  const uint32_t shown = seconds > 99u * 60u + 59u ? 99u * 60u + 59u : seconds;
  std::snprintf(out, 8, "%u:%02u", static_cast<unsigned>(shown / 60),
                static_cast<unsigned>(shown % 60));
}

void format_of(uint32_t seconds, char out[11]) {
  // The cap slot takes "OF 99:59", and a note's cap is now the whole card
  // (DeviceConfig::note_max_s defaults to 86400 s), so anything longer is shown
  // in hours and minutes: "OF 24:00" reads as "as long as the card lasts", where
  // a clamped "OF 99:59" would be a lie.
  if (seconds > 99u * 60u + 59u) {
    // Clamped to what the slot can hold (999 h), so the compiler can bound the
    // conversion instead of refusing it: nothing the firmware records is close.
    uint32_t hours = seconds / 3600u;
    if (hours > 999u) hours = 999u;
    std::snprintf(out, 11, "OF %u:%02u", static_cast<unsigned>(hours),
                  static_cast<unsigned>((seconds / 60u) % 60u));
    return;
  }
  std::snprintf(out, 11, "OF %u:%02u", static_cast<unsigned>(seconds / 60u),
                static_cast<unsigned>(seconds % 60u));
}

Stamp stamp_from_unix(int64_t unix_s) {
  Stamp out;
  if (unix_s <= 0) return out;
  const std::time_t t = static_cast<std::time_t>(unix_s);
  std::tm tm{};
#if defined(_WIN32)
  if (gmtime_s(&tm, &t) != 0) return out;
#else
  if (localtime_r(&t, &tm) == nullptr) return out;
#endif
  out.year = static_cast<uint16_t>(tm.tm_year + 1900);
  out.month = static_cast<uint8_t>(tm.tm_mon + 1);
  out.day = static_cast<uint8_t>(tm.tm_mday);
  out.hour = static_cast<uint8_t>(tm.tm_hour);
  out.minute = static_cast<uint8_t>(tm.tm_min);
  out.valid = true;
  return out;
}

bool stamp_from_iso(std::string_view iso8601, Stamp& out) {
  int64_t unix_s = 0;
  if (!notes::parse_iso8601(iso8601, unix_s)) return false;
  out = stamp_from_unix(unix_s);
  return out.valid;
}

void format_capacity(uint64_t bytes, char out[12]) {
  // 62.9G: one decimal of a gigabyte. Above 999.9 it reads "999G" so the
  // string never outgrows About's value column.
  const double gb = static_cast<double>(bytes) / 1e9;
  if (gb >= 999.5) {
    copy_out("999G", out, 12);
    return;
  }
  std::snprintf(out, 12, "%.1fG", gb);
}

HomeView build_home(const Snapshot& snap) {
  HomeView view;
  view.wifi_up = snap.wifi_up;
  view.battery_pct = snap.battery_pct;
  view.external = snap.external;

  // Hero: the highest-priority true statement about the device (§4.2).
  if (!snap.card) {
    view.hero = home::kNoCard;
  } else if (snap.low_power) {
    view.hero = home::kLowPower;
  } else if (snap.upload_active) {
    view.hero = home::kUploading;
  } else if (snap.upload_failed) {
    view.hero = home::kNoSync;
  } else if (snap.count_pending > 0 && !snap.wifi_up) {
    view.hero = home::kOffline;
  } else if (!snap.clock_set) {
    view.hero = home::kNoTime;
  } else {
    view.hero = home::kReady;
  }

  // The band answers what is queued, and is blank when the hero said it.
  if (!snap.card) {
    view.band[0] = '\0';
  } else if (snap.count_pending > 0) {
    std::snprintf(view.band, sizeof view.band, band::kToSend,
                  static_cast<unsigned>(snap.count_pending > 99 ? 99 : snap.count_pending));
  } else {
    copy_out(band::kAllSent, view.band, sizeof view.band);
  }

  // The clock and the radio's mark ride the same band (t_3beb371a §4): the time
  // in its own slot left of the battery, the mark in the Wi-Fi glyph's box. Both
  // are copies, so the frame stays self-contained like the rest of the snapshot.
  if (snap.clock_set && snap.clock_text[0] != '\0') copy_out(snap.clock_text, view.clock, sizeof view.clock);
  view.bt = snap.bt;

  // One line, context first, and never a restatement of the band.
  if (snap.low_power) {
    copy_out(sub::kPlugIn, view.sub, sizeof view.sub);
  } else if (!snap.card) {
    copy_out(sub::kNothingLost, view.sub, sizeof view.sub);
  } else if (snap.upload_active && snap.upload_total > 0) {
    std::snprintf(view.sub, sizeof view.sub, sub::kOfN, static_cast<unsigned>(snap.upload_done),
                  static_cast<unsigned>(snap.upload_total));
  } else if (const NoteStub* newest = stub_at(snap, 0)) {
    // The newest note's local minute: a timestamp, never a clock (§3.2).
    unsigned hour = 0, minute = 0;
    if (std::sscanf(newest->clock, "%u:%u", &hour, &minute) == 2) {
      std::snprintf(view.sub, sizeof view.sub, sub::kLast, hour, minute);
    } else {
      view.sub[0] = '\0';
    }
  } else {
    copy_out(sub::kBootToRecord, view.sub, sizeof view.sub);
  }

  if (snap.upload_active && snap.upload_total > 0) {
    unsigned pct = static_cast<unsigned>(snap.upload_done) * 100u / snap.upload_total;
    view.bar_pct = static_cast<uint8_t>(pct > 100 ? 100 : pct);
  }
  return view;
}

MenuView build_menu(const Snapshot& snap, uint8_t selected) {
  MenuView view;
  view.selected = selected < kMenuCount ? selected : 0;
  for (uint8_t i = 0; i < kMenuCount; ++i) {
    const MenuItem item = static_cast<MenuItem>(i);
    view.rows[i].label = menu_label(item);
    switch (item) {
      case MenuItem::Notes:
        std::snprintf(view.rows[i].value, sizeof view.rows[i].value, "%u",
                      static_cast<unsigned>(snap.card ? snap.notes_total() : 0));
        break;
      case MenuItem::SyncNow:
        std::snprintf(view.rows[i].value, sizeof view.rows[i].value, "%u",
                      static_cast<unsigned>(snap.count_pending));
        break;
      case MenuItem::Wifi:
        copy_out(snap.wifi_up ? menu::kWifiOn : menu::kWifiOff, view.rows[i].value,
                 sizeof view.rows[i].value);
        break;
      case MenuItem::Phone:
      case MenuItem::About:
      case MenuItem::Back:
      case MenuItem::Count:
        view.rows[i].value[0] = '\0';  // a deliberate empty column
        break;
    }
  }
  return view;
}

uint8_t list_visible_first(uint16_t selected, uint16_t count) {
  if (count == 0) return 0;
  if (selected >= count) selected = static_cast<uint16_t>(count - 1);
  return static_cast<uint8_t>(selected < kListRows ? 0 : selected - (kListRows - 1));
}

ListView build_list(const Snapshot& snap, uint16_t selected) {
  ListView view;
  view.count = snap.note_count;
  view.empty = snap.note_count == 0;
  if (view.empty) {
    view.selected = 0;
    view.key_line = nullptr;
    return view;
  }
  if (selected >= snap.note_count) selected = static_cast<uint16_t>(snap.note_count - 1);
  view.selected = static_cast<uint8_t>(selected);
  view.first = list_visible_first(selected, snap.note_count);
  for (uint8_t row = 0; row < kListRows; ++row) {
    const uint16_t index = static_cast<uint16_t>(view.first + row);
    if (index >= snap.note_count) break;
    const NoteStub* note = stub_at(snap, index);
    if (note == nullptr) break;  // outside the window the caller filled
    std::snprintf(view.row[row].when, sizeof view.row[row].when, "%s %s", note->day, note->clock);
    view.row[row].mark = note->mark == NoteMark::Pending ? '*' : note->mark == NoteMark::Failed ? '!' : ' ';
    ++view.rows;
  }
  // The mark key lives on the title line, so it costs no vertical space (§6.2).
  if (snap.count_failed > 0) {
    view.key_line = list::kFailed;
  } else if (snap.count_pending > 0) {
    view.key_line = list::kToSend;
  } else if (snap.list_truncated) {
    // The list is the newest kMaxListRows of a bigger card: the title line says
    // so instead of printing a count that is not the card's (QA D7).
    std::snprintf(view.key_count, sizeof view.key_count, "%u+", static_cast<unsigned>(snap.note_count));
  } else {
    std::snprintf(view.key_count, sizeof view.key_count, "%u", static_cast<unsigned>(snap.note_count));
  }
  return view;
}

bool build_note(const Snapshot& snap, uint16_t index, uint8_t selected, NoteView& out) {
  const NoteStub* note = stub_at(snap, index);
  if (note == nullptr) return false;
  out = NoteView{};
  copy_out(note->day, out.day, sizeof out.day);
  char duration[8];
  format_duration(note->duration_s, duration);
  std::snprintf(out.when, sizeof out.when, "%s · %.5s", note->clock, duration);
  out.state = note->mark == NoteMark::Pending ? note::kPending
              : note->mark == NoteMark::Failed ? note::kFailed
                                              : note::kSent;
  out.selected = selected;
  return true;
}

bool build_confirm(const Snapshot& snap, uint16_t index, ConfirmView& out) {
  const NoteStub* note = stub_at(snap, index);
  if (note == nullptr) return false;
  std::snprintf(out.when, sizeof out.when, "%s %s", note->day, note->clock);
  return true;
}

// The Phone screen: the port's phase as one word, the window's clock, and the
// pairing table. A refusal is a word too - "PAIRING FULL" is the reason, in the
// same slot the state goes in, so the screen never says nothing (t_9972e2a1).
PhoneView build_phone(const Snapshot& snap, const PhoneStatus& status) {
  PhoneView view;
  copy_out(snap.device, view.name, sizeof view.name);
  switch (status.phase) {
    case PhonePhase::Advertising:
      view.state = phone::kAdvertising;
      break;
    case PhonePhase::Waiting:
      view.state = phone::kWaiting;
      break;
    case PhonePhase::Connected:
      view.state = phone::kConnected;
      break;
    case PhonePhase::Closed:
      view.state = phone::kClosed;
      break;
    case PhonePhase::Refused:
      view.state = status.refusal == PhoneRefusal::TableFull ? phone::kTableFull
                   : status.refusal == PhoneRefusal::RadioBusy ? phone::kBusy
                                                              : phone::kBleOff;
      break;
  }
  // The countdown is a real deadline, so it is drawn only while one is running:
  // a connected phone holds the window open and a refusal has no clock at all.
  // Rounded up, so a fresh window reads 5:00 rather than 4:59.
  if (status.left_ms != 0 && status.phase != PhonePhase::Connected) {
    format_duration((status.left_ms + 999) / 1000, view.countdown);
  }
  if (status.slots != 0) {
    std::snprintf(view.table, sizeof view.table, phone::kTable, static_cast<unsigned>(status.paired),
                  static_cast<unsigned>(status.slots));
  }
  if (status.phase == PhonePhase::Connected) view.note = phone::kHolding;
  else if (status.phase == PhonePhase::Closed) view.note = phone::kOff;
  return view;
}

AboutView build_about(const Snapshot& snap) {
  AboutView view;
  view.label[0] = about::kBattery;
  if (snap.battery_pct >= 0) {
    std::snprintf(view.value[0], sizeof view.value[0], "%u%%",
                  static_cast<unsigned>(snap.battery_pct));
  } else {
    copy_out("?", view.value[0], sizeof view.value[0]);
  }
  view.label[1] = about::kFirmware;
  copy_out(snap.firmware, view.value[1], sizeof view.value[1]);
  view.label[2] = about::kDevice;
  copy_out(snap.device, view.value[2], sizeof view.value[2]);
  view.label[3] = about::kCard;
  if (snap.card) {
    format_capacity(snap.card_free_bytes, view.value[3]);
  } else {
    copy_out("-", view.value[3], sizeof view.value[3]);
  }
  view.label[4] = about::kNotes;
  std::snprintf(view.value[4], sizeof view.value[4], "%u",
                static_cast<unsigned>(snap.notes_total()));
  return view;
}

}  // namespace hgp::notesui
