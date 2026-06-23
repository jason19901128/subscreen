# Subscreen

在 **ESP32-C6-LCD-1.47** 小屏上显示 Cursor IDE Agent 工作状态与会话 Context / On-Demand 用量。

## 架构

```text
Cursor Hooks ──► Bridge (FastAPI :8765) ◄── WiFi 轮询 ── ESP32 固件 (LVGL)
                      │
                      ├── state.json（Hook 状态机）
                      ├── state.vscdb（Context / Review blocking，1s）
                      └── Cursor 账户 API（On-Demand，5min）
```

| 组件 | 职责 |
|------|------|
| **Mac Bridge** | 接收 Hooks，维护 `agent_status` / 确认态，提供 `GET /status` |
| **Cursor Hooks** | Agent 生命周期事件（`~/.cursor/hooks.json`） |
| **ESP32 固件** | 轮询 Bridge，映射为 THINKING / TOOL / CONFIRM / IDLE / OFFLINE 等 UI |

## 快速开始

### 1. 安装 Mac 端服务

```bash
chmod +x scripts/install.sh
./scripts/install.sh
```

验证：

```bash
curl http://127.0.0.1:8765/status
curl http://127.0.0.1:8765/health
```

安装内容：`~/.cursor/subscreen/bridge/`、`~/.cursor/subscreen/subscreen-update.py`、合并 `~/.cursor/hooks.json`、launchd 服务 `com.subscreen.bridge`（监听 `0.0.0.0:8765`）。

### 2. 配置 WiFi 与 Mac IP

编辑 `firmware/platformio.ini`：

```ini
-D WIFI_SSID=\"你的WiFi名\"
-D WIFI_PASS=\"你的WiFi密码\"
-D BRIDGE_HOST=\"<Mac 局域网 IP>\"
```

查看 Mac IP：`ipconfig getifaddr en0`

**Mac DHCP 换 IP 后**（副屏 OFFLINE 常见原因）：

```bash
./scripts/update-bridge-host.sh          # 只改 platformio.ini
./scripts/update-bridge-host.sh --upload # 改配置并烧录固件
```

建议在路由器为 Mac **固定 DHCP**，减少反复改 `BRIDGE_HOST`。

### 3. 编译并烧录

需要 [PlatformIO](https://platformio.org/)（pioarduino 平台，支持 ESP32-C6 + Arduino）：

```bash
pip install platformio
cd firmware
pio run -t upload
pio device monitor
```

设备端口默认为 `/dev/cu.usbmodem101`，可在 `platformio.ini` 中修改。

### 4. 使 Hooks 与 Bridge 生效

1. **重启 Cursor**（必须，否则 `hooks.json` 不加载）
2. 在 Agent Chat 中发送一条消息，观察小屏状态变化

仅更新代码、不重装时：

```bash
cp bridge/*.py ~/.cursor/subscreen/bridge/
cp hooks/subscreen-update.py ~/.cursor/subscreen/subscreen-update.py
launchctl kickstart -k "gui/$(id -u)/com.subscreen.bridge"
```

固件有改动时需重新 `pio run -t upload`。

## 屏幕显示

界面为英文。各状态通过**屏幕四周边框**（颜色 + 顶边大写状态字）与 **RGB LED** 区分；**CONFIRM** / **ERROR** 时边框与 LED **闪烁**。

### 布局

| 区域 | 内容 |
|------|------|
| **四周边框** | 8px 圆角色环（外圆角 18px）；顶边居中大写状态（如 `THINKING`、`CONFIRM`）；待确认时底边加粗至 12px |
| **状态条** | 中间彩色圆角条 + 20px 大写状态字（与边框同色，确认/错误时闪烁） |
| 中间内容 | `Cursor` 标题 + **连接圆点**、项目名、详情（忙碌时滚动）、`Model: …`（`default` 显示为 **Auto**） |
| **On-Demand** | 已用 / 上限（如 `$10.50 / $110.00`）+ 进度条；已用 ≥ 约 80% 时变红 |
| Context（底部） | 当前对话上下文 token（如 `56.2K`）+ 进度条（**200K** 满格） |

### 顶栏圆点（Bridge / Cursor）

| 颜色 | 含义 |
|------|------|
| 绿 | Bridge 可达，且 Cursor 近期有活动（`updated_at` 5 分钟内） |
| 黄 | Bridge 可达但 Cursor 空闲；或短暂重连中（保留上次状态） |
| 红 | Bridge 连续约 **5 次**不可达，或 WiFi 断开 |

### 边框颜色与 RGB LED

| 顶边状态字 | Bridge `agent_status`（典型） | 边框颜色 | RGB LED |
|--------|------------------------------|------------|---------|
| `THINKING` | `thinking`（含 `sessionStart` 会话开始） | 深绿 | 绿 |
| `TOOL` | `running_tool` | 深黄 | 黄 |
| `RUNNING` | `running` | 深绿 | 绿 |
| `CONFIRM` | `awaiting_confirm` | 橙（闪烁） | 橙（闪烁） |
| `ERROR` | `error` | 红（闪烁） | 红（闪烁） |
| `IDLE` | `idle` | 深蓝 | 蓝 |
| `OFFLINE` | `offline`（固件本地） | 灰 | 红 |

### 固件侧状态映射

从 `GET /status` JSON 推导小屏状态（`StatusClient.cpp`），按优先级：

| 优先级 | 条件 | 小屏显示 |
|--------|------|----------|
| 1 | `pending_confirm` **或** `composer_blocking_pending` **或** `agent_status == awaiting_confirm` **或** `agent_detail` 以 `Confirm` / `Review:` 开头 | **CONFIRM** |
| 2 | `agent_detail` 为 `Task aborted` / `Task completed` / `Session ended` / `Waiting for Cursor` | **IDLE** |
| 3 | `agent_turn_active` 且 `cursor_online` 且 `agent_status` 为 `idle` / `running` | **THINKING** |
| 4 | `cursor_online` 为 false 且状态为 thinking / tool / running | **IDLE** |
| 5 | 否则 | 沿用 Bridge 的 `agent_status` |
| — | HTTP 失败 / WiFi 断开 | **OFFLINE** |

Bridge 启动时会清理磁盘残留的 Review / 确认态；超过 **90 秒**未更新的 Review 元数据在快照时也会被修剪。

## 何时显示 CONFIRM

副屏 **CONFIRM**（橙色闪烁）表示：Cursor 里需要你点一下（Run、Keep、选项等）。满足下列**任一**即显示。

### 会显示 CONFIRM 的场景

| 类型 | 触发来源 | 详情示例 | 何时清除 |
|------|----------|----------|----------|
| **Shell 审批** | Hook `beforeShellExecution` | `Confirm Shell: ls -la` | `afterShellExecution` |
| **MCP 审批** | Hook `beforeMCPExecution` | `Confirm MCP: my_tool` | `afterMCPExecution` |
| **Subagent 审批** | Hook `subagentStart` | `Confirm subagent: explore` | `subagentStop` |
| **AskQuestion** | Hook `preToolUse`（`AskQuestion`） | `Confirm: 请选择…` | `postToolUse`（`AskQuestion`） |
| **SwitchMode** | Hook `preToolUse`（`SwitchMode`） | `Confirm: switch to plan` | 对应 `postToolUse` |
| **Review（Keep/Undo）** | DB `hasBlockingPendingActions=true` | `Review: Keep / Undo (README.md)` | 点 **Keep** 后 blocking 连续 2 次为 false；或下一条 prompt |

Review 条额外说明：

- 需开启 **Inline Diffs**（Settings → Agents → Applying Changes）
- **不会**仅凭改文件 Hook 就闪 CONFIRM；须 Composer 数据库确认 blocking（后台 **1s** 轮询，**连续 2 次**一致才切换；`GET /status` 也会实时读 DB 兜底）
- 详情示例：`Review: Keep / Undo (3 files)`、`Review: Keep / Undo in Cursor`

### 不会显示 CONFIRM 的场景

| 场景 | 小屏通常显示 |
|------|----------------|
| Agent 思考、处理 prompt | **THINKING** |
| 普通工具执行（Read、Grep、已批准的 Shell 等） | **TOOL** / **THINKING** |
| 改文件但 Review 条未出现 / DB 未 blocking | **THINKING** / **TOOL** |
| 任务正常结束 | **IDLE**（`Task completed`） |
| 任务中止 / 强关 Cursor | **IDLE**（`Task aborted`）；**45s** 无 Hook 也会自动 idle |
| Bridge 或 WiFi 不可达 | **OFFLINE** |

### 忙碌态与其它规则

| 规则 | 说明 |
|------|------|
| **执行中保持忙碌** | `beforeSubmitPrompt` 起 `agent_turn_active=true`，至 `stop`（无待确认）结束 |
| **`stop` 与确认** | 若仍在 Shell/MCP/Review 等待，**不会**打成 `idle`；会查 DB blocking 兜底 |
| **并行 `preToolUse`** | Shell/MCP/subagent 待确认时，其他工具的 `preToolUse` **不会**清 CONFIRM 或切成 TOOL |
| **快照校正** | `pending_confirm` 或 blocking 时强制 `agent_status=awaiting_confirm`；`Task aborted` 等终态不会被抬回 thinking |

**前置条件**：Hooks 已安装并**重启 Cursor**；Bridge 在运行（`curl http://127.0.0.1:8765/status`）。

## 熄屏 / 省电

| 措施 | 说明 |
|------|------|
| 熄屏 | 非 **Thinking / Tool / Confirm / Error** 约 **60 秒** 后关背光、关 RGB、暂停 LVGL；空闲时顶栏右侧圆环倒计时（≤10s 黄、≤5s 红） |
| 背光 | 正常约 **40%**；进出熄屏强制刷新 PWM |
| WiFi | modem sleep（`WIFI_PS_MIN_MODEM`） |
| CPU | **80MHz**；亮屏循环约 **30ms** |
| 轮询 Bridge | 忙碌 **0.5s** / 空闲 **1.5s** / 熄屏 **3s** |
| 刷新 | 状态无 UI 变化不重绘 LVGL；空闲详情不滚动 |

**唤醒**（仅以下情况，避免后台 token/用量刷新误亮屏）：

- Agent **状态**变化（如 idle → thinking）
- 进入 thinking / tool / confirm / error
- Bridge **可达性**变化
- 忙碌态下 **详情**或 Cursor 在线状态变化

唤醒会重置 60s 熄屏计时。无触摸屏。

## Context / Token（当前对话）

小屏底部 **Context** 与 Cursor 聊天一致的上下文占用；进度条按 **200K** 满格。

Bridge 每 **1 秒**从 `~/Library/Application Support/Cursor/User/globalStorage/state.vscdb` 读取活跃会话的 `contextUsagePercent`、`hasBlockingPendingActions`（`composer.composerHeaders`）。Hooks 的 `conversation_id` 用于匹配会话；`preCompact` 若带 `context_tokens` 则优先使用。

```bash
curl -X POST http://127.0.0.1:8765/refresh/context
```

## On-Demand Usage

Bridge 每 **5 分钟**拉取 Cursor 账户用量（`GetCurrentPeriodUsage`，需本机已登录）。小屏显示已用/上限；≥ 约 80% 变红；无上限显示 `Unlimited`。

```bash
curl -X POST http://127.0.0.1:8765/refresh/usage
curl http://127.0.0.1:8765/status | python3 -m json.tool
```

## Bridge `/status` 主要字段

| 字段 | 说明 |
|------|------|
| `agent_status` | `idle` / `thinking` / `running_tool` / `awaiting_confirm` / `error` / …（`sessionStart` → `thinking`） |
| `agent_detail` | 详情一行（英文） |
| `pending_confirm` | 是否待确认 |
| `composer_blocking_pending` | DB `hasBlockingPendingActions`（Review 条是否仍在，经 2 次轮询防抖） |
| `agent_turn_active` | 本轮 Agent 是否仍在执行 |
| `review_session_active` | 本轮是否有过文件类工具（仅元数据，**不单独**触发 CONFIRM） |
| `pending_review_files` | 待审查文件名（最多 8 个） |
| `model` | 模型名（`default` → 小屏 **Auto**） |
| `session_metrics.context_tokens` | 当前对话上下文 token |
| `cursor_online` | `updated_at` 是否在 5 分钟内 |
| `confirm_since` | 进入确认态的时间戳（用于过期清理） |
| `bridge_online` | 恒为 `true`（能访问 `/status` 即表示 Bridge 在线） |

## Hooks 配置

`hooks/hooks.fragment.json` 合并进 `~/.cursor/hooks.json`，统一调用：

```text
python3 ~/.cursor/subscreen/subscreen-update.py
```

Hook 命令超时 **3 秒**（`hooks.json`）；脚本请求 Bridge 超时 **2.5 秒**。

覆盖事件：`sessionStart`、`beforeSubmitPrompt`、`preToolUse`、`postToolUse`、`afterFileEdit`、`afterAgentResponse`、`afterAgentThought`、`beforeShellExecution`、`afterShellExecution`、`beforeMCPExecution`、`afterMCPExecution`、`subagentStart`、`subagentStop`、`stop`、`preCompact` 等。

## 测试脚本

```bash
# 需 Bridge 运行；依次模拟各状态
python3 scripts/test-all-status.py

# 本地单元测试 CONFIRM / Review / 忙碌态（无需硬件）
python3 scripts/test-confirm-status.py
```

检查当前 Bridge 状态：

```bash
curl -s http://127.0.0.1:8765/status | python3 -c "
import sys, json
d = json.load(sys.stdin)
print('status:', d['agent_status'])
print('confirm:', d.get('pending_confirm'), 'blocking:', d.get('composer_blocking_pending'))
print('turn:', d.get('agent_turn_active'), 'detail:', (d.get('agent_detail') or '')[:50])
"
```

## 文件结构

```
subscreen/
├── bridge/                 # FastAPI（state.py 状态机、context.py、usage.py）
├── hooks/                  # subscreen-update.py、hooks.fragment.json
├── firmware/               # PlatformIO 固件（UiScreen、StatusClient）
└── scripts/
    ├── install.sh
    ├── update-bridge-host.sh   # 自动写入 Mac IP 到 platformio.ini
    ├── test-all-status.py
    └── test-confirm-status.py
```

持久化与日志：

```text
~/.cursor/subscreen/state.json      # Bridge 状态快照
~/.cursor/subscreen/hook-debug.jsonl
~/.cursor/subscreen/bridge.log
~/.cursor/subscreen/bridge.err.log
```

## 故障排查

| 现象 | 检查 |
|------|------|
| 副屏 **OFFLINE** | Mac IP 是否变化：`ipconfig getifaddr en0` ↔ `platformio.ini` 的 `BRIDGE_HOST`；`curl http://<Mac-IP>:8765/health`；同 WiFi |
| Bridge 无响应 | `launchctl list \| grep subscreen`；`bridge.log` / `bridge.err.log` |
| 屏幕不更新 | `curl http://<Mac-IP>:8765/status`；固件是否已烧录；WiFi SSID/密码 |
| **Thinking 闪 CONFIRM** | 更新 Bridge（Review 仅 blocking 时确认）；`composer_blocking_pending` 是否抖动 |
| **CONFIRM 闪 IDLE** | 更新 Bridge + 固件；确认时 `curl` 看 `pending_confirm`、`composer_blocking_pending`、`agent_detail` |
| Cursor 里在确认但 `curl` 为 idle | 查 `hook-debug.jsonl` 是否有 `beforeShellExecution` 等；Hook 未打到 Bridge 则小屏无法 CONFIRM |
| Review 时一直 IDLE | 重启 Cursor + Bridge；开启 Inline Diffs；`composer_blocking_pending` 是否为 true |
| 执行中显示 IDLE | `agent_turn_active` 应为 true；Bridge/固件为最新 |
| **关 Cursor 仍 THINKING** | 更新 Bridge/固件；`stop aborted` 后应为 idle；或等 **45s** 无 Hook 自动 idle |
| CONFIRM 点 Keep 不消失 | 等约 2s（2 次轮询）；或发下一条消息；DB 中 blocking 是否已 false |
| 熄屏后无故亮屏 | 需含「唤醒忽略 token/用量」的固件版本 |
| 熄屏唤不醒 | Agent 进入 thinking/confirm 等；熄屏轮询约 **3s** |
| Cursor 无 Hook | `~/.cursor/hooks.json` 含 subscreen；**重启 Cursor** |
| 烧录失败 | 按住 BOOT 再 RESET 进入下载模式 |

## 手动启动 Bridge

```bash
cd ~/.cursor/subscreen/bridge
python3 main.py --host 0.0.0.0 --port 8765
```
