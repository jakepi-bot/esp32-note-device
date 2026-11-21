# Voice-note fixtures

Real audio, committed so `tests/test_notes_e2e.py` needs no ffmpeg, TTS,
model download or network — in CI or locally.

| File | Container | Format | Size | SHA-256 |
|---|---|---|---|---|
| `sample.wav` | WAV | RIFF/WAVE, PCM16, mono, 24 000 Hz, 3.45 s | 165 794 B | `526ea0b3e1f577df1e1c7e40f20c6651af621dc7b7ed4d06cbbc81fbb670eb7f` |
| `sample.opus` | Ogg Opus | `OggS`/`OpusHead`, mono, 16 000 Hz input rate, 3.46 s | 9 653 B | `74318cfb4595e1bb0c31b2737e768bef829f0461813737d6588ff66d63beaab7` |

Both files are the same spoken sentence, synthesised on the development host
with Piper (voice `en_US-joe-medium`, pitch-shifted), so the WAV and the Opus
differ only in container and are the pair the contract's two codecs need
(`docs/voice-note-ingestion.md` §2.3).

The sentence, the timestamp, the duration and the idempotency key the test
sends are fixed constants in `tests/test_notes_e2e.py`; the transcript and
summary are produced by test doubles, because STT and the summarization model
cannot run in CI. The fixtures' audio content is therefore not asserted to
transcribe — the *staging* is: the STT double checks it is handed these exact
bytes.

## Regenerating

From the repository root, with `ffmpeg` (libopus) and a TTS that writes a WAV:

```bash
printf '%s\n' 'Call the plumber tomorrow about the kitchen sink leak before noon.' > /tmp/say.txt
growl-tts.sh /tmp/say.txt /tmp/say.wav                       # any WAV-producing TTS works
ffmpeg -y -i /tmp/say.wav -ar 24000 -ac 1 -c:a pcm_s16le tests/fixtures/voice-notes/sample.wav
ffmpeg -y -i /tmp/say.wav -ar 16000 -ac 1 -c:a libopus -b:a 24k -application voip \
       tests/fixtures/voice-notes/sample.opus
sha256sum tests/fixtures/voice-notes/sample.*
```

If the recording changes, update `DURATION`, `TRANSCRIPT` and the hashes above
(or, if the words change, also the summary constants — the test asserts the
double's output, not that STT heard the audio). Keep the file small: the test
uploads it in several cases.
