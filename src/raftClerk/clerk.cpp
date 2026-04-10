//
// Created by swx on 23-6-4.
//
#include "clerk.h"

#include "raftServerRpcUtil.h"

#include "util.h"

#include <string>
#include <vector>
std::string Clerk::Get(std::string key) {
  // 每发起一次客户端请求，都分配一个新的 RequestId。
  // ClientId + RequestId 一起组成请求唯一标识，服务端会用它做去重。
  m_requestId++;
  auto requestId = m_requestId;

  // 优先尝试最近一次成功响应的 leader，命中时可以减少重试成本。
  int server = m_recentLeaderId;
  raftKVRpcProctoc::GetArgs args;
  args.set_key(key);
  args.set_clientid(m_clientId);
  args.set_requestid(requestId);

  while (true) {
    raftKVRpcProctoc::GetReply reply;
    // 先向当前猜测的 leader 发请求。
    bool ok = m_servers[server]->Get(&args, &reply);
    if (!ok ||
        reply.err() ==
            ErrWrongLeader) {  //会一直重试，因为requestId没有改变，因此可能会因为RPC的丢失或者其他情况导致重试，kvserver层来保证不重复执行（线性一致性）
      // 网络失败或对方不是 leader，都切到下一个节点继续试。
      server = (server + 1) % m_servers.size();
      continue;
    }
    if (reply.err() == ErrNoKey) {
      return "";
    }
    if (reply.err() == OK) {
      // 一旦成功，就记住当前 leader，作为后续请求的首选目标。
      m_recentLeaderId = server;
      return reply.value();
    }
  }
  return "";
}

void Clerk::PutAppend(std::string key, std::string value, std::string op) {
  // Put 和 Append 在客户端侧只差一个操作类型，公共逻辑统一走这里。
  m_requestId++;
  auto requestId = m_requestId;
  auto server = m_recentLeaderId;
  while (true) {
    // 每轮重试都重新构造参数，但 RequestId 保持不变。
    // 这样即使请求被重发多次，服务端也能识别为同一个逻辑请求。
    raftKVRpcProctoc::PutAppendArgs args;
    args.set_key(key);
    args.set_value(value);
    args.set_op(op);
    args.set_clientid(m_clientId);
    args.set_requestid(requestId);

    raftKVRpcProctoc::PutAppendReply reply;

    bool ok = m_servers[server]->PutAppend(&args, &reply);
    
    if (!ok || reply.err() == ErrWrongLeader) {
      DPrintf("【Clerk::PutAppend】原以为的leader：{%d}请求失败，向新leader{%d}重试  ，操作：{%s}", server, server + 1,
              op.c_str());
      if (!ok) {
        DPrintf("重试原因 ，rpc失敗 ，");
      }
      if (reply.err() == ErrWrongLeader) {
        DPrintf("重試原因：非leader");
      }
      server = (server + 1) % m_servers.size();  // try the next server
      continue;
    }
    if (reply.err() == OK) {
      // 请求已经被真正提交并成功处理。
      m_recentLeaderId = server;
      return;
    }
  }
}

void Clerk::Put(std::string key, std::string value) { PutAppend(key, value, "Put"); }

void Clerk::Append(std::string key, std::string value) { PutAppend(key, value, "Append"); }
// 初始化客户端：
// 1. 读取配置文件里的所有 Raft 节点地址
// 2. 为每个节点创建一个 RPC 调用封装
// 3. 后续 Clerk 会在这些节点间自动重试和定位 leader
void Clerk::Init(std::string configFileName) {
  //获取所有raft节点ip、port ，并进行连接
  MprpcConfig config;
  config.LoadConfigFile(configFileName.c_str());
  std::vector<std::pair<std::string, short>> ipPortVt;
  for (int i = 0; i < INT_MAX - 1; ++i) {
    std::string node = "node" + std::to_string(i);

    std::string nodeIp = config.Load(node + "ip");
    std::string nodePortStr = config.Load(node + "port");
    if (nodeIp.empty()) {
      break;
    }
    ipPortVt.emplace_back(nodeIp, atoi(nodePortStr.c_str()));  //沒有atos方法，可以考慮自己实现
  }
  // 为每个节点准备 RPC 客户端能力。
  for (const auto& item : ipPortVt) {
    std::string ip = item.first;
    short port = item.second;
    // 2024-01-04 todo：bug fix
    auto* rpc = new raftServerRpcUtil(ip, port);
    m_servers.push_back(std::shared_ptr<raftServerRpcUtil>(rpc));
  }
}

// Clerk 构造时生成唯一 ClientId。
// 之后每个请求再叠加递增 RequestId，用于服务端去重和线性一致性保证。
Clerk::Clerk() : m_clientId(Uuid()), m_requestId(0), m_recentLeaderId(0) {}
