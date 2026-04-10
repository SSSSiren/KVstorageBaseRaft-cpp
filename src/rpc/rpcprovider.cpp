#include "rpcprovider.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <cstring>
#include <fstream>
#include <string>
#include "rpcheader.pb.h"
#include "util.h"
/*
service_name =>  service描述
                        =》 service* 记录服务对象
                        method_name  =>  method方法对象
json   protobuf
*/
// 这里是 RPC 服务发布阶段的核心入口。
// 用户把一个继承自 protobuf Service 的业务对象传进来，
// 框架会提取：
// 1. service 的名字
// 2. service 下所有 method 的描述符
// 并把这些元信息缓存起来。
//
// 后面网络请求到达时，框架就能根据
// service_name + method_name
// 在本地找到应当分发到哪个业务对象、哪个业务方法。
void RpcProvider::NotifyService(google::protobuf::Service *service) {
  ServiceInfo service_info;

  // protobuf 在编译 .proto 时，会为服务生成完整的“反射信息”。
  // 这里先取到 service 的描述符，它记录了服务名、方法数、每个方法的签名等。
  const google::protobuf::ServiceDescriptor *pserviceDesc = service->GetDescriptor();

  // 服务名通常来自 .proto 中定义的 service 名字。
  std::string service_name = pserviceDesc->name();

  // 该服务暴露出来的 RPC 方法数量。
  int methodCnt = pserviceDesc->method_count();

  std::cout << "service_name:" << service_name << std::endl;

  for (int i = 0; i < methodCnt; ++i) {
    // 方法描述符中不保存具体实现，只保存“这是哪个方法”的元信息。
    // 真正执行时，会借助 protobuf 生成类中的 CallMethod 做动态分发。
    const google::protobuf::MethodDescriptor *pmethodDesc = pserviceDesc->method(i);
    std::string method_name = pmethodDesc->name();
    service_info.m_methodMap.insert({method_name, pmethodDesc});
  }

  // 保存 service 实例本身，后续收到请求时最终要靠它来真正执行业务方法。
  service_info.m_service = service;
  m_serviceMap.insert({service_name, service_info});
}

// 启动rpc服务节点，开始提供rpc远程网络调用服务
void RpcProvider::Run(int nodeIndex, short port) {
  // 获取可用 ip 的旧逻辑被注释掉了。
  // 原因是通过主机名在 WSL/容器环境下常常拿到内网地址，
  // 对调试并不友好，因此这里直接绑定 0.0.0.0，表示监听本机所有网卡。
  // char *ipC;
  // char hname[128];
  // struct hostent *hent;
  // gethostname(hname, sizeof(hname));
  // hent = gethostbyname(hname);
  // for (int i = 0; hent->h_addr_list[i]; i++) {
  //   ipC = inet_ntoa(*(struct in_addr *)(hent->h_addr_list[i]));  // IP地址
  // }
  // std::string ip = std::string(ipC);
  std::string ip = "0.0.0.0";

  // 把当前节点监听信息写入配置文件。
  // 这个做法比较原始，但能让其他模块知道当前节点的 ip / port。
  // 在完整的分布式系统里，这类信息通常由配置中心、静态配置或注册中心维护。
  std::string node = "node" + std::to_string(nodeIndex);
  std::ofstream outfile;
  outfile.open("test.conf", std::ios::app);
  if (!outfile.is_open()) {
    std::cout << "打开文件失败！" << std::endl;
    exit(EXIT_FAILURE);
  }
  outfile << node + "ip=" + ip << std::endl;
  outfile << node + "port=" + std::to_string(port) << std::endl;
  outfile.close();

  // muduo::InetAddress 对监听地址做封装。
  muduo::net::InetAddress address(ip, port);

  // 创建 TcpServer。
  // RpcProvider 本质上是“在 muduo 网络框架之上的一层 RPC 分发器”。
  m_muduo_server = std::make_shared<muduo::net::TcpServer>(&m_eventLoop, address, "RpcProvider");

  // 绑定连接回调和消息回调，网络读写与业务分发在这里完成解耦。
  /*
  bind的作用：
  如果不使用std::bind将回调函数和TcpConnection对象绑定起来，那么在回调函数中就无法直接访问和修改TcpConnection对象的状态。因为回调函数是作为一个独立的函数被调用的，它没有当前对象的上下文信息（即this指针），也就无法直接访问当前对象的状态。
  如果要在回调函数中访问和修改TcpConnection对象的状态，需要通过参数的形式将当前对象的指针传递进去，并且保证回调函数在当前对象的上下文环境中被调用。这种方式比较复杂，容易出错，也不便于代码的编写和维护。因此，使用std::bind将回调函数和TcpConnection对象绑定起来，可以更加方便、直观地访问和修改对象的状态，同时也可以避免一些常见的错误。
  */
  m_muduo_server->setConnectionCallback(std::bind(&RpcProvider::OnConnection, this, std::placeholders::_1));
  m_muduo_server->setMessageCallback(
      std::bind(&RpcProvider::OnMessage, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));

  // muduo 的 IO 线程数。
  // 线程池越大，服务端可同时处理的网络事件越多，但并不代表业务一定线性扩容。
  m_muduo_server->setThreadNum(4);

  // RPC 服务端启动前打印监听信息。
  std::cout << "RpcProvider start service at ip:" << ip << " port:" << port << std::endl;

  // start() 负责启动底层监听 socket 和线程池；
  // loop() 进入事件循环，线程随后会阻塞在事件分发上。
  m_muduo_server->start();
  m_eventLoop.loop();
  /*
  这段代码是在启动网络服务和事件循环，其中server是一个TcpServer对象，m_eventLoop是一个EventLoop对象。

首先调用server.start()函数启动网络服务。在Muduo库中，TcpServer类封装了底层网络操作，包括TCP连接的建立和关闭、接收客户端数据、发送数据给客户端等等。通过调用TcpServer对象的start函数，可以启动底层网络服务并监听客户端连接的到来。

接下来调用m_eventLoop.loop()函数启动事件循环。在Muduo库中，EventLoop类封装了事件循环的核心逻辑，包括定时器、IO事件、信号等等。通过调用EventLoop对象的loop函数，可以启动事件循环，等待事件的到来并处理事件。

在这段代码中，首先启动网络服务，然后进入事件循环阶段，等待并处理各种事件。网络服务和事件循环是两个相对独立的模块，它们的启动顺序和调用方式都是确定的。启动网络服务通常是在事件循环之前，因为网络服务是事件循环的基础。启动事件循环则是整个应用程序的核心，所有的事件都在事件循环中被处理。
  */
}

// 新的socket连接回调
void RpcProvider::OnConnection(const muduo::net::TcpConnectionPtr &conn) {
  // 新连接建立时不需要主动处理，等对方发数据即可。
  // 这里只在“连接已断开”的情况下做收尾。
  if (!conn->connected()) {
    // 连接断开后关闭底层连接对象。
    conn->shutdown();
  }
}

/*
在框架内部，RpcProvider和RpcConsumer协商好之间通信用的protobuf数据类型
service_name method_name args    定义proto的message类型，进行数据头的序列化和反序列化
                                 service_name method_name args_size
16UserServiceLoginzhang san123456

header_size(4个字节) + header_str + args_str
10 "10"
10000 "1000000"
std::string   insert和copy方法
*/
// 已建立连接用户的读写事件回调 如果远程有一个rpc服务的调用请求，那么OnMessage方法就会响应
// 这里来的肯定是一个远程调用请求
// 因此本函数需要：解析请求，根据服务名，方法名，参数，来调用service的来callmethod来调用本地的业务
void RpcProvider::OnMessage(const muduo::net::TcpConnectionPtr &conn, muduo::net::Buffer *buffer, muduo::Timestamp) {
  // 从网络缓冲区中一次性取出当前收到的字节流。
  // 这个 recv_buf 就是调用方按自定义 RPC 协议拼好的完整请求报文。
  std::string recv_buf = buffer->retrieveAllAsString();

  // 使用 protobuf 提供的流式解码器解析变长整数和字符串，
  // 避免手写字节偏移逻辑。
  google::protobuf::io::ArrayInputStream array_input(recv_buf.data(), recv_buf.size());
  google::protobuf::io::CodedInputStream coded_input(&array_input);
  uint32_t header_size{};

  // 先读出报文头长度。
  // 该项目协议大体格式如下：
  // varint(header_size) + rpc_header_str + args_str
  coded_input.ReadVarint32(&header_size);

  // 读取 rpc_header_str，并将其反序列化为 RpcHeader。
  // RpcHeader 里包含：
  // 1. service_name
  // 2. method_name
  // 3. args_size
  std::string rpc_header_str;
  RPC::RpcHeader rpcHeader;
  std::string service_name;
  std::string method_name;

  // 设置读取边界，确保只在 header_size 范围内读 header。
  google::protobuf::io::CodedInputStream::Limit msg_limit = coded_input.PushLimit(header_size);
  coded_input.ReadString(&rpc_header_str, header_size);
  // 恢复读取边界，后续还要继续读取 args。
  coded_input.PopLimit(msg_limit);
  uint32_t args_size{};
  if (rpcHeader.ParseFromString(rpc_header_str)) {
    // 头部解析成功后，就知道“我要调用谁、传参长度是多少”了。
    service_name = rpcHeader.service_name();
    method_name = rpcHeader.method_name();
    args_size = rpcHeader.args_size();
  } else {
    // 头部都解不出来，说明请求不合法，直接丢弃。
    std::cout << "rpc_header_str:" << rpc_header_str << " parse error!" << std::endl;
    return;
  }

  // 继续按 args_size 读取业务参数区。
  std::string args_str;
  bool read_args_success = coded_input.ReadString(&args_str, args_size);

  if (!read_args_success) {
    // 参数区长度不匹配，说明数据包异常。
    return;
  }

  // 打印调试信息
  //    std::cout << "============================================" << std::endl;
  //    std::cout << "header_size: " << header_size << std::endl;
  //    std::cout << "rpc_header_str: " << rpc_header_str << std::endl;
  //    std::cout << "service_name: " << service_name << std::endl;
  //    std::cout << "method_name: " << method_name << std::endl;
  //    std::cout << "args_str: " << args_str << std::endl;
  //    std::cout << "============================================" << std::endl;

  // 第一步，根据 service_name 找注册过的 service。
  auto it = m_serviceMap.find(service_name);
  if (it == m_serviceMap.end()) {
    std::cout << "服务：" << service_name << " is not exist!" << std::endl;
    std::cout << "当前已经有的服务列表为:";
    for (auto item : m_serviceMap) {
      std::cout << item.first << " ";
    }
    std::cout << std::endl;
    return;
  }

  // 第二步，在对应 service 下查找 method_name。
  auto mit = it->second.m_methodMap.find(method_name);
  if (mit == it->second.m_methodMap.end()) {
    std::cout << service_name << ":" << method_name << " is not exist!" << std::endl;
    return;
  }

  // service 是用户注册的业务对象，例如 UserService / FriendService。
  // method 是 protobuf 描述的“抽象方法元信息”。
  google::protobuf::Service *service = it->second.m_service;
  const google::protobuf::MethodDescriptor *method = mit->second;

  // 通过 protobuf 反射机制创建 request / response 对象。
  // 这一步非常关键：框架并不知道具体方法类型，但能根据 method 动态构造参数对象。
  google::protobuf::Message *request = service->GetRequestPrototype(method).New();
  if (!request->ParseFromString(args_str)) {
    std::cout << "request parse error, content:" << args_str << std::endl;
    return;
  }
  google::protobuf::Message *response = service->GetResponsePrototype(method).New();

  // 注册一个 Closure 回调。
  // 业务方法执行完成后，会调用 done->Run()，
  // done 内部绑定的正是 SendRpcResponse(conn, response)。
  // 也就是说：业务层只负责填 response，不直接碰网络发送。
  google::protobuf::Closure *done =
      google::protobuf::NewCallback<RpcProvider, const muduo::net::TcpConnectionPtr &, google::protobuf::Message *>(
          this, &RpcProvider::SendRpcResponse, conn, response);

  // 到这里就实现了“从网络请求 -> 本地函数调用”的桥接。
  // 实际调用形态等价于：
  // service->某个业务方法(request, response, done)
  // 只是这里通过 protobuf 的反射接口做成了统一动态分发。

  /*
  为什么下面这个service->CallMethod 要这么写？或者说为什么这么写就可以直接调用远程业务方法了
  这个service在运行的时候会是注册的service
  // 用户注册的service类 继承 .protoc生成的serviceRpc类 继承 google::protobuf::Service
  // 用户注册的service类里面没有重写CallMethod方法，是 .protoc生成的serviceRpc类 里面重写了google::protobuf::Service中
  的纯虚函数CallMethod，而 .protoc生成的serviceRpc类 会根据传入参数自动调取 生成的xx方法（如Login方法），
  由于xx方法被 用户注册的service类 重写了，因此这个方法运行的时候会调用 用户注册的service类 的xx方法
  真的是妙呀
  */
  // 真正调用方法。
  // 第 2 个参数 controller 这里传 nullptr，说明当前框架没有把更多调用状态透传给业务层。
  service->CallMethod(method, nullptr, request, response, done);
}

// Closure 回调：把 response 序列化后发回调用方。
void RpcProvider::SendRpcResponse(const muduo::net::TcpConnectionPtr &conn, google::protobuf::Message *response) {
  std::string response_str;
  if (response->SerializeToString(&response_str))
  {
    // 服务端回包时只发送业务响应体，没有再额外封装头部。
    // 客户端此时已经知道自己调用的是哪个方法，因此只需按预期 response 类型反序列化即可。
    conn->send(response_str);
  } else {
    std::cout << "serialize response_str error!" << std::endl;
  }
  // 这里不主动断开连接，说明当前 RPC 框架支持连接复用。
  // 这对 Raft 场景很重要，因为节点间会高频通信，频繁建连代价较高。
}

RpcProvider::~RpcProvider() {
  std::cout << "[func - RpcProvider::~RpcProvider()]: ip和port信息：" << m_muduo_server->ipPort() << std::endl;
  m_eventLoop.quit();
  // muduo::TcpServer 没有像某些框架那样暴露显式 stop()，
  // 当前析构逻辑主要是退出事件循环。
}
