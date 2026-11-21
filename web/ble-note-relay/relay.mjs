// The phone half of the Bluetooth note sync (docs/ble-note-sync.md).
//
// This module is the protocol and the upload, with no Bluetooth in it: the
// packets the device notifies are parsed, checked and assembled into a WAV,
// then POSTed to Hermes with the same multipart request and HMAC the device's
// own uploader uses (docs/voice-note-ingestion.md). index.html adds the radio
// (Web Bluetooth) on top. Keeping the two apart is what makes this testable:
// `node --test selftest.mjs` drives every rule below without a device.

export const SERVICE_UUID = 'dc5300a0-7a2a-400f-b8f6-e87185576192';
export const CONTROL_UUID = 'dc5300a1-7a2a-400f-b8f6-e87185576192';
export const DATA_UUID = 'dc5300a2-7a2a-400f-b8f6-e87185576192';

// Packet tags: the first byte of every notification on the data characteristic.
export const TAG = { header: 0x48, block: 0x42, text: 0x4c, error: 0x45, end: 0x4b };

const BLOCK_HEADER = 11; // tag + offset(4) + crc32(4) + length(2)

/** CRC-32 (IEEE): the value the firmware's crc32() computes, per block. */
export function crc32(bytes) {
  let crc = 0xffffffff;
  for (let i = 0; i < bytes.length; i++) {
    crc ^= bytes[i];
    for (let bit = 0; bit < 8; bit++) crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1));
  }
  return (crc ^ 0xffffffff) >>> 0;
}

export async function sha256Hex(bytes) {
  const digest = await crypto.subtle.digest('SHA-256', bytes);
  return [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, '0')).join('');
}

export async function hmacBase64(keyBytes, text) {
  const key = await crypto.subtle.importKey('raw', keyBytes, { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']);
  const mac = await crypto.subtle.sign('HMAC', key, new TextEncoder().encode(text));
  return btoa(String.fromCharCode(...new Uint8Array(mac)));
}

/**
 * The canonical string the server signs (docs/voice-note-ingestion.md §2.2).
 * Every field is the raw string as transmitted.
 */
export function canonicalString({ deviceId, ts, recordedAt, durationSeconds, audioSha256Hex, idempotencyKey }) {
  const parts = ['hermes-gadget/v1', 'note', deviceId, String(ts), recordedAt, String(durationSeconds), audioSha256Hex];
  if (idempotencyKey) parts.push(idempotencyKey);
  return parts.join('|');
}

/** One note's header line: "<id> <bytes> <duration_s> <sha256|-> <recorded_at>". */
export function parseHeaderLine(line) {
  const fields = line.split(' ');
  if (fields.length < 5) throw new Error(`unreadable header: ${line}`);
  return {
    id: fields[0],
    bytes: Number(fields[1]),
    durationSeconds: Number(fields[2]),
    sha256: fields[3] === '-' ? '' : fields[3],
    recordedAt: fields.slice(4).join(' '),
  };
}

/**
 * Reassembles one note from the data characteristic's packets.
 *
 * The device sends the header, then blocks that each carry their own offset and
 * CRC, then an end packet. Text answers arrive the same way: fragments of one
 * line, ended by a newline, so a long list survives a 20-byte MTU.
 */
export class Assembler {
  constructor() {
    this.header = null;
    this.audio = new Uint8Array(0);
    this.received = 0;      // contiguous bytes, i.e. what to acknowledge
    this.blocks = 0;
    this.badBlocks = 0;     // a block whose CRC or length did not check out
    this.lines = [];        // completed text answers, without their tag
    this.errors = [];       // completed error lines
    this.ended = false;
    this._partial = new Map();
  }

  /** Feed one notification. Returns what arrived: 'header' | 'block' | 'text' | 'error' | 'end' | 'ignore'. */
  feed(packet) {
    const bytes = packet instanceof Uint8Array ? packet : new Uint8Array(packet);
    if (bytes.length === 0) return 'ignore';
    const tag = bytes[0];
    if (tag === TAG.block) {
      if (bytes.length < BLOCK_HEADER) {
        this.badBlocks++;
        return 'ignore';
      }
      const offset = readU32(bytes, 1);
      const crc = readU32(bytes, 5);
      const length = bytes[9] | (bytes[10] << 8);
      if (bytes.length !== length + BLOCK_HEADER) {
        this.badBlocks++;
        return 'ignore';
      }
      const payload = bytes.subarray(BLOCK_HEADER);
      if (crc32(payload) !== crc) {
        this.badBlocks++;
        return 'ignore';
      }
      if (this.audio.length < offset + length) {
        // Grow by what the device says it has in total, so a later block does
        // not leave holes the uplink would send as zeros.
        const want = Math.max(offset + length, this.header ? this.header.bytes : offset + length);
        const grown = new Uint8Array(want);
        grown.set(this.audio);
        this.audio = grown;
      }
      this.audio.set(payload, offset);
      this.blocks++;
      // Contiguous coverage only: an out-of-order block does not move the ack.
      if (offset <= this.received) this.received = Math.max(this.received, offset + length);
      return 'block';
    }
    if (tag === TAG.end) {
      this.ended = true;
      return 'end';
    }
    const bucket = tag === TAG.error ? this.errors : tag === TAG.text ? this.lines : null;
    if (!bucket && tag !== TAG.header) return 'ignore';
    const buffer = (this._partial.get(tag) || '') + new TextDecoder().decode(bytes.subarray(1));
    const cut = buffer.indexOf('\n');
    if (cut < 0) {
      this._partial.set(tag, buffer);
      return 'ignore';
    }
    const line = buffer.slice(0, cut);
    this._partial.set(tag, buffer.slice(cut + 1));
    if (tag === TAG.header) {
      this.header = parseHeaderLine(line);
      if (this.audio.length < this.header.bytes) {
        const grown = new Uint8Array(this.header.bytes);
        grown.set(this.audio);
        this.audio = grown;
      }
      return 'header';
    }
    bucket.push(line);
    return tag === TAG.error ? 'error' : 'text';
  }

  /** The audio as received, trimmed to the length the header promised. */
  bytes() {
    const want = this.header ? this.header.bytes : this.audio.length;
    return this.audio.subarray(0, Math.min(want, this.audio.length));
  }
}

function readU32(bytes, at) {
  return (bytes[at] | (bytes[at + 1] << 8) | (bytes[at + 2] << 16) | (bytes[at + 3] << 24)) >>> 0;
}

/**
 * The multipart body the notes endpoint expects: the audio file first, then the
 * five text parts. Returns the bytes and the Content-Type with its boundary.
 */
export function buildNoteForm({ audio, deviceId, recordedAt, durationSeconds, idempotencyKey, checksum, filename = 'note.wav' }) {
  const boundary = '----note-relay-' + randomHex(12);
  const encoder = new TextEncoder();
  const chunks = [];
  const put = (text) => chunks.push(encoder.encode(text));

  put(`--${boundary}\r\n`);
  put(`Content-Disposition: form-data; name="audio"; filename="${filename}"\r\n`);
  put('Content-Type: audio/wav\r\n\r\n');
  chunks.push(audio);
  put('\r\n');
  const field = (name, value) => {
    put(`--${boundary}\r\n`);
    put(`Content-Disposition: form-data; name="${name}"\r\n\r\n`);
    put(`${value}\r\n`);
  };
  field('device_id', deviceId);
  field('recorded_at', recordedAt);
  field('duration_seconds', String(durationSeconds));
  if (idempotencyKey) field('idempotency_key', idempotencyKey);
  if (checksum) field('checksum', checksum);
  put(`--${boundary}--\r\n`);

  const size = chunks.reduce((n, c) => n + c.length, 0);
  const body = new Uint8Array(size);
  let at = 0;
  for (const chunk of chunks) {
    body.set(chunk, at);
    at += chunk.length;
  }
  return { body, contentType: `multipart/form-data; boundary=${boundary}` };
}

function randomHex(n) {
  const bytes = new Uint8Array(n);
  crypto.getRandomValues(bytes);
  return [...bytes].map((b) => b.toString(16).padStart(2, '0')).join('');
}

/**
 * Clears a note's newline-terminated answer out of the device's wire language:
 * `ok nothing pending`, `2`, `<id> <bytes> ...`, `END`.
 */
export function parseListLines(lines) {
  const notes = [];
  let count = null;
  for (const line of lines) {
    if (line === 'END') break;
    if (count === null && /^\d+$/.test(line)) {
      count = Number(line);
      continue;
    }
    if (/^\d+$/.test(line) === false && line.includes(' ')) {
      try {
        notes.push(parseHeaderLine(line));
      } catch {
        // A line that is not a note entry (an "ok ..." answer) is not one.
      }
    }
  }
  return { count, notes };
}

/**
 * POSTs one assembled note to Hermes. The device key is the board's own (the
 * server only knows devices that enrolled), so the note is signed as the board
 * and the server's idempotency key makes a repeat harmless.
 */
export async function postNote({ url, keyBase64, accessToken, deviceId, recordedAt, durationSeconds, idempotencyKey, audio, fetchImpl = fetch }) {
  const sha = await sha256Hex(audio);
  const ts = Math.floor(Date.now() / 1000);
  const canonical = canonicalString({
    deviceId,
    ts,
    recordedAt,
    durationSeconds,
    audioSha256Hex: sha,
    idempotencyKey,
  });
  const keyBytes = Uint8Array.from(atob(keyBase64), (c) => c.charCodeAt(0));
  const mac = await hmacBase64(keyBytes, canonical);
  const form = buildNoteForm({ audio, deviceId, recordedAt, durationSeconds, idempotencyKey, checksum: sha });
  const headers = { 'Content-Type': form.contentType, 'X-Gadget-Ts': String(ts), 'X-Gadget-Mac': mac };
  if (accessToken) headers['X-Gadget-Token'] = accessToken;
  const response = await fetchImpl(url, { method: 'POST', headers, body: form.body });
  const text = await response.text();
  return { status: response.status, ok: response.ok, body: text, sha256: sha };
}
