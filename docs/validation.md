# 本机验证记录

验证日期：2026-10-04。环境：Windows 上的 Ubuntu 24.04 WSL2、GCC 13.3、C++17、Release 构建。编译无警告。

## 正确性

Release 与 AddressSanitizer / UndefinedBehaviorSanitizer 构建均通过 9 项集成测试：

- AOF 独占锁。
- 8 个并发连接共 1600 次 INCR，返回值无重复且最终值正确。
- 基础命令、参数错误、整数溢出。
- 过期时间、强杀进程后恢复。
- 2 MiB value、大回复、慢读与客户端断线。
- AOF 不完整尾部修复、明确损坏记录拒绝启动。
- 500 次流水线自增、逐字节拆包、TCP 写半关闭。
- 非法协议、行内 PING、混合大小写 QUIT。
- redis-cli 的 PING / SET / GET / INCR。

内存检查最终运行没有报告内存错误、泄漏或未定义行为。两套持久化测试同时运行时曾有一次客户端 10 秒超时；测试客户端现使用 30 秒超时，单独运行完整内存检查约 12 秒通过。

## 压测工具验证

自带 Python 压测脚本在无持久化与 AOF 逐条 fsync 两种模式下，均完成 PING / SET / GET / INCR。参数：每项 2000 请求、4 连接、pipeline=16、value=64 字节。

| 模式 | PING 请求/秒 | SET 请求/秒 | GET 请求/秒 | INCR 请求/秒 |
| --- | ---: | ---: | ---: | ---: |
| 无持久化 | 195125 | 112221 | 167040 | 173869 |
| AOF + fsync always | 160380 | 188 | 128215 | 192 |

这是**工具与路径验证的小样本**，不能用作生产容量结论。内存模式测量仅约 10–18 毫秒，波动较大；逐条 fsync 的写入约 10 秒，体现当前环境的存储成本。Python 开销、流水线、虚拟化和磁盘都影响结果。

redis-benchmark 也完成了 PING_INLINE、PING_MBULK、SET、GET、INCR 的无持久化验证。由于项目没有实现 CONFIG，它会输出无法获取 CONFIG 的提示，测试命令仍可执行。

复现方式（Ubuntu 终端，先构建）：

```bash
cd /mnt/d/code/c++
ctest --test-dir build --output-on-failure
ctest --test-dir build-asan --output-on-failure
python3 tests/benchmark_smoke.py build/mini_redis
```

建议顺序执行，避免逐条刷盘测试互相争用磁盘。最后一个脚本自动启动临时端口服务、使用临时 AOF，并在结束后关闭服务；需要安装 redis-tools。
