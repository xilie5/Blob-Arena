// =============================================================================
//  io_reactor.h —— 独立 IO Reactor 线程
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>

#include "concurrency/bounded_queue.h"
#include "net/poller.h"
#include "net/ws_connection.h"

namespace blob {
namespace net {

struct HttpResult {
    int statusCode = 200;
    std::string statusText = "OK";
    std::string contentType = "application/json; charset=utf-8";
    std::string body;
    bool close = false;
};

class IoReactor {
public:
    using PacketHandler =
        std::function<void(socket_t, uint16_t, const std::string&)>;
    using HttpHandler =
        std::function<HttpResult(socket_t, const HttpRequest&)>;
    using CloseHandler = std::function<void(socket_t)>;

    IoReactor(std::size_t id, PacketHandler packetHandler,
              HttpHandler httpHandler, CloseHandler closeHandler,
              std::size_t commandCapacity = 16384,
              std::size_t maxPendingOutBytes = 1024 * 1024);
    ~IoReactor();

    IoReactor(const IoReactor&) = delete;
    IoReactor& operator=(const IoReactor&) = delete;

    void start();
    void stop();

    bool assign(socket_t fd);
    bool postPacket(socket_t fd, std::shared_ptr<std::string> packet,
                    std::function<void()> completion = {});
    bool postClose(socket_t fd);

    std::size_t id() const { return id_; }
    std::size_t commandDepth() const { return commands_.size(); }

private:
    struct AddSocket {
        socket_t fd = BLOB_INVALID_SOCKET;
    };
    struct SendPacket {
        socket_t fd = BLOB_INVALID_SOCKET;
        std::shared_ptr<std::string> packet;
        std::function<void()> completion;
    };
    struct CloseSocket {
        socket_t fd = BLOB_INVALID_SOCKET;
    };
    using Command = std::variant<AddSocket, SendPacket, CloseSocket>;

    struct Connection {
        std::unique_ptr<WsConnection> socket;
    };

    void run();
    void handleCommand(Command& command);
    void handleReadable(socket_t fd);
    void handleWritable(socket_t fd);
    void closeSocket(socket_t fd, const char* reason);
    void updateEvents(Connection& connection);
    bool flush(Connection& connection);

    std::size_t id_;
    PacketHandler packetHandler_;
    HttpHandler httpHandler_;
    CloseHandler closeHandler_;
    std::size_t maxPendingOutBytes_;

    std::unique_ptr<Poller> poller_;
    BoundedQueue<Command> commands_;
    std::unordered_map<socket_t, Connection> connections_;
    std::vector<PollResult> pollResults_;
    std::vector<socket_t> pendingClose_;

    std::thread thread_;
    bool running_ = false;
};

}  // namespace net
}  // namespace blob
