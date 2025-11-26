//
// Created by user on 2025/11/26.
//

#ifndef CALLBACKS_H
#define CALLBACKS_H

#include <memory>
#include <functional>

class Buffer;
class TcpConnection;
class TimeStamp;

// 使用 shared_ptr 管理 TcpConnection 的生命周期
// 这是防止连接在处理过程中被意外销毁的关键
using TcpConnectionPtr = std::shared_ptr<TcpConnection>;

// --- 核心回调类型 ---

// 1. 连接建立或断开时的回调
// void(const TcpConnectionPtr&)
using ConnectionCallback = std::function<void(const TcpConnectionPtr&)>;

// 2. 收到消息时的回调 (业务层的核心入口)
// 参数：
//   - conn: 连接对象
//   - buffer: 缓冲区 (数据都在这里，用户负责取走)
//   - receiveTime: 请求接收的时间 (epoll_wait 返回的时间)
using MessageCallback = std::function<void(const TcpConnectionPtr&, Buffer*, TimeStamp)>;

// 3. 消息发送完成时的回调 (低水位回调)
// 当输出缓冲区的数据全部发送给内核后触发，通常用于解除“停止发送”的状态
using WriteCompleteCallback = std::function<void(const TcpConnectionPtr&)>;

// 4. 高水位回调 (流量控制)
// 当输出缓冲区积压数据超过阈值时触发，通常用于停止发送
using HighWaterMarkCallback = std::function<void(const TcpConnectionPtr&, size_t)>;

// 5. 关闭连接的回调 (内部使用，通知 Server 移除连接)
using CloseCallback = std::function<void(const TcpConnectionPtr&)>;

// 6. 定时器回调
using TimerCallback = std::function<void()>;


#endif //CALLBACKS_H
