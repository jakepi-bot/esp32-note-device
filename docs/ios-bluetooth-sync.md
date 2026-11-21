# Voice notes off the gadget with an iPhone

The e-paper notes gadget records without any network. When it has no Wi-Fi, the
notes still get out over Bluetooth: an iPhone takes the audio off the card and
posts it to your server, which transcribes it and writes it into Obsidian.

Bluetooth is not an add-on for this board: the ePaper-1.54 build always has the
note service compiled in, running beside the console channel on the same radio.
Wi-Fi is the fast path; Bluetooth is the one that works in a car, in a lift, in
a basement.

```
  gadget ──BLE──▶ iPhone (Bluefy, the relay page) ──HTTPS──▶ your server ──▶ Obsidian
```

- The server side, in Docker with a step-by-step guide:
  [deploy/bluetooth-sync](../deploy/bluetooth-sync/README.md).
- The wire format and the device's rules: [ble-note-sync.md](ble-note-sync.md).
- The GATT table: [protocol.md § Note sync](protocol.md#note-sync-voice-notes-off-the-card).

## What you need

| | |
|---|---|
| The gadget | ePaper-1.54 with a build from this repository (the note service is always in it) |
| An iPhone | with **Bluefy** from the App Store — Safari has no Web Bluetooth, and no other iOS browser does |
| The server | running and reachable from the phone on the same network ([deploy/bluetooth-sync](../deploy/bluetooth-sync/README.md)) |
| The device id and key | shown by the server's enroll step, or `hermes gadget devices` on a Hermes host |

## First time: pair this phone

Pairing is a conversation between three things, and the gadget has the last word.

1. **Open the pairing window on the gadget.** Settings → **Phone app** → the
   window opens and the screen says *Pair a phone*. (Over the USB console:
   `phone pair`. Or, from another Bluetooth phone, the device settings menu.)
   The window lasts two minutes; if it closes, open it again.

2. **Load the relay page on the iPhone.** In Bluefy open
   `https://<your-server-name>:8443/` — for example `https://hermes.local:8443/`.
   The page is "Hermes note relay"; the notes endpoint is already filled in
   because the page came from that same server.

3. **Fill in the device.** Type the **device id** (`hg-…`) and the **device
   key** (base64) into the page. They stay in the page's local storage on the
   phone. Treat the phone like the board: the key can post notes as it.

4. **Tap Connect.** Bluefy asks which device to use: pick the gadget (its name is
   on the device's screen, `Bluetooth: <name>`).

5. **Allow it on the gadget.** The screen changes to *Allow this phone?* and
   shows the phone's Bluetooth address. **Tap the screen** to allow it. (On a
   board without a touch screen, press the talk control; over the console,
   `phone allow`. To refuse: swipe down, `phone deny`, or just wait 20 seconds.)

6. **Confirm the code.** The gadget now shows a six-digit code, and iOS shows
   the same six digits in a *Bluetooth Pairing Request*. Tap **Pair** only if
   they match — that is the passkey step of LE Secure Connections, on top of the
   acknowledgement in step 5.

7. **The page connects**, lists the notes waiting on the card, and you can send
   them.

Pairing is kept: from then on the gadget advertises for that phone and step 5–6
do not repeat. Up to three phones can be paired; the fourth forgets the oldest.
`phone forget` on the console removes them all and drops a connected phone.

### Why the gadget asks first

A pairing window alone is not a decision — a phone in radio range can ask while
the window is open, and a six-digit code on a screen that nobody is looking at
decides nothing. So the gadget asks, names the phone, and waits for someone
standing there to allow it:

- nothing is shared, and no passkey is generated, until the tap;
- a refused or ignored ask drops that link and the phone's pairing fails with an
  authentication error, so the phone cannot bond and try again later;
- one phone at a time: a second phone asking while one waits is refused;
- the window is still open after a refusal, so the same phone can ask again if
  the tap was a mistake.

Combined with the passkey step, a phone has to be both *allowed* at the device
and *confirmed* by the code, which is what makes it safe to leave the pairing
window open for two minutes.

## Sending a note

1. Record on the gadget as usual (the brightness control, or hold the screen).
   The note lands in `pending/` on the card.
2. On the page, tap **List notes** (it also refreshes on connect). Each note
   shows its size, length and recorded time.
3. **Send** takes one note: the device streams it in blocks, the page checks
   every block's CRC-32 and the whole file's SHA-256, posts it to the server,
   and only when the server answers 2xx tells the device the note is done — the
   device then moves it to `sent/`.
4. **Fetch every note** walks the list. The page stays open while it runs: iOS
   suspends a backgrounded browser, and the transfer stops with the page.

If the link drops, the bytes already received stay where they are; tapping
**Send** again resumes from that offset, and nothing is deleted from the gadget
until the server has the note.

## After the first time

- The gadget advertises about once a second while a phone is paired, so the page
  finds it without opening the pairing window again.
- Open the page, tap Connect, send. No code, no tapping Allow.
- If the phone forgets the gadget ("Forget This Device" in iOS Bluetooth
  settings) or the gadget runs `phone forget`, pair again from step 1.

## Troubleshooting

| What you see | What to do |
|---|---|
| Bluefy says Web Bluetooth is unavailable | the page is not on HTTPS; use the `https://` address, and trust the server's certificate (deploy README step 7) |
| `no gadget chosen` | the pairing window closed (two minutes), or the device is asleep: press a key, open Settings → Phone app again |
| `Connect` fails with `authentication`/`insufficient encryption` | the gadget did not allow this phone: open the pairing window and tap Allow when it asks |
| The gadget asks, you tap Allow, iOS shows a different code | something else is in the middle; tap Cancel, then `phone forget` on the gadget and start again |
| The page connects but lists nothing | no notes are waiting: they may already be in `sent/` after a previous sync |
| A note stops at a percentage | keep the page in front and tap Send again; it resumes where it stopped |
| `401 device … is not enrolled` | the key in the page does not match a key the server knows |

## What this does not do

- No background sync: the page must be open, in front, with the phone awake.
- No notes without the phone: the gadget never uploads over Bluetooth itself; it
  needs Bluefy holding the link.
- Nothing is deleted from the card until the server has the note, and the card's
  retention policy only ever deletes notes that were already synced.
