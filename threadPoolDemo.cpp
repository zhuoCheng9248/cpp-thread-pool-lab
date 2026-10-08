#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

class ThreadPool {
 public:
  ThreadPool(size_t n);
  ~ThreadPool();

  template <class F, class... Args>
  auto submit(F&& f, Args&&... args);

 private:
  std::vector<std::thread> workers;

  std::mutex mtx;
  std::condition_variable cv;
  // Two shared resources that require protection.
  std::queue<std::function<void()>> tasks;
  bool stop = false;
};

ThreadPool::ThreadPool(size_t n) {
  for (size_t i = 0; i < n; i++) {
    workers.emplace_back(
        [this] {  // The 'this' pointer needs to be passed here.
          std::cout << "worker start\n";
          while (true) {
            std::function<void()> task;
            {
              std::unique_lock<std::mutex> lock(mtx);

              cv.wait(lock, [this] { return !tasks.empty() || stop; });
              if (stop && tasks.empty()) {
                break;
              }
              task = tasks.front();
              tasks.pop();
            }
            task();
          }
        });
  }
}

ThreadPool::~ThreadPool() {
  {
    std::lock_guard<std::mutex> lock(mtx);
    stop = true;
  }
  cv.notify_all();
  // Allow the members to be destroyed only after all threads have exited.
  for (auto& worker : workers) {
    worker.join();
  }
}

template <class F, class... Args>
auto ThreadPool::submit(F&& f, Args&&... args) {
  // 1. 将可调用对象 f 与其参数 args 绑定成一个无参可调用对象 bound
  //    调用 bound() 等价于调用 f(args...)，参数按值或引用正确传递
  auto bound = std::bind(std::forward<F>(f), std::forward<Args>(args)...);

  // 2. 编译期推导 bound() 的返回类型
  //    decltype(bound) 是 std::bind 返回的可调用对象类型
  //    std::invoke_result_t<...> 推导“调用它时返回什么类型”
  using ReturnType = std::invoke_result_t<decltype(bound)>;

  // 3. 用 packaged_task 包装 bound
  //    packaged_task 负责执行任务，并把结果或异常存入共享状态
  //    注意：packaged_task 是 move-only 类型，不可拷贝
  std::packaged_task<ReturnType()> task(std::move(bound));

  // 4. 从 packaged_task 获取关联的 future
  //    调用者之后可以通过这个 future 获取任务结果或异常
  auto future = task.get_future();

  // 5. 用 shared_ptr 包裹 packaged_task
  //    原因：std::function 要求其包装的可调用对象必须可拷贝，
  //    而 packaged_task 是 move-only，不能直接放入 std::function。
  //    shared_ptr 本身可拷贝，因此捕获 shared_ptr 的 lambda 也可拷贝。
  auto taskPtr =
      std::make_shared<std::packaged_task<ReturnType()>>(std::move(task));

  // 6. 将任务放入共享队列（临界区）
  {
    std::lock_guard<std::mutex> lock(mtx);
    if (stop) {
      throw std::runtime_error("ThreadPool is stopped");
    }

    // lambda 按值捕获 shared_ptr，因此 lambda 是可拷贝的，
    // 可以安全地转换为 std::function<void()> 存入队列。
    // 执行时通过 (*taskPtr)() 调用 packaged_task。
    tasks.emplace([taskPtr] { (*taskPtr)(); });
  }  // 离开作用域自动解锁

  // 7. 唤醒一个正在等待的工作线程来处理新任务
  cv.notify_one();

  // 8. 将 future 返回给调用者，供其获取任务结果
  return future;
}

int main() {
  ThreadPool pool(4);
  auto f1 = pool.submit([] { return 10; });
  auto f2 = pool.submit([](int a, int b) { return a + b; }, 10, 20);
  auto f3 = pool.submit([](int x) { return x * 2; }, 21);

  std::cout << f1.get() << '\n';
  std::cout << f2.get() << '\n';
  std::cout << f3.get() << '\n';
}