#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include "monsoon.h"

// 这里把套接字设成全局变量，是为了让多个回调都能访问同一个 fd。
// 在真实工程里通常会封装成连接对象，而不是直接暴露全局变量。
int sockfd;

// 单独拆出来的原因是：读事件是一次性的，每次处理完后需要重新注册。
void watch_io_read();

// 写事件回调。
// 对非阻塞 connect 而言，“套接字可写”通常表示连接结果已经出来了，
// 但是否成功还要再用 getsockopt(SO_ERROR) 确认一次。
void do_io_write() {
  std::cout << "write callback" << std::endl;
  int so_err;
  socklen_t len = size_t(so_err);
  getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &so_err, &len);
  if (so_err) {
    std::cout << "connect fail" << std::endl;
    return;
  }
  std::cout << "connect success" << std::endl;
}

// 读事件回调。
// 这里演示 IOManager 的事件驱动模式：
// 1. fd 上有读事件时，调度器回调这个函数
// 2. 这里执行一次 read
// 3. 如果连接还在，想继续关注后续可读事件，就要重新注册 READ 事件
void do_io_read() {
  std::cout << "read callback" << std::endl;
  char buf[1024] = {0};
  int readlen = 0;
  readlen = read(sockfd, buf, sizeof(buf));
  if (readlen > 0) {
    buf[readlen] = '\0';
    std::cout << "read " << readlen << " bytes, read: " << buf << std::endl;
  } else if (readlen == 0) {
    // Question: 这里peer closed要表示的是什么情况？
    // Answer:
    // peer 指的是“连接对端”，这里就是远端服务器。
    // read 返回 0 在 TCP 里通常表示：
    // 对端已经正常关闭了写方向，当前连接收到了 EOF，再也读不到新数据了。
    // 这不是“暂时没数据”，而是“这条连接已经结束”，所以这里应关闭本地 sockfd 并返回。
    std::cout << "peer closed";
    close(sockfd);
    return;
  } else {
    // Question: 这里的errno变量哪里来的？
    // Answer:
    // errno 是 C/POSIX 运行时提供的线程局部错误码变量。
    // 很多系统调用失败时不会直接返回详细原因，而是：
    // 1. 先返回 -1 或其他失败标记
    // 2. 再把具体错误原因写到 errno 里
    // 头文件通常经由 <unistd.h> / <fcntl.h> / <sys/socket.h> 等间接带进来。
    // strerror(errno) 则是把这个错误码翻译成人类可读的字符串。
    std::cout << "err, errno=" << errno << ", errstr=" << strerror(errno) << std::endl;
    close(sockfd);
    return;
  }
  // 读完之后如果连接未关闭，需要重新订阅下一次读事件。
  // 这里不直接在当前回调里 addEvent，而是把“重新注册读事件”包装成一个新任务丢回调度器，
  // 避免还处在旧事件上下文时重复注册相同事件。
  // Question: 什么情况下会出现重复注册相同事件的问题？带来了什么后果？注册是哪个操作？为什么需要注册？
  // Answer:
  // “注册事件”指的就是 addEvent(sockfd, monsoon::READ, do_io_read) 这个操作。
  // 它的含义是：告诉 IOManager/epoll“以后这个 fd 一旦可读，就回调 do_io_read”。
  //
  // 会出现重复注册问题的典型场景是：
  // 当前 do_io_read 这个回调还没完全退出，但你又立刻对同一个 fd、同一个 READ 事件执行一次 addEvent。
  // 从逻辑上看，相当于旧的读事件上下文还没清理完，你又重复订阅了一次同类事件。
  //
  // 可能带来的后果包括：
  // 1. IOManager 内部状态不一致，认为同一事件被重复监听
  // 2. 断言失败、重复回调、覆盖旧回调，或者直接返回注册失败
  // 3. 更难排查的情况是事件边界混乱，导致调试困难
  //
  // 为什么需要重新注册？
  // 因为这里的读事件设计成一次性消费：本次 READ 触发并回调后，后续如果还想继续监听下一次可读，就必须再次订阅。
  // Question：代码是如何保证读事件是一次性消费的？为什么写事件不设计成一次性消费？
  // Answer:
  // 这段代码层面能观察到“它按一次性消费来使用”，依据有两个：
  // 1. READ 事件触发后，do_io_read() 本身并不会自动继续监听
  // 2. 想监听下一次可读，必须显式再调一次 addEvent(...)
  //
  // 也就是说，不管 IOManager 内部是用 epoll 的何种细节实现，
  // 从这个示例暴露出来的使用语义看，当前框架把一次 addEvent + 一次回调
  // 视为一轮完整消费，之后监听关系就结束了。
  //
  // 写事件这里通常不需要像读事件那样持续监听，原因是它的职责不同：
  // 1. 这里注册 WRITE 只是为了判断“非阻塞 connect 是否完成”
  // 2. 一旦 connect 成功或失败，目的就达到了，后续没有必要反复监听“可写”
  // 3. TCP 套接字在大多数时候本来就经常是可写的，如果持续监听 WRITE，反而容易产生大量无意义回调
  //
  // 所以这个示例里：
  // - READ 更像“数据来了就处理一次，之后视情况继续订阅”
  // - WRITE 更像“只等一个连接完成信号，用完就结束”
  monsoon::IOManager::GetThis()->scheduler(watch_io_read);
}

void watch_io_read() {
  std::cout << "watch_io_read" << std::endl;
  // 重新订阅 sockfd 的 READ 事件。
  monsoon::IOManager::GetThis()->addEvent(sockfd, monsoon::READ, do_io_read);
}

void test_io() {
  // 1. 创建 TCP 套接字
  sockfd = socket(AF_INET, SOCK_STREAM, 0);
  monsoon::CondPanic(sockfd > 0, "scoket should >0");

  // 2. 设为非阻塞。
  // 这是 IOManager 示例的关键前提：connect/read 等不会把线程卡死，
  // “等待就绪”的那部分工作由 IO 事件驱动完成。
  fcntl(sockfd, F_SETFL, O_NONBLOCK);

  // 3. 构造目标地址。这里直接连一个外部 HTTP 服务，只是为了演示事件驱动流程。
  // Question: 这个外部http服务具体是什么？
  // Answer:
  // 从代码本身只能确定：这是一个监听在 36.152.44.96:80 的 HTTP 服务。
  // 它更像是“随便找一个能连通、会返回 HTTP 数据的公网服务”来演示流程，
  // 而不是这个项目业务的一部分。
  // 所以这里真正重要的不是“它到底属于哪家公司/哪个站点”，
  // 而是它提供了一个可连接、可读写的 TCP/HTTP 目标，方便观察非阻塞 connect 和 READ/WRITE 事件回调。
  sockaddr_in servaddr;
  memset(&servaddr, 0, sizeof(servaddr));
  servaddr.sin_family = AF_INET;
  servaddr.sin_port = htons(80);
  inet_pton(AF_INET, "36.152.44.96", &servaddr.sin_addr.s_addr);

  // 4. 发起非阻塞 connect。
  int rt = connect(sockfd, (const sockaddr *)&servaddr, sizeof(servaddr));
  if (rt != 0) {
    if (errno == EINPROGRESS) {
      std::cout << "EINPROGRESS" << std::endl;
      // 非阻塞 connect 立即返回 EINPROGRESS 是正常现象，
      // 表示“连接还在建立中”。
      //
      // 之后通过 IOManager 监听 WRITE 事件：
      // 当 fd 可写时，说明 connect 过程结束了，再用 SO_ERROR 判断成败。
      monsoon::IOManager::GetThis()->addEvent(sockfd, monsoon::WRITE, do_io_write);

      // 同时也可以注册 READ 事件。
      // 注意这里的事件是一次性的，回调执行完后需要手动重新注册。
      // Question: 为什么这里直接add do_io_read 而不是 add watch_io_read？
      // Answer:
      // 因为这里第一次注册 READ 事件的目标，就是“当 sockfd 真正可读时，立刻执行一次 read 处理”。
      // do_io_read 正是实际消费数据的回调，所以首次注册直接挂它最自然。
      //
      // watch_io_read 的职责并不是“处理读事件”，而是“重新注册下一轮 READ 监听”。
      // 它更像一个辅助函数，专门在 do_io_read 已经执行完之后，延迟发起下一次订阅。
      //
      // 如果这里一开始注册的是 watch_io_read，那么当 fd 第一次可读时，
      // 回调里只会再次 addEvent，而不会真正 read 数据，相当于多绕了一层：
      // 第一次可读 -> 只重新注册 -> 下一次再可读时才真正进入 do_io_read
      // 这既不直观，也可能让第一次已经到来的可读机会被浪费掉。
      monsoon::IOManager::GetThis()->addEvent(sockfd, monsoon::READ, do_io_read);
    } else {
      std::cout << "connect error, errno:" << errno << ", errstr:" << strerror(errno) << std::endl;
    }
  } else {
    // connect 直接成功的情况在非阻塞场景下较少，但也是合法分支。
    std::cout << "else, errno:" << errno << ", errstr:" << strerror(errno) << std::endl;
  }
}

void test_iomanager() {
  // 默认构造一个 IOManager。
  // 可以先把它理解成“带 IO 事件处理能力的协程调度器”。
  monsoon::IOManager iom;
  // monsoon::IOManager iom(10); // 演示多线程下IO协程在不同线程之间切换

  // 把 test_io 作为任务交给 IOManager。
  // 后续 connect/read/write 的等待与回调，都由 IOManager 接管。
  iom.scheduler(test_io);
}

int main(int argc, char *argv[]) {
  test_iomanager();

  return 0;
}
