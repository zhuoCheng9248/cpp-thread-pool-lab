# cpp-thread-pool-lab

一个基于 C++17 的固定大小线程池。支持提交任意可调用对象并在需要时通过 `std::future` 获取返回值或异常；销毁时执行优雅关闭：停止接收新任务，等待队列中剩余任务执行完毕后回收所有工作线程。

## 特性

- 固定数量的工作线程在构造时创建、全程复用，避免频繁创建/销毁线程的开销。
- `submit()` 接受任意可调用对象与参数，返回 `std::future<R>`，异步获取返回值。
- 任务中抛出的异常由 `packaged_task` 捕获，通过 `future` 重新抛出，不会终止工作线程。
- 基于 `std::mutex` + `std::condition_variable` 的任务队列，无忙等待，空闲线程休眠。
- 优雅关闭：析构时先置位停止标志并唤醒全部线程，已入队的任务仍会被执行完毕。
- 关闭后调用 `submit()` 会立刻抛出 `std::runtime_error`，避免任务被静默丢弃。

## 环境要求

- 支持 C++17 的编译器（GCC 7+ / Clang 5+ / MSVC 2017+）
- POSIX 线程库（Linux/macOS 下需要 `-pthread`）

## 构建与运行

```bash
g++ -std=c++17 -O2 -pthread threadPoolDemo.cpp -o thread_pool_demo
./thread_pool_demo
```

期望输出（`worker start` 的出现顺序与线程调度有关）：

```text
worker start
worker start
10
30
42
worker start
worker start
```

## 快速开始

```cpp
#include "threadPoolDemo.cpp"  // 或抽取为 thread_pool.hpp

int main() {
  ThreadPool pool(4);  // 4 个工作线程

  // 无参任务
  std::future<int> f1 = pool.submit([] { return 10; });

  // 带参任务，参数会被转发给可调用对象
  std::future<int> f2 = pool.submit([](int a, int b) { return a + b; }, 10, 20);

  // 返回值类型自动推导，无需显式指定
  auto f3 = pool.submit([](int x) { return x * 2; }, 21);

  std::cout << f1.get() << '\n';  // 10
  std::cout << f2.get() << '\n';  // 30
  std::cout << f3.get() << '\n';  // 42
}
```

`get()` 会阻塞直到任务完成；若任务内部抛出异常，该异常会在 `get()` 处重新抛出：

```cpp
auto f = pool.submit([]() -> int { throw std::runtime_error("boom"); });
try {
  f.get();
} catch (const std::runtime_error& e) {
  std::cerr << "caught: " << e.what() << '\n';
}
```

## API

| 成员 | 说明 |
| --- | --- |
| `ThreadPool(size_t n)` | 创建 `n` 个工作线程，并让它们进入等待任务的循环。 |
| `template <class F, class... Args> auto submit(F&& f, Args&&... args)` | 提交任务，返回 `std::future<ReturnType>`。池已关闭时抛出 `std::runtime_error`。 |
| `~ThreadPool()` | 置位停止标志，唤醒所有线程，等待已入队任务执行完毕后 `join` 所有线程。 |

`ReturnType` 由 `std::invoke_result_t<F, Args...>` 推导，因此 `void` 返回类型的任务同样适用（此时 `submit` 返回 `std::future<void>`）。

## 内部实现

一次 `submit(f, args...)` 的完整链路是：`std::bind` 把 `f` 与 `args` 绑定成无参可调用对象 → 移入 `std::packaged_task<R()>` → 用 `std::shared_ptr` 持有它 → 以 lambda 形式转成 `std::function<void()>` 入队 → 工作线程取出并执行 `packaged_task` → 调用方从 `future` 取回结果。

各部分的职责如下：

- **任务队列**：`std::queue<std::function<void()>>`，是所有工作线程共享的临界资源，出入队均在 `mtx` 保护下进行。
- **同步原语**：`std::mutex` + `std::condition_variable`。工作线程在 `cv.wait(lock, pred)` 上休眠，谓词为“队列非空或已停止”，既不会忙等，也不会漏掉唤醒。
- **类型擦除**：`submit` 需要把返回类型各异的任务放进同一个队列，`std::function` 负责擦除类型。由于 `std::function` 要求其包装的可调用对象可拷贝，而 `std::packaged_task` 是 move-only 类型，这里用 `std::shared_ptr` 包一层，捕获该指针的 lambda 即为可拷贝。
- **结果与异常传递**：`std::packaged_task` 执行时把返回值或异常写入共享状态，调用方从关联的 `future` 中取出。
- **工作线程循环**：加锁取任务、解锁执行任务。执行阶段不持锁，避免任务内的耗时操作阻塞其他线程入队/出队。
- **优雅关闭**：`stop` 标志与队列同受 `mtx` 保护。析构函数先置位 `stop` 再 `notify_all`；工作线程醒来后只有在“已停止且队列为空”时才退出，因此不会丢弃已提交的任务。

## 使用注意

- **不要在任务中同步等待同一个池上的其他任务**（例如在任务内部 `get()` 另一个提交到本池的 `future`）。工作线程数量有限，互相等待会耗尽线程并造成死锁。
- 参数按 `std::bind` 的语义保存，即以值拷贝（`std::ref` / `std::cref` 除外）。需要传递引用或 move-only 参数时需显式包装。
- 析构会等待所有已入队任务执行完毕；若某个任务耗时很长，析构也会相应阻塞。

## Roadmap

按主题分组，每项标注了动机，便于挑选下一步要做的内容。

### 1. 接口与实现细节

- [ ] **用 lambda + `std::invoke` 替换 `std::bind`**
  当前实现里 `std::bind` 会丢失完美转发：参数在绑定时即被按值保存并衰减，传引用必须显式写 `std::ref`；它也无法接受重载函数名与模板函数名。改成
  `[f = std::forward<F>(f), ...args = std::forward<Args>(args)...]() mutable -> decltype(auto) { return std::invoke(f, std::move(args)...); }`
  可同时解决这些问题，也不再需要 `std::bind` 的占位符语义。
- [ ] **支持 move-only 参数**
  `pool.submit([](std::unique_ptr<int> p) { return *p; }, std::make_unique<int>(1))` 目前编译失败（`std::invoke_result` 推导不出类型），因为 `std::bind` 保存的实参无法在其调用约定下被移动。改用以捕获列表按值保存参数的 lambda 即可天然支持。
- [ ] **用 move-only function 包装器替代 `shared_ptr` 方案**
  目前为了把 move-only 的 `packaged_task` 放进 `std::function`，用 `shared_ptr` 包了一层，每个任务多一次堆分配与一层间接。可以实现满足 `std::function` 子集的 `UniqueFunction<void()>`（C++23 起也可直接用 `std::move_only_function`），把 `packaged_task` 直接移进队列。
- [ ] **显式禁用拷贝与拷贝赋值**
  `ThreadPool(const ThreadPool&) = delete;` 等。虽然 `std::mutex` 成员已隐式删除拷贝构造，但显式声明能表达意图，也让错误信息更清晰。
- [ ] **补齐头文件与接口边界**
  显式包含 `<memory>`（`std::make_shared` 目前靠 `<future>` 传递包含）；把实现从 `threadPoolDemo.cpp` 拆成 `thread_pool.hpp` 与独立的 `main.cpp`，使库代码可被复用。
- [ ] **构造函数支持默认线程数**
  例如以 `std::thread::hardware_concurrency()` 作为默认值，并处理其返回 0 的情况。
- [ ] **`submit` 的返回值语义细化**
  提供一个不返回 `future` 的 `submit_detached()`，用于不需要结果的场景，省去共享状态的分配；同时明确 `future` 被丢弃时任务的执行保证。
- [ ] **异常安全兜底**
  为工作线程的执行体加 `try/catch`，防止非 `packaged_task` 路径（或未来的 detach 接口）中逃逸的异常触发 `std::terminate`。

### 2. 生命周期与调度能力

- [ ] **显式的 `shutdown()` / `wait_for_all()`**
  把“停止接收任务”“等待队列排空”“回收线程”拆成可独立调用的操作，而不是只能靠析构，例如 `shutdown(ShutdownMode::Drain | Discard)`、`wait_idle()`。
- [ ] **关闭时可选择丢弃或取消未开始的任务**
  目前析构必须等所有任务跑完，长任务会拖住析构。可提供 `cancel_pending()`：清空队列，让对应 `future` 收到 `broken_promise` 异常。
- [ ] **任务优先级**
  把 `std::queue` 换成 `std::priority_queue` 或分层队列，支持“紧急任务插队”，这在响应式服务里是常见需求。
- [ ] **延迟 / 定时任务**
  支持 `submit_after(duration, f)`：维护一组按到期时间排序的定时任务，由条件变量的 `wait_until` 驱动，工作线程等待最近任务的到期时间。
- [ ] **动态调整线程数量**
  按队列长度自动扩容 / 缩容（空闲超时后回收），需要空闲线程计数与相应的唤醒、退出协议，注意扩容时的锁与通知时机。
- [ ] **任务取消机制**
  引入 C++20 的 `std::stop_token`，让长时间运行的任务可以协作式中断。
- [ ] **C++20 `std::jthread` 版本**
  用 `std::jthread` 与 `std::stop_source` 替代手写的 `stop` 标志 + `join` 逻辑，作为对比实现。

### 3. 性能

- [ ] **降低任务入队的分配次数**
  当前每个任务至少有一次 `shared_ptr` 分配（见第 1 节），任务量大时是明显开销。可考虑小对象内联存储（SBO）或对象池。
- [ ] **无锁 / 低竞争队列**
  用 MPSC 环形缓冲（如 Vyukov 队列）替换互斥锁保护的 `std::queue`，减少高并发入队时的锁竞争；需权衡正确性与复杂度，并用 TSan 验证。
- [ ] **单队列 → 每线程队列 + work stealing**
  多核下单一队列的缓存行争用会限制扩展性。每工作线程一个本地队列、空闲时从其他队列窃取任务，是进一步提升吞吐的经典方案。
- [ ] **自适应唤醒策略**
  评估队列积压时一次唤醒多个线程（`notify_all` 或按积压量唤醒）对吞吐与延迟的影响。
- [ ] **量化类型擦除开销**
  对比 `std::function` 与函数指针 + 参数块方案，统计每次任务投递的指令数与分配次数。
- [ ] **伪共享（false sharing）排查**
  统计计数器、空闲标记等共享变量按缓存行对齐（`alignas(std::hardware_destructive_interference_size)`）。
- [ ] **基线对比与压测**
  与“每任务一个 `std::thread`”以及 `std::async` 对比固定任务量下的总耗时；给出任务数 / 线程数 / 任务耗时的扩展性曲线。

### 4. 可观测性与运维

- [ ] **移除调试输出**
  工作线程里的 `std::cout << "worker start\n"` 是临时调试语句，且多线程并发写 `std::cout` 会交错。改为可开关的日志，或直接删除。
- [ ] **运行时统计**
  暴露 `pending_tasks()`、`active_workers()`、`completed_tasks()` 等指标（原子计数器），便于压测时观察池的状态。
- [ ] **线程命名与亲和性**
  为工作线程设置有意义的名称（`pthread_setname_np`），便于 profiling；可选地将线程绑定到指定 CPU 核。

### 5. 工程化

- [ ] **CMake 构建**
  提供 `CMakeLists.txt`，包含 `-Wall -Wextra -Wpedantic`、`-pthread`，并区分 Release / Debug 配置。
- [ ] **单元测试**
  引入 GoogleTest 或 Catch2，覆盖：返回值正确性、`void` 任务、异常传播、参数转发（含引用与 move-only）、关闭后 `submit` 抛异常、析构时队列中的任务不丢失、空闲线程确实在等待（无忙等）。
- [ ] **并发压力测试与 Sanitizer**
  大量任务下的压力测试；用 TSan（`-fsanitize=thread`）验证无数据竞争，用 ASan / UBSan 验证内存与未定义行为，并接入 CI。
- [ ] **CI**
  GitHub Actions：多编译器（GCC / Clang）、多标准（C++17 / 20）矩阵构建 + 测试 + Sanitizer。
- [ ] **`.gitignore` 与构建产物**
  忽略 `a.out`、`build/` 等，避免误提交二进制文件（当前仓库根目录已有未跟踪的 `a.out`）。
- [ ] **基准测试目标**
  单独的可执行文件 + 简单计时框架，输出可复现的性能数字，用于验证第 3 节的优化收益。

### 6. 文档

- [ ] **补一张时序图**
  目前 README 只描述了任务提交链路，可以补调用时序图（提交线程视角 + 工作线程视角），并说明锁的持有范围。
- [ ] **补充“常见陷阱”示例**
  例如任务内互相等待导致的线程饥饿与死锁，给出错误写法与正确写法对照。
- [ ] **示例程序拆分**
  把 `main` 中的演示拆成 `examples/` 下的多个小例子（返回值、异常、批量提交、关闭语义），并在 README 中引用。

## 项目结构

```text
.
├── threadPoolDemo.cpp   # 线程池实现与示例 main
├── README.md
└── LICENSE
```

## License

[MIT](LICENSE)
