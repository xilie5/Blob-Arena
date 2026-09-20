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
      poller_(Poller::create()),
      commands_(commandCapacity) {}

IoReactor::~IoReactor() {
    stop();
}

void IoReactor::start() {
    if (running_) return;
    running_ = true;
    commands_.reopen();
    thread_ = std::thread([this] { run(); });
}

void IoReactor::stop() {
    if (!running_) return;
    running_ = false;
    commands_.close();
    if (thread_.joinable()) thread_.join();
    for (auto& entry : connections_) {
        if (closeHandler_) closeHandler_(entry.first);
        net::closeSocket(entry.first);
    }
    connections_.clear();
}

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

void IoReactor::run() {
    while (running_) {
        Command command;
        int processed = 0;
        while (processed < 256 &&
               commands_.waitPop(command, std::chrono::milliseconds(0))) {
            handleCommand(command);
            ++processed;
        }

        pollResults_.clear();
        const int ready = poller_->wait(pollResults_, processed > 0 ? 0 : 3);
        if (ready < 0) break;

        for (const PollResult& result : pollResults_) {
            auto it = connections_.find(result.fd);
            if (it == connections_.end()) continue;
            if (hasEvent(result.events, PollEvent::Error)) {
                pendingClose_.push_back(result.fd);
                continue;
            }
            if (hasEvent(result.events, PollEvent::Read)) {
                handleReadable(result.fd);
            }
            if (connections_.find(result.fd) == connections_.end()) continue;
            if (hasEvent(result.events, PollEvent::Write)) {
                handleWritable(result.fd);
            }
            it = connections_.find(result.fd);
            if (it != connections_.end()) updateEvents(it->second);
        }

        for (socket_t fd : pendingClose_) {
            closeSocket(fd, "closed");
        }
        pendingClose_.clear();
    }
}

void IoReactor::handleCommand(Command& command) {
    if (auto* add = std::get_if<AddSocket>(&command)) {
        if (connections_.find(add->fd) != connections_.end()) return;
        Connection connection;
        connection.socket.reset(new WsConnection(add->fd));
        auto inserted = connections_.emplace(add->fd, std::move(connection));
        if (!poller_->add(add->fd, PollEvent::Read)) {
            connections_.erase(inserted.first);
            net::closeSocket(add->fd);
            if (closeHandler_) closeHandler_(add->fd);
        }
        return;
    }
    if (auto* send = std::get_if<SendPacket>(&command)) {
        auto it = connections_.find(send->fd);
        if (it == connections_.end() || !send->packet) {
            if (send->completion) send->completion();
            return;
        }
        Connection& connection = it->second;
        if (connection.socket->pendingOutSize() + send->packet->size() >
            maxPendingOutBytes_) {
            if (send->completion) send->completion();
            closeSocket(send->fd, "slow client");
            return;
        }
        connection.socket->sendBinary(*send->packet);
        flush(connection);
        updateEvents(connection);
        if (send->completion) send->completion();
        return;
    }
    if (auto* close = std::get_if<CloseSocket>(&command)) {
        closeSocket(close->fd, "server close");
    }
}

void IoReactor::handleReadable(socket_t fd) {
    auto it = connections_.find(fd);
    if (it == connections_.end()) return;
    Connection& connection = it->second;
    char buffer[16 * 1024];
    bool gotData = false;
    for (int i = 0; i < 8; ++i) {
        const int count = ::recv(fd, buffer, static_cast<int>(sizeof(buffer)), 0);
        if (count > 0) {
            connection.socket->appendInput(buffer,
                                           static_cast<std::size_t>(count));
            gotData = true;
            continue;
        }
        if (count == 0) {
            pendingClose_.push_back(fd);
            return;
        }
        if (isWouldBlock(lastError())) break;
        pendingClose_.push_back(fd);
        return;
    }
    if (!gotData) return;

    if (!connection.socket->isWebSocket()) {
        const auto status = connection.socket->parseHttpRequest();
        if (status == WsConnection::HttpStatus::NeedMore) return;
        if (status == WsConnection::HttpStatus::Error) {
            pendingClose_.push_back(fd);
            return;
        }

        const HttpRequest& request = connection.socket->request();
        if (request.isWebSocketUpgrade()) {
            if (!connection.socket->acceptWebSocketUpgrade()) {
                pendingClose_.push_back(fd);
                return;
            }
            flush(connection);
            updateEvents(connection);
        } else {
            if (!httpHandler_) {
                pendingClose_.push_back(fd);
                return;
            }
            const HttpResult result = httpHandler_(fd, request);
            connection.socket->sendHttpResponse(
                result.statusCode, result.statusText, result.contentType,
                result.body);
            flush(connection);
            connection.socket->prepareForNextRequest();
            updateEvents(connection);
            if (result.close) {
                pendingClose_.push_back(fd);
                return;
            }
            return;
        }
    }

    std::vector<std::string> messages;
    bool closed = false;
    if (!connection.socket->decodeFrames(messages, closed)) {
        pendingClose_.push_back(fd);
        return;
    }
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
        pendingClose_.push_back(fd);
        return;
    }
    flush(connection);
    updateEvents(connection);
}

void IoReactor::handleWritable(socket_t fd) {
    auto it = connections_.find(fd);
    if (it == connections_.end()) return;
    flush(it->second);
    updateEvents(it->second);
}

void IoReactor::closeSocket(socket_t fd, const char* reason) {
    (void)reason;
    auto it = connections_.find(fd);
    if (it == connections_.end()) return;
    poller_->remove(fd);
    net::closeSocket(fd);
    connections_.erase(it);
    if (closeHandler_) closeHandler_(fd);
}

void IoReactor::updateEvents(Connection& connection) {
    if (!connection.socket) return;
    PollEvent events = PollEvent::Read;
    if (connection.socket->hasPendingOut()) events |= PollEvent::Write;
    poller_->modify(connection.socket->fd(), events);
}

bool IoReactor::flush(Connection& connection) {
    while (connection.socket && connection.socket->hasPendingOut()) {
        const std::string& buffer = connection.socket->outBuffer();
        const int count = ::send(connection.socket->fd(), buffer.data(),
                                 static_cast<int>(buffer.size()),
                                 sendFlags());
        if (count > 0) {
            connection.socket->consumeOut(static_cast<std::size_t>(count));
            continue;
        }
        if (count < 0 && isWouldBlock(lastError())) break;
        pendingClose_.push_back(connection.socket->fd());
        return false;
    }
    return true;
}

}  // namespace net
}  // namespace blob
