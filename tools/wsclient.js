#!/usr/bin/env node
/**
 * wsclient.js —— 无浏览器联调与压测客户端
 *
 * 用法：
 *   node tools/wsclient.js --port 8080 --quick --seconds 12
 *   node tools/wsclient.js --port 8080 --create --seconds 20
 *   node tools/wsclient.js --port 8080 --join-code ABCD --seconds 20
 */
'use strict';

const crypto = require('node:crypto');
const net = require('node:net');

const C2S = {
  AUTH: 0x0001,
  JOIN: 0x0010,
  RESUME: 0x0011,
  INPUT: 0x0020,
  PING: 0x0030,
  LEAVE: 0x0031,
  CREATE_ROOM: 0x0040,
  JOIN_ROOM: 0x0041,
};

const S2C = {
  AUTH_OK: 0x0080,
  JOIN_OK: 0x0081,
  ENTITY_ADD: 0x0082,
  ENTITY_REMOVE: 0x0083,
  SNAPSHOT: 0x0084,
  ROOM_JOINED: 0x008A,
  RESUME_OK: 0x008B,
  PONG: 0x0090,
  STATS: 0x0091,
  KILL_FEED: 0x0092,
  MATCH_STATE: 0x0093,
  MATCH_RESULT: 0x0094,
  ERROR: 0x00FF,
};

const PHASE_NAMES = ['Waiting', 'Countdown', 'Playing', 'Ended'];

function parseArgs(argv) {
  const args = {
    host: '127.0.0.1',
    port: 8080,
    name: 'Client',
    seconds: 12,
    mode: 'quick',
    joinCode: '',
    resumeCode: '',
    token: '',
    idle: false,
    center: false,
    light: false,
    quiet: false,
    exitOnPlaying: false,
    assertMove: false,
  };
  for (let i = 2; i < argv.length; i += 1) {
    const key = argv[i];
    const next = () => {
      i += 1;
      return argv[i];
    };
    if (key === '--host') args.host = next();
    else if (key === '--port') args.port = Number(next());
    else if (key === '--name') args.name = next();
    else if (key === '--seconds') args.seconds = Number(next());
    else if (key === '--quick') args.mode = 'quick';
    else if (key === '--create') args.mode = 'create';
    else if (key === '--join-code') {
      args.mode = 'join';
      args.joinCode = String(next() || '').toUpperCase();
    } else if (key === '--resume-code') {
      args.resumeCode = String(next() || '').toUpperCase();
    } else if (key === '--token') {
      args.token = String(next() || '');
    } else if (key === '--idle') args.idle = true;
    else if (key === '--center') args.center = true;
    else if (key === '--light') args.light = true;
    else if (key === '--quiet') args.quiet = true;
    else if (key === '--exit-on-playing') args.exitOnPlaying = true;
    else if (key === '--assert-move') args.assertMove = true;
  }
  return args;
}

const ARGS = parseArgs(process.argv);

function log(message) {
  if (!ARGS.quiet) {
    console.log(`[wsclient] ${message}`);
  }
}

function packPacket(msgId, payload = Buffer.alloc(0)) {
  const packet = Buffer.alloc(8 + payload.length);
  packet.writeUInt32LE(packet.length, 0);
  packet.writeUInt16LE(msgId, 4);
  packet.writeUInt16LE(0, 6);
  payload.copy(packet, 8);
  return packet;
}

function encodeFrame(payload, opcode = 0x2) {
  const mask = crypto.randomBytes(4);
  const length = payload.length;
  let header;
  if (length < 126) {
    header = Buffer.alloc(6);
    header[1] = 0x80 | length;
  } else if (length < 65536) {
    header = Buffer.alloc(8);
    header[1] = 0x80 | 126;
    header.writeUInt16BE(length, 2);
  } else {
    header = Buffer.alloc(14);
    header[1] = 0x80 | 127;
    header.writeBigUInt64BE(BigInt(length), 2);
  }
  header[0] = 0x80 | opcode;
  mask.copy(header, header.length - 4);
  const masked = Buffer.alloc(length);
  for (let i = 0; i < length; i += 1) {
    masked[i] = payload[i] ^ mask[i % 4];
  }
  return Buffer.concat([header, masked]);
}

function decodeFrames(buffer) {
  const frames = [];
  let offset = 0;
  while (buffer.length - offset >= 2) {
    const b0 = buffer[offset];
    const b1 = buffer[offset + 1];
    const fin = (b0 & 0x80) !== 0;
    const opcode = b0 & 0x0f;
    const masked = (b1 & 0x80) !== 0;
    let length = b1 & 0x7f;
    let headerLength = 2;
    if (length === 126) {
      if (buffer.length - offset < 4) break;
      length = buffer.readUInt16BE(offset + 2);
      headerLength = 4;
    } else if (length === 127) {
      if (buffer.length - offset < 10) break;
      length = Number(buffer.readBigUInt64BE(offset + 2));
      headerLength = 10;
    }
    if (masked) headerLength += 4;
    if (buffer.length - offset < headerLength + length) break;
    let payload = Buffer.from(
      buffer.subarray(offset + headerLength, offset + headerLength + length)
    );
    if (masked) {
      const mask = buffer.subarray(offset + headerLength - 4, offset + headerLength);
      for (let i = 0; i < payload.length; i += 1) payload[i] ^= mask[i % 4];
    }
    offset += headerLength + length;
    if (fin || opcode === 0x8 || opcode === 0x9 || opcode === 0xA) {
      frames.push({ opcode, payload });
    }
  }
  return { frames, rest: buffer.subarray(offset) };
}

function unpackPackets(buffer) {
  const packets = [];
  let offset = 0;
  while (buffer.length - offset >= 8) {
    const total = buffer.readUInt32LE(offset);
    if (total < 8 || offset + total > buffer.length) break;
    packets.push({
      msgId: buffer.readUInt16LE(offset + 4),
      payload: buffer.subarray(offset + 8, offset + total),
    });
    offset += total;
  }
  return packets;
}

function str8(buffer, offsetRef) {
  const length = buffer[offsetRef.value];
  offsetRef.value += 1;
  const value = buffer.toString('utf8', offsetRef.value, offsetRef.value + length);
  offsetRef.value += length;
  return value;
}

class Reader {
  constructor(buffer) {
    this.buffer = buffer;
    this.offset = 0;
  }
  u8() { return this.buffer.readUInt8(this.offset++); }
  u16() { const v = this.buffer.readUInt16LE(this.offset); this.offset += 2; return v; }
  u32() { const v = this.buffer.readUInt32LE(this.offset); this.offset += 4; return v; }
  i32() { const v = this.buffer.readInt32LE(this.offset); this.offset += 4; return v; }
  f32() { const v = this.buffer.readFloatLE(this.offset); this.offset += 4; return v; }
  str8() { const ref = { value: this.offset }; const v = str8(this.buffer, ref); this.offset = ref.value; return v; }
}

const startedAt = Date.now();
const socket = net.createConnection({ host: ARGS.host, port: ARGS.port });
let recvBuffer = Buffer.alloc(0);
let handshaken = false;
let joined = false;
let selfId = 0;
let authToken = ARGS.token;
let roomCode = '';
let snapshotCount = 0;
let entityCount = 0;
let lastTick = 0;
let matchPhase = 'Waiting';
let lastPlayers = 0;
let rttSamples = [];
let inputSeq = 0;
let firstSelfX = null;
let lastSelfX = null;
let directionIndex = 0;
let finished = false;

const directions = [
  [1, 0], [0.7, 0.7], [0, 1], [-0.7, 0.7],
  [-1, 0], [-0.7, -0.7], [0, -1], [0.7, -0.7], [0, 0],
];

function sendPacket(msgId, payload) {
  socket.write(encodeFrame(packPacket(msgId, payload), 0x2));
}

function sendJoin() {
  const name = Buffer.from(ARGS.name, 'utf8').subarray(0, 255);
  const payload = Buffer.alloc(1 + name.length);
  payload[0] = name.length;
  name.copy(payload, 1);
  sendPacket(C2S.JOIN, payload);
}

function sendAuth() {
  const token = Buffer.from(authToken, 'utf8').subarray(0, 255);
  const payload = Buffer.alloc(1 + token.length);
  payload[0] = token.length;
  token.copy(payload, 1);
  sendPacket(C2S.AUTH, payload);
}

function sendModeAction() {
  if (ARGS.resumeCode) {
    sendResume();
    return;
  }
  if (ARGS.mode === 'create') sendCreateRoom();
  else if (ARGS.mode === 'join') sendJoinRoom();
  else sendJoin();
}

async function ensureLogin() {
  if (authToken) return authToken;
  const response = await fetch(`http://${ARGS.host}:${ARGS.port}/api/login`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ username: ARGS.name }),
  });
  if (!response.ok) {
    throw new Error(`login failed: ${response.status}`);
  }
  const data = await response.json();
  if (!data.token) {
    throw new Error('login response has no token');
  }
  authToken = data.token;
  return authToken;
}

function sendCreateRoom() {
  sendPacket(C2S.CREATE_ROOM, Buffer.alloc(0));
}

function sendJoinRoom() {
  const code = Buffer.from(ARGS.joinCode, 'utf8').subarray(0, 255);
  const payload = Buffer.alloc(1 + code.length);
  payload[0] = code.length;
  code.copy(payload, 1);
  sendPacket(C2S.JOIN_ROOM, payload);
}

function sendResume() {
  const code = Buffer.from(ARGS.resumeCode, 'utf8').subarray(0, 255);
  const payload = Buffer.alloc(1 + code.length);
  payload[0] = code.length;
  code.copy(payload, 1);
  sendPacket(C2S.RESUME, payload);
}

function sendInput(dx, dy) {
  const payload = Buffer.alloc(6);
  payload.writeUInt32LE(inputSeq++ >>> 0, 0);
  payload.writeInt8(Math.round(dx * 127), 4);
  payload.writeInt8(Math.round(dy * 127), 5);
  sendPacket(C2S.INPUT, payload);
}

function onPacket(msgId, payload) {
  const reader = new Reader(payload);
  if (msgId === S2C.AUTH_OK) {
    sendModeAction();
  } else if (msgId === S2C.RESUME_OK) {
    roomCode = reader.str8();
    selfId = reader.u32();
    const width = reader.f32();
    const height = reader.f32();
    joined = true;
    log(`ResumeOk id=${selfId} room="${roomCode}" world=${width}x${height}`);
  } else if (msgId === S2C.JOIN_OK) {
    selfId = reader.u32();
    const width = reader.f32();
    const height = reader.f32();
    const tickRate = reader.u16();
    reader.u32();
    reader.u32();
    joined = true;
    log(`JoinOk id=${selfId} world=${width}x${height} tick=${tickRate}Hz`);
  } else if (msgId === S2C.ROOM_JOINED) {
    roomCode = reader.str8();
    reader.f32();
    reader.f32();
    const maxPlayers = reader.u8();
    log(`RoomJoined code="${roomCode}" maxPlayers=${maxPlayers}`);
  } else if (msgId === S2C.MATCH_STATE) {
    const phase = reader.u8();
    const remain = reader.u16();
    const players = reader.u16();
    const shrink = reader.f32();
    matchPhase = PHASE_NAMES[phase] || `Unknown(${phase})`;
    lastPlayers = players;
    log(`MatchState=${matchPhase} remain=${remain}s players=${players} shrink=${shrink.toFixed(1)}`);
    if (ARGS.exitOnPlaying && matchPhase === 'Playing') {
      finish(0);
    }
  } else if (msgId === S2C.MATCH_RESULT) {
    const count = reader.u16();
    const rows = [];
    for (let i = 0; i < count; i += 1) {
      const id = reader.u32();
      const score = reader.i32();
      reader.u16();
      reader.u16();
      reader.f32();
      const name = reader.str8();
      rows.push(`${i + 1}.${name}#${id}:${score}`);
    }
    log(`Result ${rows.join(' ')}`);
  } else if (msgId === S2C.SNAPSHOT) {
    lastTick = reader.u32();
    const count = reader.u16();
    entityCount = count;
    for (let i = 0; i < count; i += 1) {
      const id = reader.u32();
      const x = reader.f32();
      reader.f32();
      reader.f32();
      reader.i32();
      if (id === selfId) {
        if (firstSelfX === null) firstSelfX = x;
        lastSelfX = x;
      }
    }
    snapshotCount += 1;
    if (snapshotCount % 20 === 1) {
      log(`snapshot#${snapshotCount} tick=${lastTick} entities=${count}`);
    }
  } else if (msgId === S2C.PONG) {
    const clientTime = reader.u32();
    reader.u32();
    const now = (Date.now() & 0xffffffff) >>> 0;
    rttSamples.push(Math.max(0, now - clientTime));
  } else if (msgId === S2C.ERROR) {
    const code = reader.u8();
    const message = reader.str8();
    console.error(`[wsclient] Error code=${code} message=${message}`);
    finish(2);
  }
}

function processFrames(frames) {
  for (const frame of frames) {
    if (frame.opcode === 0x8) {
      finish(0);
      return;
    }
    if (frame.opcode === 0x9) {
      socket.write(encodeFrame(frame.payload, 0xA));
      continue;
    }
    if (frame.opcode !== 0x1 && frame.opcode !== 0x2) continue;
    for (const packet of unpackPackets(frame.payload)) {
      onPacket(packet.msgId, packet.payload);
    }
  }
}

socket.on('connect', () => {
  const key = crypto.randomBytes(16).toString('base64');
  socket.write(
    'GET /ws HTTP/1.1\r\n' +
    `Host: ${ARGS.host}:${ARGS.port}\r\n` +
    'Upgrade: websocket\r\n' +
    'Connection: Upgrade\r\n' +
    `Sec-WebSocket-Key: ${key}\r\n` +
    'Sec-WebSocket-Version: 13\r\n\r\n'
  );
});

socket.on('data', (chunk) => {
  recvBuffer = Buffer.concat([recvBuffer, chunk]);
  if (!handshaken) {
    const end = recvBuffer.indexOf('\r\n\r\n');
    if (end < 0) return;
    const header = recvBuffer.subarray(0, end).toString('latin1');
    if (!header.includes('101 Switching Protocols')) {
      console.error('[wsclient] WebSocket 握手失败');
      finish(2);
      return;
    }
    recvBuffer = recvBuffer.subarray(end + 4);
    handshaken = true;
    ensureLogin().then(sendAuth).catch((error) => {
        console.error(`[wsclient] ${error.message}`);
        finish(2);
      });
  }
  const { frames, rest } = decodeFrames(recvBuffer);
  recvBuffer = rest;
  processFrames(frames);
});

socket.on('error', (error) => {
  console.error(`[wsclient] socket error: ${error.message}`);
  finish(2);
});

socket.on('close', () => {
  if (!finished) finish(joined && snapshotCount > 0 ? 0 : 2);
});

const inputTimer = setInterval(() => {
  if (!joined || ARGS.idle || matchPhase !== 'Playing') return;
  if (ARGS.center) {
    directionIndex = 0;
    sendInput(0, 0);
    return;
  }
  const [dx, dy] = directions[directionIndex];
  directionIndex = (directionIndex + 1) % directions.length;
  sendInput(dx, dy);
}, 120);

const pingTimer = setInterval(() => {
  if (!joined) return;
  const payload = Buffer.alloc(4);
  payload.writeUInt32LE((Date.now() & 0xffffffff) >>> 0, 0);
  sendPacket(C2S.PING, payload);
}, 1000);

const endTimer = setTimeout(() => finish(null), Math.max(1, ARGS.seconds) * 1000);

function finish(forcedCode) {
  if (finished) return;
  finished = true;
  clearInterval(inputTimer);
  clearInterval(pingTimer);
  clearTimeout(endTimer);

  const elapsed = ((Date.now() - startedAt) / 1000).toFixed(1);
  const avgRtt = rttSamples.length
    ? (rttSamples.reduce((sum, value) => sum + value, 0) / rttSamples.length).toFixed(1)
    : 'n/a';
  log(
    `summary elapsed=${elapsed}s joined=${joined} room="${roomCode}" ` +
    `phase=${matchPhase} players=${lastPlayers} snapshots=${snapshotCount} ` +
    `entities=${entityCount} tick=${lastTick} rtt=${avgRtt}ms`
  );

  const movedEnough =
    !ARGS.assertMove ||
    (firstSelfX !== null && lastSelfX !== null &&
     Math.abs(lastSelfX - firstSelfX) >= 20);
  const ok = forcedCode === null
    ? joined && snapshotCount > 0
    : forcedCode === 0 && joined && snapshotCount > 0;
  if (!ok || !movedEnough) {
    console.error(
      `[wsclient] conclusion failed joined=${joined} snapshots=${snapshotCount} ` +
      `phase=${matchPhase} moved=${firstSelfX ?? 'n/a'}->${lastSelfX ?? 'n/a'} ` +
      `expectedCode=${forcedCode ?? 'duration'}`
    );
  }
  socket.end(encodeFrame(Buffer.alloc(0), 0x8));
  process.exitCode = ok && movedEnough ? 0 : (forcedCode || 1);
  setTimeout(() => process.exit(process.exitCode), 30);
}
