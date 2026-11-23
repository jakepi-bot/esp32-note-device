// The bond store, as the Unpair screen and its tests reach it.
//
// The screens decide what the glass shows; this is the one thing on them that
// changes state outside the glass. The interface is here, in the UI layer, for
// the same reason ui_view.hpp is: a host test can hand the screen a table with
// two phones in it and count the deletes, which is how "the erase is called
// exactly once" is a test rather than a comment. The device implements it over
// PhoneLink (notes_app/ui_device.cpp's RadioBonds, port_ble.cpp's store calls).
#pragma once

#include <cstdint>
#include <cstring>

#include "notes_app/ui_view.hpp"

namespace hgp::notesui {

class BondPort {
 public:
  virtual ~BondPort() = default;
  // The bonded phones, written into `out` (at most `capacity` of them). Returns
  // how many were written; the order is the store's, not sorted.
  virtual uint8_t list(Peer* out, uint8_t capacity) = 0;
  // Forget this phone's bond. True when the store no longer holds it, so a
  // refused delete never takes a row off the glass.
  virtual bool erase(const Peer& peer) = 0;
};

// Read the table into the snapshot's peer window. The caller publishes when it
// moved; nothing here draws.
inline uint8_t load_peers(BondPort& port, Snapshot& snap) {
  Peer found[kMaxPeerRows];
  const uint8_t n = port.list(found, kMaxPeerRows);
  if (n != 0) std::memcpy(snap.peers, found, sizeof(Peer) * n);
  snap.peer_count = n;
  return n;
}

// The Unpair confirm's hold: one erase for the phone the question named, then
// the table is read again so the frame that follows draws one row fewer. False
// (and nothing changed) when the index is past the table or the store refused.
inline bool unpair_peer(BondPort& port, Snapshot& snap, uint8_t index) {
  if (index >= snap.peer_count) return false;
  if (!port.erase(snap.peers[index])) return false;
  load_peers(port, snap);
  return true;
}

}  // namespace hgp::notesui
