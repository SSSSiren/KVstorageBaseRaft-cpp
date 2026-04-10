#include <mprpcchannel.h>
#include <iostream>
#include <string>
#include "rpcExample/friend.pb.h"

#include <vector>
#include "rpcprovider.h"

class FriendService : public fixbug::FiendServiceRpc {
 public:
  // 这是“本地业务函数”。
  // 它不关心网络、序列化、连接管理，只负责真正的业务处理。
  // 在你的 Raft 项目里，同一思想会体现在：
  // RPC 层负责把请求送到节点；
  // Raft 层负责处理投票、心跳、日志复制；
  // 状态机/存储层负责真正修改 KV 数据。
  std::vector<std::string> GetFriendsList(uint32_t userid) {
    std::cout << "local do GetFriendsList service! userid:" << userid << std::endl;
    std::vector<std::string> vec;
    vec.push_back("gao yang");
    vec.push_back("liu hong");
    vec.push_back("wang shuo");
    return vec;
  }

  // 这是 protobuf 生成的 Service 基类要求重写的 RPC 入口函数。
  // 远端请求到达 RpcProvider 后，最终会通过 service->CallMethod(...)
  // 间接分发到这里。
  //
  // 参数含义：
  // controller: 记录本次调用状态，当前示例里未使用
  // request   : 调用方发来的请求参数
  // response  : 由服务端填写，框架稍后会序列化并返回给客户端
  // done      : 回调对象，业务逻辑处理完后必须调用 done->Run()，
  //             通知框架“可以发送响应了”
  void GetFriendsList(::google::protobuf::RpcController *controller, const ::fixbug::GetFriendsListRequest *request,
                      ::fixbug::GetFriendsListResponse *response, ::google::protobuf::Closure *done) {
    // 1. 从 protobuf 请求对象中取出业务参数
    uint32_t userid = request->userid();

    // 2. 调用真正的本地业务逻辑
    std::vector<std::string> friendsList = GetFriendsList(userid);

    // 3. 填充统一的业务返回码
    response->mutable_result()->set_errcode(0);
    response->mutable_result()->set_errmsg("");

    // 4. 填充业务数据
    for (std::string &name : friendsList) {
      std::string *p = response->add_friends();
      *p = name;
    }

    // 5. 非常关键：通知 RPC 框架“业务处理结束，可以回包”
    // RpcProvider 在注册 Closure 时，实际把“序列化 response 并发回客户端”
    // 这一动作封装到了 done 里面。
    done->Run();
  }
};

int main(int argc, char **argv) {
  // 示例服务端监听在本地 7788 端口。
  std::string ip = "127.0.0.1";
  short port = 7788;

  // 这行 stub 在当前示例里没有参与后续逻辑，属于调试/测试残留代码。
  // 它不是启动服务端所必需的；真正起作用的是下面的 RpcProvider。
  auto stub = new fixbug::FiendServiceRpc_Stub(new MprpcChannel(ip, port, false));

  // RpcProvider 是服务端网络入口。
  // NotifyService 的作用是把“服务名 -> service对象”和
  // “方法名 -> method描述符”的映射注册到 provider 内部。
  RpcProvider provider;
  provider.NotifyService(new FriendService());

  // 启动 RPC 服务节点。
  // Run 之后会启动 muduo 的 TcpServer 和事件循环，线程进入阻塞，
  // 后续等待客户端发来远程调用请求。
  provider.Run(1, 7788);

  return 0;
}
