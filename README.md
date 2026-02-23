### KVStore - 轻量级高性能键值存储系统

KVStore 是一个从零实现的类 Redis 键值存储系统，采用哈希表存储引擎、Reactor 网络模型、主从复制和 AOF/RDB 持久化。单实例 pipeline 模式 QPS 可达 8 万，内嵌专用内存池QPS相比于jemalloc提高百分之20。 支持二进制安全和大 value 存储。


## 1.特性

    高性能网络：基于 epoll 的单线程 Reactor 模型，无锁设计，轻松应对高并发。

    RESP 协议：兼容 Redis 序列化协议，可使用 redis-cli 直接访问
	
	指令支持：
	- HSET <key> <value>
	- HGET <key>
	- HEXSIT <key>
	- HMOD <key> <value>
	- SAVE（手动保存RDB快照）

    哈希表引擎：O(1) 读写，支持动态扩容和二进制安全（含 \0）。

    主从复制：全量同步 + 增量广播 + 断线重连 + 手动故障转移。

    持久化：AOF（增量日志）与 RDB（定期快照）双机制，数据可靠。

    大 value 支持：读写缓冲区动态扩容，可存储任意大小数据。
	 
## 2.使用
	
	1.编译
	```
	cd .
	make 
	```
	或者调式模式
	```
	make DEBUG=1 
	```
	
	2.启动
	
	```
	cd ./bin
	./kvstore <port>
	```
	
	3.(可选)从机启动
	```
	./kvstore <slave_port> --slaveof <master_ip> <master_port>
	```
	
	4.redis-cli访问
	

## 3.测试
	
	1.编译测试用例
	```
	cd .
	make test
	```
	或者调试模式
	```
	make DEBUG=1 test	
	```
	
	2.运行测试用例
	```
	cd ./bin
	./test <ip> <port> <测试命令数>
	```
