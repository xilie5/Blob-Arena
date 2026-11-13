// =============================================================================
//  game.js —— 客户端状态、协议处理和主循环
// =============================================================================
import {
  C2S,
  CONFIG,
  EntityKind,
  MatchPhase,
  PHASE_META,
  S2C,
} from './config.js';
import { InputController } from './input.js';
import { NetClient, ConnectionState } from './net.js';
import { Renderer } from './render.js';

const els = {
  canvas: document.getElementById('game-canvas'),
  hud: document.getElementById('hud'),
  overlay: document.getElementById('overlay'),
  waitPanel: document.getElementById('wait-panel'),
  settlementPanel: document.getElementById('settlement-panel'),
  hint: document.getElementById('hint'),
  online: document.getElementById('stat-online'),
  fps: document.getElementById('stat-fps'),
  ping: document.getElementById('stat-ping'),
  score: document.getElementById('stat-score'),
  rank: document.getElementById('stat-rank'),
  leaderboard: document.getElementById('leaderboard'),
  killFeed: document.getElementById('kill-feed'),
  deathFlash: document.getElementById('death-flash'),
  phaseBanner: document.getElementById('phase-banner'),
  phaseLabel: document.getElementById('phase-label'),
  phaseRemain: document.getElementById('phase-remain'),
  phaseShrink: document.getElementById('phase-shrink'),
  exitRoom: document.getElementById('btn-exit-room'),
  waitCode: document.getElementById('wait-code'),
  waitHint: document.getElementById('wait-hint'),
  waitPlayers: document.getElementById('wait-players'),
  waitCancel: document.getElementById('btn-wait-cancel'),
  settlementTitle: document.getElementById('settlement-title'),
  settlementReason: document.getElementById('settlement-reason'),
  settlementRanking: document.getElementById('settlement-ranking'),
  rematch: document.getElementById('btn-rematch'),
  backLobby: document.getElementById('btn-back-lobby'),
  overlayStatus: document.getElementById('overlay-status'),
  nickname: document.getElementById('nickname'),
  roomCode: document.getElementById('room-code'),
  joinRoomField: document.getElementById('join-room-field'),
  quick: document.getElementById('btn-endless'),
  create: document.getElementById('btn-create'),
  join: document.getElementById('btn-join'),
};

const state = {
  net: null,
  pendingAction: null,
  authToken: '',
  profile: null,
  resuming: false,
  joined: false,
  inRoom: false,
  selfId: 0,
  roomCode: '',
  maxPlayers: 4,
  publicRoom: false,
  world: { width: 0, height: 0 },
  entities: new Map(),
  matchPhase: MatchPhase.WAITING,
  remainSec: 0,
  playerCount: 0,
  shrinkRadius: 0,
  result: [],
  killFeed: [],
  lastInputAt: 0,
  lastPingAt: 0,
  online: 0,
  fps: 0,
  lastFrameAt: performance.now(),
  frameCount: 0,
  lastFpsAt: performance.now(),
  manualClose: false,
  reconnectTimer: null,
  reconnectAttempts: 0,
};

const renderer = new Renderer(els.canvas);
const input = new InputController(els.canvas);

function setView(patch) {
  const targets = {
    overlay: els.overlay,
    wait: els.waitPanel,
    hud: els.hud,
    settlement: els.settlementPanel,
  };
  for (const [key, visible] of Object.entries(patch)) {
    const element = targets[key];
    if (!element) continue;
    element.classList.toggle('hidden', !visible);
    if (key === 'hud') {
      els.hint.classList.toggle('hidden', !visible);
    }
  }
}

function setStatus(message, isError = false) {
  els.overlayStatus.textContent = message;
  els.overlayStatus.classList.toggle('is-error', isError);
}

function setMode(mode) {
  const buttons = {
    quick: els.quick,
    create: els.create,
    join: els.join,
  };
  for (const [key, button] of Object.entries(buttons)) {
    button.classList.toggle('is-active', key === mode);
  }
  els.joinRoomField.classList.toggle('hidden', mode !== 'join');
}

function sanitizeName() {
  const value = (els.nickname.value || '').trim().slice(0, 12);
  return value || `Player${Math.floor(Math.random() * 900 + 100)}`;
}

function serverUnavailableMessage() {
  if (window.location.protocol === 'file:') {
    return '请通过 http://127.0.0.1:8080 或 npm run dev 打开页面，不要直接双击 index.html';
  }
  return `无法连接 C++ 服务端（${window.location.host}）。请先运行 npm run dev，或启动 bin/blob_server.exe --port 8080`;
}

async function ensureServerReachable() {
  if (window.location.protocol === 'file:') {
    throw new Error(serverUnavailableMessage());
  }

  let response;
  try {
    response = await fetch('/health', { cache: 'no-store' });
  } catch (error) {
    throw new Error(serverUnavailableMessage());
  }

  if (!response.ok) {
    throw new Error(serverUnavailableMessage());
  }

  const health = await response.json().catch(() => null);
  if (health?.status !== 'ok') {
    throw new Error(serverUnavailableMessage());
  }
}

async function ensureSession(name) {
  const savedToken = localStorage.getItem('blob.token') || '';
  const savedName = localStorage.getItem('blob.username') || '';
  if (savedToken && savedName === name) {
    const response = await fetch('/api/profile', {
      headers: { Authorization: `Bearer ${savedToken}` },
    });
    if (response.ok) {
      state.authToken = savedToken;
      state.profile = await response.json();
      return;
    }
    localStorage.removeItem('blob.token');
    localStorage.removeItem('blob.username');
  }

  const response = await fetch('/api/login', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ username: name }),
  });
  if (!response.ok) {
    throw new Error('登录失败');
  }
  const result = await response.json();
  state.authToken = result.token;
  state.profile = result;
  localStorage.setItem('blob.token', result.token);
  localStorage.setItem('blob.username', result.username);
}

function resetMatchState() {
  state.joined = false;
  state.inRoom = false;
  state.selfId = 0;
  state.roomCode = '';
  state.maxPlayers = 4;
  state.publicRoom = false;
  state.entities.clear();
  state.matchPhase = MatchPhase.WAITING;
  state.remainSec = 0;
  state.playerCount = 0;
  state.shrinkRadius = 0;
  state.result = [];
  state.killFeed = [];
  renderer.setShrinkInfo(null);
  els.leaderboard.innerHTML = '<li class="leaderboard-empty">等待数据 ...</li>';
  els.killFeed.innerHTML = '';
  els.settlementRanking.innerHTML = '';
}

function ensureNet() {
  if (state.net) {
    return state.net;
  }
  state.net = new NetClient({
    wsPath: CONFIG.wsPath,
    onState: onConnectionState,
    onPacket: onPacket,
    onError: () => setStatus(serverUnavailableMessage(), true),
  });
  return state.net;
}

async function connectAndRun(action) {
  state.manualClose = false;
  state.reconnectAttempts = 0;
  if (state.reconnectTimer) {
    clearTimeout(state.reconnectTimer);
    state.reconnectTimer = null;
  }
  state.pendingAction = action;
  try {
    setStatus('正在检查服务端 ...');
    await ensureServerReachable();
    await ensureSession(sanitizeName());
  } catch (error) {
    setStatus(error.message || serverUnavailableMessage(), true);
    return;
  }
  const net = ensureNet();
  if (net.isOpen) {
    runPendingAction();
  } else {
    setStatus('正在连接服务器 ...');
    net.connect();
  }
}

function runPendingAction() {
  const action = state.pendingAction;
  state.pendingAction = null;
  if (!action || !state.net?.isOpen) {
    return;
  }

  const name = sanitizeName();
  if (action.type === 'quick') {
    setStatus('正在寻找房间 ...');
    state.net.sendJoin(name);
  } else if (action.type === 'create') {
    setStatus('正在创建房间 ...');
    state.net.sendCreateRoom();
  } else if (action.type === 'join') {
    const code = (action.code || '').trim().toUpperCase();
    if (!code) {
      setStatus('请输入房间码', true);
      return;
    }
    setStatus(`正在加入 ${code} ...`);
    state.net.sendJoinRoom(code);
  }
}

function scheduleReconnect() {
  if (state.manualClose || state.reconnectTimer) return;
  if (!state.authToken) return;

  const hasRoom =
    state.joined || state.inRoom || state.roomCode ||
    !!localStorage.getItem('blob.roomCode');
  if (!hasRoom) return;

  if (state.reconnectAttempts >= 5) {
    setStatus('重连失败，请刷新页面', true);
    return;
  }

  const delay = Math.min(5000, 500 * 2 ** state.reconnectAttempts);
  state.reconnectAttempts += 1;
  setStatus(`连接断开，${Math.ceil(delay / 1000)} 秒后重连 ...`, true);

  state.reconnectTimer = setTimeout(() => {
    state.reconnectTimer = null;
    if (state.manualClose) return;
    ensureNet().connect();
  }, delay);
}

function onConnectionState(connectionState, detail) {
  if (connectionState === ConnectionState.OPEN) {
    if (state.reconnectTimer) {
      clearTimeout(state.reconnectTimer);
      state.reconnectTimer = null;
    }
    state.reconnectAttempts = 0;
    setStatus('已连接，正在鉴权 ...');
    state.net.sendAuth(state.authToken);
  } else if (connectionState === ConnectionState.CLOSED) {
    if (state.joined) {
      setStatus(detail || '连接已断开', true);
    }
    state.joined = false;
    state.inRoom = false;
    scheduleReconnect();
  }
}

function handleModeAction(type) {
  if (state.net?.isOpen && state.joined) {
    return;
  }
  resetMatchState();
  connectAndRun({
    type,
    code: type === 'join' ? els.roomCode.value : '',
  }).catch((error) => setStatus(error.message || '连接失败', true));
}

function onPacket(msgId, reader) {
  switch (msgId) {
    case S2C.AUTH_OK:
      if (localStorage.getItem('blob.roomCode')) {
        state.resuming = true;
        setStatus('正在恢复上一局 ...');
        state.net.sendResume(localStorage.getItem('blob.roomCode'));
      } else {
        setStatus('鉴权成功，正在进入房间 ...');
        runPendingAction();
      }
      break;
    case S2C.JOIN_OK:
      onJoinOk(reader);
      break;
    case S2C.ROOM_JOINED:
      onRoomJoined(reader);
      break;
    case S2C.RESUME_OK:
      onResumeOk(reader);
      break;
    case S2C.ENTITY_ADD:
      onEntityAdd(reader);
      break;
    case S2C.ENTITY_REMOVE:
      onEntityRemove(reader);
      break;
    case S2C.SNAPSHOT:
      onSnapshot(reader);
      break;
    case S2C.PONG:
      onPong(reader);
      break;
    case S2C.STATS:
      onStats(reader);
      break;
    case S2C.KILL_FEED:
      onKillFeed(reader);
      break;
    case S2C.MATCH_STATE:
      onMatchState(reader);
      break;
    case S2C.MATCH_RESULT:
      onMatchResult(reader);
      break;
    case S2C.ERROR:
      onServerError(reader);
      break;
    default:
      break;
  }
}

function onJoinOk(reader) {
  state.selfId = reader.u32();
  state.world.width = reader.f32();
  state.world.height = reader.f32();
  reader.u16();  // tick rate
  reader.u32();  // color
  reader.u32();  // seed
  state.joined = true;
  state.inRoom = true;
  setView({ overlay: false, hud: true });
  setStatus('已进入房间');
}

function onRoomJoined(reader) {
  state.roomCode = reader.str8();
  state.world.width = reader.f32();
  state.world.height = reader.f32();
  state.maxPlayers = reader.u8() || 4;
  state.publicRoom = reader.u8() !== 0;
  els.waitCode.textContent = state.roomCode || '----';
  if (state.roomCode) {
    localStorage.setItem('blob.roomCode', state.roomCode);
  }
  updateWaitPanel();
}

function onResumeOk(reader) {
  state.roomCode = reader.str8();
  state.selfId = reader.u32();
  state.world.width = reader.f32();
  state.world.height = reader.f32();
  state.resuming = false;
  state.joined = true;
  state.inRoom = true;
  state.pendingAction = null;
  state.manualClose = false;
  state.reconnectAttempts = 0;
  // 服务端恢复后会把当前视野重新全量下发；先清掉断线前的旧实体，避免鬼影
  state.entities.clear();
  state.killFeed = [];
  state.result = [];
  localStorage.setItem('blob.roomCode', state.roomCode);
  els.waitCode.textContent = state.roomCode || '----';
  setView({ overlay: false, hud: true, wait: false, settlement: false });
  setStatus('已恢复上一局');
}

function onEntityAdd(reader) {
  const id = reader.u32();
  const kind = reader.u8();
  const color = reader.u32();
  const radius = reader.f32();
  const x = reader.f32();
  const y = reader.f32();
  const score = reader.i32();
  const name = reader.str8();

  const previous = state.entities.get(id);
  const entity = {
    id,
    kind,
    color,
    radius,
    x,
    y,
    rx: previous?.rx ?? x,
    ry: previous?.ry ?? y,
    score,
    name,
  };
  state.entities.set(id, entity);
}

function onEntityRemove(reader) {
  const id = reader.u32();
  reader.u8();
  state.entities.delete(id);
}

function onSnapshot(reader) {
  reader.u32();  // server tick
  const count = reader.u16();
  for (let i = 0; i < count; i += 1) {
    const id = reader.u32();
    const x = reader.f32();
    const y = reader.f32();
    const radius = reader.f32();
    const score = reader.i32();
    const entity = state.entities.get(id);
    if (!entity) {
      continue;
    }
    entity.x = x;
    entity.y = y;
    entity.radius = radius;
    entity.score = score;

    if (Math.hypot(x - entity.rx, y - entity.ry) > CONFIG.teleportSnapDistance) {
      entity.rx = x;
      entity.ry = y;
    }
  }

}

function onPong(reader) {
  const clientTime = reader.u32();
  reader.u32();
  const now = (performance.now() & 0xffffffff) >>> 0;
  state.ping = Math.max(0, Math.round(now - clientTime));
}

function onStats(reader) {
  state.online = reader.u16();
  reader.u16();
  reader.u16();
}

function onKillFeed(reader) {
  const killerId = reader.u32();
  const victimId = reader.u32();
  const killer = state.entities.get(killerId);
  const victim = state.entities.get(victimId);
  const text = `${killer?.name || '玩家'} 吞噬了 ${victim?.name || '单位'}`;
  state.killFeed.unshift({ text, expiresAt: performance.now() + CONFIG.killFeedLifetimeMs });
  state.killFeed = state.killFeed.slice(0, CONFIG.killFeedSize);
  if (victimId === state.selfId) {
    els.deathFlash.classList.remove('is-active');
    void els.deathFlash.offsetWidth;
    els.deathFlash.classList.add('is-active');
  }
  renderKillFeed();
}

function onMatchState(reader) {
  state.matchPhase = reader.u8();
  state.remainSec = reader.u16();
  state.playerCount = reader.u16();
  state.shrinkRadius = reader.f32();
  renderer.setShrinkInfo(
    state.shrinkRadius > 0
      ? {
          x: state.world.width * 0.5,
          y: state.world.height * 0.5,
          r: state.shrinkRadius,
          active: true,
        }
      : null
  );
  updateWaitPanel();
  updatePhaseBanner();
}

function onMatchResult(reader) {
  const count = reader.u16();
  state.result = [];
  for (let i = 0; i < count; i += 1) {
    state.result.push({
      id: reader.u32(),
      score: reader.i32(),
      kills: reader.u16(),
      deaths: reader.u16(),
      radius: reader.f32(),
      name: reader.str8(),
    });
  }
  state.matchPhase = MatchPhase.ENDED;
  localStorage.removeItem('blob.roomCode');
  updatePhaseBanner();
  showSettlement();
}

function onServerError(reader) {
  reader.u8();
  const message = reader.str8();
  if (state.resuming) {
    state.resuming = false;
    localStorage.removeItem('blob.roomCode');
    if (!state.pendingAction) {
      setStatus('恢复失败，请重新进入房间', true);
      setView({ overlay: true, wait: false, hud: false });
      return;
    }
    setStatus('恢复失败，正在重新匹配 ...', true);
    runPendingAction();
    return;
  }
  setStatus(message || '服务器拒绝了请求', true);
  setView({ overlay: true, wait: false, hud: false });
}

function updateWaitPanel() {
  const waiting =
    state.inRoom &&
    (state.matchPhase === MatchPhase.WAITING ||
      state.matchPhase === MatchPhase.COUNTDOWN);
  setView({ wait: waiting });
  els.waitPlayers.textContent = `${state.playerCount || 1} / ${state.maxPlayers}`;
  els.waitHint.textContent =
    state.matchPhase === MatchPhase.COUNTDOWN
      ? `即将开局，剩余 ${state.remainSec} 秒`
      : '等待玩家加入，满 4 人或倒计时结束后开局';
}

function updatePhaseBanner() {
  const meta = PHASE_META[state.matchPhase] || PHASE_META[MatchPhase.WAITING];
  els.phaseBanner.classList.remove(
    'hidden',
    'is-fading',
    'tone-waiting',
    'tone-countdown',
    'tone-playing',
    'tone-ended'
  );
  els.phaseBanner.classList.add(`tone-${meta.tone}`);
  els.phaseLabel.textContent = meta.label;
  els.phaseRemain.textContent = String(Math.max(0, state.remainSec));

  if (state.shrinkRadius > 0 && state.world.width > 0) {
    const maxRadius = Math.min(state.world.width, state.world.height) * 0.5;
    const percent = Math.max(0, Math.min(100, Math.round((state.shrinkRadius / maxRadius) * 100)));
    els.phaseShrink.classList.remove('hidden');
    els.phaseShrink.textContent = `安全区 ${percent}%`;
  } else {
    els.phaseShrink.classList.add('hidden');
  }
}

function showSettlement() {
  const self = state.result.find((item) => item.id === state.selfId);
  const rank = state.result.findIndex((item) => item.id === state.selfId) + 1;
  els.settlementTitle.textContent = rank === 1 ? '本局第一' : `本局第 ${rank || '-'} 名`;
  els.settlementReason.textContent = self
    ? `得分 ${self.score} · 击杀 ${self.kills} · 死亡 ${self.deaths}`
    : '本局已结束';
  els.settlementRanking.innerHTML = state.result
    .map((item, index) => {
      const selfClass = item.id === state.selfId ? ' is-self' : '';
      return (
        `<li class="settlement-rank-row${selfClass}">` +
        `<span class="rank-index">${index + 1}</span>` +
        `<span class="rank-name">${escapeHtml(item.name || '玩家')}</span>` +
        `<span class="rank-score">${item.score}</span>` +
        `</li>`
      );
    })
    .join('');
  setView({ settlement: true, wait: false });
}

function renderKillFeed() {
  els.killFeed.innerHTML = state.killFeed
    .map((item) => `<li>${escapeHtml(item.text)}</li>`)
    .join('');
}

function escapeHtml(text) {
  return String(text)
    .replaceAll('&', '&amp;')
    .replaceAll('<', '&lt;')
    .replaceAll('>', '&gt;')
    .replaceAll('"', '&quot;')
    .replaceAll("'", '&#039;');
}

function currentSelf() {
  return state.entities.get(state.selfId) || null;
}

function updateLeaderboard() {
  const players = Array.from(state.entities.values())
    .filter((entity) => entity.kind === EntityKind.PLAYER)
    .sort((a, b) => {
      if (a.score !== b.score) return b.score - a.score;
      if (a.radius !== b.radius) return b.radius - a.radius;
      return a.id - b.id;
    })
    .slice(0, CONFIG.leaderboardSize);

  const rank = players.findIndex((entity) => entity.id === state.selfId) + 1;
  els.rank.textContent = rank > 0 ? String(rank) : '--';
  els.score.textContent = String(currentSelf()?.score || 0);

  els.leaderboard.innerHTML = players.length
    ? players
        .map((entity, index) => {
          const selfClass = entity.id === state.selfId ? ' class="is-self"' : '';
          return (
            `<li${selfClass}>` +
            `<span class="lb-rank">${index + 1}</span>` +
            `<span class="lb-name">${escapeHtml(entity.name || '玩家')}</span>` +
            `<span class="lb-score">${entity.score}</span>` +
            `</li>`
          );
        })
        .join('')
    : '<li class="leaderboard-empty">等待数据 ...</li>';
}

function updateHud() {
  els.online.textContent = String(state.online || state.playerCount || 0);
  els.fps.textContent = String(state.fps || 0);
  els.ping.textContent = state.ping == null ? '--' : String(state.ping);
}

function sendInput(now) {
  if (!state.net?.isOpen || !state.joined || state.matchPhase !== MatchPhase.PLAYING) {
    return;
  }
  const self = currentSelf();
  if (!self) {
    return;
  }
  if (!input.shouldSend(now, state.lastInputAt)) {
    return;
  }
  state.lastInputAt = now;
  const selfScreen = renderer.worldToScreen(self.rx, self.ry);
  const direction = input.getDirection(selfScreen);
  state.net.sendInput(direction.x, direction.y);
}

function sendPing(now) {
  if (!state.net?.isOpen || now - state.lastPingAt < CONFIG.pingIntervalMs) {
    return;
  }
  state.lastPingAt = now;
  state.net.sendPing();
}

function updateMotion(dtMs) {
  const lerp = Math.min(1, dtMs / (1000 / 30) * 0.34);
  for (const entity of state.entities.values()) {
    entity.rx += (entity.x - entity.rx) * lerp;
    entity.ry += (entity.y - entity.ry) * lerp;
  }
}

function tick(now) {
  const dtMs = Math.min(100, now - state.lastFrameAt);
  state.lastFrameAt = now;

  state.frameCount += 1;
  if (now - state.lastFpsAt >= 500) {
    state.fps = Math.round((state.frameCount * 1000) / (now - state.lastFpsAt));
    state.frameCount = 0;
    state.lastFpsAt = now;
  }

  updateMotion(dtMs);
  sendInput(now);
  sendPing(now);

  const self = currentSelf();
  if (self) {
    renderer.updateCamera(self);
  }
  renderer.render(state.world, state.entities, state.selfId);
  updateLeaderboard();
  updateHud();

  state.killFeed = state.killFeed.filter((item) => item.expiresAt > now);
  renderKillFeed();
  requestAnimationFrame(tick);
}

function resetToLobby() {
  state.manualClose = true;
  state.reconnectAttempts = 0;
  if (state.reconnectTimer) {
    clearTimeout(state.reconnectTimer);
    state.reconnectTimer = null;
  }
  state.net?.disconnect();
  state.net = null;
  state.pendingAction = null;
  resetMatchState();
  localStorage.removeItem('blob.roomCode');
  setView({ overlay: true, wait: false, hud: false, settlement: false });
  setStatus('正在准备 ...');
}

function startRematch() {
  resetToLobby();
  connectAndRun({ type: 'create' });
}

els.quick.addEventListener('click', () => {
  setMode('quick');
  handleModeAction('quick');
});
els.create.addEventListener('click', () => {
  setMode('create');
  handleModeAction('create');
});
els.join.addEventListener('click', () => {
  setMode('join');
  handleModeAction('join');
});
els.roomCode.addEventListener('keydown', (event) => {
  if (event.key === 'Enter') {
    els.join.click();
  }
});
els.waitCancel.addEventListener('click', resetToLobby);
els.exitRoom.addEventListener('click', resetToLobby);
els.backLobby.addEventListener('click', resetToLobby);
els.rematch.addEventListener('click', startRematch);

setView({ overlay: true, wait: false, hud: false, settlement: false });
setMode('quick');

const savedName = localStorage.getItem('blob.username') || '';
if (savedName) {
  els.nickname.value = savedName;
}

const savedToken = localStorage.getItem('blob.token') || '';
const savedRoom = localStorage.getItem('blob.roomCode') || '';
if (savedToken && savedName && savedRoom) {
  setStatus('检测到上一局，正在恢复 ...');
  connectAndRun({ type: 'quick' });
}

requestAnimationFrame(tick);
