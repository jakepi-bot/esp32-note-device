// The Bluetooth GATT tables in esp32/main/port_ble.cpp, checked without a radio.
//
// NimBLE's ble_gatts_chr_is_sane() rejects a characteristic whose access_cb is
// NULL, and ble_gatts_count_cfg() then returns BLE_HS_EINVAL before a single
// service is registered: PhoneLink::begin() logs "can't register the phone
// service" and Bluetooth is dead (console and note sync both). That is what
// 0588422 shipped, because the notify-only data characteristic had no callback.
//
// port_ble.cpp needs NimBLE and the ESP-IDF to compile, so the table is read
// here as source: every entry that names a UUID must name an access callback
// too, exactly the rule NimBLE applies at registration time. The parser fails
// loudly if it stops recognising the tables, so a refactor cannot turn these
// cases into silent passes.
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "check.hpp"

#ifndef HG_PORT_BLE_CPP
#error "HG_PORT_BLE_CPP must name the path of esp32/main/port_ble.cpp"
#endif

namespace {

// One `g_x_chrs[i] = {};` block, from its assignment to the next one.
struct Block {
  std::string name;  // "g_note_chrs[1]"
  std::string text;  // the field assignments that follow, comments included
};

std::string read_source() {
  std::ifstream in(HG_PORT_BLE_CPP);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

// The tables are filled in as contiguous statements: `g_chrs[0] = {};` then its
// fields, then the next entry. Find each entry's first line and cut the file
// into blocks on those boundaries.
std::vector<Block> parse_tables(const std::string& src) {
  struct Start {
    size_t at;
    std::string name;
  };
  std::vector<Start> starts;
  for (const char* table : {"g_chrs[", "g_note_chrs["}) {
    const std::string needle = table;
    for (size_t at = src.find(needle); at != std::string::npos; at = src.find(needle, at + 1)) {
      // Only `table<digit>] = {};` starts an entry; the rest are references
      // such as `g_svcs[1].characteristics = g_note_chrs;`.
      size_t p = at + needle.size();
      if (p >= src.size() || src[p] < '0' || src[p] > '9') continue;
      while (p < src.size() && src[p] >= '0' && src[p] <= '9') ++p;
      if (src.compare(p, 4, "] = ") != 0) continue;
      starts.push_back({at, src.substr(at, p + 1 - at)});
    }
  }
  // Source order: both tables are filled in one after the other in begin().
  for (size_t i = 1; i < starts.size(); ++i) {
    for (size_t j = i; j > 0 && starts[j - 1].at > starts[j].at; --j) std::swap(starts[j - 1], starts[j]);
  }
  std::vector<Block> blocks;
  for (size_t i = 0; i < starts.size(); ++i) {
    size_t end = i + 1 < starts.size() ? starts[i + 1].at : src.size();
    // A characteristic's fields stop where the service tables begin: g_svcs[].uuid
    // must not be read as this entry's own UUID.
    const size_t svcs = src.find("g_svcs[", starts[i].at);
    if (svcs != std::string::npos && svcs < end) end = svcs;
    blocks.push_back({starts[i].name, src.substr(starts[i].at, end - starts[i].at)});
  }
  return blocks;
}

// `.\<field> = <value>;` on one line, without the trailing semicolon.
std::string field_value(const Block& block, const std::string& field) {
  const std::string needle = "." + field + " = ";
  size_t at = block.text.find(needle);
  if (at == std::string::npos) return "";
  at += needle.size();
  const size_t end = block.text.find(';', at);
  if (end == std::string::npos) return "";
  return block.text.substr(at, end - at);
}

TEST("the GATT tables are recognisable") {
  const std::vector<Block> blocks = parse_tables(read_source());
  int entries = 0;
  for (const Block& b : blocks) {
    if (!field_value(b, "uuid").empty()) ++entries;
  }
  // Two services: the phone console (command, reply) and the note sync
  // (control, data). Fewer entries than this means the parser, not the tables,
  // changed, and every case below has stopped checking anything.
  CHECK_EQ(entries, 4);
  if (entries != 4) {
    for (const Block& b : blocks) std::printf("  saw %s: uuid=\"%s\"\n", b.name.c_str(), field_value(b, "uuid").c_str());
  }
}

TEST("every characteristic names an access callback") {
  int checked = 0;
  for (const Block& b : parse_tables(read_source())) {
    // An entry with no UUID is the NULL terminator; NimBLE stops there and
    // never asks it anything.
    if (field_value(b, "uuid").empty()) continue;
    ++checked;
    if (field_value(b, "access_cb").empty()) {
      // Naming the offender is the point: a bare `CHECK` here would print a
      // line number in this test, not the characteristic that NimBLE rejects.
      check::fail(__FILE__, __LINE__,
                  b.name + " has a UUID and no access_cb: ble_gatts_count_cfg() returns BLE_HS_EINVAL");
    }
  }
  CHECK_EQ(checked, 4);
}

TEST("the note data characteristic is notify-only behind on_note_access") {
  const std::vector<Block> blocks = parse_tables(read_source());
  const Block* data = nullptr;
  for (const Block& b : blocks) {
    if (b.name == "g_note_chrs[1]") data = &b;
  }
  CHECK(data != nullptr);
  if (!data) return;
  const std::string flags = field_value(*data, "flags");
  CHECK(flags.find("BLE_GATT_CHR_F_NOTIFY") != std::string::npos);
  // A READ here would hand a note to anything in radio range that subscribed;
  // the data characteristic is write-never and read-never by design.
  CHECK(flags.find("BLE_GATT_CHR_F_READ") == std::string::npos);
  CHECK(flags.find("BLE_GATT_CHR_F_WRITE") == std::string::npos);
  // The callback is a table validator only: it rejects every operation.
  CHECK_EQ(field_value(*data, "access_cb"), std::string("on_note_access"));
  CHECK(!field_value(*data, "val_handle").empty());
}

}  // namespace
