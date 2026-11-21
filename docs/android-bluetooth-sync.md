# Voice notes off the gadget with an Android phone

The e-paper notes device records without any network. When it has no Wi-Fi, the
notes still get out over Bluetooth: a phone takes the audio off the card and
posts it to your server, which transcribes it and writes it into Obsidian. On
Android this needs **Chrome** and nothing from the Play Store beyond it.

```
  gadget ──BLE──▶ Chrome for Android (the relay page) ──HTTPS──▶ server ──▶ Obsidian
```

- The server side, in Docker with a step-by-step guide:
  [deploy/bluetooth-sync](../deploy/bluetooth-sync/README.md).
- The wire format and the device's rules: [ble-note-sync.md](ble-note-sync.md).
- The iPhone version of this page: [ios-bluetooth-sync.md](ios-bluetooth-sync.md).

## What you need

| | |
|---|---|
| The gadget | the notes firmware from this repository (the Bluetooth note service is always in it) |
| A phone | Android with **Chrome** (Chrome is the browser that ships Web Bluetooth; Samsung Internet and Firefox do not) |
| The server | running and reachable from the phone on the same network, over **HTTPS** ([deploy/bluetooth-sync](../deploy/bluetooth-sync/README.md)) |
| The device id and key | from the server's enroll step, or `hermes gadget devices` on a Hermes host |

## First time: pair this phone

1. **Open the window on the board.** Hold **BOOT** for two seconds to open the
   menu, BOOT-tap down to **Phone**, and PWR-tap to select. The screen shows the
   pairing screen and the window lasts two minutes. Over the console, `phone pair`
   opens the same window.
2. **Open the page in Chrome.** `https://<your-server-name>:8443/`, for example
   `https://hermes.local:8443/`. Chrome refuses Web Bluetooth on `http://`, and
   `127.0.0.1`/`localhost` are the only exceptions, neither of which a phone has.
3. **Trust the certificate once, if the page warns.** The Compose stack's Caddy
   signs with its own local authority. Take the CA out of the running container,
   send it to the phone, and install it:

   ```bash
   docker compose cp relay:/data/caddy/pki/authorities/local/root.crt ./caddy-root.crt
   ```

   On the phone: **Settings → Security → Encryption & credentials (or More
   security settings) → Install a certificate → CA certificate**, then pick the
   file. Chrome on Android uses the system trust store, so the warning stops once
   the CA is installed; restart Chrome if it does not. A certificate from a real
   authority, or the page served over a trusted origin, avoids this step
   entirely.
4. **Fill in the device if the page asks.** The notes endpoint is already filled
   in because the page came from the server. The **device id** (`hg-...`) and the
   **device key** (base64) go in the page and stay in its `localStorage`, because
   the page signs the upload itself. Treat the phone like the board: the key can
   post notes as it does.
5. **Tap Connect.** Android asks for permission the first time:
   - Android 12 and newer: **Nearby devices** for Chrome. Allow it, or the
     chooser stays empty.
   - Android 11 and older: **Location** for Chrome, and Location turned on in
     the system settings. A BLE scan is treated as a location scan on those
     releases.
6. **Pick the board** in Chrome's device chooser. It lists only devices that
   advertise the note service, so a board outside its window does not appear.
7. **Allow it on the board.** The screen changes to the pairing screen and names
   the phone's address. **Press BOOT to allow**; **PWR refuses**; twenty seconds
   of neither refuses too. From the console the same two answers are
   `phone allow` and `phone refuse`.
8. **Confirm the code.** Android shows a six-digit passkey and the board shows the
   same digits. Tap **Pair** only if they match. That is LE Secure Connections on
   top of the allow in step 7.

Pairing is kept: after this, the board advertises for that phone and steps 7 and 8
do not repeat. Up to three phones are remembered and a fourth pushes out the
oldest. To pair a phone again, remove the board in the phone's Bluetooth settings
and repeat steps 1 and 5 to 8; this build has no un-pair command of its own.

## Sending a note

1. Record on the board as usual (BOOT-tap). The note lands in `pending/`.
2. On the page, tap **List notes**. Each row shows size, length and recorded time.
3. **Send** takes one note: the board streams it in blocks, the page checks each
   block's CRC-32 and the file's SHA-256, posts it to the server, and only when the
   server answers 2xx tells the board the note is done, which moves it to `sent/`.
4. **Fetch every note** walks the list. Keep the page in front and the screen on:
   Chrome suspends a backgrounded tab and the transfer stops with it.

If the link drops, the bytes already received stay put; **Send** again resumes from
that offset, and nothing is deleted from the board until the server has the note.

## After the first time

- Open the page, tap **Connect**, send. No code, no allow.
- Chrome for Android supports `navigator.bluetooth.getDevices()`, so a page that
  was granted the device can offer it again without the chooser; where it does
  not, the chooser is one tap.
- If Android forgets the board (Bluetooth settings → **Forget**), pair again from
  step 1.

## Troubleshooting

| What you see | What to do |
|---|---|
| Chrome has no Connect button, or says Web Bluetooth is unavailable | the page is not on HTTPS, or the browser is not Chrome. Web Bluetooth is disabled in some managed/beta configurations |
| The chooser is empty and the board is in its window | grant **Nearby devices** (Android 12+) or **Location** plus Location on (Android 11 and older), then tap Connect again |
| The page loads with a certificate warning | the server's CA is not installed (step 3), or the name you typed is not the one the certificate covers |
| `Connect` fails with `authentication` or `insufficient encryption` | the board did not allow this phone: open the window again and press BOOT when it asks |
| The board asks, you allow, Android shows a different code | something else is answering; cancel, remove the board on the phone, and start again |
| The page connects but lists nothing | no notes are waiting, or they are already in `sent/` |
| A note stops at a percentage | keep the page in front, tap Send again; it resumes |
| `401 device ... is not enrolled` | the key in the page is not one the server knows: enroll it or copy `devices.json` |

## What this does not do

- No background sync. The page must be open, in front, with the phone awake.
- No upload without the phone: the board never posts over Bluetooth itself, it
  needs the page holding the link.
- Nothing is deleted from the card until the server has the note; retention only
  deletes notes that were already synced.

## The radio rests, and how to wake it

This board has no 32.768 kHz crystal, so with the Bluetooth controller running the
chip cannot light-sleep. The controller therefore runs only inside a window: boot,
pairing, a few minutes after a recording, a retry slot while notes wait, or
`phone wake` on the console. At rest the radio is off and the board sleeps, which
is why a phone must be given a window before it can find the board.
