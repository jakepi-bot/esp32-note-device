// The relay's protocol against the firmware's own bytes.
//
// test/transfer-fixture.txt is not written by hand: firmware/tools/ble_sync_capture.cpp
// runs the device's protocol and prints the packets it would notify. Feeding
// them back through relay.mjs here proves the two halves agree about offsets,
// CRCs, line fragmentation and the end of a note, without a radio or a phone.
//
//   node --test web/ble-note-relay/
import assert from 'node:assert/strict';
import { createHmac } from 'node:crypto';
import { readFile } from 'node:fs/promises';
import { test } from 'node:test';
import { fileURLToPath } from 'node:url';

import {
  Assembler,
  TAG,
  buildNoteForm,
  canonicalString,
  crc32,
  hmacBase64,
  parseHeaderLine,
  parseListLines,
  postNote,
  sha256Hex,
} from './relay.mjs';

const fixtureUrl = new URL('./test/transfer-fixture.txt', import.meta.url);

async function loadFixture() {
  const text = await readFile(fileURLToPath(fixtureUrl), 'utf8');
  const meta = {};
  const packets = [];
  for (const line of text.split('\n')) {
    if (!line || line.startsWith('#')) continue;
    const [key, ...rest] = line.split(' ');
    if (key === 'PACKET') packets.push(Uint8Array.from(Buffer.from(rest[0], 'hex')));
    else meta[key] = rest[0];
  }
  return { meta, packets };
}

test('the fixture is the device talking: header, blocks and the end marker', async () => {
  const { meta, packets } = await loadFixture();
  assert.equal(packets[0][0], TAG.header);
  assert.equal(packets.at(-1)[0], TAG.end);
  assert.equal(packets.filter((p) => p[0] === TAG.block).length, Number(meta.PACKETS_BLOCKS ?? 18));
});

test('the packets rebuild the note byte for byte', async () => {
  const { meta, packets } = await loadFixture();
  const assembler = new Assembler();
  for (const packet of packets) assembler.feed(packet);

  assert.equal(assembler.header.id, 'epaper-20261120T081504-0001');
  assert.equal(assembler.header.bytes, Number(meta.AUDIO_LEN));
  assert.equal(assembler.header.durationSeconds, 7);
  assert.equal(assembler.header.recordedAt, '2026-11-20T08:15:04Z');
  assert.equal(assembler.badBlocks, 0);
  assert.equal(assembler.ended, true);
  assert.equal(assembler.received, Number(meta.AUDIO_LEN));

  const audio = assembler.bytes();
  assert.equal(audio.length, Number(meta.AUDIO_LEN));
  assert.equal(crc32(audio).toString(16).padStart(8, '0'), meta.AUDIO_CRC);
  assert.equal(await sha256Hex(audio), meta.AUDIO_SHA256);
  // The header's own digest is the audio the phone just received: this is the
  // check the relay makes before it posts anything.
  assert.equal(assembler.header.sha256, meta.AUDIO_SHA256);
});

test('a damaged block is refused and does not move the acknowledgement', async () => {
  const { packets } = await loadFixture();
  const assembler = new Assembler();
  for (const packet of packets) assembler.feed(packet);
  const good = assembler.received;

  const again = new Assembler();
  let flipped = false;
  for (const packet of packets) {
    if (packet[0] === TAG.block && !flipped) {
      const damaged = Uint8Array.from(packet);
      damaged[damaged.length - 1] ^= 0xff;
      again.feed(damaged);
      flipped = true;
      continue;
    }
    again.feed(packet);
  }
  assert.equal(again.badBlocks, 1);
  assert.ok(again.received < good || !again.ended, 'a lost block must not be counted as received');
});

test('a repeated block is harmless', async () => {
  const { packets } = await loadFixture();
  const once = new Assembler();
  for (const packet of packets) once.feed(packet);
  const twice = new Assembler();
  for (const packet of packets) {
    twice.feed(packet);
    if (packet[0] === TAG.block) twice.feed(packet);
  }
  assert.equal(twice.badBlocks, 0);
  assert.deepEqual(Array.from(twice.bytes()), Array.from(once.bytes()));
  assert.equal(twice.received, once.received);
});

test('a split text line arrives whole, and an error is not a note entry', async () => {
  const assembler = new Assembler();
  const enc = new TextEncoder();
  // "2\n" then a note line, as the device fragments them at a small MTU.
  const lines = ['2', 'epaper-20261120T081504-0001 3000 7 ' + 'a'.repeat(64) + ' 2026-11-20T08:15:04Z', 'END'];
  for (const line of lines) {
    const bytes = enc.encode(line);
    let at = 0;
    while (at < bytes.length) {
      const chunk = bytes.subarray(at, at + 20);
      const packet = new Uint8Array(chunk.length + 1);
      packet[0] = TAG.text;
      packet.set(chunk, 1);
      assembler.feed(packet);
      at += 20;
    }
    const newline = new Uint8Array([TAG.text, 0x0a]);
    assembler.feed(newline);
  }
  assert.deepEqual(assembler.lines, lines);
  const list = parseListLines(assembler.lines);
  assert.equal(list.count, 2);
  assert.equal(list.notes.length, 1);
  assert.equal(list.notes[0].bytes, 3000);

  const error = new Assembler();
  error.feed(Uint8Array.from([TAG.error, ...enc.encode('empty note n1\n')]));
  assert.deepEqual(error.errors, ['empty note n1']);
  assert.deepEqual(error.lines, []);
});

test('an empty queue answer is not a note', () => {
  const list = parseListLines(['ok nothing pending']);
  assert.equal(list.count, null);
  assert.deepEqual(list.notes, []);
});

test('the canonical string is the one the server signs', () => {
  assert.equal(
    canonicalString({
      deviceId: 'd1',
      ts: 1700000000,
      recordedAt: '2026-11-20T08:15:04Z',
      durationSeconds: 4,
      audioSha256Hex: 'ab'.repeat(32),
      idempotencyKey: 'idem-1',
    }),
    'hermes-gadget/v1|note|d1|1700000000|2026-11-20T08:15:04Z|4|' + 'ab'.repeat(32) + '|idem-1',
  );
  // Without an idempotency key the last segment is absent, not empty.
  assert.equal(
    canonicalString({ deviceId: 'd1', ts: 1, recordedAt: 't', durationSeconds: 1, audioSha256Hex: 'aa' }),
    'hermes-gadget/v1|note|d1|1|t|1|aa',
  );
});

test('the HMAC is base64(HMAC-SHA256(key, canonical)), as the device computes it', async () => {
  const key = Buffer.from('a-device-key-0123456789');
  const canonical = 'hermes-gadget/v1|note|d1|1700000000|t|4|aa';
  const ours = await hmacBase64(new Uint8Array(key), canonical);
  const theirs = createHmac('sha256', key).update(canonical).digest('base64');
  assert.equal(ours, theirs);
});

test('the form is the six parts the endpoint expects, audio first', async () => {
  const { meta, packets } = await loadFixture();
  const assembler = new Assembler();
  for (const packet of packets) assembler.feed(packet);
  const audio = assembler.bytes();
  const form = buildNoteForm({
    audio,
    deviceId: 'dev-1',
    recordedAt: '2026-11-20T08:15:04Z',
    durationSeconds: 7,
    idempotencyKey: 'dev-1-0001',
    checksum: meta.AUDIO_SHA256,
  });
  const boundary = form.contentType.split('boundary=')[1];
  const body = Buffer.from(form.body);
  const parts = body.toString('latin1').split(`--${boundary}`);
  assert.ok(parts.length >= 7, 'six parts plus the closing boundary');
  assert.match(parts[1], /name="audio"; filename="note\.wav"/);
  assert.match(parts[2], /name="device_id"\r\n\r\ndev-1\r\n/);
  assert.match(parts[3], /name="recorded_at"\r\n\r\n2026-11-20T08:15:04Z\r\n/);
  assert.match(parts[4], /name="duration_seconds"\r\n\r\n7\r\n/);
  assert.match(parts[5], /name="idempotency_key"\r\n\r\ndev-1-0001\r\n/);
  assert.match(parts[6], /name="checksum"\r\n\r\n[0-9a-f]{64}\r\n/);
  // The audio survived the framing untouched.
  const start = body.indexOf(Buffer.from('Content-Type: audio/wav\r\n\r\n')) + 'Content-Type: audio/wav\r\n\r\n'.length;
  assert.deepEqual(Array.from(body.subarray(start, start + audio.length)), Array.from(audio));
});

test('postNote signs with the board key and reports what the host said', async () => {
  const { meta, packets } = await loadFixture();
  const assembler = new Assembler();
  for (const packet of packets) assembler.feed(packet);
  const audio = assembler.bytes();
  const key = Buffer.from('board-key-abc');
  let seen = null;
  const fakeFetch = async (url, init) => {
    seen = { url, init };
    return { status: 202, ok: true, text: async () => '{"status":"queued"}' };
  };
  const result = await postNote({
    url: 'http://hub:8766/notes/v1',
    keyBase64: key.toString('base64'),
    accessToken: 'tok',
    deviceId: 'epaper-1',
    recordedAt: assembler.header.recordedAt,
    durationSeconds: assembler.header.durationSeconds,
    idempotencyKey: assembler.header.id,
    audio,
    fetchImpl: fakeFetch,
  });
  assert.equal(result.status, 202);
  assert.equal(seen.url, 'http://hub:8766/notes/v1');
  assert.equal(seen.init.headers['X-Gadget-Token'], 'tok');

  const canonical = canonicalString({
    deviceId: 'epaper-1',
    ts: Number(seen.init.headers['X-Gadget-Ts']),
    recordedAt: assembler.header.recordedAt,
    durationSeconds: assembler.header.durationSeconds,
    audioSha256Hex: meta.AUDIO_SHA256,
    idempotencyKey: assembler.header.id,
  });
  assert.equal(seen.init.headers['X-Gadget-Mac'], createHmac('sha256', key).update(canonical).digest('base64'));
});

test('a header line that is not a header is an error, not a crash', () => {
  assert.throws(() => parseHeaderLine('nonsense'));
  assert.equal(parseHeaderLine('id 10 2 - t').sha256, '');
});
