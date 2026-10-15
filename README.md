# 基于 Asio 的即时通讯系统（服务端）

C++17 + Boost.Asio 实现的前后端即时通讯系统服务端。按职责拆分为 4 个独立进程（服务化），
支持用户注册 / 登录 / 重置密码（邮箱验证码）、好友申请与认证、单聊消息实时收发、离线消息、
会话列表与历史消息加载。配套客户端基于 Qt 5.15（Widgets）。

## 一、架构

```
                        ┌──────────────────────────────┐
       HTTP  8080       │  GateServer（HTTP 网关）      │  Boost.Beast + Asio
   客户端 ───────────────▶ 注册/登录/重置密码/验证码      │  异步 HTTP + io_context 线程池
   (Qt)                 └───────┬──────────────┬────────┘
                                │ gRPC         │ HTTP(MySQL/Redis)
                                ▼              ▼
                    ┌───────────────────┐   ┌─────────────────────┐
                    │ StatusServer 50052│   │ MySQL  用户/好友/会话 │
                    │ 状态服务/负载分配  │   │ Redis  验证码/Token/  │
                    └───────────────────┘   │        在线状态      │
                                ▲           └─────────────────────┘
                                │ gRPC                ▲
       TCP   8990              │                     │
   客户端 ───────────────▶ ChatServer（消息服务）──────┘
   (长连接)                 Asio 协程读写 + 业务线程
```

- **GateServer**：无状态 HTTP 网关，四大接口（验证码 / 注册 / 登录 / 重置密码），失败统一映射为业务错误码。
- **StatusServer**：登录时分配可用的 ChatServer 节点并签发随机 Token（UUID v4）写入 Redis。
- **ChatServer**：TCP 长连接，好友申请 / 认证 / 消息收发 / 会话与历史消息；目标在线则本机推送，
  不在线则经 gRPC 转给持有该用户连接的节点。
- **VarifyServer**（Node.js）：gRPC 服务端，负责邮箱验证码的发送与校验。

## 二、技术栈

| 方向 | 使用 |
|---|---|
| 语言/标准 | C++17、C++20 协程（`boost::asio::awaitable` / `co_spawn`）、Node.js（VarifyServer） |
| 网络 | Boost.Asio（Proactor：Windows IOCP / Linux epoll）、Boost.Beast（HTTP） |
| 服务间通信 | gRPC + Protobuf |
| 存储 | MySQL 8（自封装连接池）、Redis（验证码、Token、在线状态） |
| 构建/工具 | CMake + vcpkg、Visual Studio 2022（`*.slnx`）、Git |

代码量（不含 protobuf 生成代码）：GateServer ≈ 1900 行、StatusServer ≈ 1200 行、ChatServer ≈ 3100 行，
VarifyServer ≈ 220 行 JS。

## 三、目录结构

```
GateServer/     HTTP 网关（Asio + Beast + gRPC 客户端 + MySQL/Redis 连接池 + 60s 超时定时器）
StatusServer/   状态服务（gRPC 服务端，节点分配 + Token 签发）
ChatServer/     TCP 消息服务（会话读写协程、业务逻辑线程、消息路由、DAO/连接池）
VarifyServer/   Node.js 验证码服务（gRPC 服务端 + 邮件发送）
```

## 四、通信协议

客户端与 ChatServer 之间是**自定义 TCP 应用层协议**，逐字节拆包：

```
+--------+--------+---------------+
| Msg ID | Length |     Body      |
|  2 字节 | 2 字节 | Length 字节(JSON) |
+--------+--------+---------------+
```

`Length` 只表示消息体长度，用于解决 TCP 粘包 / 拆包 / 半包问题。

常用消息 ID（完整定义见 `ChatServer/ChatServer/const.h:55-77`）：

| ID | 含义 | ID | 含义 |
|---|---|---|---|
| 1005 / 1006 | 登录 / 登录回包 | 1019 | 通知收到文本消息 |
| 1007 / 1008 | 搜索用户 / 回包 | 1021 | 通知用户下线 |
| 1009 / 1010 | 申请添加好友 / 回包 | 1023 / 1024 | 心跳 / 心跳回包（待接入） |
| 1011 | 通知对方有新好友申请 | 1025 / 1026 | 加载会话列表 / 回包 |
| 1013 / 1014 | 认证好友 / 回包 | 1027 / 1028 | 创建私聊会话 / 回包 |
| 1015 | 通知对方认证结果 | 1029 / 1030 | 加载历史消息 / 回包 |
| 1017 / 1018 | 发送文本消息 / 回包 | | |

## 五、关键实现

- **协程读写循环**：`CSession::ReadLoop()/WriteLoop()` 为 `net::awaitable<void>`，用
  `co_await net::async_read/async_write(..., net::redirect_error(net::use_awaitable, ec))`
  把多层回调改成顺序代码；会话对象由 `enable_shared_from_this` + `co_spawn` 保活。
- **IO 与业务分离**：`AsioIOServicePool` 按 CPU 核数创建 `io_context`，网络线程只做收发与拆包，
  业务报文投入逻辑线程（`LogicSystem` 内部单线程 `std::thread` + 队列）串行处理，减少共享数据竞争。
- **同一连接串行写**：`std::queue<std::string> _send_que` + `std::mutex` + `_writing` 写标志，
  保证一条连接上的并发写不会交叉；连接断开时清理会话并复位解析状态。
- **优雅退出**：各服务用 `boost::asio::signal_set` 捕获 `SIGINT/SIGTERM`，停止 io_context 并回收线程。
- **数据库访问**：自封装 MySQL 连接池（借用/归还/保活/自动补齐），以「查重 → 发号 → 插入」单事务
  保证注册时 uid 与用户信息一致。
- **会话/好友语义**：`friend` 表存有向边 `(self_id, friend_id, back)`，`back` 是"对方给我起的备注"，
  互为好友是两行；聊天记录按 `thread_id` 归档，支持按页上翻加载历史。

## 六、编译与运行

```bash
# 依赖: Visual Studio 2022、vcpkg、Boost、gRPC、Protobuf、hiredis、mysql-connector-c++
vcpkg install boost-asio boost-beast grpc protobuf hiredis
```

1. 把 `<服务>/config.ini.example` 复制成 `<服务>/<服务>/config.ini`（与 `.vcxproj` 同级）并填写自己的密码；
   `config.ini` 已在 `.gitignore` 中，不会被提交
2. 初始化数据库：执行 `sql/init.sql` 建表（`user` / `user_id` / `friend` / `friend_apply` /
   `chat_thread` / `chat_msg` 共 6 张表）
3. 用 Visual Studio 打开各服务的 `.slnx` 编译；VarifyServer 执行 `npm install && node server.js`
   （并配置 `VarifyServer/config.json`：数据库、Redis、发件邮箱与授权码）
4. 启动顺序：MySQL / Redis → VarifyServer → StatusServer → ChatServer → GateServer

## 七、已实现 / 未实现

**已实现**：注册、登录（Token）、重置密码（邮箱验证码）、搜索用户、好友申请、申请列表、
同意/拒绝、好友列表、会话列表、消息实时收发、离线消息、历史消息翻页、未读数、
最后一条消息与时间、好友备注（申请时填写 + 同意时填写）。

**未实现（后续）**：心跳保活、重复登录踢人（1021/1023/1024 已定义未接入）、多机部署与跨服转发、
图片/文件传输、群聊。

## 八、客户端

Qt 5.15 客户端（登录/注册界面、聊天主界面、联系人页、好友资料页）源码见
**（待补充仓库链接）**。
