#include <iostream>
#include "clerk.h"
#include "util.h"
int main() {
  // Clerk 是客户端抽象层。
  // 上层代码只需要调用 Put/Get，不需要自己关心：
  // 1. 当前 leader 是谁
  // 2. 请求失败后该向哪个节点重试
  // 3. 如何携带 ClientId/RequestId 实现幂等
  Clerk client;

  // 从配置文件中读取集群所有节点地址，初始化 RPC 调用对象。
  client.Init("test.conf");
  auto start = now();
  (void)start;
  int count = 500;
  int tmp = count;
  while (tmp--) {
    // 连续写同一个 key，用来观察集群写入和后续读取的线性一致性效果。
    client.Put("x", std::to_string(tmp));

    // 立刻读取，验证当前写入是否已经通过 leader 提交并对客户端可见。
    std::string get1 = client.Get("x");
    std::printf("get return :{%s}\r\n", get1.c_str());
  }
  return 0;
}
