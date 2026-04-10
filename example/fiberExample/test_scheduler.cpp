#include "monsoon.h"

// 统一的日志前缀，便于观察多个任务在不同线程中的调度顺序。
const std::string LOG_HEAD = "[TASK] ";

void test_fiber_1() {
  // 一个最简单的任务：几乎不做任何阻塞操作，打印后立即结束。
  // 用它来观察调度器如何执行普通函数任务。
  std::cout << LOG_HEAD << "tid = " << monsoon::GetThreadId() << ",test_fiber_1 begin" << std::endl;

  std::cout << LOG_HEAD << "tid = " << monsoon::GetThreadId() << ",test_fiber_1 finish" << std::endl;
}

void test_fiber_2() {
  // 这里特意保留原生 sleep，目的是说明：
  // 如果没有 hook，sleep 会直接阻塞当前执行线程。
  // 单线程调度下，这会让整个调度器都停在这里；
  // 多线程调度下，只会阻塞当前那个工作线程。
  std::cout << LOG_HEAD << "tid = " << monsoon::GetThreadId() << ",test_fiber_2 begin" << std::endl;
  // no hook 直接将当前协程阻塞，等效于将当前线程阻塞
  sleep(3);
  std::cout << LOG_HEAD << "tid = " << monsoon::GetThreadId() << ",test_fiber_2 finish" << std::endl;
}

void test_fiber_3() {
  // 和 test_fiber_1 类似，只是后面会演示“显式构造 Fiber 再交给调度器”。
  std::cout << LOG_HEAD << "tid = " << monsoon::GetThreadId() << ",test_fiber_3 begin" << std::endl;

  std::cout << LOG_HEAD << "tid = " << monsoon::GetThreadId() << ",test_fiber_3 finish" << std::endl;
}

void test_fiber_4() {
  // 这个任务用于演示：调度器 start 之后仍然可以继续追加任务。
  std::cout << LOG_HEAD << "tid = " << monsoon::GetThreadId() << ",test_fiber_4 begin" << std::endl;

  std::cout << LOG_HEAD << "tid = " << monsoon::GetThreadId() << ",test_fiber_4 finish" << std::endl;
}

// use_caller = true，表示“当前线程自己也参与调度”。
// 这个版本等价于：
// 1. main 线程先把任务塞进调度器
// 2. stop() 时切换进调度器主协程，开始真正跑这些任务
// 3. 所有任务结束后，再切回 main 线程继续往下执行
//
// 因为这里没有额外工作线程，所以它最适合用来理解 Scheduler 的基本行为。
void test_user_caller_1() {
  std::cout << "main begin" << std::endl;

  // 默认构造下，调度线程数较少，且当前线程会作为 caller 参与调度。
  // 先不要把它想得太复杂，可以先理解成“main 线程兼任调度器线程”。
  monsoon::Scheduler sc;

  // 直接把函数对象作为任务交给调度器。
  sc.scheduler(test_fiber_1);
  sc.scheduler(test_fiber_2);

  // 也可以显式构造 Fiber 对象，再交给调度器。
  // 这说明 Scheduler 不只会调度普通函数，也能直接调度已有协程对象。
  monsoon::Fiber::ptr fiber(new monsoon::Fiber(&test_fiber_3));
  sc.scheduler(fiber);

  // start() 负责启动调度器。
  // 在“当前线程参与调度”的模式下，start() 本身不一定马上把任务都跑完，
  // Question: 不马上把任务都跑完会有什么好处吗，马上跑完会有什么影响？
  // Answer:
  // 不马上跑完的好处，是把“启动调度器”和“真正把当前线程切进调度循环”这两个动作解耦。
  // 这样 caller 线程在 start() 返回后，仍然可以继续往下做一些事情，比如继续提交任务、完成初始化、控制何时进入 stop() 收尾。
  // 如果 start() 立刻在当前线程把任务全跑完，那么：
  // 1. start() 会变成一个长时间阻塞调用，调用方很难继续做后续控制
  // 2. use_caller 模式下 main 线程会立即失去执行权，不利于理解调度器生命周期
  // 3. 已启动后继续追加任务的能力会变得不直观
  // 4. 工程上也不利于把“构造/启动”和“等待退出”分成两个阶段
  // 真正切进 caller 调度协程通常发生在 stop() 阶段。
  sc.start();

  // sc.scheduler(test_fiber_4);
  /*
   * stop() 的语义不是“粗暴终止”，而是“通知调度器收尾并等待任务执行完成”。
   * 如果当前线程参与调度，那么 stop() 会先切进调度循环，把剩余任务跑完，
   * 等 run() 退出后才返回 main。
   */
  sc.stop();

  std::cout << "main end" << std::endl;
}

// 这个版本同时开启多个调度线程，并且 caller 线程也参与调度。
// 因此它更适合观察：
// 1. 同一批任务可能在不同线程上运行
// 2. start() 之后还能继续添加任务
// 3. 阻塞型任务只会卡住当前工作线程，而不是整个进程
void test_user_caller_2() {
  std::cout << "main begin" << std::endl;

  // 3 表示额外创建 3 个调度线程；
  // true 表示当前线程也参与调度。
  // 所以总的“可运行任务的线程上下文”不止一个。
  monsoon::Scheduler sc(3, true);

  sc.scheduler(test_fiber_1);
  sc.scheduler(test_fiber_2);

  monsoon::Fiber::ptr fiber(new monsoon::Fiber(&test_fiber_3));
  sc.scheduler(fiber);

  // start() 之后，工作线程已经开始从任务队列中取任务执行。
  sc.start();

  // 调度器启动后仍然可以继续追加任务。
  sc.scheduler(test_fiber_4);

  // 留一点时间，便于观察多线程调度和阻塞任务的输出。
  sleep(5);
  /*
   * 等所有待执行任务完成后，调度线程退出。
   * 如果 caller 线程参与调度，还会在退出前切回 main 所在线程继续向下执行。
   Question: caller线程不是main所在线程吗，caller与工作线程的区别是什么？
   * Answer:
   * caller 线程通常就是“调用 Scheduler 的那个线程”，在这个例子里就是 main 线程。
   * 但 caller 和工作线程的区别不在“是不是同一个物理线程名”，而在“职责和进入方式”：
   * 1. caller 线程是外部业务线程，本来就存在；当 use_caller=true 时，它会被借用来顺带参与调度
   * 2. 工作线程是 Scheduler 自己新创建出来、专门负责跑调度循环的线程
   * 3. caller 线程除了跑调度，还要负责 start()/stop() 这一层生命周期控制；工作线程通常只负责取任务并执行
   * 4. caller 线程是否参与调度是可选的；工作线程的存在是为了提供额外并发执行能力
   *
   * 所以可以把 caller 理解成“兼任调度工作的主线程”，
   * 而工作线程是“专职调度线程池里的线程”。
   */
  sc.stop();

  std::cout << "main end" << std::endl;
}

// 当前默认跑多线程版本，更容易观察调度器在线程间分发任务。
int main() { test_user_caller_2(); }
