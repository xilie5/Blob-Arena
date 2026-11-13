// =============================================================================
//  bounded_queue.h —— 线程安全有界队列
//
//  作用：
//    本项目唯一的跨线程通信工具。IO Reactor 线程与 Room Worker 线程之间
//    不共享任何游戏数据，只通过本队列传递“小值命令”（fd、坐标、房间码等），
//    从根上杜绝数据竞争（多线程同时读写同一份内存导致的错乱/崩溃）。
//
//  设计要点：
//    1. 容量有上限：防止消费方处理不过来时生产方无限堆积、耗尽内存。
//    2. 两种投递策略：
//       - tryPush  非阻塞，满了立即失败 —— 入站消息使用，绝不能堵住网络线程；
//       - waitPush 带超时等待 —— 登录/入房等关键消息使用，尽量不丢。
//    3. 互斥锁用 std::lock_guard（RAII）：函数任何出口自动解锁，不会忘释放。
//    4. 持有 mutex/condition_variable，拷贝无意义且危险，故显式 delete 拷贝。
// =============================================================================
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>

namespace blob {

/// @brief 线程安全的有界阻塞队列（模板类，可放任意类型 T）
/// @tparam T 队列元素类型。实际使用时为 std::variant 命令小对象，
///            无堆分配、无虚函数，拷贝/移动成本低。
template <typename T>
class BoundedQueue {
public:
    /// @brief 构造函数
    /// @param capacity 队列容量上限；传 0 时自动纠正为 1，防止零容量死队列
    explicit BoundedQueue(std::size_t capacity)
        : capacity_(capacity == 0 ? 1 : capacity) {}

    // 队列持有互斥锁与条件变量，禁止拷贝（拷贝一个正在被多线程使用的队列
    // 既无意义又会造成同步原语重复拥有的严重问题）
    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    /// @brief 非阻塞入队（入站消息专用）
    /// @param value 待入队元素（按值传入，内部 std::move 进容器，避免拷贝）
    /// @return true=入队成功并已唤醒一个消费者；false=队列已关闭或已满
    /// @note 满了直接丢弃由调用方计数处理。宁可丢一帧输入，也不能让网络
    ///       线程睡在游戏线程上——这是本项目最重要的线程隔离约束之一。
    bool tryPush(T value) {
        std::lock_guard<std::mutex> lock(mutex_);   // RAII 加锁，出作用域自动解锁
        if (closed_ || queue_.size() >= capacity_) {
            return false;                            // 关闭/已满 → 立即失败，绝不等待
        }
        queue_.push_back(std::move(value));          // 移动语义，大字符串也零拷贝
        notEmpty_.notify_one();                      // 叫醒一个正在 waitPop 的消费者
        return true;
    }

    /// @brief 带超时的阻塞出队（消费者使用）
    /// @param[out] out 成功时写入取出的元素
    /// @param timeout 最多等待时长
    /// @return true=取到元素；false=超时仍为空，或队列已关闭且无残留元素
    /// @note RoomWorker 传 3ms：有消息立刻处理，没消息也能定期醒来推进
    ///       30Hz 游戏节拍，不会被队列无限期阻塞。
    bool waitPop(T& out, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        // wait_for 等价于 while (!(关闭 || 非空)) 睡眠；被唤醒后会重新检查谓词，
        // 自动过滤“虚假唤醒”。返回 false 表示超时且谓词仍不满足。
        if (!notEmpty_.wait_for(lock, timeout, [&] {
                return closed_ || !queue_.empty();
            })) {
            return false; // 等待超时：消费者长时间没取到元素
        }
        if (queue_.empty()) {
            return false;   // 被唤醒是因为 close()，且没有残留元素
        }
        out = std::move(queue_.front());
        queue_.pop_front();
        notFull_.notify_one();   // 腾出一个空位，叫醒可能在 waitPush 的生产者
        return true;
    }

    /// @brief 带超时的阻塞入队（关键消息专用）
    /// @param value 待入队元素
    /// @param timeout 队列满时最多等待多久
    /// @return true=入队成功；false=超时仍满，或队列已关闭
    /// @note Worker 的登录响应、入房绑定(BindClient)等消息走这里（100ms），
    ///       避免关键消息丢失导致客户端永久卡死。
    bool waitPush(T value, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!notFull_.wait_for(lock, timeout, [&] {
                return closed_ || queue_.size() < capacity_;
            })) {
            return false;   // 等待超时：消费者长时间没腾出位置
        }
        if (closed_) {
            return false;   // 关闭状态下拒绝入队
        }
        queue_.push_back(std::move(value));
        notEmpty_.notify_one();
        return true;
    }

    /// @brief 关闭队列：唤醒所有正在等待的生产者/消费者
    /// @note 关闭后 waitPop 可取完残留元素但取不到新元素；waitPush 直接失败。
    ///       用于停机阶段让工作线程从等待中醒来并退出循环。
    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        notEmpty_.notify_all();
        notFull_.notify_all();
    }

    /// @brief 重新打开队列（start() 时调用，使队列对象可被复用）
    void reopen() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = false;
    }

    /// @brief 查询队列是否已关闭（加锁，线程安全）
    bool closed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    /// @brief 当前元素个数（监控指标用，如控制台日志中的队列积压深度）
    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    /// @brief 容量上限（构造后不变，无需加锁）
    std::size_t capacity() const { return capacity_; }

private:
    const std::size_t capacity_;            ///< 容量上限，const 构造后不可变
    mutable std::mutex mutex_;              ///< 保护下方全部成员的唯一锁；
                                            ///< mutable 允许 const 方法 size() 加锁
    std::condition_variable notEmpty_;      ///< “非空”铃铛：消费者睡此，push 后摇响
    std::condition_variable notFull_;       ///< “有空位”铃铛：waitPush 等待时使用
    std::deque<T> queue_;                   ///< 实际存储，头尾增删均为 O(1)
    bool closed_ = false;                   ///< 关闭标志，关闭后不再接受新元素
};

}  // namespace blob
