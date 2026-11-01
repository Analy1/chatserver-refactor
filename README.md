# chatserver

基于 muduo 的 C++ 集群聊天服务器与命令行客户端。

支持多实例部署：用户连接分布在不同节点上时，消息通过 Redis Pub/Sub 转发；对方离线时写入离线消息表，下次登录补发。

## 功能

- 用户注册、登录、注销
- 一对一聊天
  - 对方在本节点：直接转发
  - 对方在其他节点：经 Redis 频道转发
  - 对方离线：写入离线消息，登录时补发
- 好友关系：添加好友、登录时返回好友列表及在线状态
- 群组：创建群组、加入群组、群聊
- 协议层按 4 字节长度前缀分帧，处理粘包/半包、超长帧和非法帧

## 技术栈

| 组件 | 说明 |
|---|---|
| 语言 | C++（C++11 及以上） |
| 网络库 | muduo（静态库，安装在 `/usr/local`） |
| 数据库 | MySQL 8，库名 `chat` |
| 缓存 / 消息 | Redis 7，hiredis 客户端 |
| JSON | nlohmann/json（`thirdparty/json.hpp`） |
| 构建 | CMake |

## 架构

```
┌────────────────────┐        TCP: 4 字节长度 + JSON
│  ChatClient        │ ─────────────────────────────┐
│  主线程：菜单/发送 │                              │
│  接收线程：poll+分帧│ ◄───────────────────────────┐ │
└────────────────────┘                              │ │
                                                    │ │
┌───────────────────────────────────────────────────▼─▼────┐
│  ChatServer (muduo TcpServer)                            │
│  1 个 main loop + 4 个 IO 线程                           │
│  onMessage：按帧解码 → json::parse → 按 msgid 分发       │
└───────────────────────────┬──────────────────────────────┘
                            │ 同线程调用
┌───────────────────────────▼──────────────────────────────┐
│  ChatService（单例）                                      │
│  8 类消息 handler、在线连接表、离线消息、Redis 转发       │
└──────┬──────────────────┬──────────────────┬─────────────┘
       │                  │                  │
┌──────▼──────┐   ┌───────▼───────┐  ┌───────▼──────────┐
│ 4 个 Model  │   │ MySQL（直连） │  │ Redis 发布/订阅   │
│ 拼 SQL 执行 │   │ 每条 SQL 新建 │  │ 独立订阅线程      │
└─────────────┘   └───────────────┘  └──────────────────┘
```

线程模型：

- 服务端：muduo main loop + 4 个 IO 线程（`setThreadNum(4)`），业务 handler 在 IO 线程同步执行
- Redis：一个独立线程阻塞等待订阅消息，收到后回调业务层
- 客户端：主线程负责菜单和发送，接收线程负责 `recv` + 分帧

## 通信协议

### 帧格式

```
+----------------+----------------------------+
| 4 字节大端长度 |  body（UTF-8 JSON）        |
| N = len(body)  |  N 字节                    |
+----------------+----------------------------+
```

- 长度字段只表示 body 字节数，不包含 4 字节头
- 单帧上限 256 KB

### 消息 ID

| ID | 名称 | 方向 | 说明 |
|---|---|---|---|
| 1 | `LOGIN_MSG` | C → S | 登录请求 |
| 2 | `LOGIN_MSG_ACK` | S → C | 登录响应（含好友、群组、离线消息） |
| 3 | `LOGINOUT_MSG` | C → S | 注销 |
| 4 | `REG_MSG` | C → S | 注册请求 |
| 5 | `REG_MSG_ACK` | S → C | 注册响应 |
| 6 | `ONE_CHAT_MSG` | C → S / S → C | 单聊 |
| 7 | `ADD_FRIEND_MSG` | C → S | 添加好友 |
| 8 | `CREATE_GROUP_MSG` | C → S | 创建群组 |
| 9 | `ADD_GROUP_MSG` | C → S | 加入群组 |
| 10 | `GROUP_CHAT_MSG` | C → S / S → C | 群聊 |

### 示例

登录请求：

```json
{"msgid":1,"id":1,"password":"123456"}
```

登录成功响应：

```json
{
  "msgid": 2,
  "errno": 0,
  "id": 1,
  "name": "zhang",
  "friends": ["{\"id\":2,\"name\":\"li\",\"state\":\"offline\"}"],
  "groups": ["{\"id\":1,\"groupname\":\"team\",\"groupdesc\":\"demo\",\"users\":[]}"]
}
```

单聊：

```json
{"msgid":6,"id":1,"name":"zhang","toid":2,"msg":"hello","time":"2026-11-01 10:00:00"}
```

## 构建与运行

### 依赖

```bash
sudo apt install cmake g++ libmysqlclient-dev libhiredis-dev mysql-server redis-server
```

muduo 需要提前编译安装到 `/usr/local`（`libmuduo_net.a`、`libmuduo_base.a`）。

### 编译

```bash
cmake -S . -B build
cmake --build build -j
```

产物输出到 `bin/`：`ChatServer`、`ChatClient`。

也可以使用 `./autobuild.sh`（要求 `build/` 目录已存在）。

### 准备环境

- MySQL：需要已存在 `chat` 库及 `user`、`friend`、`allgroup`、`groupuser`、`offlinemessage` 五张表
- 默认连接配置写在：
  - `src/server/db/db.cpp`：`127.0.0.1:3306`、`root/111111`、库名 `chat`
  - `src/server/redis/redis.cpp`：`127.0.0.1:6379`
- 环境不一致时需先修改上述两处并重新编译

### 启动

```bash
./bin/ChatServer 127.0.0.1 6000
```

另开终端：

```bash
./bin/ChatClient 127.0.0.1 6000
```

数据库中没有账号时，客户端选择 `2. register` 注册。

## 客户端命令

| 命令 | 格式 | 说明 |
|---|---|---|
| `help` | `help` | 显示命令列表 |
| `chat` | `chat:friendid:message` | 一对一聊天 |
| `addfriend` | `addfriend:friendid` | 添加好友 |
| `creategroup` | `creategroup:groupname:groupdesc` | 创建群组 |
| `addgroup` | `addgroup:groupid` | 加入群组 |
| `groupchat` | `groupchat:groupid:message` | 群聊 |
| `loginout` | `loginout` | 注销并回到登录菜单 |

## 验证

协议层与收发链路已在本机完成验证：

| 项目 | 结果 |
|---|---|
| 分帧单元测试 | 12 组用例，114 项检查全部通过 |
| 端到端测试 | 19 项全部通过 |
| 10000 帧一次发送 | 服务端全部处理，0 丢帧 |
| 100KB 单帧 | 完整转发 |
| 200 个空闲长连接 | 全部接受，服务进程存活 |

> 验证脚本保存在本地，未包含在本仓库中。

## 目录结构

```
chatserver/
├── CMakeLists.txt
├── autobuild.sh
├── include/
│   ├── public.hpp                 # 消息 ID 枚举
│   ├── net/frame_codec.hpp        # 长度前缀分帧器
│   └── server/
│       ├── chatserver.hpp
│       ├── chatservice.hpp
│       ├── db/db.h
│       ├── model/                 # User / Group / GroupUser 及 Model 声明
│       └── redis/redis.hpp
├── src/
│   ├── client/main.cpp
│   └── server/
│       ├── main.cpp
│       ├── chatserver.cpp
│       ├── chatservice.cpp
│       ├── db/db.cpp
│       ├── model/                 # 4 个 Model
│       └── redis/redis.cpp
└── thirdparty/json.hpp
```

## 运行说明

- 服务端和客户端必须使用同一版本的协议；旧版客户端需要重新编译
- 数据库与 Redis 地址目前写死在源码中，修改后需要重新编译
- 客户端连接断开后接收线程会通知主线程退出，不会在后台残留
