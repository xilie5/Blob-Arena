#include "net/io_reactor.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "net/platform.h"
#include "protocol/protocol.h"

namespace blob {
namespace net {

IoReactor::IoReactor(std::size_t id, PacketHandler packetHandler,
                     HttpHandler httpHandler, CloseHandler closeHandler,
                     std::size_t commandCapacity,
                     std::size_t maxPendingOutBytes)
    : id_(id),
      packetHandler_(std::move(packetHandler)),
      httpHandler_(std::move(httpHandler)),
      closeHandler_(std::move(closeHandler)),
      maxPendingOutBytes_(maxPendingOutBytes),
      poller_(Poller::create()),   // 编译期按平台选择 select / epoll
      commands_(commandCapacity) {}

IoReactor::~IoReactor() {
    stop();  // RAII：析构前确保线程已停止并 join，避免线程仍运行时对象被销毁
}

void IoReactor::start() {
    if (running_) return;
    running_ = true;
    commands_.reopen();
    thread_ = std::thread([this] { run(); });  // 启动 IO 线程
}

void IoReactor::stop() {
    if (!running_) return;
    running_ = false;
    commands_.close();                 // 唤醒可能正阻塞在命令队列上的线程
    if (thread_.joinable()) thread_.join();  // 等待线程安全退出
    // 关闭所有残留连接并回调 Gateway 清理会话
    for (auto& entry : connections_) {
        if (closeHandler_) closeHandler_(entry.first);
        net::closeSocket(entry.first);
    }
    connections_.clear();
}

// ---- 以下三个 post/assign 都在“别的线程”被调用，只做非阻塞入队 ----

bool IoReactor::assign(socket_t fd) {
    return commands_.tryPush(Command{AddSocket{fd}});
}

bool IoReactor::postPacket(socket_t fd, std::shared_ptr<std::string> packet,
                           std::function<void()> completion) {
    return commands_.tryPush(Command{
        SendPacket{fd, std::move(packet), std::move(completion)}});
}

bool IoReactor::postClose(socket_t fd) {
    return commands_.tryPush(Command{CloseSocket{fd}});
}

// ============================== IO 线程主循环 ==============================

void IoReactor::run() {
    while (running_) {
        // 1) 先非阻塞地把积压命令处理掉（每轮上限 256 条，防止命令洪峰饿死事件处理）
        Command command;
        int processed = 0;
        while (processed < 256 &&
               commands_.waitPop(command, std::chrono::milliseconds(0))) {
            handleCommand(command);
            ++processed;
        }

        // 2) 等待就绪事件：
        //    刚处理过命令说明可能马上有后续工作，超时给 0（不睡，尽快再收命令）；
        //    否则最多等 3ms，兼顾低延迟与空闲省电
        pollResults_.clear();
        const int ready = poller_->wait(pollResults_, processed > 0 ? 0 : 3);
        if (ready < 0) break;  // poller 致命错误，退出线程

        // 3) 逐个处理就绪事件
        for (const PollResult& result : pollResults_) {
            auto it = connections_.find(result.fd);
            if (it == connections_.end()) continue;  // 事件处理中可能已被关闭
            if (hasEvent(result.events, PollEvent::Error)) {
                pendingClose_.push_back(result.fd);
                continue;
            }
            if (hasEvent(result.events, PollEvent::Read)) {
                handleReadable(result.fd);
            }
            // 读处理可能已经关闭了连接（如对端断开），写处理前必须再查一次
            if (connections_.find(result.fd) == connections_.end()) continue;
            if (hasEvent(result.events, PollEvent::Write)) {
                handleWritable(result.fd);
            }
            it = connections_.find(result.fd);
            if (it != connections_.end()) updateEvents(it->second);
        }

        // 4) 本轮标记的关闭统一执行（不能在遍历连接表时直接 erase）
        for (socket_t fd : pendingClose_) {
            closeSocket(fd, "closed");
        }
        pendingClose_.clear();
    }
}

void IoReactor::handleCommand(Command& command) {
    // ---- 新连接接入 ----
    if (auto* add = std::get_if<AddSocket>(&command)) {
        if (connections_.find(add->fd) != connections_.end()) return;  // 幂等防重
        Connection connection;
        connection.socket.reset(new WsConnection(add->fd));
        auto inserted = connections_.emplace(add->fd, std::move(connection));
        // 先注册 poller 关注读；失败则回滚（删表、关 fd、通知 Gateway）
        if (!poller_->add(add->fd, PollEvent::Read)) {
            connections_.erase(inserted.first);
            net::closeSocket(add->fd);
            if (closeHandler_) closeHandler_(add->fd);
        }
        return;
    }
    // ---- 发送数据 ----
    if (auto* send = std::get_if<SendPacket>(&command)) {
        auto it = connections_.find(send->fd);
        if (it == connections_.end() || !send->packet) {
            if (send->completion) send->completion();  // 连接已没了，也要回调以释放记账
            return;
        }
        Connection& connection = it->second;
        // 慢客户端保护：积压 + 新包超过 1MB，直接踢掉，防止内存被慢连接拖垮
        if (connection.socket->pendingOutSize() + send->packet->size() >
            maxPendingOutBytes_) {
            if (send->completion) send->completion();
            closeSocket(send->fd, "slow client");
            return;
        }
        connection.socket->sendBinary(*send->packet);  // 加 WS 帧头拼进出站缓冲
        flush(connection);                             // 立刻尝试直接发出去
        updateEvents(connection);                      // 没发完则注册写事件
        if (send->completion) send->completion();
        return;
    }
    // ---- 关闭连接 ----
    if (auto* close = std::get_if<CloseSocket>(&command)) {
        closeSocket(close->fd, "server close");
    }
}

void IoReactor::handleReadable(socket_t fd) {
    auto it = connections_.find(fd);
    if (it == connections_.end()) return;
    Connection& connection = it->second;
    char buffer[16 * 1024];   // 每次 recv 最多 16KB
    bool gotData = false;
    // ET（边沿触发）要求一次性读到 EAGAIN：最多循环 8 轮，避免单连接占满线程
    for (int i = 0; i < 8; ++i) {
        const int count = ::recv(fd, buffer, static_cast<int>(sizeof(buffer)), 0);
        if (count > 0) {
            connection.socket->appendInput(buffer,
                                           static_cast<std::size_t>(count));
            gotData = true;
            continue;
        }
        if (count == 0) {
            pendingClose_.push_back(fd);  // 对端正常关闭（FIN）
            return;
        }
        if (isWouldBlock(lastError())) break; // 数据已读干净（ET 下的正常退出）
        pendingClose_.push_back(fd);          // 真正的读错误
        return;
    }
    if (!gotData) return;

    // ---- 尚未升级 WebSocket：先按 HTTP 处理 ----
    if (!connection.socket->isWebSocket()) {
        const auto status = connection.socket->parseHttpRequest();
        if (status == WsConnection::HttpStatus::NeedMore) return;
        if (status == WsConnection::HttpStatus::Error) {
            pendingClose_.push_back(fd);
            return;
        }

        const HttpRequest& request = connection.socket->request();
        if (request.isWebSocketUpgrade()) {
            // WebSocket 升级：回 101 握手响应
            if (!connection.socket->acceptWebSocketUpgrade()) {
                pendingClose_.push_back(fd);
                return;
            }
            flush(connection);
            updateEvents(connection);
        } else {
            // 普通 HTTP（登录/战绩/静态资源）：交给 Gateway 处理并把结果发回
            if (!httpHandler_) {
                pendingClose_.push_back(fd);
                return;
            }
            const HttpResult result = httpHandler_(fd, request);
            connection.socket->sendHttpResponse(
                result.statusCode, result.statusText, result.contentType,
                result.body);
            flush(connection);
            connection.socket->prepareForNextRequest();  // keep-alive：准备下一请求
            updateEvents(connection);
            if (result.close) {
                pendingClose_.push_back(fd);
                return;
            }
            return;
        }
    }

    // ---- 已是 WebSocket：解码帧 -> 切业务包（粘包/半包） -> 回调 ----
    std::vector<std::string> messages;
    bool closed = false;
    if (!connection.socket->decodeFrames(messages, closed)) {
        pendingClose_.push_back(fd);
        return;
    }
    // 一条 WS 消息里可能含 0~N 个业务包，循环切分直到数据不足
    for (const std::string& message : messages) {
        std::size_t offset = 0;
        uint16_t msgId = 0;
        uint16_t seq = 0;
        std::string payload;
        while (proto::unpackPacket(message, offset, msgId, seq, payload)) {
            if (packetHandler_) packetHandler_(fd, msgId, payload);
        }
    }
    if (closed) {
        pendingClose_.push_back(fd);  // 收到 close 帧
        return;
    }
    flush(connection);
    updateEvents(connection);
}

void IoReactor::handleWritable(socket_t fd) {
    auto it = connections_.find(fd);
    if (it == connections_.end()) return;
    flush(it->second);   // 内核发送缓冲腾出空间，继续发积压
    updateEvents(it->second);
}

void IoReactor::closeSocket(socket_t fd, const char* reason) {
    (void)reason;  // 原因当前仅用于潜在的日志扩展
    auto it = connections_.find(fd);
    if (it == connections_.end()) return;
    poller_->remove(fd);      // 先从 epoll/select 摘除
    net::closeSocket(fd);     // 再关 fd
    connections_.erase(it);   // 最后删连接表项
    if (closeHandler_) closeHandler_(fd);  // 通知 Gateway 清理会话
}

void IoReactor::updateEvents(Connection& connection) {
    if (!connection.socket) return;
    // 读事件始终监听；只有存在未发完数据时才监听写事件。
    // 否则 EPOLLOUT 会因“一直可写”而疯狂唤醒，造成空转。
    PollEvent events = PollEvent::Read;
    if (connection.socket->hasPendingOut()) events |= PollEvent::Write;
    poller_->modify(connection.socket->fd(), events);
}

bool IoReactor::flush(Connection& connection) {
    // 循环 send 直到发送缓冲清空或内核缓冲暂满（would-block）
    while (connection.socket && connection.socket->hasPendingOut()) {
        const std::string& buffer = connection.socket->outBuffer();
        const int count = ::send(connection.socket->fd(), buffer.data(),
                                 static_cast<int>(buffer.size()),
                                 sendFlags());
        if (count > 0) {
            connection.socket->consumeOut(static_cast<std::size_t>(count));
            continue;
        }
        if (count < 0 && isWouldBlock(lastError())) break; // 暂发不出去，等下次可写
        pendingClose_.push_back(connection.socket->fd());  // 其他错误：关闭
        return false;
    }
    return true;
}

}  // namespace net
}  // namespace blob
