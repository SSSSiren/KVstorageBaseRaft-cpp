#include "kvServer.h"

#include <rpcprovider.h>

#include "mprpcconfig.h"

void KvServer::DprintfKVDB() {
  // 调试打印当前 KV 状态机内容。
  // 这里底层实际使用的是跳表，而不是简单 map。
  if (!Debug) {
    return;
  }
  std::lock_guard<std::mutex> lg(m_mtx);
  DEFER {
    // for (const auto &item: m_kvDB) {
    //     DPrintf("[DBInfo ----]Key : %s, Value : %s", &item.first, &item.second);
    // }
    m_skipList.display_list();
  };
}

void KvServer::ExecuteAppendOpOnKVDB(Op op) {
  // 真正把 Append 命令应用到本地状态机。
  // 注意：只有当命令已经被 Raft 提交后，才会走到这里。
  // if op.IfDuplicate {   //get请求是可重复执行的，因此可以不用判复
  //	return
  // }
  m_mtx.lock();

  m_skipList.insert_set_element(op.Key, op.Value);

  // if (m_kvDB.find(op.Key) != m_kvDB.end()) {
  //     m_kvDB[op.Key] = m_kvDB[op.Key] + op.Value;
  // } else {
  //     m_kvDB.insert(std::make_pair(op.Key, op.Value));
  // }
  m_lastRequestId[op.ClientId] = op.RequestId;
  m_mtx.unlock();

  //    DPrintf("[KVServerExeAPPEND-----]ClientId :%d ,RequestID :%d ,Key : %v, value : %v", op.ClientId, op.RequestId,
  //    op.Key, op.Value)
  DprintfKVDB();
}

void KvServer::ExecuteGetOpOnKVDB(Op op, std::string *value, bool *exist) {
  // 在本地状态机上执行一次读取。
  // Get 虽然不修改 KV 数据，但为了客户端去重语义，这里同样会更新 lastRequestId。
  m_mtx.lock();
  *value = "";
  *exist = false;
  if (m_skipList.search_element(op.Key, *value)) {
    *exist = true;
    // *value = m_skipList.se //value已经完成赋值了
  }
  // if (m_kvDB.find(op.Key) != m_kvDB.end()) {
  //     *exist = true;
  //     *value = m_kvDB[op.Key];
  // }
  m_lastRequestId[op.ClientId] = op.RequestId;
  m_mtx.unlock();

  if (*exist) {
    //                DPrintf("[KVServerExeGET----]ClientId :%d ,RequestID :%d ,Key : %v, value :%v", op.ClientId,
    //                op.RequestId, op.Key, value)
  } else {
    //        DPrintf("[KVServerExeGET----]ClientId :%d ,RequestID :%d ,Key : %v, But No KEY!!!!", op.ClientId,
    //        op.RequestId, op.Key)
  }
  DprintfKVDB();
}

void KvServer::ExecutePutOpOnKVDB(Op op) {
  // 真正把 Put 命令应用到本地状态机。
  m_mtx.lock();
  m_skipList.insert_set_element(op.Key, op.Value);
  // m_kvDB[op.Key] = op.Value;
  m_lastRequestId[op.ClientId] = op.RequestId;
  m_mtx.unlock();

  //    DPrintf("[KVServerExePUT----]ClientId :%d ,RequestID :%d ,Key : %v, value : %v", op.ClientId, op.RequestId,
  //    op.Key, op.Value)
  DprintfKVDB();
}

// 处理来自clerk的Get RPC
void KvServer::Get(const raftKVRpcProctoc::GetArgs *args, raftKVRpcProctoc::GetReply *reply) {
  // 为什么 Get 也要走 Raft，而不是本地直接读？
  // 因为这个项目追求线性一致性。
  // 如果 follower 允许本地直接读，那么它可能返回旧值：
  // 1. leader 可能已经接收了新写入，但还没同步到这个 follower
  // 2. 当前节点甚至可能不是 leader，却还在对外提供读
  //
  // 因此这里把 Get 也封装成一个要经过 Raft 排序的 Op，
  // 只有在 leader 确认并提交后，才真正从状态机里读取并返回结果。
  // 把 RPC 请求统一封装成 Op，便于后续交给 Raft 和状态机处理。
  Op op;
  op.Operation = "Get";
  op.Key = args->key();
  op.Value = "";
  op.ClientId = args->clientid();
  op.RequestId = args->requestid();

  int raftIndex = -1;
  int _ = -1;
  bool isLeader = false;
  // 交给本地 Raft。如果当前节点是 leader，则会把请求作为新日志提交复制流程。
  m_raftNode->Start(op, &raftIndex, &_,
                    &isLeader);  // raftIndex：raft预计的logIndex
                                 // ，虽然是预计，但是正确情况下是准确的，op的具体内容对raft来说 是隔离的

  if (!isLeader) {
    // 不是 leader，直接让客户端换节点重试。
    reply->set_err(ErrWrongLeader);
    return;
  }

  // 为这个日志索引创建等待通道：
  // 当前 RPC 线程会阻塞等待，直到后台 apply 线程告诉它“这条日志已经真正提交”。
  //
  // waitApplyCh 的作用是把“某个前台 RPC 请求”和“某条日志真正提交完成的时刻”对应起来。
  // 键是 raftIndex，值是一个 LockQueue<Op>。
  // 前台 RPC 线程在这里等，后台 apply 线程提交完成后会按相同 raftIndex 把结果塞回来。
  m_mtx.lock();

  if (waitApplyCh.find(raftIndex) == waitApplyCh.end()) {
    waitApplyCh.insert(std::make_pair(raftIndex, new LockQueue<Op>()));
  }
  auto chForRaftIndex = waitApplyCh[raftIndex];

  // 不能拿着全局锁等待，否则后台 apply 流程会被一起卡住。
  m_mtx.unlock();

  // 带超时地等待这条日志被真正提交。
  Op raftCommitOp;

  if (!chForRaftIndex->timeOutPop(CONSENSUS_TIMEOUT, &raftCommitOp)) {
    //        DPrintf("[GET TIMEOUT!!!]From Client %d (Request %d) To Server %d, key %v, raftIndex %d", args.ClientId,
    //        args.RequestId, kv.me, op.Key, raftIndex)
    // todo 2023年06月01日
    int _ = -1;
    bool isLeader = false;
    m_raftNode->GetState(&_, &isLeader);

    if (ifRequestDuplicate(op.ClientId, op.RequestId) && isLeader) {
      // 超时不一定代表一定失败，也可能是“已经提交成功，但通知/回包过程中慢了”。
      // 如果这是一个重复请求，并且当前节点仍是 leader，那么可以安全地重新从状态机读取结果返回。
      std::string value;
      bool exist = false;
      ExecuteGetOpOnKVDB(op, &value, &exist);
      if (exist) {
        reply->set_err(OK);
        reply->set_value(value);
      } else {
        reply->set_err(ErrNoKey);
        reply->set_value("");
      }
    } else {
      reply->set_err(ErrWrongLeader);  // 让 Clerk 自己换节点重试
    }
  } else {
    // 收到了 apply 线程发来的“这条日志已经提交”的通知。
    //         DPrintf("[WaitChanGetRaftApplyMessage<--]Server %d , get Command <-- Index:%d , ClientId %d, RequestId
    //         %d, Opreation %v, Key :%v, Value :%v", kv.me, raftIndex, op.ClientId, op.RequestId, op.Operation, op.Key,
    //         op.Value)
    // 这里仍然要再次确认提交的命令就是当前请求。
    // 原因是 leader 可能在过程中切换，同一个 log index 最终落下来的命令未必还是当前这次请求。
    if (raftCommitOp.ClientId == op.ClientId && raftCommitOp.RequestId == op.RequestId) {
      std::string value;
      bool exist = false;
      // Question: 这里直接访问当前Kv Server节点的本地DB，如何保证这个KVServer的Raft节点是Leader节点。
      // Answer:
      // 严格来说，走到这里时“不要求它此刻仍然是 leader”。
      // 真正需要保证的是两件事：
      // 1. 这条 Get 请求之前确实是由本节点作为 leader 调用了 Raft::Start(...) 送进日志的
      // 2. 现在 waitApplyCh 收到的提交结果，经过 ClientId/RequestId 校验后，
      //    证明提交并 apply 到本地状态机的，仍然就是当前这条 Get 请求
      //
      // 一旦这两点成立，就说明这台机器上的状态机已经按 Raft 一致性顺序
      // 执行到了这条 Get 对应的位置。此时直接读取“本地已 apply 的状态机”就是安全的。
      //
      // 换句话说，这里依赖的不是“当前身份还是不是 leader”，
      // 而是“当前节点已经把这条 Get 所在日志及其之前的日志都应用到本地 DB 了”。
      //
      // 反过来说，如果 leader 在过程中切换，导致这个 raftIndex 最终提交的不是当前请求，
      // 上面的 ClientId/RequestId 校验就会失败，代码会返回 ErrWrongLeader，
      // 根本不会走到这里读 DB。
      ExecuteGetOpOnKVDB(op, &value, &exist);
      
      if (exist) {
        reply->set_err(OK);
        reply->set_value(value);
      } else {
        reply->set_err(ErrNoKey);
        reply->set_value("");
      }
    } else {
      reply->set_err(ErrWrongLeader);
      //            DPrintf("[GET ] 不满足：raftCommitOp.ClientId{%v} == op.ClientId{%v} && raftCommitOp.RequestId{%v}
      //            == op.RequestId{%v}", raftCommitOp.ClientId, op.ClientId, raftCommitOp.RequestId, op.RequestId)
    }
  }
  // 清理等待通道，避免泄漏。
  m_mtx.lock();
  auto tmp = waitApplyCh[raftIndex];
  waitApplyCh.erase(raftIndex);
  delete tmp;
  m_mtx.unlock();
}

void KvServer::GetCommandFromRaft(ApplyMsg message) {
  // 后台 apply 线程收到  Raft 提交后下来的消息后，会在这里把序列化命令还原成 Op。
  Op op;
  op.parseFromString(message.Command);

  DPrintf(
      "[KvServer::GetCommandFromRaft-kvserver{%d}] , Got Command --> Index:{%d} , ClientId {%s}, RequestId {%d}, "
      "Opreation {%s}, Key :{%s}, Value :{%s}",
      m_me, message.CommandIndex, &op.ClientId, op.RequestId, &op.Operation, &op.Key, &op.Value);
  if (message.CommandIndex <= m_lastSnapShotRaftLogIndex) {
    // 已经被快照覆盖的旧日志无需重复应用。
    return;
  }

  // 去重发生在状态机层。
  // 即使客户端因为网络问题把同一个请求重发多次，Put/Append 也只会生效一次。
  // 这里依赖的是 lastRequestId：
  // 服务器为每个 ClientId 记录“最新已经处理到哪个 RequestId”，
  // 只要发现新请求的 RequestId 不大于这个值，就说明它其实是旧请求重试。
  if (!ifRequestDuplicate(op.ClientId, op.RequestId)) {
    // 真正把命令应用到 KV 状态机。
    if (op.Operation == "Put") {
      ExecutePutOpOnKVDB(op);
    }
    if (op.Operation == "Append") {
      ExecuteAppendOpOnKVDB(op);
    }
    //  kv.lastRequestId[op.ClientId] = op.RequestId  在Executexxx函数里面更新的
  }
  //到这里kvDB已经制作了快照
  if (m_maxRaftState != -1) {
    IfNeedToSendSnapShotCommand(message.CommandIndex, 9);
    // 如果 Raft 状态过大，就触发快照，避免日志无限膨胀。
  }

  // 把结果投递给等待这个 raftIndex 的 RPC 处理线程。
  SendMessageToWaitChan(op, message.CommandIndex);
}

bool KvServer::ifRequestDuplicate(std::string ClientId, int RequestId) {
  // 只要某个请求号小于等于该客户端最近已处理的请求号，就认为它是重复请求。
  // 这就是 lastRequestId 保证幂等的原因。
  std::lock_guard<std::mutex> lg(m_mtx);
  if ( .find(ClientId) == m_lastRequestId.end()) {
    return false;
    // todo :不存在这个client就创建
  }
  return RequestId <= m_lastRequestId[ClientId];
}

// get和put//append執行的具體細節是不一樣的
// PutAppend在收到raft消息之後執行，具體函數裏面只判斷冪等性（是否重複）
// get函數收到raft消息之後在，因爲get無論是否重複都可以再執行
void KvServer::PutAppend(const raftKVRpcProctoc::PutAppendArgs *args, raftKVRpcProctoc::PutAppendReply *reply) {
  // Put / Append 的大体流程和 Get 类似：
  // 1. 封装 Op
  // 2. 交给 Raft::Start
  // 3. 不是 leader 就立即失败
  // 4. 是 leader 就等待真正提交
  Op op;
  op.Operation = args->op();
  op.Key = args->key();
  op.Value = args->value();
  op.ClientId = args->clientid();
  op.RequestId = args->requestid();
  int raftIndex = -1; // Question: 这个raftIndex指的是raft节点中的log index吗
  // Answer:
  // 是的，这里的 raftIndex 就是这条 Op 被 leader 追加进 Raft 日志后分配到的 log index。
  // 它来自 Raft::Start(...) 的输出参数，本质上对应：
  // “这条客户端请求在当前 leader 日志里的槽位编号”。
  //
  // 后面 KvServer 要用这个 index 去 waitApplyCh 里等待，原因是：
  // 当前台 RPC 线程把请求交给 Raft 后，后台 apply 线程最终也是按 CommandIndex
  // 把结果送回来的。两边只有通过同一个 raftIndex，才能把“这次前台请求”
  // 和“那次后台提交完成通知”准确对上。
  int _ = -1;
  bool isleader = false;

  m_raftNode->Start(op, &raftIndex, &_, &isleader);

  if (!isleader) {
    DPrintf(
        "[func -KvServer::PutAppend -kvserver{%d}]From Client %s (Request %d) To Server %d, key %s, raftIndex %d , but "
        "not leader",
        m_me, &args->clientid(), args->requestid(), m_me, &op.Key, raftIndex);

    reply->set_err(ErrWrongLeader);
    return;
  }
  
  
  DPrintf(
      "[func -KvServer::PutAppend -kvserver{%d}]From Client %s (Request %d) To Server %d, key %s, raftIndex %d , is "
      "leader ",
      m_me, &args->clientid(), args->requestid(), m_me, &op.Key, raftIndex);
  
  m_mtx.lock();
  
  if (waitApplyCh.find(raftIndex) == waitApplyCh.end()) {
    // Question: 为什么以raftIndex为键，可以用别的值为键吗
    // Answer:
    // 这里用 raftIndex 作为键，是因为它是“这条请求进入 Raft 日志后”的唯一定位信息。
    // 前台线程调用 Start() 时拿到 raftIndex，后台 apply 线程从 applyChan 收到 ApplyMsg 时，
    // 也会拿到同一个 CommandIndex。这样两边才能通过同一个键在 waitApplyCh 上会合。
    //
    // 理论上不是绝对只能用 raftIndex，但你换成别的键，必须同时满足两个条件：
    // 1. 前台 PutAppend/Get 线程在请求入 Raft 时就能拿到它
    // 2. 后台 apply 完成时也能准确得到同一个值
    //
    // 在这个项目里，最自然满足这两个条件的就是 raftIndex。
    // 像 ClientId/RequestId 虽然也能标识业务请求，但它们更适合做“幂等校验”，
    // 不如 raftIndex 适合做“日志提交完成通知”的对接键。
    waitApplyCh.insert(std::make_pair(raftIndex, new LockQueue<Op>()));
  }
  auto chForRaftIndex = waitApplyCh[raftIndex];

  // 等待提交时不能一直持锁。
  m_mtx.unlock();

  // 带超时保护地等待日志提交结果。
  Op raftCommitOp;
  // Question: timeOutPop函数的功能是什么？
  // Answer:
  // timeOutPop(timeout, &raftCommitOp) 的语义是：
  // “在最多 timeout 这么长时间内，阻塞等待队列里出现一个 Op；
  // 如果等到了，就把结果写进 raftCommitOp 并返回 true；
  // 如果超时还没等到，就返回 false。”
  //
  // 放到这里的具体含义就是：
  // 当前前台 RPC 线程把请求交给 Raft 以后，不会无限期傻等，
  // 而是最多等 CONSENSUS_TIMEOUT 这么久，看后台 apply 线程能不能把
  // ‘这条日志已经真正提交并应用完成’ 的结果送回来。
  if (!chForRaftIndex->timeOutPop(CONSENSUS_TIMEOUT, &raftCommitOp)) { 
    DPrintf(
        "[func -KvServer::PutAppend -kvserver{%d}]TIMEOUT PUTAPPEND !!!! Server %d , get Command <-- Index:%d , "
        "ClientId %s, RequestId %s, Opreation %s Key :%s, Value :%s",
        m_me, m_me, raftIndex, &op.ClientId, op.RequestId, &op.Operation, &op.Key, &op.Value);

    if (ifRequestDuplicate(op.ClientId, op.RequestId)) {
      // 超时但已是重复请求，通常说明“很可能其实已经成功提交过”，因此直接返回 OK。
      // Question: 这里为什么不像上面Get一样判断DB是不是真的成功执行？
      // Answer:
      // 因为 Put/Append 和 Get 的返回目标不同。
      //
      // Get 返回给客户端的，不只是“这次操作成功了”，
      // 还要返回“当前这个 key 对应的 value 到底是多少”，
      // 所以它在确认请求大概率已经成功后，还需要再去状态机里读一次真实结果。
      //
      // 但 Put/Append 返回的只是一个成功/失败状态，不需要像 Get 那样把业务值读出来。
      // 这里一旦通过 lastRequestId 判断它已经是重复请求，
      // 实际上就等价于“这条写请求之前已经成功 apply 过了”，
      // 因而直接返回 OK 就够了，没有必要再额外检查 DB。
      reply->set_err(OK);
    } else {
      // Question:超时且不重复请求，可能是什么原因导致的？
      // Answer:
      // 这种情况常见有几类原因：
      // 1. 当前节点虽然刚才 Start() 时还是 leader，但随后很快失去领导权，
      //    这条日志没来得及在多数派提交；
      // 2. 网络抖动、节点宕机或分区，导致日志复制迟迟凑不齐多数；
      // 3. 这条日志后来被新的 leader 覆盖了，所以前台线程一直等不到
      //    “属于自己这条请求”的提交通知；
      // 4. 集群确实还在推进，但推进速度太慢，超过了前台 RPC 的超时窗口。
      //
      // 因为前台线程无法区分这些底层原因，所以统一返回 ErrWrongLeader，
      // 让 Clerk 去别的节点重试，是这个项目里更稳妥的处理方式。
      reply->set_err(ErrWrongLeader);  // 让 Clerk 继续换节点重试
    }
  } else {
    DPrintf(
        "[func -KvServer::PutAppend -kvserver{%d}]WaitChanGetRaftApplyMessage<--Server %d , get Command <-- Index:%d , "
        "ClientId %s, RequestId %d, Opreation %s, Key :%s, Value :%s",
        m_me, m_me, raftIndex, &op.ClientId, op.RequestId, &op.Operation, &op.Key, &op.Value);
    if (raftCommitOp.ClientId == op.ClientId && raftCommitOp.RequestId == op.RequestId) {
      // 必须确认提交到该 index 的命令仍然是当前请求。
      // 否则说明 leader 变更后该 index 对应的日志已被其他命令占用。
      // Question: 这里为什么不像上面Get一样判断DB是不是真的成功执行？
      // Answer:
      // 因为走到这里时，waitApplyCh 已经收到了后台 apply 线程发回来的通知，
      // 这本身就表示：
      // 1. 这条日志已经被 Raft 提交
      // 2. KvServer 已经在 GetCommandFromRaft(...) 里处理过它
      //
      // 对 Put/Append 来说，只要再确认“提交成功的那条日志就是我原来的请求”
      // 即可，也就是校验 ClientId / RequestId 是否一致。
      //
      // 而 Get 不一样：
      // 它即使确认日志属于自己，也还必须再从当前状态机中读出 value，
      // 才能把真正的查询结果回给客户端。
      reply->set_err(OK);
    } else {
      reply->set_err(ErrWrongLeader);
    }
  }

  m_mtx.lock();

  auto tmp = waitApplyCh[raftIndex];
  waitApplyCh.erase(raftIndex);
  delete tmp;
  m_mtx.unlock();
}

void KvServer::ReadRaftApplyCommandLoop() {
  // KvServer 的后台消费循环：
  // 不断从 applyChan 中取出已经被 Raft 提交的消息，再交给状态机执行。
  //
  // applyChan 是谁写、谁读？
  // - 写入方：Raft 层。典型位置是 Raft::applierTicker() / pushMsgToKvServer()
  // - 读取方：KvServer 层。也就是这里的 ReadRaftApplyCommandLoop()
  //
  // 所以 applyChan 是“Raft -> 状态机/KVServer”的单向桥梁。
  while (true) {
    // applyChan 本身是线程安全阻塞队列，因此这里只需要阻塞等待即可。
    // 此处没有判断任何applyChannel里的信息
    auto message = applyChan->Pop();
    DPrintf(
        "---------------tmp-------------[func-KvServer::ReadRaftApplyCommandLoop()-kvserver{%d}] 收到了下raft的消息",
        m_me);
    // 处理普通日志或快照消息。

    if (message.CommandValid) {
      GetCommandFromRaft(message);
    }
    if (message.SnapshotValid) {
      GetSnapShotFromRaft(message);
    }
  }
}

// raft会与persist层交互，kvserver层也会，因为kvserver层开始的时候需要恢复kvdb的状态
//  关于快照raft层与persist的交互：保存kvserver传来的snapshot；生成leaderInstallSnapshot RPC的时候也需要读取snapshot；
//  因此snapshot的具体格式是由kvserver层来定的，raft只负责传递这个东西
//  snapShot里面包含kvserver需要维护的persist_lastRequestId 以及kvDB真正保存的数据persist_kvdb
void KvServer::ReadSnapShotToInstall(std::string snapshot) {
  // 把快照内容还原回本地 KV 状态机。
  // 这里的快照不仅包含 KV 数据，也包含客户端去重所需的 lastRequestId。
  // 这说明快照内容由 KvServer 决定，因为只有业务层知道哪些状态需要被恢复。
  if (snapshot.empty()) {
    // bootstrap without any state?
    return;
  }
  parseFromString(snapshot);

  //    r := bytes.NewBuffer(snapshot)
  //    d := labgob.NewDecoder(r)
  //
  //    var persist_kvdb map[string]string  //理应快照
  //    var persist_lastRequestId map[int64]int //快照这个为了维护线性一致性
  //
  //    if d.Decode(&persist_kvdb) != nil || d.Decode(&persist_lastRequestId) != nil {
  //                DPrintf("KVSERVER %d read persister got a problem!!!!!!!!!!",kv.me)
  //        } else {
  //        kv.kvDB = persist_kvdb
  //        kv.lastRequestId = persist_lastRequestId
  //    }
}

bool KvServer::SendMessageToWaitChan(const Op &op, int raftIndex) {
  // 把“某条日志已经提交”的结果投递给等待该 raftIndex 的 RPC 处理线程。
  // 这正是 waitApplyCh 存在的意义：让前台 RPC 线程知道“自己那条请求对应的日志什么时候真的提交了”。
  std::lock_guard<std::mutex> lg(m_mtx);
  DPrintf(
      "[RaftApplyMessageSendToWaitChan--> raftserver{%d}] , Send Command --> Index:{%d} , ClientId {%d}, RequestId "
      "{%d}, Opreation {%v}, Key :{%v}, Value :{%v}",
      m_me, raftIndex, &op.ClientId, op.RequestId, &op.Operation, &op.Key, &op.Value);

  if (waitApplyCh.find(raftIndex) == waitApplyCh.end()) {
    return false;
  }
  waitApplyCh[raftIndex]->Push(op);
  DPrintf(
      "[RaftApplyMessageSendToWaitChan--> raftserver{%d}] , Send Command --> Index:{%d} , ClientId {%d}, RequestId "
      "{%d}, Opreation {%v}, Key :{%v}, Value :{%v}",
      m_me, raftIndex, &op.ClientId, op.RequestId, &op.Operation, &op.Key, &op.Value);
  return true;
}

void KvServer::IfNeedToSendSnapShotCommand(int raftIndex, int proportion) {
  // 如果 Raft 持久化状态过大，就向 Raft 层发起快照命令。
  // 快照内容先由 KvServer 制作，再交给 Raft 保存和复制。
  if (m_raftNode->GetRaftStateSize() > m_maxRaftState / 10.0) {
    // Send SnapShot Command
    auto snapshot = MakeSnapShot();
    m_raftNode->Snapshot(raftIndex, snapshot);
  }
}

void KvServer::GetSnapShotFromRaft(ApplyMsg message) {
  // 处理 Raft 层传来的快照安装通知。
  std::lock_guard<std::mutex> lg(m_mtx);

  if (m_raftNode->CondInstallSnapshot(message.SnapshotTerm, message.SnapshotIndex, message.Snapshot)) {
    ReadSnapShotToInstall(message.Snapshot);
    m_lastSnapShotRaftLogIndex = message.SnapshotIndex;
  }
}

std::string KvServer::MakeSnapShot() {
  // 具体快照内容由 KvServer 决定，这体现了“Raft 复制快照，但不理解业务快照格式”的边界。
  // 所以：
  // - 快照在哪层制作？在 KvServer / 状态机层制作
  // - 快照最终在哪层保存？由 Raft 调用 Persister 保存
  std::lock_guard<std::mutex> lg(m_mtx);
  std::string snapshotData = getSnapshotData();
  return snapshotData;
}

void KvServer::PutAppend(google::protobuf::RpcController *controller, const ::raftKVRpcProctoc::PutAppendArgs *request,
                         ::raftKVRpcProctoc::PutAppendReply *response, ::google::protobuf::Closure *done) {
  KvServer::PutAppend(request, response);
  done->Run(); // 表示完成后把response序列化打包发回
}

void KvServer::Get(google::protobuf::RpcController *controller, const ::raftKVRpcProctoc::GetArgs *request,
                   ::raftKVRpcProctoc::GetReply *response, ::google::protobuf::Closure *done) {
  KvServer::Get(request, response);
  done->Run();
}

KvServer::KvServer(int me, int maxraftstate, std::string nodeInforFileName, short port) : m_skipList(6) {
  // KvServer 构造函数里串起了整个节点启动流程：
  // 1. 创建持久化对象和 apply 通道
  // 2. 创建本地 Raft 节点对象
  // 3. 启动对外 RPC 服务，既对客户端暴露 KV RPC，也对其他节点暴露 Raft RPC
  // 4. 读取配置文件，连接其他节点
  // 5. 初始化 Raft
  // 6. 启动后台 apply 消费循环
  std::shared_ptr<Persister> persister = std::make_shared<Persister>(me);

  m_me = me;
  m_maxRaftState = maxraftstate;

  applyChan = std::make_shared<LockQueue<ApplyMsg> >();

  m_raftNode = std::make_shared<Raft>();
  // 同一个进程里同时对外发布两类 RPC：
  // 1. KvServer 自己的 Get/PutAppend RPC，供客户端 Clerk 调用
  // 2. Raft 节点的 RequestVote/AppendEntries 等 RPC，供其他 Raft 节点调用
  std::thread t([this, port]() -> void {
    // provider是一个rpc网络服务对象。把UserService对象发布到rpc节点上
    RpcProvider provider;
    provider.NotifyService(this); // Question:这里notifyService(this)的效果是什么？
    // Answer:
    // 这里是在把“当前 KvServer 对象里的 protobuf RPC 服务”注册到 RpcProvider 里。
    // 注册后，provider 会通过 protobuf 反射拿到：
    // 1. 服务名
    // 2. 这个服务下有哪些 method
    // 3. 当某个 method 被调用时，最终应该回调到 this 上的哪个重写函数
    //
    // 也就是说，这一行注册的是 KvServer 对外提供的 Get/PutAppend RPC，
    // 供客户端 Clerk 通过网络调用。
    // 紧接着下面的 provider.NotifyService(this->m_raftNode.get())，
    // 注册的则是 Raft 节点对外提供的 RequestVote / AppendEntries / InstallSnapshot RPC，
    // 供其他 Raft 节点互相通信。
    //
    // 所以这两行连在一起的效果就是：
    // “同一个进程、同一个 RPC Server 端口，同时暴露 KV 服务接口和 Raft 内部通信接口。”
    provider.NotifyService(
        this->m_raftNode.get());
    // Run 之后线程会阻塞在网络事件循环中，持续等待远程 RPC 请求。
    provider.Run(m_me, port);
  });
  t.detach();

  // 这里用 sleep 粗略等待其他节点先把 RPC 服务启动起来。
  // 这样后续读取配置并互连时，目标端口大概率已经可用。
  std::cout << "raftServer node:" << m_me << " start to sleep to wait all ohter raftnode start!!!!" << std::endl;
  sleep(6);
  std::cout << "raftServer node:" << m_me << " wake up!!!! start to connect other raftnode" << std::endl;
  
  // 读取配置文件里的所有节点地址，准备构造“连接其他节点”的 RPC 工具对象。
  MprpcConfig config;
  config.LoadConfigFile(nodeInforFileName.c_str());
  std::vector<std::pair<std::string, short> > ipPortVt;
  for (int i = 0; i < INT_MAX - 1; ++i) {
    std::string node = "node" + std::to_string(i);

    std::string nodeIp = config.Load(node + "ip");
    std::string nodePortStr = config.Load(node + "port");
    if (nodeIp.empty()) {
      break;
    }
    ipPortVt.emplace_back(nodeIp, atoi(nodePortStr.c_str()));  //沒有atos方法，可以考慮自己实现
  }
  std::vector<std::shared_ptr<RaftRpcUtil> > servers;
  // 依次为其他节点构造 RPC 调用封装；对自己则填 nullptr 占位。
  for (int i = 0; i < ipPortVt.size(); ++i) {
    if (i == m_me) {
      servers.push_back(nullptr);
      continue;
    }
    std::string otherNodeIp = ipPortVt[i].first;
    short otherNodePort = ipPortVt[i].second;
    auto *rpc = new RaftRpcUtil(otherNodeIp, otherNodePort);
    servers.push_back(std::shared_ptr<RaftRpcUtil>(rpc));

    std::cout << "node" << m_me << " 连接node" << i << "success!" << std::endl;
  }
  // 再次等待一小段时间，尽量保证所有节点都完成互连后再启动 Raft。
  sleep(ipPortVt.size() - me);
  m_raftNode->init(servers, m_me, persister, applyChan);
  // applyChan 是 KvServer 和 Raft 之间的桥梁：
  // Raft 提交日志后，把 ApplyMsg 推到 applyChan；
  // KvServer 再从 applyChan 中取出并真正应用到状态机。
  //
  // waitApplyCh 则解决另一个问题：
  // applyChan 面向“后台提交消息流转”；
  // waitApplyCh 面向“某个具体 RPC 请求如何等到属于自己的提交结果”。

  //////////////////////////////////

  // You may need initialization code here.
  // m_kvDB; //kvdb初始化
  m_skipList;
  waitApplyCh;
  m_lastRequestId;
  m_lastSnapShotRaftLogIndex = 0;  // todo:感覺這個函數沒什麼用，不如直接調用raft節點中的snapshot值？？？
  auto snapshot = persister->ReadSnapshot();
  if (!snapshot.empty()) {
    // 崩溃恢复时先安装已有快照，再继续处理后续日志。
    ReadSnapShotToInstall(snapshot);
  }
  // 启动后台 apply 消费线程。
  std::thread t2(&KvServer::ReadRaftApplyCommandLoop, this);
  // 这里 join 是为了让当前构造完成后，节点主执行流一直挂在 apply 循环上，不退出。
  t2.join();
}
