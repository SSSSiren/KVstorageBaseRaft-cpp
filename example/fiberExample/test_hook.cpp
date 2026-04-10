#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include "monsoon.h"

const std::string LOG_HEAD = "[TASK] ";

void test_sleep() {
  std::cout << LOG_HEAD << "tid = " << monsoon::GetThreadId() << ",test_fiber_sleep begin" << std::endl;

  // IOManager 除了处理 IO 事件，也常常承担 hook 后阻塞调用的调度基础设施。
  // 这里使用 1 个工作线程 + caller 线程参与调度，方便观察：
  // 同一个线程里写出来像阻塞代码，底层却可以被 hook 改造成协作式让出。
  monsoon::IOManager iom(1, true);

  iom.scheduler([] {
    // 如果 sleep 被 hook，这里不会粗暴阻塞整条线程，
    // 而是让当前协程挂起，在到时后再被调度回来继续执行。
    while (1) {
      sleep(6);
      std::cout << "task 1 sleep for 6s" << std::endl;
    }
  });

  iom.scheduler([] {
    // 第二个任务也在同一个 IOManager 中运行。
    // 如果 hook 生效，你会观察到两个“看起来都是阻塞 sleep”的任务能交替输出。
    while (1) {
      sleep(2);
      std::cout << "task2 sleep for 2s" << std::endl;
    }
  });

  std::cout << LOG_HEAD << "tid = " << monsoon::GetThreadId() << ",test_fiber_sleep finish" << std::endl;
}

void test_sock() {
  // 这个示例用于演示：connect / send / recv 这类传统阻塞 socket 调用，
  // 在 hook 生效后也能写得像同步代码，但底层由调度器接管等待过程。
  int sock = socket(AF_INET, SOCK_STREAM, 0);

  sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(80);
  inet_pton(AF_INET, "36.152.44.96", &addr.sin_addr.s_addr);

  std::cout << "begin connect" << std::endl;
  int rt = connect(sock, (const sockaddr *)&addr, sizeof(addr));
  std::cout << "connect rt=" << rt << " errno=" << errno << std::endl;

  if (rt) {
    // 连接失败直接返回。
    return;
  }

  // 发起一个最简 HTTP 请求，便于验证 socket 流程。
  const char data[] = "GET / HTTP/1.0\r\n\r\n";
  rt = send(sock, data, sizeof(data), 0);
  std::cout << "send rt=" << rt << " errno=" << errno << std::endl;

  if (rt <= 0) {
    return;
  }

  std::string buff;
  buff.resize(4096);

  rt = recv(sock, &buff[0], buff.size(), 0);
  std::cout << "recv rt=" << rt << " errno=" << errno << std::endl;

  if (rt <= 0) {
    return;
  }

  buff.resize(rt);
  std::cout << "--------------------------------" << std::endl;
  std::cout << buff << std::endl;
  std::cout << "--------------------------------" << std::endl;
}

int main() {
  // 如果想演示 socket hook，可以打开下面两行。
  monsoon::IOManager iom;
  iom.scheduler(test_sock);

  // 当前默认跑 sleep hook 示例，更容易直观看到“阻塞调用被协程化”的效果。
  // test_sleep();
}
