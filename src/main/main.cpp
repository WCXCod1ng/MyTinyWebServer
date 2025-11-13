#include <format>
#include <iostream>
#include <semaphore>

// 使用std::counting_semaphore通过构造函数初始化
std::counting_semaphore<> worker_slots(4);


void task(int id) {
    // 阻塞等待
    worker_slots.acquire();
    std::cout << std::format("Task {} is running.", id);
    // 模拟睡眠一秒钟
    std::this_thread::sleep_for(std::chrono::seconds(1));
    std::cout << std::format("Task {} finished.", id);
    worker_slots.release();
}

int main()
{
    std::cout << std::format("hello from {}, that's all", 1);
    std::cout << "Hello, World!" << std::endl;
    return 0;
}
