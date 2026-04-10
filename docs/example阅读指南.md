# Example 阅读指南

这份文档不是按目录机械罗列代码，而是给你一个适合面试准备的阅读切入顺序。

建议顺序：

1. 先看 `rpcExample`
2. 再看 `fiberExample`
3. 最后看 `raftCoreExample`

目标不是一开始记住所有细节，而是先回答 3 个问题：

- 请求从哪里进来？
- 状态在哪里保存？
- 谁在什么时机唤醒谁？

---

## 1. `rpcExample` 怎么看

推荐顺序：

1. `example/rpcExample/callee/friendService.cpp`
2. `example/rpcExample/caller/callFriendService.cpp`
3. `src/rpc/mprpcchannel.cpp`
4. `src/rpc/rpcprovider.cpp`

### 先看什么

先从业务示例看，不要一上来就钻进 RPC 框架。

服务端示例 `friendService.cpp` 里，你要看清 3 件事：

- 业务类 `FriendService` 继承了 protobuf 生成的 `FiendServiceRpc`
- 业务方法 `GetFriendsList(...)` 在本地真正执行业务逻辑
- `RpcProvider` 通过 `NotifyService(...)` 把服务注册出去，再通过 `Run(...)` 开始监听

客户端示例 `callFriendService.cpp` 里，你要看清 3 件事：

- protobuf 生成的 `Stub` 是客户端入口
- `Stub` 最终会走到 `MprpcChannel::CallMethod(...)`
- RPC 框架失败和业务失败是两回事

### 你要读懂的核心调用链

一次 RPC 请求的大致路径是：

```text
caller main
-> protobuf Stub::GetFriendsList
-> MprpcChannel::CallMethod
-> send 自定义报文
-> RpcProvider::OnMessage
-> service->CallMethod(...)
-> FriendService::GetFriendsList
-> SendRpcResponse
-> caller 反序列化 response
```

### 这一部分要抓住的结论

这个项目的 RPC 本质上是：

`protobuf service + 自定义 header + muduo TCP server`

### 重点函数

- `MprpcChannel::CallMethod(...)`
  - 负责序列化请求参数
  - 负责组装 `RpcHeader`
  - 负责把 `header + args` 发到网络上
  - 负责收回响应并反序列化

- `RpcProvider::OnMessage(...)`
  - 负责从网络包中解析 header
  - 负责找到 service 和 method
  - 负责构造 request/response 对象
  - 负责回调到真正的业务实现

### 这一轮你要能回答的问题

- 服务端是怎么把业务方法暴露成 RPC 服务的？
- 客户端为什么不直接调业务类，而是调 protobuf `Stub`？
- `service->CallMethod(...)` 为什么最终能调用到你自己写的 `FriendService::GetFriendsList(...)`？
- 自定义协议头里至少包含了什么信息？

参考回答：

- 服务端先定义一个业务类 `FriendService`，让它继承 protobuf 根据 `friend.proto` 生成的 `FiendServiceRpc` 服务基类，然后重写其中的 `GetFriendsList(...)`。接着把这个业务对象通过 `RpcProvider::NotifyService(...)` 注册到框架里。注册时，框架会拿到 protobuf 的反射信息，把 `service_name -> service对象`、`method_name -> method描述符` 保存起来。之后 `RpcProvider::Run(...)` 启动 muduo 服务器开始监听，请求一到达就能按服务名和方法名分发到这个业务对象上。

- 客户端不能直接调业务类，因为业务类只存在于服务端进程内。RPC 的本质就是“调用方和实现方不在同一个地址空间”。客户端真正需要的是一个“本地代理”，让它看起来像普通函数调用，但底层能自动完成序列化、组包、发网络请求、收响应、反序列化。protobuf 生成的 `Stub` 正好承担这个角色，因此客户端调的是 `FiendServiceRpc_Stub`，而不是直接 new 一个服务端业务类。

- `service->CallMethod(...)` 之所以最终能调用到你自己写的 `FriendService::GetFriendsList(...)`，关键在于 protobuf 生成代码里的动态分发机制。`FriendService` 继承的是 protobuf 生成的 `FiendServiceRpc`，而这个生成类已经重写了基类 `google::protobuf::Service` 的 `CallMethod(...)`。当框架把 `method`、`request`、`response` 传进去后，生成类会根据 `method` 判断当前要调哪个 RPC 方法，再转而调用虚函数 `GetFriendsList(...)`。因为这个虚函数在 `FriendService` 里被你重写了，所以最终落到的是你自己的业务实现。

- 自定义协议头里至少要包含 3 类信息：`service_name`、`method_name`、`args_size`。前两者用来让服务端知道“这次请求要调用哪个服务的哪个方法”，后者用来让服务端知道后面的参数区到底有多长，从而正确切分请求报文。在这个项目里，这些字段被定义在 `RpcHeader` 里，客户端会先发 `header_size + header_str + args_str`，服务端再按这个协议解包。

---

## 2. `fiberExample` 怎么看

推荐顺序：

1. `example/fiberExample/test_scheduler.cpp`
2. `example/fiberExample/test_iomanager.cpp`
3. `example/fiberExample/test_hook.cpp`
4. `src/raftCore/raft.cpp` 中的 `Raft::init(...)`

### 阅读原则

这一部分先不要试图一次看懂整个协程库实现。

先把它理解成作者自己封装的两个工具：

- `Scheduler`：管理协程任务调度
- `IOManager`：在调度协程的基础上，再处理 IO 事件和 hook

### `test_scheduler.cpp` 要看什么

这个示例主要是帮助你理解：

- 调度器可以调度函数，也可以调度 `Fiber`
- `start()` 和 `stop()` 在什么时候真正开始和结束调度
- `use_caller` 为 `true` 时，当前线程会参与协程调度

读完后，你应该知道：

“协程不是自己跑的，而是被调度器统一管理。”

### `test_iomanager.cpp` 要看什么

这个示例重点不是 socket 编程，而是：

- `IOManager` 能给 fd 注册读写事件
- 事件触发后会回调到指定函数
- 读事件如果想持续监听，需要重新注册

你要把它理解成：

“协程调度器 + IO 事件驱动”的组合。

### `test_hook.cpp` 要看什么

这个示例是为了说明：

- 在 hook 生效时，`sleep`、`connect`、`recv` 这些阻塞调用可以被改造成更友好的协程行为
- 从写法上看像同步代码，但底层调度方式已经变了

你不需要先深究 hook 的全部实现细节，但要记住它的用途：

“减少传统阻塞调用对线程的卡死影响。”

### 为什么这部分和 Raft 有关

回到 `Raft::init(...)` 去看：

- `leaderHearBeatTicker()` 是用协程调度起来的
- `electionTimeOutTicker()` 也是用协程调度起来的
- `applierTicker()` 仍然是单独的线程

这里反映的是作者的职责划分：

- 定时任务用协程调度
- 应用已提交日志这种持续阻塞消费行为用线程做

### 这一轮你要能回答的问题

- `Scheduler` 和 `IOManager` 的关系是什么？
- 为什么 Raft 里的心跳和选举任务适合放进协程调度器？
- 为什么 `applierTicker()` 仍然保留为线程？

参考回答：

- `Scheduler` 可以理解成最基础的协程任务调度器，它负责管理任务队列、协程切换和调度线程。`IOManager` 则是在 `Scheduler` 之上扩展出来的版本，它除了能调度协程任务，还能把 fd 的读写事件和 hook 后的阻塞调用纳入统一调度。因此两者不是并列关系，而更像“基础调度器”和“带 IO 事件能力的增强版调度器”。

- Raft 里的 `leaderHearBeatTicker()` 和 `electionTimeOutTicker()` 适合放进协程调度器，是因为它们的行为模式都很像“长期循环 + 定时等待 + 到点执行一小段逻辑”。这类任务大部分时间都在等待超时，真正执行时只是检查状态、发送心跳或触发选举，单次执行耗时相对稳定。用协程调度它们，比每个 ticker 单独开线程更轻量，也更符合这类定时后台任务的特点。

- `applierTicker()` 仍然保留为线程，是因为它的工作负载和心跳/选举不同。它不是简单的固定周期定时器，而是要不断检查 `commitIndex` 和 `lastApplied` 的差距，把一批批已提交日志推给状态机或 KVServer。这个过程的耗时会受到提交速度、待应用日志数量、下游状态机处理速度等因素影响，更像一个持续消费任务的后台 worker，而不是轻量的周期性协程，所以单独保留线程更稳妥。

---

## 3. `raftCoreExample` 怎么看

推荐先看：

1. `example/raftCoreExample/raftKvDB.cpp`
2. `example/raftCoreExample/caller.cpp`

然后按下面顺序回主实现：

1. `src/raftClerk/clerk.cpp`
2. `src/raftCore/kvServer.cpp`
3. `src/raftCore/raft.cpp`

### 先看 `raftKvDB.cpp`

这个文件的作用不是实现 Raft，而是“把整个集群跑起来”。

你要先搞清：

- 节点数怎么从命令行传进来
- 配置文件怎么生成
- 每个节点怎么通过 `fork()` 拉起
- 每个子进程里怎么构造一个 `KvServer`

这一步是帮你建立“进程级视角”。

### 再看 `caller.cpp`

这个文件是客户端入口。

你要看明白：

- `Clerk` 是客户端抽象
- 客户端通过 `Put`、`Get` 发请求
- 配置文件会告诉客户端集群里有哪些节点

这一步是帮你建立“请求入口视角”。

---

## 4. 回主实现时的阅读顺序

### 第一层：`src/raftClerk/clerk.cpp`

先看：

- `Clerk::Get(...)`
- `Clerk::PutAppend(...)`
- `Clerk::Put(...)`
- `Clerk::Append(...)`

这一层重点是：

- 客户端如何缓存最近的 leader
- 为什么失败后会轮询下一个 server
- 为什么每次请求都带 `ClientId + RequestId`

读完后你要能说：

“客户端采用重试模型，但为了避免写请求重复执行，服务端必须做去重。”

### 第二层：`src/raftCore/kvServer.cpp`

重点函数：

- `KvServer::PutAppend(...)`
- `KvServer::Get(...)`
- `KvServer::ReadRaftApplyCommandLoop(...)`
- `KvServer::GetCommandFromRaft(...)`
- `KvServer::SendMessageToWaitChan(...)`

这一层的关键不是记语法，而是理解它在 Raft 和状态机之间起什么作用。

你要抓住：

- `KvServer` 收到 RPC 后，并不直接写数据库
- 它先把请求封装成 `Op`
- 再通过 `Raft::Start(...)` 交给 Raft 提交
- 等日志真正 apply 后，才会唤醒之前等待的 RPC 处理线程

所以这一层本质是一个“中间协调层”。

### 第三层：`src/raftCore/raft.cpp`

第一轮阅读只盯住下面这些函数：

- `Raft::Start(...)`
- `Raft::doHeartBeat(...)`
- `Raft::AppendEntries1(...)`
- `Raft::sendAppendEntries(...)`
- `Raft::doElection(...)`
- `Raft::RequestVote(...)`
- `Raft::applierTicker(...)`

不要一开始就看快照、恢复、细枝末节优化。

你先只回答一个问题：

“一条写请求是怎么从 leader 本地日志，变成多数节点提交，再变成状态机执行的？”

### 第一轮必须走通的一条链路

把这条链路彻底走通：

```text
client.Put
-> KvServer::PutAppend
-> Raft::Start
-> leader 心跳线程复制日志
-> follower AppendEntries
-> leader 推进 commitIndex
-> applierTicker 生成 ApplyMsg
-> KvServer::ReadRaftApplyCommandLoop
-> 真正执行状态机写入
-> 唤醒等待中的 RPC 处理逻辑
-> 返回 OK
```

如果这条链你能不看代码讲出来，Raft 主流程就已经理解了一大半。

参考讲解：

这条链路建议你按“谁在等待、谁在推进、谁在通知”来理解。

第一步，客户端在 [caller.cpp](/home/siaren/Raft/KVstorageBaseRaft-cpp/example/raftCoreExample/caller.cpp) 里调用 `client.Put("x", value)`。这个调用会进入 [clerk.cpp](/home/siaren/Raft/KVstorageBaseRaft-cpp/src/raftClerk/clerk.cpp) 的 `Clerk::Put()`，再进入 `Clerk::PutAppend()`。`Clerk` 会给这次请求分配 `ClientId + RequestId`，然后优先把请求发给它“最近一次认为的 leader”。如果 RPC 失败，或者返回 `ErrWrongLeader`，它就轮询下一个节点继续试。

第二步，请求到达某个节点后，会进入 [kvServer.cpp](/home/siaren/Raft/KVstorageBaseRaft-cpp/src/raftCore/kvServer.cpp) 的 `KvServer::PutAppend(const PutAppendArgs*, PutAppendReply*)`。这里首先把 RPC 参数封装成一个统一的 `Op` 对象，也就是把业务请求抽象成一条“待复制的状态机命令”。然后它调用 [raft.cpp](/home/siaren/Raft/KVstorageBaseRaft-cpp/src/raftCore/raft.cpp) 的 `Raft::Start(op, &raftIndex, &term, &isLeader)`。

第三步，`Raft::Start(...)` 做的事情其实很克制。它先判断当前节点是不是 leader。如果不是，就立刻把 `isLeader=false` 返回给 `KvServer`，后者再返回 `ErrWrongLeader` 给客户端；客户端再去找别的节点。如果当前节点是 leader，`Start(...)` 就把这条 `Op` 序列化后包装成新的 `LogEntry`，追加到 leader 本地 `m_logs` 里，并持久化。注意，这时它只是“写进了 leader 本地日志”，还没有提交，更没有执行状态机。

第四步，`KvServer::PutAppend(...)` 在拿到 `raftIndex` 以后，会在 `waitApplyCh[raftIndex]` 里准备一个等待队列，然后阻塞在 `timeOutPop(...)` 上。你可以把它理解成：“RPC 处理线程现在先停在这里，等 Raft 后台流程真正把这条日志提交成功，再回来叫醒我。”这一点很关键，因为它解释了为什么客户端 RPC 不会在 `Start(...)` 一返回就立刻成功。

第五步，leader 后台的心跳线程会周期性触发 [raft.cpp](/home/siaren/Raft/KVstorageBaseRaft-cpp/src/raftCore/raft.cpp) 里的 `leaderHearBeatTicker()`，然后进入 `doHeartBeat()`。虽然名字叫心跳，但它不只是发空包。`doHeartBeat()` 会针对每个 follower，根据该 follower 当前的 `nextIndex[i]` 构造一份 `AppendEntriesArgs`。如果 follower 落后，它就把缺失的日志条目一起带过去；如果没有新日志，就只是一个空 `entries` 的心跳。

第六步，leader 为每个 follower 启动一个 `sendAppendEntries(...)`。这个函数真正发 RPC，调用远端 follower 的 `AppendEntries`。在 follower 侧，对应会进入 [raft.cpp](/home/siaren/Raft/KVstorageBaseRaft-cpp/src/raftCore/raft.cpp) 的 `AppendEntries1(...)`。这个函数会先检查 term，再检查 `prevLogIndex` 和 `prevLogTerm` 是否匹配本地日志前缀。如果匹配，就把 leader 发来的新日志真正追加或覆盖到 follower 本地日志里，同时根据 `leaderCommit` 推进 follower 自己的 `m_commitIndex`。

第七步，follower 返回成功后，leader 侧的 `sendAppendEntries(...)` 会更新这个 follower 的 `m_matchIndex[server]` 和 `m_nextIndex[server]`。然后它用 `appendNums` 统计“现在有多少节点确认收到了这批日志”。因为 `appendNums` 初始就把 leader 自己算进去了，所以只要达到 `1 + m_peers.size() / 2`，就说明多数派已经复制成功。此时 leader 就可以推进自己的 `m_commitIndex`。这一步就是“日志从已追加变成已提交”的关键转折点。

第八步，`m_commitIndex` 推进之后，这条日志还没有立刻执行状态机。真正负责把“已提交日志”送给上层的是 [raft.cpp](/home/siaren/Raft/KVstorageBaseRaft-cpp/src/raftCore/raft.cpp) 的 `applierTicker()`。它会不断检查 `m_lastApplied < m_commitIndex` 是否成立；只要还有“已经提交但尚未应用”的日志，就通过 `getApplyLogs()` 取出来，包装成 `ApplyMsg`，然后推入 `applyChan`。

第九步，`applyChan` 是 Raft 和 KVServer 之间的桥。在 [kvServer.cpp](/home/siaren/Raft/KVstorageBaseRaft-cpp/src/raftCore/kvServer.cpp) 里，后台线程 `KvServer::ReadRaftApplyCommandLoop()` 会一直阻塞地从 `applyChan->Pop()` 里取消息。拿到 `CommandValid=true` 的消息后，就调用 `GetCommandFromRaft(message)`。这个函数先把 `message.Command` 反序列化回 `Op`，然后检查这是不是重复请求；如果不是重复的，才真正执行 `ExecutePutOpOnKVDB(op)` 或 `ExecuteAppendOpOnKVDB(op)`，也就是把数据写进跳表状态机。

第十步，状态机执行完以后，`GetCommandFromRaft(...)` 还会调用 `SendMessageToWaitChan(op, message.CommandIndex)`。这一步特别重要，因为它会把“这条日志已经真正提交并执行完”的结果，投递回前面 `KvServer::PutAppend(...)` 正在等待的 `waitApplyCh[raftIndex]`。于是最开始那个阻塞等待的 RPC 处理线程就被唤醒了。

第十一步，`KvServer::PutAppend(...)` 从等待队列里拿到 `raftCommitOp` 后，会再核对一次 `ClientId` 和 `RequestId` 是否和当前请求一致。这样做是为了防止 leader 在过程中发生切换，导致同一个 `raftIndex` 最终提交的命令不是自己原来发出的那条。如果一致，就返回 `reply->set_err(OK)`；否则返回 `ErrWrongLeader`，让客户端继续重试。

所以把整条链压缩成一句话就是：

客户端写请求先被 leader 追加到本地日志，然后由 leader 周期性通过 `AppendEntries` 复制给 followers；当多数节点复制成功后，leader 推进 `commitIndex`，再由 `applierTicker()` 把已提交日志转成 `ApplyMsg` 交给 `KvServer` 执行状态机；最后 `KvServer` 通过等待队列唤醒最初阻塞的 RPC 处理线程，向客户端返回 `OK`。

---

## 5. 第二轮再补的内容

第一轮主链路打通后，再单独补这几个问题：

- 为什么 `Get` 也要走 Raft，而不是本地直接读？
- `waitApplyCh` 为什么存在？
- `applyChan` 是谁写、谁读？
- `lastRequestId` 为什么能保证幂等？
- 快照是在哪一层制作的，在哪一层保存的？
- 持久化的是哪些状态？

参考答案：

- `Get` 也要走 Raft，是为了保证线性一致性。如果允许 follower 本地直接读，那么它可能还没收到 leader 最新提交的写入，返回的就是旧值。更严重的是，一个已经失去领导地位的旧 leader 也可能还在对外读服务。这个项目的做法是把 `Get` 也封装成一个要经过 `Raft::Start(...)` 的 `Op`，只有 leader 在当前一致性顺序里确认并提交后，才真正从状态机读取结果返回。这样客户端看到的读一定不会越过已经成功返回的写。

- `waitApplyCh` 的存在，是为了把“前台某个 RPC 请求”和“后台某条日志真正提交完成的时刻”对应起来。`KvServer::Get(...)` 或 `KvServer::PutAppend(...)` 在把命令交给 `Raft::Start(...)` 之后，会拿到一个 `raftIndex`，然后在 `waitApplyCh[raftIndex]` 上阻塞等待。等后台 `ReadRaftApplyCommandLoop()` 发现这条日志已经提交并执行后，再通过 `SendMessageToWaitChan(op, raftIndex)` 把结果投递回来，唤醒最初那个等待中的 RPC 处理线程。没有它，前台线程就不知道“自己那条请求什么时候真的提交了”。

- `applyChan` 是 Raft 和 KvServer 之间的桥梁。写入方是 Raft 层，典型位置在 `Raft::applierTicker()`，它会把已经提交但尚未应用的日志包装成 `ApplyMsg`，然后 `applyChan->Push(message)`。读取方是 KvServer 层，也就是 `KvServer::ReadRaftApplyCommandLoop()`，它会一直阻塞在 `applyChan->Pop()` 上，拿到消息后再真正执行状态机写入或安装快照。所以它是“Raft 提交结果流向状态机”的通道。

- `lastRequestId` 能保证幂等，是因为客户端每次请求都会带上固定的 `ClientId` 和单调递增的 `RequestId`。KvServer 为每个 `ClientId` 记录“这个客户端最新已经处理到哪个请求号”。如果后续又收到相同 `ClientId` 且 `RequestId` 更小或相等的请求，就说明这只是旧请求重试，而不是一个新的逻辑写入。这样即使网络超时、回包丢失，客户端重复发送 Put/Append，也不会让状态机重复执行。

- 快照的制作发生在 KvServer 层，保存发生在 Raft/Persister 这一层。更准确地说，快照“内容格式”由 KvServer 决定，因为只有业务层自己知道要保存哪些业务状态，比如跳表里的 KV 数据和 `lastRequestId`。对应代码是 `KvServer::MakeSnapShot()`。当 KvServer 觉得 Raft 状态太大时，会调用 `m_raftNode->Snapshot(raftIndex, snapshot)`，此时 Raft 负责根据这个快照截断旧日志、更新快照边界索引和 term，并最终调用 `Persister::Save(raftstate, snapshot)` 把 Raft 状态和快照一起落盘。

- 持久化的状态分成两层看。Raft 层自己需要持久化的主要有：`currentTerm`、`votedFor`、当前仍保留的日志 `m_logs`、以及快照边界 `lastSnapshotIncludeIndex` 和 `lastSnapshotIncludeTerm`，这些由 `Raft::persistData()` 打包，再通过 `persist()` 调用 `Persister::SaveRaftState(...)` 或 `Save(...)` 保存。业务层状态则不在 `persistData()` 里，它们通过 KvServer 制作的快照保存，快照里通常包含真正的 KV 数据以及为幂等性服务的 `lastRequestId`。

这时候再去看：

- `Persister`
- `Snapshot(...)`
- `InstallSnapshot(...)`
- `CondInstallSnapshot(...)`

会更顺。

---

## 5.1 `raft.cpp` 深挖时建议优先拿下的 7 个函数

如果你已经看完 `Start / doHeartBeat / AppendEntries1 / applierTicker` 这条主链，下一轮最值得补的是下面 7 个点：

- `sendRequestVote(...)`
- `leaderUpdateCommitIndex()`
- `getApplyLogs()`
- `persistData()`
- `readPersist(...)`
- `Snapshot(...)`
- `InstallSnapshot(...)`

这 7 个函数分别对应 4 类高频面试问题：

- 选举是怎么“真正赢下来”的？
- leader 到底怎么判断一条日志已经提交？
- 崩溃恢复时哪些状态需要持久化？
- 快照是谁做的、谁保存的、谁安装的？

你可以按下面的口语化方式记：

- `sendRequestVote(...)` 不是“发请求本身”那么简单，它更像 candidate 收投票回包后的决策点。谁先在这里拿到多数票，谁就把自己切成 leader，然后初始化 `m_nextIndex` 和 `m_matchIndex`，再立刻发一轮心跳宣布上任。

- `leaderUpdateCommitIndex()` 体现了 leader 的一个核心职责：不是 follower 告诉 leader “你现在可以提交了”，而是 leader 根据每个 follower 的 `m_matchIndex` 自己统计哪条日志已经被多数复制。这里还特地限制“只提交当前 term 的日志”，这是 Raft 的安全规则，面试很容易问到。

- `getApplyLogs()` 是把共识层和状态机层接起来的关键小函数。`m_commitIndex` 表示这条日志已经被 Raft 多数确认，`m_lastApplied` 表示它是否真的已经交给 KV 状态机执行。两者之间的差值，就是还没真正下发给 `KvServer` 的那一段。

- `persistData()` 和 `readPersist(...)` 一起回答“节点崩溃恢复靠什么”。这个项目里真正被 Raft 持久化的是 `currentTerm`、`votedFor`、剩余日志和快照边界；`commitIndex` 和 `lastApplied` 这种易失推进量并不直接写进去，而是靠恢复后的日志/快照状态继续推进。

- `Snapshot(...)` 要从分层职责去理解：快照内容由 `KvServer` 制作，因为只有业务层知道 KV 数据和 `lastRequestId` 要怎么编码；Raft 的职责是接过这份快照后，更新 `m_lastSnapshotIncludeIndex / Term`，截断旧日志，再交给 `Persister` 一起保存。

- `InstallSnapshot(...)` 则是 follower 接收 leader 快照时的入口。它不是自己在 Raft 层直接“恢复业务数据库”，而是把快照封装成 `ApplyMsg{SnapshotValid=true}` 推给 `KvServer`，让状态机层去安装。这样整个项目里“业务状态怎么恢复”仍由业务层自己掌控。

- 如果面试官继续追问 `CondInstallSnapshot(...)`，你可以先诚实说明：这个函数在当前实现里基本留空，核心快照安装逻辑主要落在 `InstallSnapshot(...)` + `KvServer` 的快照处理链上。然后再补一句：标准 Raft 语义里它原本是给 service 一个机会，判断这份快照是否仍值得安装。

这 7 个函数读完以后，你基本就能把 `raft.cpp` 讲成一句完整的话：

“这个实现里，前台 RPC 线程只负责把请求塞进 leader 日志；后台心跳复制线程负责把日志推到多数节点；leader 根据 `matchIndex` 推进 `commitIndex`；applier 线程再把已提交日志交给 `KvServer`；如果日志太长，就由 `KvServer` 做业务快照、Raft 截断日志并通过 `Persister` 持久化，落后的 follower 则通过 `InstallSnapshot` 追平状态。”

---

## 6. 阅读时怎么做笔记

不要按文件顺序抄笔记。按问题做笔记。

建议每次只带一个问题：

- RPC 请求是怎么从 caller 到 callee 的？
- 新日志是谁创建的？
- 日志什么时候算提交？
- 提交后谁通知 KV 层？
- 客户端重试为什么不会重复执行写请求？

每个问题只画 5 到 8 个函数节点。

如果一个函数看不懂，只追它直接调用的下一级，不要横向扩散。

阅读时优先记下面 4 类信息：

- 输入是什么
- 输出是什么
- 持有什么共享状态
- 会唤醒谁、依赖谁

---

## 7. 建议你最终做到的 3 个验收标准

### 验收标准 1

不看代码，能口头讲清 `rpcExample` 的一次调用全链路。

### 验收标准 2

能自己画出 `Put` 请求在 `raftCoreExample` 中的完整时序图。

### 验收标准 3

能解释下面 4 个东西分别解决什么问题：

- `ClientId`
- `RequestId`
- `waitApplyCh`
- `applyChan`

---

## 8. 一句话阅读策略

不要直接从 `src/raftCore/raft.cpp` 第一行硬啃。

更有效的方式是：

先用 `example` 建立“请求怎么流动”的感觉，再回主实现确认每一层具体做了什么。
