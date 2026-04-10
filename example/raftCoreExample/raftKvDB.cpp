#include <iostream>
#include "raft.h"
#include <kvServer.h>
#include <unistd.h>
#include <iostream>
#include <random>

void ShowArgsHelp();

int main(int argc, char **argv) {
  // 这个文件不是 Raft 算法核心，而是“集群启动器”。
  // 它负责把多个 KvServer 节点作为多个进程拉起来，形成一个可运行的 Raft KV 集群。
  //
  // 启动流程可以概括为：
  // 1. 读取节点数量和配置文件名
  // 2. 生成一组端口
  // 3. fork 出多个子进程
  // 4. 每个子进程里构造一个 KvServer
  //////////////////////////////////读取命令参数：节点数量、写入raft节点节点信息到哪个文件
  if (argc < 2) {
    ShowArgsHelp();
    exit(EXIT_FAILURE);
  }
  int c = 0;
  int nodeNum = 0;
  std::string configFileName;
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> dis(10000, 29999);
  // 随机挑一个起始端口，减少多次运行时端口冲突的概率。
  unsigned short startPort = dis(gen);
  while ((c = getopt(argc, argv, "n:f:")) != -1) {
    switch (c) {
      case 'n':
        nodeNum = atoi(optarg);
        break;
      case 'f':
        configFileName = optarg;
        break;
      default:
        ShowArgsHelp();
        exit(EXIT_FAILURE);
    }
  }

  // 先把配置文件清空，保证这是本轮启动的干净配置。
  // 后续各节点会把自己的 ip/port 追加写进去，供其他节点和客户端读取。
  std::ofstream file(configFileName, std::ios::out | std::ios::app);
  file.close();
  file = std::ofstream(configFileName, std::ios::out | std::ios::trunc);
  if (file.is_open()) {
    file.close();
    std::cout << configFileName << " 已清空" << std::endl;
  } else {
    std::cout << "无法打开 " << configFileName << std::endl;
    exit(EXIT_FAILURE);
  }

  // 依次创建多个子进程，每个子进程对应一个 KvServer 节点。
  for (int i = 0; i < nodeNum; i++) {
    short port = startPort + static_cast<short>(i);
    std::cout << "start to create raftkv node:" << i << "    port:" << port << " pid:" << getpid() << std::endl;
    pid_t pid = fork();  // 创建新进程
    if (pid == 0) {
      // 子进程内部真正启动一个 KVServer。
      // KvServer 内部会继续初始化本地 Raft、RPC 服务、apply 线程等。

      auto kvServer = new KvServer(i, 500, configFileName, port);
      (void)kvServer;
      // 子进程不能退出，否则服务进程会结束。
      pause();  // 子进程进入等待状态，不会执行 return 语句
    } else if (pid > 0) {
      // 父进程稍作等待，给子进程初始化和绑定端口留一点时间。
      sleep(1);
    } else {
      // fork 失败说明集群无法完整拉起，直接退出。
      std::cerr << "Failed to create child process." << std::endl;
      exit(EXIT_FAILURE);
    }
  }

  // 父进程自身也保持存活，作为这批子进程的宿主。
  pause();
  return 0;
}

void ShowArgsHelp() { std::cout << "format: command -n <nodeNum> -f <configFileName>" << std::endl; }
