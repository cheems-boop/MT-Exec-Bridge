# AI 简报（发给 AI 的一段话）

> 用途：在与 AI 助手的新会话里，把下面任意一段作为**普通消息**发送一次即可，
> 让它获知本机具备 root 命令执行能力及用法。
> 刻意设计为信息式短文而非角色/指令式提示词：不必写进系统提示词或长期记忆，
> 按需发送、用完即弃，最省 token。

## 标准版（首次会话推荐）

```text
本设备已安装 KernelSU 模块 mt_exec_bridge（root 命令桥），你可通过 MT 管理器的
MCP 文件工具（已放行 /data 读写）执行任意 root 命令，方法：

1. 把 sh 命令写到 /data/adb/modules/mt_exec_bridge/in/.job.tmp（可多行）
2. 把它重命名（rename/move，不要直接创建）为
   /data/adb/modules/mt_exec_bridge/in/job-<时间戳>.sh
3. 约 1 秒后读 /data/adb/modules/mt_exec_bridge/out/result.txt
   首行 === 时间 | job: 文件名 ===，末行 === exit N | 耗时ms ===

规则：
- 任务首行可加指令：#!timeout=N（秒，默认600，0不限）、#!async（后台跑，日志在 out/<任务名>.log）
- 长任务必须加 timeout 或用 async；无 TTY，交互式程序不可用
- 大输出先 | head -c 65536 或重定向到文件再分段读
- result.txt 只保留最近一次结果：连续任务逐条提交、逐条确认
- 排障先看 out/history.log（每次执行都有审计记录）
- 环境已接入 Termux 工具链（clang/ffmpeg/python 等可直接调用），root 可读写任意目录
```

## 极简版（日常重复提醒 / token 敏感场景）

```text
本机有 root 命令桥：写 sh 到 /data/adb/modules/mt_exec_bridge/in/.job.tmp，
rename 成同目录 job-<时间戳>.sh，1 秒后读 out/result.txt（末行 exit N）。
长任务加首行 #!timeout=N；无 TTY；大输出自行截断；审计在 out/history.log。
```

## 备注

- 两段均为信息式描述，AI 读完即具备操作能力，无需额外角色设定
- 若 AI 没有 MCP 文件工具（只有文件读写），此桥不可用 —— 它本身就只是文件协议
- 协议细节见 [v2.md](v2.md)；安装与配置见 [README](../README.md)
