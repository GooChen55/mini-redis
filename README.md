# Mini Redis

一个用于学习 C++、Linux、操作系统与计算机网络的 Redis 子集。采用 **C++17 + TCP + epoll Reactor + 线程池 + KV Store + AOF**，支持 Redis RESP2 协议和 `redis-cli`。

## 快速开始：Windows + WSL

在 PowerShell 中进入 Ubuntu：

```powershell
wsl -d Ubuntu-24.04
```

下面的命令在 **Ubuntu 终端** 中执行：

```bash
cd /mnt/d/code/c++
# 仅首次需要；如果工具已安装可跳过
sudo apt-get update
sudo apt-get install -y g++ cmake redis-tools

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/mini_redis
```

服务默认监听 `127.0.0.1:6379`，用 `Ctrl+C` 退出。另开一个 Ubuntu 终端体验：

```bash
redis-cli PING
redis-cli SET name mini-redis
redis-cli GET name
redis-cli INCR counter
redis-cli SET session hello EX 60
redis-cli TTL session
redis-cli MGET name counter session
redis-cli DEL name
```

也可以进入交互模式：`redis-cli`。原生 Linux 使用相同构建方式；原生 Windows 无 epoll，请在 WSL 编译。

## 已实现的功能

| 模块 | 内容 |
| --- | --- |
| TCP 服务 | 非阻塞 socket、连接接入、部分读写、半关闭、断线回收 |
| epoll Reactor | 水平触发，网络收发集中在一个线程，eventfd 唤醒 |
| RESP2 | 二进制安全、拆包/粘包、流水线、协议错误检查 |
| KV Store | 哈希表，字符串 key/value，命令原子执行 |
| 线程池 | 固定工作线程、有界任务队列、同连接命令有序 |
| 过期时间 | SET EX/PX/PXAT、EXPIRE、TTL/PTTL，访问时清理及每秒后台扫描 |
| 持久化 | AOF 写前日志、启动恢复、绝对过期时间、截断不完整尾记录 |
| 验证工具 | 集成测试、并发压测、延迟分位数 |

命令名不区分大小写；参数、key 和 value 区分大小写。

| 命令 | 用法 / 返回 |
| --- | --- |
| PING | `PING [message]`，PONG 或 message |
| ECHO | `ECHO message` |
| SET | `SET key value [EX seconds / PX milliseconds / PXAT unix_ms]` |
| GET / MGET | 缺失值返回 nil |
| DEL | `DEL key [key ...]`，删除数量 |
| EXISTS | `EXISTS key [key ...]`，存在数量 |
| INCR / DECR | 有符号 64 位整数，检查溢出，保留已有 TTL |
| EXPIRE | `EXPIRE key seconds`，非正数立即删除 |
| PEXPIREAT | `PEXPIREAT key unix_ms`，也用于 AOF |
| TTL / PTTL | 剩余秒数 / 毫秒数，-1 无过期，-2 key 不存在 |
| DBSIZE | 未过期 key 的数量 |
| QUIT | 回复 OK 后关闭连接 |
| COMMAND | 返回空数组，用于基础客户端兼容 |

这是 Redis 的学习子集。没有列表、集合、事务、发布订阅、AUTH、SELECT、集群、RDB、淘汰策略或完整的 COMMAND 元数据；现代 SDK 可能需要禁用 RESP3/HELLO 握手。支持 RESP2 数组命令，并额外支持无参数的行内 PING，供 redis-benchmark 使用；其他 telnet 风格的行内命令不支持。

## 启动配置

```bash
./build/mini_redis --bind 127.0.0.1 --port 6380 --threads 4 \
  --aof mini-redis.aof --fsync always

# 测量内存路径，不写持久化文件
./build/mini_redis --port 6380 --no-aof

# 写 AOF，但不逐条 fsync
./build/mini_redis --port 6380 --aof mini-redis.aof --fsync no
```

默认 `--fsync always`：每条修改命令先写 AOF 并 fsync，再改变内存并回复。`--fsync no` 依赖操作系统刷盘，掉电时可能丢失最近写入，正常退出仍会 fsync。AOF 路径相对于启动目录，父目录须存在。独占文件锁阻止多个实例同时使用同一 AOF。

恢复时只截断末尾未完成的 RESP 记录；明确损坏的记录会让启动失败。SET 使用绝对毫秒时间保存 TTL，停机时间计入过期时间。日志 I/O 出错后阻止后续修改并返回错误，读取仍可继续。日志只追加，暂未实现 AOF 重写，长期运行会增大文件。

默认仅本机访问。主动监听其他地址时，请自行配置隔离环境；项目没有认证和加密。

## 压测

服务启动后，在另一终端运行：

```bash
python3 tools/benchmark.py --requests 20000 --clients 8 --pipeline 32 --command PING
python3 tools/benchmark.py --requests 20000 --clients 8 --pipeline 32 --command SET
python3 tools/benchmark.py --requests 20000 --clients 8 --pipeline 32 --command GET
python3 tools/benchmark.py --requests 20000 --clients 8 --pipeline 32 --command INCR

# 或使用 Redis 自带工具，指定本项目实现的命令
redis-benchmark -h 127.0.0.1 -p 6379 -t ping,set,get,incr -n 20000 -c 8 -P 32
```

`redis-benchmark` 可能提示无法获取 CONFIG；项目未实现 CONFIG，不影响上述命令的压测。

脚本报告吞吐量和**整批流水线往返延迟**，p50/p95/p99 不是单条命令延迟。比较结果时保持机器、编译模式、value 大小、连接数、pipeline 和持久化策略相同。Python 客户端和 WSL 挂载盘都会影响测量；默认逐条 fsync 的 SET/INCR 比无持久化模式慢是预期行为。脚本只清理本轮生成的唯一前缀 key。

## 代码阅读顺序与 408 对应

1. `src/resp.cpp`：字节流如何切成命令；理解 TCP 不保留消息边界。
2. `src/store.cpp`：哈希表、整数边界、过期时间；对应数据结构。
3. `src/server.cpp`：非阻塞 I/O、epoll、Reactor、读写缓冲；对应网络和操作系统。
4. `src/thread_pool.hpp`：互斥锁、条件变量、生产者消费者；对应进程/线程同步。
5. 回到 `src/store.cpp` 的 AOF：写前日志、fsync、崩溃恢复；对应文件系统与存储。
6. `tests/integration.py` 和 `tools/benchmark.py`：通过实验验证并发正确性与性能。

详见 [架构说明](docs/architecture.md) 和 [本机验证记录](docs/validation.md)。建议先独立写一个命令，再修改测试验证，接着研究 Reactor；直接背代码不如跟踪一次完整 SET/GET 的执行过程。

## 测试与后续路线

```bash
ctest --test-dir build --output-on-failure

# 内存与未定义行为检查
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=ON
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
```

测试涵盖命令、错误输入、二进制数据、500 条有序流水线、逐字节拆包、半关闭、并发 INCR、TTL、强杀后恢复、不完整 AOF 尾部修复、损坏 AOF 拒绝启动、大响应与断线。

接下来可以逐步实现：

1. 分片 KV 锁与性能对比，分析全局锁瓶颈。
2. 定时器堆 / 时间轮，替换每秒全表过期扫描。
3. AOF 重写、快照与磁盘空间管理。
4. 全局内存上限、LRU/LFU 淘汰、完善慢客户端背压。
5. 多 Reactor 与连接分配，比较单线程、线程池、多 Reactor 的适用场景。
