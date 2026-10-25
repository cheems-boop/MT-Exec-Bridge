# MT Exec Bridge

> **文件写入即 root 执行。**
> 把「写文件」变成「以 root 执行命令」的 KernelSU 模块 ——
> 为没有 shell 能力的 AI 助手 / MCP 文件代理设计。

`kernelsu` `module` `root` `file-protocol` `mcp` `ai-agent`

---

## 文档索引

- [CHANGELOG.md](CHANGELOG.md) —— v1 → v2 → v2.1 完整变更记录
- [docs/v1.md](docs/v1.md) —— 一代（v1.0）设计、协议与存档
- [docs/v2.md](docs/v2.md) —— 二代（v2.x）协议规范与实现细节
- [docs/ai-briefing.md](docs/ai-briefing.md) —— 「发给 AI 的一段话」：一次性告知本功能，替代常驻提示词（省 token）

---

## 这是什么

Android 上 root 后想跑命令，通常需要一个终端或 shell 接口。
但有一类调用方**只有文件读写能力**：比如通过 MCP 协议接入的 AI 助手、自动化脚本、仅开放文件 API 的远控通道。

MT Exec Bridge 把 root shell 封装成一套**纯文件协议**：

```
调用方写文件 in/job-xxx.sh   →   守护进程 execd 以 root 执行   →   结果写 out/result.txt
```

调用方不需要任何特殊权限接口，只需要「能写一个文件」，就能以 root 身份执行任意命令并取回输出。

典型场景：让 AI 助手（通过 MT 管理器 MCP 文件工具）拥有 root 命令执行能力；文件型远控；定时任务触发器。

---

## 特性

- **纯文件协议**：命令投递、结果回传、审计全部基于文件，两端解耦，任何 root 进程都能当客户端
- **任务队列**：`in/` 下任意 `*.sh` 按文件名排序依次执行，多客户端并发投递互不覆盖
- **超时看门狗**：每条任务可指定超时，到点 `kill` 整个进程组，杜绝挂起死锁（默认 600s）
- **原子发布**：命令文件读取带竞态检测；结果 `.tmp + rename` 原子替换，永远读不到半截数据
- **异步任务**：`#!async` 首行指令后台运行，不阻塞队列，日志独立文件
- **审计日志**：`out/history.log` 记录每次执行的时间/任务/来源uid/耗时/退出码/输出量/命令首行，超 4MB 自动轮转
- **看门狗自愈**：`supervisor.sh` 常驻，守护进程意外退出 2 秒自动拉起；`flock` 保证全局单实例
- **持久环境**：`in/env.sh` 每个任务执行前自动 source，可自定义环境变量；检测到 Termux 自动接入其工具链
- **资源上限**：脚本 ≤ 8MB、输出 ≤ 8MB，异常任务隔离为 `out/rejected_*`，可防日志炸盘
- **零第三方依赖**：C 二进制仅依赖 bionic libc；脚本兼容系统 sh

---

## 工作原理

```
KSU 内核 (root 授权)
 └─ ksud 开机 late_start 执行 service.sh   (uid=0, SELinux context: u:r:ksu:s0)
     └─ supervisor.sh 看门狗 (崩溃 2s 自动拉起, 常驻)
         └─ bin/execd  (flock 单实例, 200ms 轮询)
             ├─ 扫描 in/*.sh → 按文件名排序 → 逐个执行
             │    └─ fork + setpgid + /system/bin/sh -c
             │         ├─ source in/env.sh (持久环境)
             │         ├─ 超时看门狗 kill(-pid, SIGKILL)
             │         └─ 输出经管道回写 (8MB 上限)
             └─ 结果 .tmp + rename 原子发布 → out/result.txt + out/history.log
```

模块不 mount /system、不碰 Zygisk、无网络 socket —— 本质是「KSU 愿意开机以 root 跑我的 service.sh」这一件事之上的纯文件轮询。

---

## 安装

### 方式一：刷入模块 zip

1. 下载 Release 中的 `mt_exec_bridge_v2.1.zip`
2. KernelSU 管理器 → 模块 → 从存储安装
3. 重启（或按模块页「执行」按钮立即可用）

> 模块内置预编译的 `bin/execd`（aarch64），也可删除后按下文自行编译替换。

### 方式二：从源码构建

```sh
# Termux（最简）
pkg install clang
git clone <repo-url> && cd mt-exec-bridge
clang -O2 -o bin/execd execd.c
# 重新打包 zip（module.prop 必须在 zip 根目录）或直接把文件放入 /data/adb/modules/mt_exec_bridge/
```

```sh
# NDK 交叉编译
aarch64-linux-android24-clang -O2 -o bin/execd execd.c
```

---

## 前置配置（MT 管理器 MCP 服务）

本模块的「调用方」一侧通常是 MT 管理器的 MCP 文件工具（AI 助手通过它读写文件）。使用前需要两步配置：

### 1. 启动 MCP 服务

- MT 管理器 → MCP 服务设置 → 启动服务（默认监听 `127.0.0.1:8787`）
- 在 AI 客户端中添加该 MCP 服务器并完成连接（「认证」标签页按需配置）
- 若 AI 端报 `Connection refused / 8787`，即服务未启动，回 MT 打开即可

### 2. 放行 `/data` 文件权限

模块的任务通道位于 `/data/adb/modules/mt_exec_bridge/`，AI 必须能写 `in/`、读 `out/` 才能使用本桥。默认规则是**禁止访问**，需要手动放行：

1. MT 管理器 → MCP 服务设置 → **文件** 标签页
2. 点击左下角「添加规则」，添加：

   | 路径 | 类型 | 权限 |
   |---|---|---|
   | `/data` | 文件夹 | 读写 |

3. 规则自上而下匹配，**首条命中生效**，按住右侧图标可拖动排序
4. Home 目录（`/storage/emulated/0/MT2/mcp`）始终可读写，适合存放源码与构建产物

> **最小权限建议**：只放行 `/data` 即可满足本模块全部需求，其余路径保持默认禁止。
> 注意：MT 自身数据目录（如 `/data/data/bin.mt.plus`）受策略永久保护，无法放行。

---

## 使用指南

### 任务提交协议（推荐）

```text
1. 把任务写到临时文件      in/.job.tmp      （. 开头会被守护进程忽略）
2. 原子改名提交            → in/job-<时间戳>.sh
3. 轮询读取结果            out/result.txt
4. 审计追踪                out/history.log
```

> 为什么先写临时文件再 rename？`rename()` 是原子的，守护进程永远不会读到写了一半的脚本。
> 直接写 `cmd.sh` 也兼容，但存在极小概率的写入竞态。

### 任务文件格式

```sh
#!timeout=30          ← 可选首行指令：30 秒超时（默认 600，0 = 不限）
#!async               ← 可选首行指令：后台运行，日志写 out/<任务名>.log
echo "hello root"
whoami                                # → root
```

### 输出文件

| 文件 | 说明 |
|---|---|
| `out/result.txt` | 最近一次任务的 stdout+stderr 与退出码（原子发布） |
| `out/history.log` | 全部任务审计：时间/任务/uid/耗时/退出码/输出量/命令首行 |
| `out/<任务名>.log` | `#!async` 任务的独立日志 |
| `out/rejected_*` | 被拒收的异常任务（超限/竞态/空文件），供排查 |
| `out/execd.pid` | 当前守护进程 pid |

### 持久环境变量

编辑 `in/env.sh`，每个任务执行前自动 source：

```sh
# 例：把 Termux 工具链接入 PATH
export PATH=/data/data/com.termux/files/usr/bin:$PATH
export LD_LIBRARY_PATH=/data/data/com.termux/files/usr/lib
```

### 手动触发

不想常驻守护进程？在 KernelSU 管理器 → 本模块 → 点「执行」按钮（`action.sh`），会立即处理一次 `in/cmd.sh`。

---

## 安全模型（必读）

- `in/`、`out/` 权限 **0700**，仅 root 可读写；普通 App 因 DAC 与 SELinux 双重限制无法触达 `/data/adb`
- **本质是无认证 root IPC**：任何拥有 root 的进程都可投递任务。文件协议无法验证「调用者是谁」，请勿把模块目录暴露到共享存储
- 审计日志记录 `st_uid` 与命令首行，可做事后追溯
- 不用时在 KSU 中**禁用模块**即可完全切断
- 本模块与「隐藏 root」无关，它只做本机自动化执行

---

## 已知局限

- **无 TTY**：交互式程序（vim/top 等）不可用，需 tmux 等绕行
- **无流式输出**：任务完成后才能读取结果（轮询日志文件可近似实时）
- **无调用者认证**：见安全模型
- **依赖 KernelSU**：Magisk 理论兼容（同样执行 service.sh/customize.sh），但仅在 KSU 上实测
- 命令以 `sh -c` 执行，脚本中的 NUL 字节会截断（纯文本协议）

---

## 项目结构

```
├── module.prop      模块元数据
├── customize.sh     安装脚本（权限配置）
├── service.sh       开机入口：目录初始化 + env.sh 模板 + 拉起看门狗
├── supervisor.sh    看门狗：守护进程崩溃 2s 自动拉起
├── polld.sh         shell 降级方案（bin/execd 缺失时自动启用）
├── action.sh        KSU「执行」按钮：手动处理一次任务
├── execd.c          核心守护进程源码（C99，仅依赖 bionic libc）
├── bin/execd        预编译二进制（aarch64）
├── in/              任务投递目录（0700）
└── out/             结果/日志目录（0700）
```

---

## 版本历史

- **v1.0** 初版：cmd.sh 单命令轮询
- **v2.0** 十项重构：任务队列 / 动态读取+竞态检测 / 超时看门狗 / 输出上限 / 单实例锁 / 持久环境 / 异步任务 / 审计日志 / 原子发布 / 看门狗自愈
- **v2.1** 修复：管道排空改为非阻塞读，解决「静默子进程卡住排空循环导致看门狗失效」（由超时验收测试捕获）

---

> 每个版本的完整变更明细见 [CHANGELOG.md](CHANGELOG.md)。

---

## FAQ

**Q: 投递任务后 `result.txt` 没更新？**
A: 依次检查：① MT 的 MCP 服务是否已启动（AI 端连接失败即服务未运行）；② MCP 服务设置 → 文件 → 是否已放行 `/data` 读写；③ `out/execd.pid` 是否存在、用进程列表确认守护进程存活（supervisor 会在 2 秒内自动拉起）。

**Q: 和 `adb shell su -c` / Termux `su` 有什么区别？**
A: 它们需要 shell 通道；本模块只需要文件读写。专为 MCP/AI 助手这类「只有文件 API」的调用方设计。

**Q: 轮询会不会费电？**
A: 200ms 间隔的 `opendir` 扫描，CPU 占用可忽略（实测 CPU 时间分钟级不足 1s）。

**Q: 支持并发吗？**
A: 多客户端可同时投递（各自独立文件名），队列按文件名顺序执行；`result.txt` 只保留最近一次结果，需要并行结果请用 `#!async`。

**Q: 为什么用 C 而不是 shell 常驻？**
A: shell 轮询（polld.sh）作为降级保留，但 C 版有精确的进程组超时控制和原子性保证。

---

## 作者与致谢

- **GLM (Z.ai)** —— 架构设计与全部实现，包括由验收测试捕获并修复的 v2.1 看门狗缺陷
- **cheems** —— 提出需求，参与全流程实测

---

## 开源许可

[MIT](LICENSE) © 2026 GLM (Z.ai)
