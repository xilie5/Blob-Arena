'use strict';

const fs = require('node:fs');
const http = require('node:http');
const path = require('node:path');
const { spawn, spawnSync } = require('node:child_process');

const root = path.resolve(__dirname, '..');
const isWindows = process.platform === 'win32';
const makeCommand = isWindows ? 'mingw32-make' : 'make';
const serverPath = path.join(root, 'bin', isWindows ? 'blob_server.exe' : 'blob_server');

let serverProcess = null;
let webProcess = null;
let shuttingDown = false;

function probeHealth() {
  return new Promise((resolve) => {
    const request = http.get('http://127.0.0.1:8080/health', { timeout: 800 }, (response) => {
      let body = '';
      response.setEncoding('utf8');
      response.on('data', (chunk) => { body += chunk; });
      response.on('end', () => {
        try {
          resolve(response.statusCode === 200 && JSON.parse(body).status === 'ok');
        } catch (error) {
          resolve(false);
        }
      });
    });
    request.on('timeout', () => request.destroy());
    request.on('error', () => resolve(false));
  });
}

async function waitForHealth(timeoutMs = 10000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (await probeHealth()) return true;
    await new Promise((resolve) => setTimeout(resolve, 250));
  }
  return false;
}

function buildServer() {
  console.log(`[dev] 构建服务端：${makeCommand} -B server`);
  const result = spawnSync(makeCommand, ['-B', 'server'], {
    cwd: root,
    stdio: 'inherit',
    shell: false,
  });
  if (result.error) {
    console.error(`[dev] 找不到 ${makeCommand}，请安装 MinGW 或改用 Docker Compose。`);
    return false;
  }
  if (result.status !== 0) {
    console.error('[dev] 服务端构建失败。');
    return false;
  }
  return true;
}

function startServer() {
  if (!fs.existsSync(serverPath)) {
    console.error(`[dev] 找不到 ${serverPath}`);
    return false;
  }

  serverProcess = spawn(serverPath, [
    '--port', '8080',
    '--web', 'web',
    '--data-dir', 'data',
    '--io-threads', '2',
  ], {
    cwd: root,
    stdio: 'inherit',
    windowsHide: true,
  });

  serverProcess.on('exit', (code) => {
    if (!shuttingDown) {
      console.error(`[dev] C++ 服务端已退出，code=${code ?? 'unknown'}`);
      stopAll(1);
    }
  });
  return true;
}

function startWeb() {
  let viteCli;
  try {
    const vitePackage = require.resolve('vite/package.json', { paths: [root] });
    viteCli = path.join(path.dirname(vitePackage), 'bin', 'vite.js');
  } catch (error) {
    console.error('[dev] 前端依赖不存在，请先运行 npm ci。');
    return false;
  }

  webProcess = spawn(process.execPath, [viteCli], {
    cwd: root,
    stdio: 'inherit',
    windowsHide: true,
  });
  webProcess.on('exit', (code) => {
    if (!shuttingDown) {
      console.log(`[dev] Vite 已退出，code=${code ?? 0}`);
      stopAll(code ?? 0);
    }
  });
  return true;
}

function stopAll(exitCode = 0) {
  if (shuttingDown) return;
  shuttingDown = true;
  if (webProcess && !webProcess.killed) webProcess.kill();
  if (serverProcess && !serverProcess.killed) serverProcess.kill();
  setTimeout(() => process.exit(exitCode), 300).unref();
}

async function main() {
  console.log('[dev] 检查 C++ 服务端 ...');
  const alreadyRunning = await probeHealth();
  if (alreadyRunning) {
    console.log('[dev] 检测到 8080 上已有 Blob Arena 服务端，直接复用。');
  } else {
    if (!buildServer() || !startServer()) {
      stopAll(1);
      return;
    }
    if (!(await waitForHealth())) {
      console.error('[dev] C++ 服务端未能在 8080 上通过 /health 检查。');
      stopAll(1);
      return;
    }
  }

  if (!startWeb()) {
    stopAll(1);
    return;
  }

  console.log('[dev] C++ 服务端：http://127.0.0.1:8080');
  console.log('[dev] 前端开发页：http://127.0.0.1:5173');
  console.log('[dev] 数据保存在本地 data/，不需要连接 MySQL 或 Redis。');
}

process.on('SIGINT', () => stopAll(0));
process.on('SIGTERM', () => stopAll(0));

main().catch((error) => {
  console.error('[dev] 启动失败：', error);
  stopAll(1);
});
