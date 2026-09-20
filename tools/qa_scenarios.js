#!/usr/bin/env node
/**
 * qa_scenarios.js —— 竞技场端到端联调
 *
 * 前置：服务端已启动。
 * 用法：node tools/qa_scenarios.js --port 8199
 */
'use strict';

const { spawn } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');

const root = path.resolve(__dirname, '..');
const wsclient = path.join(__dirname, 'wsclient.js');

const args = { port: 8199, graceMs: 0 };
for (let i = 2; i < process.argv.length; i += 1) {
  if (process.argv[i] === '--port') {
    args.port = Number(process.argv[i + 1]);
    i += 1;
  } else if (process.argv[i] === '--grace-ms') {
    args.graceMs = Number(process.argv[i + 1]);
    i += 1;
  }
}

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const results = [];

function logPath(name) {
  return path.join(root, `qa.${name}.log`);
}

function spawnClient(name, clientArgs) {
  const output = fs.openSync(logPath(name), 'w');
  const proc = spawn(process.execPath, [
    wsclient,
    '--host', '127.0.0.1',
    '--port', String(args.port),
    '--name', name,
    ...clientArgs,
  ], {
    cwd: root,
    stdio: ['ignore', output, output],
  });
  const done = new Promise((resolve) => {
    proc.on('exit', (code) => resolve(code === null ? -1 : code));
  });
  return { proc, done };
}

function text(name) {
  try {
    return fs.readFileSync(logPath(name), 'utf8');
  } catch {
    return '';
  }
}

function waitFor(name, pattern, timeoutMs = 10000) {
  const started = Date.now();
  return new Promise((resolve) => {
    const poll = () => {
      const match = text(name).match(pattern);
      if (match) {
        resolve(match);
        return;
      }
      if (Date.now() - started >= timeoutMs) {
        resolve(null);
        return;
      }
      setTimeout(poll, 120);
    };
    poll();
  });
}

function report(name, reasons) {
  const ok = reasons.length === 0;
  results.push({ name, ok, reasons });
  console.log(`\n[qa] ===== ${name} : ${ok ? 'PASS' : 'FAIL'} =====`);
  for (const reason of reasons) {
    console.log(`  x ${reason}`);
  }
}

async function scenarioCountdown() {
  const name = '单客户端倒计时开局';
  const client = spawnClient('countdown', [
    '--quick',
    '--seconds', '15',
    '--exit-on-playing',
  ]);
  const code = await client.done;
  const output = text('countdown');
  const reasons = [];
  if (code !== 0) reasons.push(`退出码 ${code}`);
  if (!output.includes('MatchState=Countdown')) reasons.push('未观察到 Countdown');
  if (!output.includes('MatchState=Playing')) reasons.push('倒计时后未进入 Playing');
  if (!output.includes('snapshot#')) reasons.push('未收到 Snapshot');
  report(name, reasons);
}

async function scenarioHttpAuth() {
  const name = 'HTTP 登录与 Bearer 鉴权';
  const username = `Qa${Date.now()}`;
  const reasons = [];
  try {
    const loginResponse = await fetch(`http://127.0.0.1:${args.port}/api/login`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ username }),
    });
    if (loginResponse.status !== 200) {
      reasons.push(`登录状态码 ${loginResponse.status}`);
    }
    const login = await loginResponse.json();
    if (!login.token || !login.playerId) reasons.push('登录响应缺少 token/playerId');

    const profileResponse = await fetch(
      `http://127.0.0.1:${args.port}/api/profile`,
      { headers: { Authorization: `Bearer ${login.token}` } }
    );
    const profile = await profileResponse.json();
    if (profileResponse.status !== 200 || profile.username !== username) {
      reasons.push('有效 Token 无法恢复同一个玩家');
    }

    const unauthorized = await fetch(
      `http://127.0.0.1:${args.port}/api/profile`
    );
    if (unauthorized.status !== 401) {
      reasons.push(`无 Token 获取资料返回 ${unauthorized.status}，期望 401`);
    }
  } catch (error) {
    reasons.push(`HTTP 请求失败：${error.message}`);
  }
  report(name, reasons);
}

async function scenarioFillRoom() {
  const name = '四人快速匹配立即开局';
  const clients = [];
  for (let i = 0; i < 4; i += 1) {
    clients.push(spawnClient(`fill${i}`, [
      '--quick',
      '--seconds', '8',
    ]));
  }
  const codes = await Promise.all(clients.map((client) => client.done));
  const rooms = [];
  const reasons = [];
  codes.forEach((code, index) => {
    if (code !== 0) reasons.push(`客户端 ${index} 退出码 ${code}`);
    const match = text(`fill${index}`).match(/RoomJoined code="([A-Z0-9]{4})"/);
    if (match) rooms.push(match[1]);
    if (!text(`fill${index}`).includes('MatchState=Playing')) {
      reasons.push(`客户端 ${index} 未进入 Playing`);
    }
  });
  if (rooms.length !== 4) reasons.push(`房间码解析不足：${rooms.join(',')}`);
  if (new Set(rooms).size !== 1) reasons.push(`四人未进入同一房间：${rooms.join(',')}`);
  report(name, reasons);
}

async function scenarioAuthRequired() {
  const name = '无效 Token 被 WebSocket 拒绝';
  const client = spawnClient('auth', [
    '--quick',
    '--token', 'invalid-token',
    '--seconds', '5',
  ]);
  const code = await client.done;
  const output = text('auth');
  const reasons = [];
  if (code === 0) reasons.push('无效 Token 仍然通过鉴权');
  if (!output.includes('invalid token')) reasons.push('未返回 invalid token');
  if (output.includes('JoinOk')) reasons.push('鉴权失败后仍然进入房间');
  report(name, reasons);
}

async function scenarioPrivateRooms() {
  const name = '双私有房间隔离';
  const a = spawnClient('roomA', ['--create', '--seconds', '8']);
  const roomA = await waitFor('roomA', /RoomJoined code="([A-Z0-9]{4})"/);
  if (!roomA) {
    report(name, ['房主 A 未拿到房间码']);
    a.proc.kill();
    return;
  }
  const b = spawnClient('roomB', [
    '--join-code', roomA[1],
    '--seconds', '8',
  ]);

  const c = spawnClient('roomC', ['--create', '--seconds', '8']);
  const roomC = await waitFor('roomC', /RoomJoined code="([A-Z0-9]{4})"/);
  if (!roomC) {
    report(name, ['房主 C 未拿到房间码']);
    [a.proc, b.proc, c.proc].forEach((proc) => proc.kill());
    return;
  }
  const d = spawnClient('roomD', [
    '--join-code', roomC[1],
    '--seconds', '8',
  ]);

  const codes = await Promise.all([a.done, b.done, c.done, d.done]);
  const reasons = [];
  codes.forEach((code, index) => {
    if (code !== 0) reasons.push(`客户端 ${index} 退出码 ${code}`);
  });
  if (roomA[1] === roomC[1]) reasons.push('两个私有房生成了相同房间码');
  if (!text('roomB').includes(`code="${roomA[1]}"`)) reasons.push('B 未加入 A 的房间');
  if (!text('roomD').includes(`code="${roomC[1]}"`)) reasons.push('D 未加入 C 的房间');
  if (text('roomA').includes(roomC[1]) || text('roomC').includes(roomA[1])) {
    reasons.push('两个房间日志出现彼此房间码');
  }
  report(name, reasons);
}

async function scenarioDisconnect() {
  const name = '玩家断线后房间继续';
  const a = spawnClient('disconnectA', ['--create', '--seconds', '16']);
  const room = await waitFor('disconnectA', /RoomJoined code="([A-Z0-9]{4})"/);
  if (!room) {
    report(name, ['A 未拿到房间码']);
    a.proc.kill();
    return;
  }
  const b = spawnClient('disconnectB', [
    '--join-code', room[1],
    '--seconds', '30',
  ]);
  await waitFor('disconnectB', /MatchState=/);
  await sleep(1500);
  b.proc.kill('SIGKILL');
  await b.done.catch(() => {});
  const code = await a.done;
  const output = text('disconnectA');
  const reasons = [];
  if (code !== 0) reasons.push(`A 退出码 ${code}`);
  if (!/MatchState=.*players=2/.test(output)) reasons.push('A 未观察到两名玩家');
  if (!/MatchState=.*players=1/.test(output)) reasons.push('B 断线后 A 未观察到人数降为 1');
  if (!output.includes('MatchState=Playing')) reasons.push('断线后对局未继续');
  report(name, reasons);
}

async function scenarioReconnect() {
  const name = '断线宽限内恢复原房间';
  const username = `Reconnect${Date.now()}`;
  const loginResponse = await fetch(`http://127.0.0.1:${args.port}/api/login`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ username }),
  });
  const login = await loginResponse.json();
  if (!login.token) {
    report(name, ['登录未返回 Token']);
    return;
  }

  const first = spawnClient('reconnectA', [
    '--token', login.token,
    '--create',
    '--seconds', '30',
  ]);
  const room = await waitFor('reconnectA', /RoomJoined code="([A-Z0-9]{4})"/);
  if (!room) {
    report(name, ['首个连接未拿到房间码']);
    first.proc.kill();
    return;
  }
  await waitFor('reconnectA', /MatchState=Playing/, 12000);
  const statusBefore = await fetch(`http://127.0.0.1:${args.port}/api/status`)
    .then((response) => response.json());
  first.proc.kill('SIGKILL');
  await first.done.catch(() => {});
  await sleep(1200);
  const statusAfter = await fetch(`http://127.0.0.1:${args.port}/api/status`)
    .then((response) => response.json());

  const resumed = spawnClient('reconnectB', [
    '--token', login.token,
    '--resume-code', room[1],
    '--seconds', '8',
  ]);
  const exitCode = await resumed.done;
  const output = text('reconnectB');
  const reasons = [];
  if (exitCode !== 0) reasons.push(`恢复连接退出码 ${exitCode}`);
  if (!output.includes(`ResumeOk`)) reasons.push('未收到 ResumeOk');
  if (!output.includes(`room="${room[1]}"`)) reasons.push('ResumeOk 房间码不一致');
  if (!output.includes('snapshot#')) reasons.push('恢复后未收到增量快照');
  if (statusAfter.aiTakeoverTicks <= statusBefore.aiTakeoverTicks) {
    reasons.push('断线期间 AI 托管没有运行');
  }
  report(name, reasons);
}

async function scenarioReconnectExpired() {
  const name = '超过重连宽限期后拒绝恢复';
  const username = `Expired${Date.now()}`;
  const loginResponse = await fetch(`http://127.0.0.1:${args.port}/api/login`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ username }),
  });
  const login = await loginResponse.json();
  const first = spawnClient('expiredA', [
    '--token', login.token,
    '--create',
    '--seconds', '30',
  ]);
  const room = await waitFor('expiredA', /RoomJoined code="([A-Z0-9]{4})"/);
  if (!room) {
    report(name, ['首个连接未拿到房间码']);
    first.proc.kill();
    return;
  }
  await waitFor('expiredA', /MatchState=Playing/, 12000);
  first.proc.kill('SIGKILL');
  await first.done.catch(() => {});
  await sleep(args.graceMs + 900);

  const resumed = spawnClient('expiredB', [
    '--token', login.token,
    '--resume-code', room[1],
    '--seconds', '4',
  ]);
  const exitCode = await resumed.done;
  const output = text('expiredB');
  const reasons = [];
  if (exitCode === 0) reasons.push('超时后仍然恢复成功');
  if (!output.includes('resume unavailable')) {
    reasons.push('未返回 resume unavailable');
  }
  report(name, reasons);
}

(async () => {
  console.log(`[qa] 目标端口 ${args.port}`);
  await scenarioHttpAuth();
  await scenarioCountdown();
  await scenarioAuthRequired();
  await scenarioFillRoom();
  await scenarioPrivateRooms();
  await scenarioDisconnect();
  await scenarioReconnect();
  if (args.graceMs > 0) {
    await scenarioReconnectExpired();
  }

  let failed = 0;
  console.log('\n================ QA 汇总 ================');
  for (const result of results) {
    console.log(`${result.ok ? '  PASS' : '  FAIL'} ${result.name}` +
      (result.ok ? '' : `: ${result.reasons.join('；')}`));
    if (!result.ok) failed += 1;
  }
  console.log('=========================================');
  process.exit(failed === 0 ? 0 : 1);
})().catch((error) => {
  console.error('[qa] uncaught:', error);
  process.exit(2);
});
