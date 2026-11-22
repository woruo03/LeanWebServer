# LeanWebServer — 轻量级高性能现代 C++ Web 服务器

<p align="center">
  <img src="https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus&logoColor=white" alt="C++17"/>
  <img src="https://img.shields.io/badge/platform-Linux-FCC624?logo=linux&logoColor=black" alt="Linux"/>
  <img src="https://img.shields.io/badge/CMake-3.25%2B-064F8C?logo=cmake&logoColor=white" alt="CMake"/>
  <img src="https://img.shields.io/badge/build-Ninja-brightgreen" alt="Ninja"/>
  <img src="https://img.shields.io/badge/tests-50%2F50%20passing-brightgreen" alt="Tests"/>
  <img src="https://img.shields.io/badge/sanitizers-ASan%20%7C%20UBSan%20clean-success" alt="Sanitizers"/>
  <img src="https://img.shields.io/badge/license-Apache%202.0-blue" alt="Apache 2.0 License"/>
</p>

基于 **C++17** 的轻量级、高并发现代 Web 服务器。网络层采用 **epoll 主从多 Reactor（Main-Sub Reactors / One Loop Per Thread）** 模型与 **Sub-Reactor Share-Nothing 无锁去中心化** 架构，HTTP 层采用状态机流式零拷贝解析，内置基于单一内核 `timerfd` 与动态可配置散列时间轮的双轨超时治理系统、4MB 双缓冲异步单例日志系统、基于原生 `signalfd` 的 5 阶段分布式优雅停机协调机制，并配套完备的现代 CMake 目标化工程与全方位质量保障工具链（单元测试、集成测试、ASan/UBSan、严苛编译告警、Clang-Tidy 静态分析与性能剖析）。

---

## 核心架构特性

### 1. 并发与网络模型
- **Sub-Reactor Share-Nothing 架构**：彻底消除主线程集中连接表与跨线程锁争用。Main Reactor 专职 `accept` 并生成连接全局自增 ID，仅以 `(sockfd, conn_id, config)` 整型三元组跨线程投递至目标 IO 线程。连接的创建、注册、读写驱动、超时管理与析构 100% 在 Sub-Reactor 本地 `unordered_map` 完成，无跨线程持锁竞争，零跨线程跳回开销；IO 线程池默认动态自适应主机硬件并发核心数（$N_{\text{cpu}} - 1$，预留 1 核给 Main Reactor，保底 1），杜绝过度上下文切换。
- **高并发 I/O 驱动**：基于 Linux `epoll` 边缘触发（`EPOLLET`）+ 非阻塞 I/O，小包默认开启 `TCP_NODELAY`（禁用 Nagle 算法消除延迟抖动），`EpollPoller` 内置异常熔断守卫（防止 `epoll_wait` 持续故障自旋）。
- **细粒度写缓冲治理**：重构非阻塞写循环与事件驱动重试，防止慢客户端误杀。在数据未一次性写完时，将剩余数据追加至应用层输出缓冲区，精准让出并在 `EPOLLOUT` 事件驱动下接管重试，仅在发生致命网络故障（如 `EPIPE`, `ECONNRESET`）时进入错误拆除分支。

### 2. 双轨高精度定时器系统
- **单一内核 `timerfd` 驱动队列（`TimerQueue`）**：采用 Linux 原生 `timerfd_create(CLOCK_MONOTONIC)`，彻底免疫系统墙钟（NTP 或手工修改）跳变带来的雪崩风险。红黑树（`std::set`）维护到期时间点，支持单次、周期任务以及基于代际序列号的 `TimerId` 弱令牌 O(log N) 安全撤销，具备回调内自撤销安全屏障。
- **动态散列时间轮（`TimerWheel`）**：
  - **动态可配置槽位**：支持通过全局配置指定槽位数（需为 2 的幂次，默认 1024 槽），取模优化为常数级位与操作；
  - **O(1) 滚动续期**：统一向 `EventLoop` 的 `TimerQueue` 注册 1s 节拍驱动；
  - **纳秒级同秒短路去重**：连接在同 1 秒内的多次读写交互直接命中短路，不发生多余的槽位迁移与弱引用追加；
  - **代际验真防误杀**：槽位仅保存 `std::weak_ptr<TcpConnection>`，超时触发时严格校验游标与连接的 `last_slot` 一致性，防范续期后假死误关；
  - **零残留保证**：连接正常断开时自动清空槽位标记，TimerQueue 中除时间轮自身 1s 驱动外无任何残留。

### 3. 连接全生命周期与超时治理
- **四态原子生命周期状态机**：
  - `kConnecting` $\to$ `kConnected` $\to$ `kDisconnecting` $\to$ `kDisconnected`
  - 核心不可逆防重入屏障，严格拦截同帧 `EPOLLIN + EPOLLOUT` 重复关闭、连接延迟拔树期野指针访问与误复活问题。
- **双层超时矩阵**：
  - **L1 滚动空闲超时（默认 30s）**：HTTP 每次请求解析完成与数据收发均自动触发时间轮槽位续期，杜绝 Slowloris 慢速拒绝服务攻击。
  - **L2 绝对存活上限（默认 120s）**：防止恶意长连接无限占据服务器文件描述符池。
- **Keep-Alive 协议兼容**：HTTP/1.1 请求自动注入 `Connection: keep-alive` 与 `Keep-Alive: timeout=30` 响应头；HTTP/1.0 缺省返回 `Connection: close` 并自动触发写完优雅半关闭。

### 4. 高性能异步单例日志系统
- **Meyers 单例前端门面**：双门面日志宏支持，兼具现代化流式宏（`LOG_INFO`, `LOG_WARN`, `LOG_ERROR`, `LOG_FATAL`）与支持 `-Wformat=2` 编译期校验的格式化宏（`LOG_FMT_*`）。
- **4MB 双缓冲交换与背压截断**：前端基于 4KB 栈上 `FixedBuffer` 配合二位数字查表法（`k_digits`）快速组装行文本；后端 `AsyncLogging` 维持 4MB 双缓冲区热交换，设置 25 块（100MB）背压丢弃上限保护，杜绝瞬时突发洪峰引发 OOM 崩溃。
- **秒级自动切分与应急兜底**：满足 64MB 大小或跨日变更时自动轮转生成新日志文件，保留最近 10 个历史文件；支持 `init()` 初始化前以及极端致命错误崩溃（`abort()`）时的 `stderr` / `emergency_flush()` 零丢失落盘机制。

### 5. 分布式优雅停机治理（Graceful Shutdown）
- **早期信号屏蔽**：进程启动伊始立即调用 `pthread_sigmask(SIG_BLOCK, ...)`，确保包括日志后台线程与工作线程池在内的所有派生线程统一屏蔽 `SIGINT`/`SIGTERM`。
- **`signalfd` 原生事件化**：主事件循环挂接 Linux 原生 `signalfd` 文件描述符，消除传统异步信号处理函数（Signal Handler）对互斥锁与不可重入函数的死锁风险。
- **5 阶段分布式协同排水**：
  1. `Accepter::stop()` 立即关闭监听套接字，拒绝新连接请求；
  2. 向所有 Sub-Reactor 广播 `initiate_graceful_shutdown()`；
  3. 各 IO 线程并行执行本地连接 `force_close()`：无积压连接立即拆除，有积压连接切入 `kDisconnecting` 等待写缓冲排空或 5s 宽限期；
  4. 最后一个完成排空的 Sub-Reactor 唤醒 `main_loop` 退出循环；
  5. 线程池安全 `join()` 回收所有线程，冲刷日志系统，进程确定性以退出码 0 终止。
- **二次信号强杀兜底**：停机排空中若用户再次发送中断信号，立即恢复系统默认处理强杀退出，杜绝僵死。

---

## 高可靠性工程与架构设计保障

| 架构维度 | 关键挑战与隐患 | 现代 C++ 设计方案与可靠性保障 | 验证指标 |
| :--- | :--- | :--- | :--- |
| **连接生命周期** | 恶意慢速连接无限占用套接字资源 | L1（滚动空闲散列时间轮）+ L2（绝对生存期 TimerQueue）双轨超时矩阵 | 自动超时精确回收 |
| **高吞吐日志** | 同步写磁盘阻塞 I/O 事件循环 | 4MB 双缓冲异步单例日志系统，25 块背压丢弃与 64MB/跨日自动轮转 | > 1,500,000 行/秒吞吐 |
| **确定性停机** | 进程无法平滑退出或粗暴强杀丢失数据 | Linux 原生 `signalfd` 事件化接管 + 5 阶段分布式协同排空 | ≤ 2ms 极速优雅退出，退出码 0 |
| **非阻塞写保护** | 慢客户端单次未写全导致连接误杀 | 细粒度 `EPOLLOUT` 事件驱动写重试，区分非阻塞让出与致命网络故障 | 慢客户端持续大包正常传输 |
| **测试与质量** | 模块测试缺失，重构缺乏回归屏障 | 模块化 GoogleTest 框架，覆盖 base / logger / net / http / config | 50 个 CTest 用例 100% 通过 |
| **内存与资源** | 对象析构未关闭句柄或工作线程脱垂泄漏 | 严格 RAII 全生命周期管理，`join()` 回收所有线程，排空所有描述符 | ASan 内存零泄漏通过 |
| **端口安全探活** | 端口冲突或绑定失败时静默异常运行 | 暴露 `valid()` 前置探活接口，监听失败立即记录 `LOG_FATAL` 并退出 | 避免服务进入僵死假活状态 |
| **无锁并发分发** | 主线程维护集中连接表引发跨线程锁竞争 | Main Reactor 仅分发整型三元组，Sub-Reactor 100% 自治管理连接表 | Share-Nothing 零锁竞争 |
| **网络低延迟** | Nagle 算法导致小报文 40ms 延迟抖动 | 建连时显式启用 `TCP_NODELAY` 禁用 Nagle 算法 | 降低尾部延迟，消除抖动 |
| **异常自旋熔断** | `epoll_wait` 持续异常返回引发 CPU 100% 忙轮询 | 区分 `EINTR` 打断与连续 1000 次致命系统故障自杀熔断守卫 | 防止系统资源被耗尽瘫痪 |
| **统一配置管理** | 配置散落各处，裸参数与异常隐患 | `ServerConfig` 全局统一配置与 POSIX `getopt_long` 严格解析，零异常与前置校验 | 支持丰富选项，边界安全拦截 |

---

## 目录结构

```text
.
├── CMakeLists.txt              # 根构建配置
├── CMakePresets.json           # 多构建模式预设（dev / release / asan / perf）
├── .clang-format               # 统一代码格式化规范
├── .clang-tidy                 # 静态代码检查规则集
├── LICENSE                     # Apache 2.0 许可证
├── README.md                   # 项目工程说明文档
├── cmake/                      # 模块化 CMake 扩展模块
│   ├── CompilerWarnings.cmake  # 严苛编译器告警策略与 -Werror 开关
│   ├── Sanitizers.cmake        # 内存安全（ASan）与未定义行为（UBSan）检查
│   ├── Profiling.cmake         # 性能剖析与保留帧指针配置
│   └── StaticAnalyzers.cmake   # clang-tidy 与 clang-format 工具链集成
├── include/                    # 公共头文件
│   ├── base/                   # 基础设施层（时间戳、线程缓存、不可拷贝基类、异步日志）
│   │   ├── AsyncLogging.h      # 异步落盘双缓冲引擎核心
│   │   ├── CurrentThread.h     # 线程真实 TID 缓存
│   │   ├── Logger.h            # 前端日志单例门面与流式格式化
│   │   ├── NonCopyable.h       # 不可拷贝基类
│   │   └── Timestamp.h         # 微秒级高精度时间戳
│   ├── net/                    # 事件驱动网络层核心
│   │   ├── Accepter.h          # 监听套接字 RAII 封装
│   │   ├── Buffer.h            # 自动扩容应用层读写缓冲区
│   │   ├── Channel.h           # 事件通道封装（关注事件与就绪事件解耦）
│   │   ├── EpollPoller.h       # epoll I/O 多路复用器与熔断守卫
│   │   ├── EventLoop.h         # 事件循环核心（定时器驱动、无锁任务队列、Sub-Reactor 自治）
│   │   ├── EventLoopThreadPool.h # Sub-Reactor 线程池与负载均衡
│   │   ├── TcpConnection.h     # TCP 连接四态状态机与双轨超时治理
│   │   ├── TimerQueue.h        # 基于 timerfd 与红黑树的高精度定时器调度队列
│   │   └── TimerWheel.h        # 动态散列时间轮
│   ├── http/                   # HTTP 协议层
│   │   ├── HttpContext.h       # 零拷贝状态机流式 HTTP 请求解析
│   │   ├── HttpServer.h        # HTTP 服务器主体与停机协调
│   │   └── ServerConfig.h      # 全局统一配置类与 getopt_long 参数解析器
│   └── version.hpp.in          # 版本号模板（构建期自动生成）
├── src/                        # 核心库实现源码（LeanWebServer::core）
│   ├── base/                   # 基础设施实现（Timestamp.cpp, Logger.cpp, AsyncLogging.cpp）
│   ├── net/                    # 网络层实现
│   └── http/                   # HTTP 层实现（HttpContext.cpp, HttpServer.cpp, ServerConfig.cpp）
├── app/                        # 应用程序入口
│   └── main.cpp                # 14 步启动编排、早期信号屏蔽与优雅停机
└── tests/                      # 测试体系
    ├── CMakeLists.txt          # GoogleTest 自动发现构建
    ├── test_timestamp.cpp      # 基础时间戳与不可拷贝测试
    ├── test_logger.cpp         # 异步日志与性能基准测试
    ├── test_timer.cpp          # 定时器与时间轮单元测试
    ├── test_integration_timeout.cpp # 超时集成、状态机与优雅停机测试
    ├── test_server_config.cpp  # 统一配置与命令行解析单元测试
    └── scripts/                # 随仓库维护的集成验收与压测脚本
        ├── run_smoke.sh        # 功能冒烟测试脚本（HTTP/1.1、400 Bad Request、HTTP/1.0）
        ├── run_shutdown.sh     # 优雅停机耗时与退出码检验脚本
        ├── run_timeout.sh      # 空闲连接超时剔除验证脚本
        └── gen_load.cpp        # 受控高并发负载生成器
```

---

## 环境要求

| 项目 | 要求 |
| :--- | :--- |
| **操作系统** | Linux（支持 `epoll`, `timerfd`, `signalfd`, `eventfd`） |
| **C++ 编译器** | `clang++`（推荐）或 `g++`，完整支持 C++17 |
| **构建系统** | CMake ≥ 3.25，Ninja（推荐）或 Make |
| **可选依赖** | GoogleTest（未安装时构建脚本将自动经由 FetchContent 下载拉取） |

---

## 快速上手

### 1. 开发构建与运行

```bash
# 配置 Debug 开发环境（默认启用测试构建，生成 build/dev）
cmake --preset dev

# 编译所有目标
cmake --build --preset dev

# 运行服务器（基于标准 getopt 选项，如指定端口与工作线程数，支持 -h/--help 查看完整选项）
./build/dev/app/lean_web_server -p 9008 -t 15

# 客户端测试（直连本地，避免代理环境变量干扰）
curl --noproxy "*" -i http://127.0.0.1:9008/
```

### 2. 运行自动化测试套件（CTest）

测试在 `dev` 预设中开箱即用默认开启，执行以下命令运行全部 50 个单元与集成测试：

```bash
ctest --preset dev --output-on-failure
```

### 3. 内存与未定义行为安全检查（ASan + UBSan）

一键在 AddressSanitizer 与 UndefinedBehaviorSanitizer 监控下编译与执行测试，确保零内存泄漏、零越界、零非法行为：

```bash
cmake --preset asan
cmake --build --preset asan
ctest --preset asan --output-on-failure
```

### 4. 运行端到端集成测试脚本

在 `build/dev` 构建完成后，可直接执行配套的集成验收脚本：

```bash
# 1. 冒烟测试（验证 HTTP/1.1 Keep-Alive、畸形 400 校验、HTTP/1.0 短连接）
bash tests/scripts/run_smoke.sh ./build/dev/app/lean_web_server

# 2. 优雅停机测试（验证信号通知、2ms 极速停机、退出码 0 与日志排空）
bash tests/scripts/run_shutdown.sh ./build/dev/app/lean_web_server

# 3. 编译受控负载生成器并进行压力测试
clang++ -std=c++17 -O2 tests/scripts/gen_load.cpp -o tests/scripts/gen_load -lpthread
./build/dev/app/lean_web_server -p 9008 -t 4 &
SERVER_PID=$!
tests/scripts/gen_load 9008 4 2   # 4 线程压测 2 秒
kill -TERM $SERVER_PID
wait $SERVER_PID
```

### 5. 性能剖析与火焰图构建（perf 预设）

```bash
cmake --preset perf
cmake --build --preset perf

# 启动服务并采集调用栈数据
./build/perf/app/lean_web_server &
SERVER_PID=$!
perf record -F 99 -g -p $SERVER_PID -- sleep 30

# 生成火焰图（配合 FlameGraph 脚本）
perf script | ./FlameGraph/stackcollapse-perf.pl | ./FlameGraph/flamegraph.pl > flamegraph.svg
```

---

## 性能基准指标

在 Linux x86_64 测试环境下实测指标如下：

| 测试场景 | 指标项 | 测量值 | 评测结论 |
| :--- | :--- | :--- | :--- |
| **异步日志吞吐** | 4 线程并发写入 96B 日志 | **> 1,500,000 行/秒** | 零磁盘阻塞，极低锁争用 |
| **定时器吞吐** | 定时器添加与撤销操作 | **> 400,000 ops/秒** | O(log N) 高效红黑树查找与调度 |
| **HTTP 并发吞吐** | 4 线程持续并发 Keep-Alive 请求 | **> 250,000 QPS** | Sub-Reactor Share-Nothing 无锁高吞吐 |
| **优雅停机延迟** | 突发流量中触发 `SIGTERM` | **≤ 2ms** | 5 阶段分布式并行排空，极速零泄漏 |
| **内存与安全** | 长期高并发与异常请求轮番冲击 | **0 泄漏 / 0 UAF** | ASan / UBSan 100% 干净通过 |

---

## 构建选项速查

| 选项 | 默认值 | 说明 |
| :--- | :--- | :--- |
| `LEAN_WEB_SERVER_BUILD_APPS` | 顶层 `ON` | 构建可执行程序（`lean_web_server`） |
| `LEAN_WEB_SERVER_BUILD_TESTS` | 顶层 `ON`，`dev` 中 `ON` | 构建 GoogleTest 单元与集成测试套件 |
| `LEAN_WEB_SERVER_ENABLE_WARNINGS` | `ON` | 启用现代严苛编译器告警（`-Wall -Wextra ...`） |
| `LEAN_WEB_SERVER_WARNINGS_AS_ERRORS` | `OFF` | 告警视为编译错误（`-Werror`） |
| `LEAN_WEB_SERVER_ENABLE_SANITIZERS` | `OFF` | 启用 ASan（地址检查）+ UBSan（未定义行为检查） |
| `LEAN_WEB_SERVER_ENABLE_PROFILING` | `OFF` | 启用剖析优化（保留帧指针与完整符号） |
| `LEAN_WEB_SERVER_ENABLE_CLANG_TIDY` | `OFF` | 编译期执行 Clang-Tidy 静态代码分析 |
| `LEAN_WEB_SERVER_ENABLE_CCACHE` | `ON` | 启用 ccache 编译加速（若系统已安装则自动生效） |

---

## 许可证

本项目采用 [Apache 2.0 许可证](LICENSE)。
