// =============================================================================
//  io_reactor.h —— 独立 IO Reactor 线程
//
//  Reactor 模式：线程主循环只做三件事——
//    ① 取出别的线程投递来的命令（加连接/发包/关连接）
//    ② 在 poller 上等待 socket 就绪事件
//    ③ 对就绪连接执行读/写，并把收到的完整业务包通过回调交回 Gateway
//
//  线程铁律：
//    IoReactor 只做 socket 读写与协议解析，绝不访问 World/Room 等游戏数据；
//    每条连接在接入时被永久分配给某一个 Reactor，之后始终由该线程处理。
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

/// @brief HTTP 处理结果（由 Gateway 的 HttpHandler 回填，IoReactor 负责发出）
struct HttpResult {
    int statusCode = 200;                          ///< HTTP 状态码
    std::string statusText = "OK";                 ///< 状态文本
    std::string contentType = "application/json; charset=utf-8"; ///< 内容类型
    std::string body;                              ///< 响应体
    bool close = false;                            ///< 发完是否关闭连接
};

/// @brief IO Reactor：一个线程 + 一个 poller + 名下全部连接
class IoReactor {
public:
    /// 收到完整业务包时的回调（fd、消息号、载荷）→ Gateway::onPacket
    using PacketHandler =
        std::function<void(socket_t, uint16_t, const std::string&)>;
    /// 收到普通 HTTP 请求时的回调 → Gateway::onHttpRequest
    using HttpHandler =
        std::function<HttpResult(socket_t, const HttpRequest&)>;
    /// 连接关闭时的回调（用于清理会话表）→ Gateway::onClientClosed
    using CloseHandler = std::function<void(socket_t)>;

    /// @brief 构造 Reactor（不创建线程，需再调 start()）
    /// @param id 线程编号（日志用）
    /// @param packetHandler/httpHandler/closeHandler 由 Gateway 注入的三个回调
    /// @param commandCapacity 跨线程命令队列容量（默认 16384）
    /// @param maxPendingOutBytes 单连接发送积压上限（默认 1MB，超过判慢客户端踢掉）
    IoReactor(std::size_t id, PacketHandler packetHandler,
              HttpHandler httpHandler, CloseHandler closeHandler,
              std::size_t commandCapacity = 16384,
              std::size_t maxPendingOutBytes = 1024 * 1024);
    ~IoReactor();

    // 持有线程与 poller，不可拷贝
    IoReactor(const IoReactor&) = delete;
    IoReactor& operator=(const IoReactor&) = delete;

    void start();  ///< 启动 IO 线程
    void stop();   ///< 停止并 join 线程、关闭残留连接

    /// @brief 把一个新 accept 的连接交给本 Reactor（投递 AddSocket 命令）
    /// @note 非阻塞：队列满返回 false，由调用方关闭该连接
    bool assign(socket_t fd);
    /// @brief 请求向某连接发送一个已打包好的数据包
    /// @param completion 数据真正写入发送缓冲后执行的回调（用于扣减 queuedBytes）
    bool postPacket(socket_t fd, std::shared_ptr<std::string> packet,
                    std::function<void()> completion = {});
    /// @brief 请求关闭某连接
    bool postClose(socket_t fd);

    std::size_t id() const { return id_; }              ///< Reactor 编号
    std::size_t commandDepth() const { return commands_.size(); } ///< 命令队列积压（监控）

private:
    /// 命令①：注册新连接
    struct AddSocket {
        socket_t fd = BLOB_INVALID_SOCKET;
    };
    /// 命令②：发送数据包（shared_ptr 共享同一份字节，避免跨线程拷贝大串）
    struct SendPacket {
        socket_t fd = BLOB_INVALID_SOCKET;
        std::shared_ptr<std::string> packet;
        std::function<void()> completion;
    };
    /// 命令③：关闭连接
    struct CloseSocket {
        socket_t fd = BLOB_INVALID_SOCKET;
    };
    // 命令的 tagged union：variant 无堆分配、无虚函数、类型安全
    using Command = std::variant<AddSocket, SendPacket, CloseSocket>;

    /// Reactor 名下的一条连接（只包协议状态机）
    struct Connection {
        std::unique_ptr<WsConnection> socket;
    };

    void run();                  // IO 线程主循环
    void handleCommand(Command& command); // 执行一条跨线程命令
    void handleReadable(socket_t fd);     // 连接可读：收数据/握手/解帧/解包
    void handleWritable(socket_t fd);     // 连接可写：继续冲刷发送缓冲
    void closeSocket(socket_t fd, const char* reason); // 关闭并清理一条连接
    void updateEvents(Connection& connection); // 按积压情况增删写事件监听
    bool flush(Connection& connection);        // 尽量把出站缓冲 send 出去

    std::size_t id_;                 ///< 编号
    PacketHandler packetHandler_;    ///< 业务包回调
    HttpHandler httpHandler_;        ///< HTTP 请求回调
    CloseHandler closeHandler_;      ///< 连接关闭回调
    std::size_t maxPendingOutBytes_; ///< 单连接发送积压上限（慢客户端保护）

    std::unique_ptr<Poller> poller_;     ///< select/epoll 抽象（本线程独占）
    BoundedQueue<Command> commands_;     ///< 外部线程 -> IO 线程的命令队列
    std::unordered_map<socket_t, Connection> connections_; ///< 名下连接表（仅本线程访问）
    std::vector<PollResult> pollResults_;///< wait 结果复用缓冲（避免每帧分配）
    std::vector<socket_t> pendingClose_; ///< 本轮待关闭 fd（遍历后统一删，防迭代器失效）

    std::thread thread_;   ///< IO 工作线程
    bool running_ = false; ///< 运行标志
};

}  // namespace net
}  // namespace blob
