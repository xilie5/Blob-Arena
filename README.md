# Blob Arena

基于 C++17 的 H5 实时竞技场服务端。浏览器通过原生 Canvas 和 WebSocket 直接连接服务端，
支持多人匹配、私有房间、服务端权威模拟、断线重连和战绩排行榜。

## 当前玩法

- 4 人短局竞技，180 秒后按分数排名。
- 第一人进入后开始 8 秒准备倒计时，满 4 人立即开局。
- 吃食物、NPC 和其他玩家获得分数并成长。
- 玩家被吞噬后原地重生，不会退出对局。
- 后半局安全区收缩，圈外单位持续掉体积。
- 支持快速匹配、创建私有房间和房间码加入。

## 已实现

- C++17 服务端，无游戏引擎和业务框架依赖。
- Windows `select` / Linux `epoll ET` 跨平台事件循环。
- 1 个 Acceptor、N 个 IO Reactor 和独立 Room Worker。
- I/O 与逻辑线程通过有界队列通信，支持队列指标和慢客户端保护。
- HTTP 静态资源与 WebSocket 共用端口。
- 自研 WebSocket 帧编解码和二进制业务协议。
- 服务端权威 30Hz 固定步长模拟。
- 均匀网格空间索引和 450 世界单位 AOI。
- 每连接增量实体同步，支持新增、变化和离开视野差分。
- 4 人房间、快速匹配、私有码房、空房回收。
- Arena 状态机：`Waiting -> Countdown -> Playing -> Ended`。
- NPC 漫游与恐慌、吞噬、成长、计分、缩圈和结算排名。
- 用户名登录、随机 Session Token、WebSocket 鉴权。
- 断线宽限期、同 Token 恢复、AI 托管和客户端自动 Resume。
- 对局幂等结算、玩家战绩、累计排行榜。
- 无外部依赖的 TSV 文件持久化，可平滑替换为 SQLite/MySQL。
- 核心单元测试和无浏览器端到端 QA。

## 工程结构

```text
src/core/      权威世界、竞技场状态机、NPC AI
src/net/       socket、select/epoll、WebSocket 帧
src/protocol/  二进制包头和序列化
src/server/    Gateway、Lobby、Room
src/storage/   账号、会话、战绩和排行榜数据层
tests/         纯逻辑测试
tools/         无浏览器联调客户端和 QA
web/           H5 Canvas 客户端
```

## 构建与运行

Windows MinGW：

```powershell
mingw32-make -B all
bin\blob_server.exe --port 8080 --web web --data-dir data
```

启动后访问 `http://127.0.0.1:8080`。不要直接双击 `web/index.html`，浏览器需要通过
HTTP 加载页面，并通过同一个地址访问 `/api/*` 和 `/ws`。

CMake：

```powershell
cmake -S . -B build
cmake --build build --config Release
```

测试：

```powershell
mingw32-make -B testrun
node tools/qa_scenarios.js --port 8199
```

开发模式（推荐）：

```powershell
npm ci
npm run dev
```

`npm run dev` 会先构建并启动 C++ 服务端，再启动 Vite：

- 前端开发页：`http://127.0.0.1:5173`
- C++ 服务端：`http://127.0.0.1:8080`

如果只想单独启动 Vite，可使用 `npm run dev:web`，但必须先自行启动 C++ 服务端。

生产构建：

```powershell
npm run build
```

数据默认保存在本地 `data/` 目录。当前实现使用 TSV 文件，不需要连接 MySQL、Redis
或其他数据库服务。页面出现“无法连接 C++ 服务端”时，应检查 8080 服务是否启动，
而不是检查数据库连接。

## 服务端架构

```text
Browser
  |
  +-- HTTP/WS -> Acceptor -> IO Reactor xN
                              |
                    bounded command queue
                              |
                         Room Worker
                              |
                    Lobby -> Room -> World
                              |
                    bounded output queue
                              |
                         IO Reactor -> Browser

Data:
HTTP API / Room Worker -> DataStore -> FileDataStore
```

当前版本是单进程、多线程架构。Acceptor 只负责接入，Reactor 负责连接 I/O，Room Worker
独占房间状态，避免房间逻辑加锁。

## 协议

所有业务包使用：

```text
[u32 totalLength][u16 msgId][u16 seq][payload]
```

主要消息：

| 消息 | 方向 | 说明 |
|---|---|---|
| `Auth` | C -> S | Bearer Session Token 鉴权 |
| `Join` | C -> S | 快速匹配 |
| `CreateRoom` | C -> S | 创建私有房间 |
| `JoinRoom` | C -> S | 房间码加入 |
| `Input` | C -> S | 方向输入 |
| `JoinOk` | S -> C | 玩家 ID、世界尺寸、Tick 等 |
| `RoomJoined` | S -> C | 房间码、人数上限 |
| `EntityAdd/Remove` | S -> C | 实体名册变更 |
| `Snapshot` | S -> C | 位置、半径和分数快照 |
| `MatchState` | S -> C | 阶段、倒计时、人数、缩圈 |
| `MatchResult` | S -> C | 最终排名 |

HTTP：

| 路径 | 说明 |
|---|---|
| `POST /api/login` | 用户名登录，开发环境免密码 |
| `GET /api/profile` | Bearer Token 查询玩家资料 |
| `GET /api/matches` | Bearer Token 查询个人战绩 |
| `GET /api/leaderboard` | 查询累计排行榜 |
| `GET /health` | 存活检查 |
| `GET /api/status` | 房间、玩家、实体和 Tick 状态 |
| `GET /metrics` | Prometheus 文本指标 |

本地数据默认写入 `data/`：

| 文件 | 内容 |
|---|---|
| `players.tsv` | 玩家资料 |
| `sessions.tsv` | Session Token |
| `matches.tsv` | 对局主记录 |
| `match_players.tsv` | 玩家对局明细 |

## 部署

Docker Compose：

```powershell
docker compose -p blob-arena up --build -d
```

启动后访问 `http://127.0.0.1:8080`，健康检查为：

```text
GET /health
```

## 后续方向

- 增加 SQLite 或 MySQL 存储适配器。
- 将单个 Room Worker 扩展为多个房间分片。
- 增加 HTTPS/WSS 和正式公网部署。
- 完善账号安全、Token 过期和限流。
