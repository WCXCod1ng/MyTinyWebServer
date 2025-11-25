//
// Created by user on 2025/11/11.
//

#ifndef THREAD_POOL_H
#define THREAD_POOL_H
#include <functional>
#include <future>
#include <queue>
#include <vector>

#include "log/logger.h"

/// 这同样是一个生产者、消费者模型，因此我们在同步时也可以使用信号量。但是这里也可以使用另一种方式：条件变量，这也是现代C++编程实践中面对这类场景的更安全选择
/// 1. 语义更清晰：工作线程等待的不是一个抽象的“计数”，而是一个具体的“条件”——任务队列不为空。使用条件变量的代码 m_condition.wait(lock, [this]{ return !this->m_tasks.empty(); }); 能够非常直白地表达这个意图，可读性更高。
/// 2. 避免伪唤醒问题 (Spurious Wakeups)：线程有时可能会在没有被 notify 的情况下从 wait 中被唤醒，这就是“伪唤醒”。信号量无法处理这种情况，你必须在 wait() 之后手动加锁并写一个 while 循环来重新检查条件。而 std::condition_variable 的 wait 方法的谓词（predicate）版本完美地、自动地处理了这个问题，使代码更简洁、更健壮。
/// 3. 更强的表达能力：条件变量可以用来等待任意复杂的条件（例如 队列不满 或 队列为空 等），而信号量只能表示一个简单的计数值。notify_all() 的广播功能也比信号量更灵活，例如在线程池关闭时，可以一次性唤醒所有线程让它们退出。
class ThreadPool {
public:
    // constructor
    ThreadPool(size_t threads, size_t max_tasks);

    ~ThreadPool();

    // 任务入队函数：将一个任务添加到队列，并返回一个 future 以获取结果
    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args)
        -> std::future<typename std::invoke_result_t<F, Args...>>;

private:
    // 工作线程函数
    void worker_run();

    // 成员变量
    std::vector<std::thread> m_workers;           // 存储所有工作线程的容器
    size_t m_worker_number;                          // 工作线程的数量

    std::queue<std::function<void()>> m_tasks;    // 任务队列（也即请求队列）
    size_t m_max_tasks;                              // 最大请求数量

    std::mutex m_queue_mutex;                     // 保护任务队列的互斥锁
    std::condition_variable m_condition;          // 用于线程同步的条件变量
    std::atomic<bool> m_stop;                     // 原子布尔值，用于停止线程池
};

// 这里可以接受一个可调用对象和对应的参数列表。
// 可以使用concepts来约束F是callable的，但是没有办法做到参数类型任意。而不使用concepts约束则可以认为就是任意的
template<class F, class... Args>
auto ThreadPool::enqueue(F&& f, Args&&... args)
    -> std::future<typename std::invoke_result_t<F, Args...>> {

    // 1. 获取任务的返回类型
    using return_type = typename std::invoke_result_t<F, Args...>;

    // 2. 将任务函数和其参数打包
    // 使用 std::make_shared 创建智能指针，确保 task 对象在需要时能被安全地释放
    auto task = std::make_shared<std::packaged_task<return_type()>>(
        // std::bind 将函数 f 和其参数 args 绑定成一个可调用对象
        // std::forward 保持参数的值类别（左值/右值），实现完美转发
        std::bind(std::forward<F>(f), std::forward<Args>(args)...)
    );

    // 3. 获取与任务关联的 std::future
    std::future<return_type> res = task->get_future();

    // 4. 将任务放入队列（临界区）
    {
        // 使用 std::unique_lock 创建一个独占锁，在作用域结束时自动解锁
        std::unique_lock<std::mutex> lock(m_queue_mutex);

        // 如果设置了最大任务数，并且队列已满，则等待
        // 使用条件变量等待，直到 "线程池停止" 或 "任务队列未满"
        if (m_max_tasks > 0) {
            m_condition.wait(lock, [this]{
                return m_stop || (m_tasks.size() < m_max_tasks);
            });
        }

        // 检查线程池是否已经停止，或者等待后队列仍然是满的（可能因为停止而被唤醒）
        if (m_stop || (m_max_tasks > 0 && m_tasks.size() >= m_max_tasks)) {
            throw std::runtime_error("enqueue on stopped or full ThreadPool");
        }

        // 将任务封装成一个无返回值的 std::function<void()> 后放入队列
        m_tasks.emplace([task](){ (*task)(); });

        // 将任务加入线程池
        LOG_INFO("加入后队列的大小为：{}", m_tasks.size());
    } // 锁在这里被释放

    // 5. 通知一个等待中的工作线程
    m_condition.notify_one();
    return res;
}

inline ThreadPool::ThreadPool(const size_t threads, const size_t max_tasks = 10000):
    m_worker_number(threads),
    m_max_tasks(max_tasks),
    m_stop(false)
{
    if(threads <= 0) {
        throw std::invalid_argument("Thread pool size must be greater than zero.");
    }

    // 预留vector空间
    m_workers.reserve(m_worker_number);

    // 创建并启动指定数量的工作线程
    for(size_t i = 0; i < m_worker_number; ++i) {
        // 相当于n_workers.push_back(std::thread(xxx));
        m_workers.emplace_back([this] {
            this->worker_run();
        });
    }
}

inline ThreadPool::~ThreadPool() {
    // 1. 设置停止标志 (临界区)
    {
        // 加锁以保护 m_stop 标志的写入，确保所有线程都能看到一致的状态
        std::unique_lock<std::mutex> lock(m_queue_mutex);
        m_stop = true;
    } // 锁在这里释放

    // 2. 唤醒所有等待的线程
    // 使用 notify_all() 而不是 notify_one()，因为我们需要唤醒所有可能
    // 因为队列为空而陷入休眠的线程，让它们重新检查 m_stop 标志。
    m_condition.notify_all();

    // 3. 等待所有工作线程执行完毕
    // 遍历线程容器，并对每个线程调用 join()
    for (std::thread &worker : m_workers) {
        // join() 会阻塞当前线程（在这里是主线程），直到目标线程（worker）
        // 执行完毕并退出。
        worker.join();
    }
}

inline void ThreadPool::worker_run() {
    while (true) {
        std::function<void()> task;
        { // 使用花括号来限定 lock 的作用域
            // 上锁
            std::unique_lock<std::mutex> lock(m_queue_mutex);

            // 等待条件：线程池已停止 或 任务队列不为空
            m_condition.wait(lock, [this] {
                return m_stop || !m_tasks.empty();
            });

            // 关键的退出逻辑：
            // 只有在线程池停止 并且 任务队列也为空时，工作线程才能安全退出。
            // note 这种方式可以实现优雅停机：当线程池被关闭时，线程池中的任务队列会继续被处理直到剩余的所有任务都被处理完
            if (m_stop && m_tasks.empty()) {
                return;
            }

            // 从队列中取出一个任务
            // 使用 std::move 可以避免不必要的拷贝，提高效率
            task = std::move(m_tasks.front());
            m_tasks.pop();
            LOG_INFO("从线程池中提取一个任务并完成，提取后队列大小为{}", m_tasks.size());

        } // unique_lock 在这里作用域结束，自动解锁

        // 在无锁状态下执行任务
        task();
    }
}


#endif //THREAD_POOL_H
