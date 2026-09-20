// =============================================================================
//  config.js —— 客户端协议、配色与渲染参数
// =============================================================================
export const CONFIG = {
  wsPath: '/ws',
  inputHz: 30,
  pingIntervalMs: 1000,

  cameraLerp: 0.18,
  zoomBaseRadius: 20,
  zoomExponent: 0.34,
  zoomMin: 0.42,
  zoomMax: 1.30,

  gridSize: 100,
  minNameRadius: 9,
  leaderboardSize: 10,
  teleportSnapDistance: 400,

  killFeedSize: 5,
  killFeedLifetimeMs: 5200,
  bannerFadeMs: 360,
};

export const PALETTE = {
  background: '#070b16',
  grid: 'rgba(96, 140, 210, 0.10)',
  gridBold: 'rgba(96, 140, 210, 0.20)',
  worldBorder: 'rgba(120, 190, 255, 0.55)',
  worldFill: 'rgba(20, 34, 62, 0.55)',
  selfRing: '#ffffff',
  nameText: '#0b1020',
  textDim: 'rgba(234, 242, 255, 0.6)',
  primary: '#4fc3f7',
  selfHighlight: '#ffd54f',
  danger: '#ff8a80',
  success: '#81c784',
  shrinkRing: 'rgba(255, 138, 128, 0.95)',
  shrinkRingDanger: 'rgba(255, 80, 80, 1.00)',
  shrinkZoneFill: 'rgba(40, 8, 8, 0.32)',
};

export const EntityKind = {
  PLAYER: 0,
  FOOD: 1,
  BOT: 2,
};

export const MatchPhase = {
  WAITING: 0,
  COUNTDOWN: 1,
  PLAYING: 2,
  ENDED: 3,
};

export const PHASE_META = {
  [MatchPhase.WAITING]: { label: '等待玩家', tone: 'waiting' },
  [MatchPhase.COUNTDOWN]: { label: '准备', tone: 'countdown' },
  [MatchPhase.PLAYING]: { label: '对局中', tone: 'playing' },
  [MatchPhase.ENDED]: { label: '已结算', tone: 'ended' },
};

export const C2S = {
  AUTH: 0x0001,
  JOIN: 0x0010,
  RESUME: 0x0011,
  INPUT: 0x0020,
  PING: 0x0030,
  LEAVE: 0x0031,
  CREATE_ROOM: 0x0040,
  JOIN_ROOM: 0x0041,
};

export const S2C = {
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
