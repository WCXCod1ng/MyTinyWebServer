### 场景假设
假设我们在 `main.cpp` 中写了如下代码（伪代码）：

```cpp
int main() {
    // 1. 主线程创建自己的 Loop
    EventLoop baseLoop; 
    
    // 2. 创建线程池，包含 2 个子线程
    EventLoopThreadPool pool(&baseLoop, "MyPool");
    pool.setThreadNum(2);
    pool.start(); // 启动子线程

    // 3. 获取下一个 Loop (轮询拿到 Loop-0)
    EventLoop* subLoop = pool.getNextLoop();

    // 4. 主线程命令 Loop-0 打印一句话
    subLoop->runInLoop([](){
        printf("Hello from SubThread!\n");
    });

    // 5. 主线程自己开始循环
    baseLoop.loop();
}
```

下面是这几行代码背后，我们刚刚实现的这些类是如何精密协作的。

---

### 阶段一：启动与组装 (Startup)

#### 1. 主线程创建 `baseLoop`
*   `EventLoop` 构造。
*   初始化 `EpollPoller`，创建一个 `epoll_create` 的句柄。
*   初始化 `wakeupFd_` (eventfd) 和 `wakeupChannel_`。
*   **关键动作**：`wakeupChannel_->enableReading()`。
    *   这会调用 `poller_->updateChannel()` -> `epoll_ctl(EPOLL_CTL_ADD)`。
    *   此时，`baseLoop` 的 epoll 内核表中已经监听了一个 fd（`wakeupFd`）。

#### 2. 线程池启动 (`pool.start()`)
*   `EventLoopThreadPool` 创建 2 个 `EventLoopThread` 对象。
*   调用 `EventLoopThread::startLoop()`。
    *   **主线程阻塞**：主线程卡在 `cond_.wait()`，等待子线程初始化完成。
    *   **子线程启动**：系统创建新线程，执行 `threadFunc`。
    *   **子线程初始化**：
        1.  在**子线程栈上**创建 `EventLoop` 对象（这就是 Sub Reactor）。
        2.  Sub Loop 初始化自己的 `EpollPoller` 和 `wakeupFd`。
    *   **唤醒主线程**：子线程获取到 `&loop` 地址，通过 `cond_.notify_one()` 告诉主线程“我好了”。
    *   **子线程运转**：子线程执行 `loop.loop()`，进入 `while(!quit)` 循环，阻塞在 `epoll_wait` 上（因为它还没连接，只监听了自己的 wakeupFd）。

---

### 阶段二：跨线程任务分发 (Dispatch)

这是目前最复杂的逻辑。

#### 3. 主线程分发任务 (`subLoop->runInLoop(...)`)
*   主线程调用 `subLoop->runInLoop(task)`。
*   **判断线程**：`subLoop` 检查 `threadId_`。
    *   发现：`CurrentThreadId` (主线程) != `threadId_` (子线程)。
    *   结论：不能直接运行，必须排队。
*   **加入队列 (`queueInLoop`)**：
    *   主线程获取 `subLoop` 的互斥锁 `mutex_`。
    *   将任务 push 到 `subLoop->pendingFunctors_` 向量中。
    *   释放锁。
*   **唤醒子线程 (`wakeup`)**：
    *   因为不在同一个线程，主线程必须叫醒正在睡觉的子线程。
    *   主线程向 `subLoop` 的 `wakeupFd_` 写入 8 字节数据 (`write(fd, &one, 8)`).

---

### 阶段三：子线程被唤醒与执行 (Execution)

此时，视角切换到 **子线程 (Sub Loop)**。它正阻塞在 `epoll_wait` 里。

#### 4. `EpollPoller` 感知事件
*   操作系统内核发现 `wakeupFd` 可读（因为主线程刚写了数据）。
*   `epoll_wait` 立即返回！
*   `Poller` 将 `wakeupChannel` 填入 `activeChannels` 列表，返回给 `EventLoop`。

#### 5. `EventLoop` 处理事件
*   `loop()` 函数拿到活跃通道列表。
*   **处理 Channel 事件**：
    *   调用 `wakeupChannel_->handleEvent()`。
    *   执行 `EventLoop::handleRead()` -> `read(wakeupFd, ...)`。
    *   **作用**：把那 8 字节读走，清空缓冲区。否则下次 `epoll_wait` 又会被立即触发（Level Trigger 模式下），或者防止缓冲区满。

#### 6. 执行 Pending Tasks
*   `loop()` 函数执行完 `Channel` 处理后，紧接着调用 `doPendingFunctors()`。
*   **Swap 队列**：
    *   加锁。
    *   把 `pendingFunctors_`（里面有主线程塞的打印任务）倒换到局部变量 `functors` 中。
    *   解锁。
*   **执行回调**：
    *   遍历 `functors`，执行那个 `printf` 任务。
    *   控制台输出："Hello from SubThread!"。

#### 7. 下一轮循环
*   子线程处理完所有任务，再次回到 `while(!quit)` 开头。
*   调用 `poller_->poll()`。
*   再次阻塞在 `epoll_wait`，等待下一个任务或（未来会有的）网络数据包。
