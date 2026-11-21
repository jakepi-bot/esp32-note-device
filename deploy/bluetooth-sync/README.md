# Bluetooth note sync: the server in Docker

The e-paper gadget records a voice note with no network at all, an iPhone takes
the note off it over Bluetooth, and the phone posts it to a server that
transcribes it and writes it into an Obsidian vault. This directory is that
server, as two containers, plus the page the phone loads.

```
  ePaper gadget  ──Bluetooth LE──▶  iPhone (Bluefy)  ──HTTPS──▶  caddy ──▶ note-sync ──▶ vault/*.md
   records, keeps                   the relay page              :8443      :8766          + inbox/
```

Nothing here talks to anything in the cloud unless you point it at one: the
speech and summary services are whatever OpenAI-compatible URLs you configure.

| File | What it is |
|---|---|
| `docker-compose.yml` | the two services, their ports, their volumes |
| `Dockerfile` | the server image: the note endpoint plus the phone's page, one origin |
| `serve.py` | the entrypoint: device store, endpoint, page, speech and summary calls |
| `Caddyfile` | TLS in front, so an iPhone will load the page (Web Bluetooth needs HTTPS) |
| `.env.example` | every setting, with the defaults |

The protocol and the device-side rules are in
[docs/ble-note-sync.md](../../docs/ble-note-sync.md); the phone steps and the
pairing acknowledgement are in
[docs/ios-bluetooth-sync.md](../../docs/ios-bluetooth-sync.md).

## What you need

- Docker with the Compose plugin (`docker --version`, `docker compose version`).
- The gadget, with the Bluetooth note-sync firmware, and its device id and key.
- An iPhone with **Bluefy** installed (App Store; Safari has no Web Bluetooth).
- Optional but recommended: a transcription service and a chat model, both
  reachable over HTTP as OpenAI-compatible APIs. whisper.cpp's server
  (`/v1/audio/transcriptions`), speaches, and any `/v1/chat/completions`
  endpoint all work. Without them the server still accepts notes: it keeps the
  audio and stops there.

## Step 1 — get the repository on the machine that will keep the notes

```sh
git clone <this repository> hermes-gadget
cd hermes-gadget/deploy/bluetooth-sync
```

## Step 2 — write the settings

```sh
cp .env.example .env
```

Open `.env` and fill in at least:

```ini
# The name the phone will type, and the certificate's name: it must match
# exactly. Use the host's mDNS name ("hermes.local"), a DNS name, or a tailnet
# name — not a bare IP address, because the phone sends no name for an IP and
# the TLS certificate then cannot be selected.
RELAY_HOST=hermes.local

# Where the notes go. /data is the container's mount, so a relative path here
# means deploy/bluetooth-sync/data/<path>.
# (Leave the paths alone unless you are changing the compose file.)

# Transcription and summaries, if you have them:
STT_URL=http://192.168.1.20:8081/v1
STT_MODEL=whisper-1
LLM_URL=http://192.168.1.20:11434/v1
LLM_MODEL=qwen2.5:7b
```

`NOTES_PORT` defaults to 8766. If a Hermes gateway already serves the note
endpoint on this machine it owns 8766; set `NOTES_PORT=8767` (and use
`http://<host>:8767/notes/v1` for the endpoint field) or stop the gateway's
endpoint.

## Step 3 — make the folders the server writes into

The containers run as uid 1000, so create the folders as yourself and they stay
yours:

```sh
mkdir -p data/notes data/vault data/gadget
```

- `data/notes` — the staging area: `inbox/`, `done/`, `failed/`.
- `data/vault` — the Obsidian vault: one markdown note per voice note, its audio
  under `_audio/`. To write into the real vault instead, add a line to the
  `volumes:` of the `note-sync` service and point `VAULT_DIR` at it there:
  `- /srv/obsidian/voice notes:/vault` with `VAULT_DIR: /vault`.
  Lowercase `voice notes` at the vault root is the one spelling: on a
  case-sensitive host `Voice Notes` is a different folder, and against the
  iPhone and MacBook it splits the vault and makes Syncthing churn.
- `data/gadget` — `devices.json`, the enrolled device keys.

## Step 4 — give the server the gadget's key

The phone signs each upload with the board's key, so the server has to know it.
If the board already enrolled with a Hermes gateway, copy that file:

```sh
cp ~/.hermes/plugin-data/gadget/devices.json data/gadget/devices.json
```

Otherwise enroll the key yourself (it is the one the board prints, or the one
`hermes gadget devices` shows):

```sh
docker compose run --rm --entrypoint python note-sync /app/serve.py enroll --key '<base64 key>'
```

It prints the device id and key to type into the page.

## Step 5 — start it

```sh
docker compose up -d --build
```

Three lines in the log say what happened:

```sh
docker compose logs -f note-sync
```

```
note sync on :8766  |  complete: whisper at http://192.168.1.20:8081/v1, summaries at http://192.168.1.20:11434/v1
page at /  |  notes at /notes/v1  |  keys from /data/gadget/devices.json
listening on :8766
```

`stage only:` instead of `complete:` means no `STT_URL` was set; see
[Troubleshooting](#troubleshooting).

## Step 6 — check both listeners

```sh
curl -s http://localhost:8766/health          # {"status":"ok", ...}
curl -s -o /dev/null -w '%{http_code}\n' http://localhost:8080/    # 200: the page
curl -sk -o /dev/null -w '%{http_code}\n' https://localhost:8443/  # 200 over TLS
```

If the last one fails with `SSL connect error`, read the certificate step below:
an address with no name cannot select a certificate (SNI), so use the name.

## Step 7 — trust the certificate on the iPhone (one time)

iOS will not run Web Bluetooth on a plain `http://` page, so the phone loads the
page over HTTPS. The certificate comes from Caddy's own small certificate
authority, which the phone has to trust once:

```sh
docker compose cp relay:/data/caddy/pki/authorities/local/root.crt ./caddy-root.crt
```

1. Send `caddy-root.crt` to the iPhone (AirDrop, or email it to yourself) and
   open it — iOS offers to install the profile.
2. **Settings → General → VPN & Device Management** → the downloaded profile →
   **Install**.
3. **Settings → General → About → Certificate Trust Settings** → turn on
   **Caddy Local Authority**.

That authority's certificates are short-lived and renewed by Caddy on its own;
you only install the root once. Using a real certificate for a real domain
instead is a four-line change at the bottom of `Caddyfile`.

## Step 8 — pair the phone and sync

Follow [docs/ios-bluetooth-sync.md](../../docs/ios-bluetooth-sync.md). The short
version: on the gadget hold **BOOT** for two seconds, open **Phone app**, load
`https://hermes.local:8443/` in Bluefy, tap **Connect**, press **BOOT** on the
gadget when it names the phone (BOOT allows, PWR refuses; no answer within 20 s
refuses), confirm the code iOS shows, then **Send**.

## Everyday use

```sh
docker compose ps                  # both services up?
docker compose logs -f note-sync   # uploads, transcription, vault writes
docker compose restart note-sync   # after editing .env
docker compose pull && docker compose up -d --build   # after a git pull
docker compose down                # stop (data stays in ./data)
```

The server is stateless between uploads, so restarting it mid-sync is safe: the
device keeps a note in `pending/` until the phone reports a 2xx.

## Checking an upload by hand

`POST /notes/v1` is the same contract the board and the gateway use
([docs/voice-note-ingestion.md](../../docs/voice-note-ingestion.md)), so you can
post a WAV with `curl` if you sign it (`X-Gadget-Ts`, `X-Gadget-Mac`). The
reply is `202` with a `job_id` as soon as the note is durably staged; the
markdown appears in the vault a moment later.

## Troubleshooting

| What you see | What it is |
|---|---|
| `bind: address already in use` on 8766 | a Hermes gateway (or another copy) is serving the endpoint; set `NOTES_PORT=8767` in `.env` |
| `https://…:8443` fails with a TLS error | the phone used an IP, or `RELAY_HOST` does not match the name typed; both must be the same name |
| The phone shows a certificate warning | the root CA was not installed, or not trusted in **Certificate Trust Settings** (step 7) |
| Page loads, `Connect` finds no device | the gadget's pairing window is not open (two minutes), or Bluetooth is off on the phone |
| `Connect` answers `authentication`/`insufficient` | the gadget did not allow this phone: open the pairing window again and press BOOT when it asks (PWR refuses, and 20 s of neither refuses too) |
| `401 device … is not enrolled` | `data/gadget/devices.json` has no key for that id — step 4 |
| `log` says `stage only` | no `STT_URL`; notes are kept in `data/notes/inbox` until a transcriber is configured |
| Notes land with a timestamp title | no `LLM_URL`; the transcript is the summary, so the title is its first line |
| `422 recorded_at is more than 300 s in the future` | the board's clock is wrong; it fixes it on the next Wi-Fi connection, or post again after it does |

## Without a transcriber

`STT_URL` unset is a supported mode, not a broken one: the endpoint verifies and
stores the audio in `data/notes/inbox/<job_id>/` and moves nothing else. That is
what you want when a Hermes gateway on the same machine already runs the
pipeline — point it at the same `notes` directory, or leave the notes for later.

## Why not just run the gateway endpoint?

You can: the gateway serves the same contract on the same port, and the page
works against it. This container exists so the phone-facing half can run on a
machine that has no Hermes, so the page and the endpoint share one HTTPS origin,
and so a clean deploy is two commands instead of a Python environment.
