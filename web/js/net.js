// =============================================================================
//  net.js —— 二进制协议编解码 + WebSocket 客户端
//
//  包结构（小端序，与 src/protocol/protocol.h 完全一致）：
//    [u32 length][u16 msgId][u16 seq][payload...]
// =============================================================================

import { C2S, S2C } from './config.js';

// ---------------------------------------------------------------------------
// 写入器
// ---------------------------------------------------------------------------
export class Writer {
  constructor() {
    this.bytes = [];
  }

  u8(v) {
    this.bytes.push(v & 0xff);
    return this;
  }

  i8(v) {
    this.bytes.push(v & 0xff);
    return this;
  }

  u16(v) {
    this.bytes.push(v & 0xff, (v >> 8) & 0xff);
    return this;
  }

  u32(v) {
    const b = new Uint8Array(4);
    new DataView(b.buffer).setUint32(0, v >>> 0, true);
    for (const x of b) this.bytes.push(x);
    return this;
  }

  f32(v) {
    const b = new Uint8Array(4);
    new DataView(b.buffer).setFloat32(0, v, true);
    for (const x of b) this.bytes.push(x);
    return this;
  }

  str8(text) {
    const encoded = new TextEncoder().encode(String(text ?? '')).slice(0, 255);
    this.u8(encoded.length);
    for (const x of encoded) this.bytes.push(x);
    return this;
  }

  build() {
    return new Uint8Array(this.bytes);
  }
}

// ---------------------------------------------------------------------------
// 读取器
// ---------------------------------------------------------------------------
export class Reader {
  constructor(buffer) {
    this.view = new DataView(buffer.buffer, buffer.byteOffset, buffer.byteLength);
    this.offset = 0;
    this.decoder = new TextDecoder('utf-8');
  }

  get remaining() {
    return this.view.byteLength - this.offset;
  }

  u8() {
    const v = this.view.getUint8(this.offset);
    this.offset += 1;
    return v;
  }

  i8() {
    const v = this.view.getInt8(this.offset);
    this.offset += 1;
    return v;
  }

  u16() {
    const v = this.view.getUint16(this.offset, true);
    this.offset += 2;
    return v;
  }

  u32() {
    const v = this.view.getUint32(this.offset, true);
    this.offset += 4;
    return v;
  }

  i32() {
    const v = this.view.getInt32(this.offset, true);
    this.offset += 4;
    return v;
  }

  f32() {
    const v = this.view.getFloat32(this.offset, true);
    this.offset += 4;
    return v;
  }

  str8() {
    const len = this.u8();
    const slice = new Uint8Array(this.view.buffer, this.view.byteOffset + this.offset, len);
    this.offset += len;
    return this.decoder.decode(slice);
  }
}

// ---------------------------------------------------------------------------
// 连接状态
// ---------------------------------------------------------------------------
export const ConnectionState = {
  IDLE: 'idle',
  CONNECTING: 'connecting',
  OPEN: 'open',
  CLOSED: 'closed',
};

/**
 * WebSocket 客户端封装。
 * handlers: { onState(state, detail), onPacket(msgId, payloadString, seq), onError(msg) }
 */
export class NetClient {
  constructor(handlers = {}) {
    this.handlers = handlers;
    this.socket = null;
    this.state = ConnectionState.IDLE;
    this.sendSeq = 0;
    this.rttMs = null;
  }

  get isOpen() {
    return this.state === ConnectionState.OPEN;
  }

  buildUrl() {
    const scheme = window.location.protocol === 'https:' ? 'wss' : 'ws';
    return `${scheme}://${window.location.host}${this.handlers.wsPath ?? '/ws'}`;
  }

  connect() {
    if (this.state === ConnectionState.CONNECTING || this.state === ConnectionState.OPEN) {
      return;
    }
    this.setState(ConnectionState.CONNECTING);

    let socket;
    try {
      socket = new WebSocket(this.buildUrl());
    } catch (err) {
      this.setState(ConnectionState.CLOSED, String(err));
      return;
    }

    socket.binaryType = 'arraybuffer';
    this.socket = socket;

    socket.onopen = () => {
      this.setState(ConnectionState.OPEN);
    };

    socket.onmessage = (event) => {
      this.onBinary(event.data);
    };

    socket.onerror = () => {
      this.handlers.onError?.('网络异常');
    };

    socket.onclose = (event) => {
      this.setState(ConnectionState.CLOSED, event.reason || '');
    };
  }

  disconnect() {
    if (this.socket) {
      try {
        this.send(C2S.LEAVE, null);
        this.socket.close();
      } catch (err) {
        /* 忽略关闭异常 */
      }
      this.socket = null;
    }
    this.setState(ConnectionState.CLOSED, 'client closed');
  }

  setState(state, detail = '') {
    this.state = state;
    this.handlers.onState?.(state, detail);
  }

  send(msgId, writer) {
    if (!this.socket || this.socket.readyState !== WebSocket.OPEN) {
      return false;
    }
    const payload = writer ? writer.build() : new Uint8Array(0);
    const total = 8 + payload.length;
    const buffer = new ArrayBuffer(total);
    const view = new DataView(buffer);

    view.setUint32(0, total, true);
    view.setUint16(4, msgId, true);
    view.setUint16(6, this.sendSeq & 0xffff, true);
    new Uint8Array(buffer, 8).set(payload);

    this.sendSeq = (this.sendSeq + 1) & 0xffff;
    this.socket.send(buffer);
    return true;
  }

  onBinary(data) {
    const buffer = new Uint8Array(data);
    let offset = 0;

    while (buffer.byteLength - offset >= 8) {
      const view = new DataView(buffer.buffer, buffer.byteOffset + offset, buffer.byteLength - offset);
      const total = view.getUint32(0, true);
      if (total < 8 || offset + total > buffer.byteLength) {
        break;
      }
      const msgId = view.getUint16(4, true);
      const seq = view.getUint16(6, true);
      const payload = new Reader(new Uint8Array(buffer.buffer, buffer.byteOffset + offset + 8, total - 8));
      this.handlers.onPacket?.(msgId, payload, seq);
      offset += total;
    }
  }

  // ------------------------------ 便捷发送 ------------------------------
  sendAuth(token) {
    const writer = new Writer();
    writer.str8(token);
    return this.send(C2S.AUTH, writer);
  }

  sendJoin(name) {
    const writer = new Writer();
    writer.str8(name);
    return this.send(C2S.JOIN, writer);
  }

  sendResume(roomCode) {
    const writer = new Writer();
    writer.str8(roomCode);
    return this.send(C2S.RESUME, writer);
  }

  sendInput(dirX, dirY) {
    const writer = new Writer();
    writer.u32((this.sendSeq * 7919) >>> 0);
    writer.i8(Math.round(Math.max(-127, Math.min(127, dirX * 127))));
    writer.i8(Math.round(Math.max(-127, Math.min(127, dirY * 127))));
    return this.send(C2S.INPUT, writer);
  }

  sendPing() {
    const writer = new Writer();
    const stamp = (performance.now() & 0xffffffff) >>> 0;
    writer.u32(stamp);
    this.lastPingStamp = stamp;
    return this.send(C2S.PING, writer);
  }

  // 建房（无 payload）
  sendCreateRoom() {
    return this.send(C2S.CREATE_ROOM, null);
  }

  // 加房（payload = str8(code)）
  sendJoinRoom(code) {
    const writer = new Writer();
    writer.str8(code);
    return this.send(C2S.JOIN_ROOM, writer);
  }

}

export { C2S, S2C };
