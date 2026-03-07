## KVStore - 轻量级高性能键值存储系统

KVStore 是一个从零实现的类 Redis 键值存储系统，采用哈希表存储引擎、Reactor 网络模型、主从复制和 AOF/RDB 持久化。单实例 pipeline 模式 QPS 可达 19w，基于 jemalloc 内存分配器。支持二进制安全和大 value 存储。

## 1. 特性

- **高性能网络**：基于 epoll 的单线程 Reactor 模型，无锁设计，轻松应对高并发。

- **RESP 协议**：兼容 Redis 序列化协议，可使用 redis-cli 直接访问。

- **指令支持**：
  - `SET <key> <value>` - 设置键值对
  - `GET <key>` - 获取键对应的值
  - `EXISTS <key>` - 检查键是否存在
  - `DEL <key>` - 删除键
  - `SAVE` - 手动保存 RDB 快照

- **键值存储引擎**：O(1) 读写，支持动态扩容和二进制安全（含 `\0`）。

- **主从复制**：全量同步 + 增量广播 + 断线重连 + 手动故障转移。

- **持久化**：支持三种持久化策略（AOF 日志、RDB 快照、混合模式），可通过配置文件灵活切换。

- **大 value 支持**：读写缓冲区动态扩容，可存储任意大小数据。

- **配置管理**：支持配置文件（INI 格式）与命令行参数双重配置，可设置端口、持久化模式、日志级别、复制开关等。

- **日志分级**：支持 INFO、WARN、DEBUG 三级日志，可根据需要调整输出详细程度。

## 2. 配置文件说明

KVStore 支持通过配置文件进行灵活配置，配置文件采用 INI 格式，包含三个主要部分：

```ini
# kvstore.conf

[server]
port = 6379
log_level = 2          # 日志级别: 1=INFO, 2=WARN, 3=DEBUG

[persist]
mode = 3               # 持久化模式: 0=关闭, 1=仅AOF, 2=仅RDB, 3=混合
rdb_file = ../data/kvstore.rdb
rdb_save_interval = 300
rdb_min_changes = 100
rdb_save_on_shutdown = true
aof_file = ../data/kvstore.aof
aof_rewrite_size = 1    # MB
aof_auto_rewrite = true

[replication]
enabled = false        # 是否开启主从复制
role = master          # 角色: master 或 slave
# 如果 role = slave，需配置以下两项
# master_ip = 127.0.0.1
# master_port = 6379
```

配置文件搜索顺序（优先级递减）：
1. 命令行 `-c` 参数指定的路径
2. 当前目录 `./kvstore.conf`
3. 用户主目录 `~/.kvstore.conf`
4. 系统目录 `/etc/kvstore.conf`

## 3. 使用

### 3.1 编译
```bash
cd .
make
```
或者调试模式（带详细日志和 AddressSanitizer）：
```bash
make DEBUG=1
```

### 3.2 启动

**使用默认配置**（自动查找配置文件）：
```bash
cd ./bin
./kvstore
```

**指定端口**（覆盖配置文件）：
```bash
./kvstore -p 6379
```

**指定配置文件**：
```bash
./kvstore -c /path/to/kvstore.conf
```

**指定持久化模式**：
```bash
./kvstore --persist-mode 3
```

### 3.3 从机启动
```bash
./kvstore --slaveof <master_ip> <master_port>
```

### 3.4 命令行参数优先级
命令行参数 > 配置文件 > 默认值

### 3.5 redis-cli 访问
```bash
redis-cli -p 6379
> SET name "John Doe"
+OK
> GET name
"John Doe"
```

### 3.6 redis-benchmark 基准测试
```bash
# 基本 SET/GET 测试
redis-benchmark -p 6379 -t set,get -n 10000

# 并发测试（100个客户端，持续10秒）
redis-benchmark -p 6379 -c 100 -n 100000 -t set,get

# Pipeline 模式测试
redis-benchmark -p 6379 -P 16 -n 100000 -t set,get
```

## 4. 持久化模式详解

| 模式 | 说明 | 适用场景 |
|------|------|----------|
| 0 | 关闭持久化 | 纯缓存，允许数据丢失 |
| 1 | 仅 AOF | 需要高数据安全性，允许少量性能损耗 |
| 2 | 仅 RDB | 数据量大，可接受短时间数据丢失 |
| 3 | 混合模式 | 兼顾性能与安全（默认） |

## 5. 日志级别

| 级别 | 说明 | 输出内容 |
|------|------|----------|
| 1 | INFO | 仅打印必要信息（启动、关闭、重要事件） |
| 2 | WARN | 打印警告和必要信息 |
| 3 | DEBUG | 打印所有调试信息（包括协议解析、命令执行等） |

## 6. 测试

### 6.1 编译测试用例
```bash
cd .
make test
```
或者调试模式：
```bash
make DEBUG=1 test
```

### 6.2 运行测试用例
```bash
cd ./bin
# 先启动服务器（另一个终端）
./kvstore -p 8888

# 运行测试
./testcase 127.0.0.1 8888 1000
./test_resp 127.0.0.1 8888
```

### 6.3 可用的测试程序
- `testcase` - 批量处理性能测试
- `test_resp` - RESP 协议测试
- `test_special` - 特殊字符测试
- `test_aof` - AOF 持久化测试
- `test_rdb` - RDB 持久化测试
- `test_repl` - 主从复制测试

### 6.4 一键运行所有测试
```bash
make run-tests   # 需先启动服务器
```

## 7. 性能调优建议

- **生产环境**：建议使用混合模式（mode=3），日志级别设为 WARN（2）
- **开发调试**：建议使用 DEBUG 模式编译，日志级别设为 DEBUG（3）
- **内存分配器**：基于 jemalloc 进行内存管理

## 8. 注意事项

1. 确保数据目录（如 `../data/`）存在且有写入权限
2. 主从复制需要网络互通，从机启动时会自动连接主机
3. AOF 重写功能默认开启，当 AOF 文件超过设定阈值时自动触发
4. RDB 自动保存间隔默认为 300 秒，可通过配置文件调整

## 9. 版本信息

- **当前版本**：1.1.0
- **更新内容**：新增配置管理、日志分级、持久化模式选择
- **作者**：Neil Yuan

---

如有问题或建议，欢迎提交 Issue 或 Pull Request。