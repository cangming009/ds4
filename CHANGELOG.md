# Changelog

## 2026-05-10 合入上游代码 + 自定义功能

### 上游变更 (antirez/ds4)

#### 安全性修复
- **修复 JSON 解析器嵌套 DoS** (8e7575b) — 限制递归 JSON 跳过深度，防止恶意嵌套未使用字段耗尽服务端栈空间

#### 新功能
- **精确工具调用重放 (Exact DSML Tool Replay)** (52f8b95, d967f63) — 引入 rax（基数树）存储工具调用历史，确保重放时产生与原始请求完全相同的 DSML 字节序列，避免因 JSON 键排序导致的 token 不匹配
- **DSML 工具调用检查点恢复加固** (c184499) — 增强工具调用中断后的恢复逻辑，处理模型在 checkpoint 处重写 tool call 的边界情况
- **后端无关的 token dump 调试** (9c0e6ac) — 新增 `ds4 --dump-tokens` 命令，在 CLI 中解码并显示 token id 序列，不依赖推理后端，便于调试 tokenizer 边界问题

#### 修复
- **Metal 调试层兼容性** (f9e8715, PR #39) — 使用类型化 char 指针确保 get_rows/set_rows 只读访问权限正确，为无效的 router finalize 参数绑定占位值以通过 Metal API 验证
- **README 拼写错误修复** (8284994, PR #7)

#### 配套变更
- ds4_server.c 重构 — 新增工具调用记忆系统 (`tool_memory`)，集成 rax 库管理
- Makefile — 新增 rax 编译支持
- ds4.h — 新增 token dump 和工具记忆相关 API

### 自定义变更 (cangming009)

#### 新增文件
- **`ds4_server_custom.c`** — 自定义功能独立模块，通过 unity build (`#include`) 集成到 ds4_server.c，方便合入上游时减少冲突
- **`start_server.sh`** — 服务器启动脚本，支持硬件检测、自动参数配置、日志管理、KV 缓存选项
- **`CLAUDE.md`** — 项目说明和 AI 辅助开发指南

#### 统计面板 (Stats Dashboard)
- **实时性能监控面板** — 访问 `http://<host>:<port>/` 查看，2 秒自动刷新
  - 8 指标卡片：请求数、首字时延(TTFT)、生成速度、缓存命中率、延迟、Token 总量、错误率、显存占用
  - 1天/7天/30天 范围切换
  - **请求详情查看** — 点击表格行可查看该请求的完整精简信息（不产生额外网络请求）
- **统计持久化** — `stats.ndjson` 文件追加写入，重启后自动恢复历史数据
- 修复：仪表盘时间与日志时间不匹配的问题（使用 wall clock 而非 monotonic time）

#### KV 缓存管理
- **LRU 淘汰** — 通过 `--kv-cache-max-entries` 限制最大缓存文件数（默认 10），超过时淘汰最久未使用的缓存
- **Web UI 管理** — 仪表盘配置区显示缓存大小，支持一键在 Finder 中打开缓存目录、一键清空缓存

#### 其他改进
- 显存显示从 MiB 改为 GiB 单位（如 72.5 GiB）
- `.gitignore` 添加 `kv-cache/`、`logs/`、`stats.ndjson`
- 编译优化 — Makefile 添加 `ds4_server_custom.c` 依赖跟踪
