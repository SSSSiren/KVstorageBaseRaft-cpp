#include <iostream>

// #include "mprpcapplication.h"
#include "rpcExample/friend.pb.h"

#include "mprpcchannel.h"
#include "mprpccontroller.h"
#include "rpcprovider.h"

int main(int argc, char **argv) {
  // 这里直接指定 RPC 服务端地址。
  // 该示例不依赖注册中心，而是采用“客户端已知服务端 ip:port”的最简调用模式。
  // 127.0.1.1 在部分 Linux/WSL 环境下也会被映射到本机回环地址。
  std::string ip = "127.0.1.1";
  short port = 7788;

  // Stub 是 protobuf 为该服务生成的“客户端代理对象”。
  // 用户看起来是在调用本地函数 stub.GetFriendsList(...)，
  // 但实际会进入自定义的 MprpcChannel::CallMethod：
  // 1. 取出 service_name / method_name
  // 2. 序列化 request
  // 3. 按项目自定义协议组包
  // 4. 通过 TCP 发给远端 RpcProvider
  // 5. 同步等待响应并反序列化到 response
  fixbug::FiendServiceRpc_Stub stub(
      new MprpcChannel(ip, port, true));  // true 表示当前作为调用方使用 channel

  // request 对象承载“业务参数”。
  // 在真实 Raft 项目中，类似对象会承载日志复制参数、心跳参数、客户端读写请求等。
  fixbug::GetFriendsListRequest request;
  request.set_userid(1000);

  // response 对象承载远端业务方法的返回结果。
  // 注意：response 中的 errcode/errmsg 属于“业务层返回值”，
  // 与网络是否连通、序列化是否成功等“框架层错误”是两个维度。
  fixbug::GetFriendsListResponse response;

  // controller 用来记录本次 RPC 调用在框架层是否失败。
  // 例如：连接失败、超时、对端不存在该服务/方法、反序列化失败等。
  // 因此面试时一定要区分：
  // 1. controller.Failed() == true     -> RPC 框架调用失败
  // 2. response.result().errcode() != 0 -> RPC 到达成功，但业务执行失败
  MprpcController controller;

  // 长连接测试：连续发 10 次请求，观察连接复用效果。
  // 这里能帮助理解项目后续为什么要自己维护连接和通信协议，
  // 因为 Raft 节点之间会频繁发送心跳、投票和日志复制请求。
  int count = 10;
  while (count--) {
    std::cout << " 倒数" << count << "次发起RPC请求" << std::endl;

    // 同步调用：
    // 调用线程会阻塞在这里，直到收到服务端响应或框架层判定失败。
    stub.GetFriendsList(&controller, &request, &response, nullptr);
    // 所有 rpc 方法最终都会汇聚到 RpcChannel::CallMethod，
    // 这也是 RPC 框架最核心的“统一出口”。

    // 一次 RPC 调用完成后，先检查框架层是否失败。
    // RPC 调用失败 != 业务逻辑返回 false，两者必须分开处理。
    if (controller.Failed()) {
      std::cout << controller.ErrorText() << std::endl;
    } else {
      if (0 == response.result().errcode()) {
        std::cout << "rpc GetFriendsList response success!" << std::endl;
        int size = response.friends_size();
        for (int i = 0; i < size; i++) {
          std::cout << "index:" << (i + 1) << " name:" << response.friends(i) << std::endl;
        }
      } else {
        // 这里说明网络和协议层都成功了，
        // 只是远端业务逻辑返回了失败结果。
        std::cout << "rpc GetFriendsList response error : " << response.result().errmsg() << std::endl;
      }
    }

    // 间隔 5 秒再次发送，便于观察长连接下多次调用的日志输出。
    sleep(5);
  }
  return 0;
}
