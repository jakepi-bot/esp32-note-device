// Every English word the notes UI draws, and nothing else.
//
// This replaces the core's i18n table: there is no locale layer and no pair of
// translated strings (docs/notes-ui-spec.md §14). The twelve rows the spec marks
// `ui_keys.cpp` - menu_label()'s nine and action_label()'s three - stay in
// ui_keys.cpp, so no row is written twice; the renderer asks ui_keys for those.
// Every string defined here has its own §14 row; a new one needs a new row
// there, or a line here naming the row it shares and why.
//
// Rules the table keeps (§14.1):
//   * uppercase means state or a key legend, sentence case means a thing you
//     act on;
//   * no plurals machinery - "%u TO SEND" is right at 0, 1 and 40;
//   * the only punctuation is : . · ! ? * (the font has no U+2026 and draws a
//     '?' for one, so nothing is ever truncated with an ellipsis);
//   * a value that could outgrow its slot is capped at the source, never cut
//     at draw time.
#pragma once

namespace hgp::notesui {

// Home, zone B - the hero. Scale 3, one word, 5..9 columns (§4.2).
namespace home {
inline constexpr const char* kReady = "READY";
inline constexpr const char* kUploading = "UPLOADING";
inline constexpr const char* kOffline = "OFFLINE";
inline constexpr const char* kNoCard = "NO CARD";
inline constexpr const char* kLowPower = "LOW POWER";
inline constexpr const char* kNoSync = "NO SYNC";
inline constexpr const char* kNoTime = "NO TIME";
}  // namespace home

// Home, zone A - the queue slot, 10 columns (§4.2).
namespace band {
inline constexpr const char* kToSend = "%u TO SEND";
inline constexpr const char* kAllSent = "ALL SENT";
}  // namespace band

// Home, the one line under the hero (§4.2).
namespace sub {
inline constexpr const char* kOfN = "%u OF %u SENT";
inline constexpr const char* kLast = "LAST %02u:%02u";
inline constexpr const char* kBootToRecord = "BOOT TO RECORD";
inline constexpr const char* kNothingLost = "NOTHING LOST";
inline constexpr const char* kPlugIn = "PLUG IN SOON";
// A capture the stop threw away because it held no audio: the reason the log
// gives, in the space of the line. 14 columns, the width of BOOT TO RECORD.
inline constexpr const char* kNoAudioSaved = "NO AUDIO SAVED";
}  // namespace sub

// Footers: one binding each, two only where both keys are load-bearing (§3.3).
namespace key {
inline constexpr const char* kBootRecord = "BOOT: REC";
// Home's hint line, above the rule: the menu is a hold, and nothing on the glass
// said so (Jake, 2026-11-22).
inline constexpr const char* kHoldMenuHint = "hold BOOT for menu";
// Home's second footer line. The first version of the hint was scale 1 (6 px)
// squeezed above the rule, which is not readable at arm's length (Jake,
// 2026-11-23). It is a footer line now, at the body size, where the other key
// legend already lives.
inline constexpr const char* kHoldMenu = "HOLD: MENU";
inline constexpr const char* kBootNext = "BOOT: next";
inline constexpr const char* kBootStop = "BOOT: stop";
inline constexpr const char* kPwrSelect = "PWR: select";
inline constexpr const char* kPwrOpen = "PWR: open";
inline constexpr const char* kBootAllow = "BOOT: allow";
inline constexpr const char* kPwrRefuse = "PWR: refuse";
inline constexpr const char* kHoldDiscard = "HOLD: discard";
inline constexpr const char* kHoldDelete = "HOLD: delete";
inline constexpr const char* kHoldWipe = "HOLD: wipe";
inline constexpr const char* kHoldFormat = "HOLD: format";
inline constexpr const char* kHoldBack = "HOLD: back";
inline constexpr const char* kHoldNothing = "HOLD: nothing";
inline constexpr const char* kTapKeep = "TAP: keep";
// The busy face's one line: neither key does anything while the card is being
// emptied, so the footer says what the person should do - nothing (t_0070a731).
inline constexpr const char* kPleaseWait = "PLEASE WAIT";
// The queued recording face: the press ends the wait, it does not keep anything.
inline constexpr const char* kBootCancel = "BOOT: cancel";
inline constexpr const char* kHoldCancel = "HOLD: cancel";
inline constexpr const char* kPwrClose = "PWR: close";
// The Unpair list's select: the row answers with a question, not with the act.
inline constexpr const char* kPwrUnpair = "PWR: unpair";
}  // namespace key

// Durations, shared by Recording and Playing: "%u:%02u" is M:SS and
// "OF %u:%02u" is the cap/denominator line.
namespace fmt {
inline constexpr const char* kClock = "%u:%02u";
inline constexpr const char* kOfCap = "OF %u:%02u";
}  // namespace fmt

namespace recording {
inline constexpr const char* kLabel = "RECORDING";
// The queued face: the tap is kept, the capture has not begun. "QUEUED" is the
// hero, so the glass never shows a counter for a note that is not being made.
inline constexpr const char* kQueued = "QUEUED";
inline constexpr const char* kWaiting = "FOR THE UPLOAD";
}  // namespace recording

// The sleeping face: the last frame the panel keeps, so it is also the only one
// a person reads with the device in their hand and no way to press anything.
// "zzz" is a mark, not a word; the line says what the panel is and how it got
// there, in one line because that is all an off device should be saying.
namespace sleep {
inline constexpr const char* kZzz = "zzz";
// Short enough to sit at the body size: 12 characters is 144 px of the panel's
// 200, and the old 21-character caption only fitted at 6 px, which nobody could
// read (Jake, 2026-11-23).
// Two lines, because one line at the hero size does not fit 200 px: "hold PWR"
// is 8 characters (144 px) and the "2 seconds" rides under it at the body size.
inline constexpr const char* kHint = "hold PWR";
inline constexpr const char* kHint2 = "2 seconds";
}  // namespace sleep

namespace menu {
// The screen's own title. §14 lists it as `menu.title`; it is drawn from here
// so no literal lives in the renderer.
inline constexpr const char* kTitle = "MENU";
inline constexpr const char* kWifiOn = "on";
inline constexpr const char* kWifiOff = "off";
}  // namespace menu

namespace list {
inline constexpr const char* kTitle = "NOTES";
inline constexpr const char* kToSend = "* TO SEND";
inline constexpr const char* kFailed = "! FAILED";
inline constexpr const char* kEmpty = "NO NOTES YET";
}  // namespace list

namespace note {
inline constexpr const char* kTitle = "NOTE";
inline constexpr const char* kPending = "to send";
inline constexpr const char* kSent = "sent";
inline constexpr const char* kFailed = "failed";
}  // namespace note

namespace confirm {
inline constexpr const char* kHero = "DELETE?";
// The menu's two destructive rows. Scale 3 is 195 px wide at 11 columns, so the
// hero stays at ten or fewer and the rest of the sentence goes on the body
// lines, where the count and what is lost are stated plainly.
inline constexpr const char* kWipeHero = "WIPE NOTES";
inline constexpr const char* kWipeCount = "%u NOTES GO";
inline constexpr const char* kWipeKeeps = "THE CARD STAYS";
inline constexpr const char* kFormatHero = "FORMAT";
inline constexpr const char* kFormatTarget = "THE SD CARD";
inline constexpr const char* kFormatLoss = "NOTES AND LOG GO";
}  // namespace confirm

// The busy face the work runs behind (t_0070a731). The confirm's copy is the
// question; this is the answer in progress - the same number it promised, now
// moving, and the same warning, now while the card is actually being written.
// Ten columns at most at scale 3, fourteen at scale 2 (the 176 px body width).
namespace busy {
inline constexpr const char* kWipeHero = "WIPING";
inline constexpr const char* kFormatHero = "FORMATTING";
inline constexpr const char* kCount = "%u OF %u";
// The first progress call carries the total; until then the card is being read.
inline constexpr const char* kCounting = "READING CARD";
inline constexpr const char* kCardStays = "THE CARD STAYS";
inline constexpr const char* kFormatTarget = "THE SD CARD";
inline constexpr const char* kDontUnplug = "DO NOT UNPLUG";
}  // namespace busy

namespace pairing {
inline constexpr const char* kTitle = "PAIRING";
inline constexpr const char* kSame = "Same as phone?";
}  // namespace pairing

// The Phone screen: the Bluetooth window a person opened from the menu. The
// state words are uppercase, one word each, and the refusals name themselves -
// a window that did not open has to say why (t_9972e2a1).
namespace phone {
inline constexpr const char* kTitle = "PHONE";
inline constexpr const char* kAdvertising = "ADVERTISING";
inline constexpr const char* kWaiting = "WAITING";
inline constexpr const char* kConnected = "CONNECTED";
inline constexpr const char* kClosed = "WINDOW CLOSED";
inline constexpr const char* kTableFull = "PAIRING FULL";
inline constexpr const char* kBusy = "RADIO BUSY";
inline constexpr const char* kBleOff = "BLE OFF";
inline constexpr const char* kHolding = "Held open";
inline constexpr const char* kOff = "Radio off";
inline constexpr const char* kTable = "%u OF %u PAIRED";
}  // namespace phone

// The Unpair screen: the bonded phones, and the question one of them gets. An
// empty table says what to do about it - the Phone row is where a phone is
// paired - rather than leaving a blank page under a title.
namespace unpair {
inline constexpr const char* kTitle = "UNPAIR";
inline constexpr const char* kCount = "%u PAIRED";
inline constexpr const char* kEmpty = "NOTHING PAIRED";
inline constexpr const char* kEmptyHint = "open PHONE to pair";
inline constexpr const char* kConfirmHero = "UNPAIR?";
inline constexpr const char* kHold = "HOLD: unpair";
}  // namespace unpair

namespace about {
inline constexpr const char* kTitle = "ABOUT";
inline constexpr const char* kBattery = "Battery";
inline constexpr const char* kFirmware = "Firmware";
inline constexpr const char* kDevice = "Device";
inline constexpr const char* kCard = "Card free";
inline constexpr const char* kNotes = "Notes";
}  // namespace about

// The month abbreviations the list and the note screen print ("21 Nov").
// January is index 0; a bad month reads "---" rather than a wrong month.
const char* month_name(unsigned month);

}  // namespace hgp::notesui
