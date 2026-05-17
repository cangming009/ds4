# ds4-server 服务监控面板设计与实现

## 1. 背景与动机

### 1.1 为什么需要监控面板

ds4-server 是一个运行在 Mac Studio M3 Ultra（512GB）上的本地推理服务，面向日常 LLM 使用。在长期运行中，以下几个问题驱动了监控面板的开发：

- **服务黑盒问题**：ds4-server 只有终端日志输出，无法直观了解服务的运行状态、吞吐量和缓存效率
- **缓存效果不可见**：KV 磁盘缓存是性能关键，但命中率、缓存大小、条目数量等指标没有可视化
- **性能监控缺失**：生成速度（tok/s）、首字时延（TTFT）、请求延迟等关键指标没有聚合统计
- **多客户端场景**：服务同时服务于 Claude Code、opencode、Pi 等多个 agent 客户端，需要一个统一的观测入口

### 1.2 设计目标

- **零外部依赖**：不使用数据库、中间件或第三方监控服务，所有数据存储在本地文件系统
- **对上游零侵入**：所有自定义代码通过 unity build 机制隔离在 `ds4_server_custom.c` 中，合并上游时不产生冲突
- **服务重启后数据不丢失**：统计信息持久化到 `stats.ndjson` 文件
- **实时性**：2 秒自动刷新，显示最近 200 条请求记录
- **轻量级**：不影响推理性能，仅记录请求完成时的元数据

## 2. 架构设计

### 2.1 整体架构

```
┌──────────────────────────────────────────────────────┐
│                    ds4-server                         │
│  ┌──────────────────────────────────────────────────┐ │
│  │              ds4_server.c (上游)                  │ │
│  │  ┌──────────────────────────────────────────┐   │ │
│  │  │  5 个 hook 调用点                         │   │ │
│  │  │  • custom_kv_cache_evict_max_entries()    │   │ │
│  │  │  • custom_record_stats()                  │   │ │
│  │  │  • custom_handle_route()                  │   │ │
│  │  │  • custom_server_init()                   │   │ │
│  │  │  • custom_server_close()                  │   │ │
│  │  └──────────────────────────────────────────┘   │ │
│  └──────────────────────────────────────────────────┘ │
│                        ↑ #include                      │
│  ┌──────────────────────────────────────────────────┐ │
│  │          ds4_server_custom.c (自定义)             │ │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────┐  │ │
│  │  │ Stats 引擎   │  │ KV 缓存管理  │  │ HTTP    │  │ │
│  │  │ • ndjson 持久 │  │ • LRU 驱逐   │  │ 路由    │  │ │
│  │  │ • 天级聚合   │  │ • 缓存清理   │  │ /stats  │  │ │
│  │  │ • 环型缓冲区 │  │ • 磁盘浏览   │  │ /dashboard│ │
│  │  └─────────────┘  └─────────────┘  └─────────┘  │ │
│  └──────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────┘
             ↑ HTTP /stats
    ┌────────────────┐
    │  Dashboard     │ (dashboard.html)
    │  • 1/7/30 天   │
    │  • 实时刷新    │
    │  • 请求详情    │
    └────────────────┘
```

### 2.2 Unity Build 模式

为了避免自定义代码与上游 `ds4_server.c` 产生合并冲突，采用 **unity build** 模式：

```c
// ds4_server_custom.c — 编译入口点
#define DS4_CUSTOM_ENTRY
#include "ds4.h"
// 自定义 hook 前向声明...
#include "ds4_server.c"  // 拉入整个上游
#include "dashboard_html.h"  // 嵌入 HTML
```

编译时，Makefile 直接编译 `ds4_server_custom.c` 而非 `ds4_server.c`：

```makefile
ds4_server.o: ds4_server_custom.c ds4_server.c dashboard_html.h ds4.h rax.h
    $(CC) $(CFLAGS) -D DS4_CUSTOM_ENTRY -c -o $@ ds4_server_custom.c
```

上游代码通过前向声明的 5 个 hook 函数调用自定义模块，测试路径（`ds4_test.o`）则直接编译 `ds4_server.c`（含 stub 实现），不拉入自定义代码。

### 2.3 Hook 调用点

上游 `ds4_server.c` 中预埋的 5 个 hook 调用点：

| Hook | 调用时机 | 作用 |
|------|----------|------|
| `custom_kv_cache_evict_max_entries()` | 每次磁盘缓存驱逐后 | 额外按 max_entries 做 LRU 限额 |
| `custom_record_stats()` | 每个请求完成时 | 记录请求统计 |
| `custom_handle_route()` | HTTP 路由匹配时 | 处理 `/stats`, `/dashboard`, `/kv-cache/*` |
| `custom_server_init()` | 服务启动时 | 初始化自定义上下文 |
| `custom_server_close()` | 服务关闭时 | 清理资源 |

### 2.4 数据存储

统计信息使用 **stats.ndjson** 文件持久化，每行一个 JSON 对象：

```json
{"t":1715846400,"p":2048,"c":0,"g":512,"e":19.5,"s":26.3,"ttft":1200.5,
 "f":"stop","tl":1,"th":1,"er":0,"k":0}
```

字段说明：
- `t` — 时间戳
- `p` — prompt tokens
- `c` — 缓存命中的 tokens
- `g` — 生成的 tokens
- `e` — 请求总耗时（秒）
- `s` — 生成速度（tok/s）
- `ttft` — 首字时延（ms）
- `f` — 结束原因（stop/length/tool_calls/error）
- `tl` — 是否包含工具调用
- `th` — 是否启用思考模式
- `er` — 是否错误
- `k` — 请求类型（0=chat, 1=completion）

服务启动时自动读取 `stats.ndjson` 恢复历史统计，然后以追加模式打开继续写入。数据保留 31 天（按天分桶聚合），重启不丢失。

### 2.5 环型缓冲区

内存中维护 200 条记录的环型缓冲区，存储最近的请求详情。支持通过前端查看每条请求的详细字段和时间序列。

### 2.6 HTTP API

| 路径 | 方法 | 返回 |
|------|------|------|
| `/` 或 `/dashboard` | GET | 监控面板 HTML |
| `/stats` | GET | 统计信息 JSON |
| `/kv-cache/open` | GET | 在 Finder 中打开 KV 缓存目录 |
| `/kv-cache/clear` | POST | 清空所有磁盘缓存 |

### 2.7 前端面板

前端为单页 HTML（约 280 行），内嵌 CSS 和 JavaScript，设计要点：

- **深色主题**，适配 Mac 使用环境
- **中英双语标签**，每项指标同时显示中文和英文
- **范围切换**：支持 1 天 / 7 天 / 30 天聚合视图
- **关键指标卡片**：请求数、TTFT、生成速度、缓存命中率、延迟、Token 总量、错误率、内存占用
- **近期请求表格**：显示最近 200 条请求，可点击查看详情
- **配置展示**：显示服务器运行配置（模型路径、上下文大小、MTP、Quality 等）
- **KV 缓存管理**：一键打开缓存目录、一键清空缓存
- **自动刷新**：2 秒轮询 `/stats` 接口

### 2.8 缓存管理增强

除了上游的 budget-based（磁盘空间限额）驱逐策略，自定义模块增加了 **max_entries-based** LRU 驱逐：

```c
static void custom_kv_cache_evict_max_entries(kv_disk_cache *kc) {
    if (kc->opt.max_entries <= 0) return;
    while (kc->len > kc->opt.max_entries) {
        // 按 last_used 驱逐最旧条目
        int victim = 0;
        uint64_t oldest = kc->entry[0].last_used;
        for (int i = 1; i < kc->len; i++) {
            if (kc->entry[i].last_used < oldest) {
                oldest = kc->entry[i].last_used;
                victim = i;
            }
        }
        // 驱逐...
    }
}
```

这解决了单个大会话撑满 max_entries 导致新会话无缓存可写的问题。配合上游最新的 `protected_sha` 保护逻辑（防止刚写入的 checkpoint 被立即驱逐），整体缓存策略更加健壮。

### 2.9 迁移到自定义模式的过程

自定义模式的实现需要以下步骤：

1. **创建 `ds4_server_custom.c`** — 作为统一编译入口，定义 hook 实现
2. **创建空 `dashboard_html.h`** — 内置 web 面板前端
3. **在上游 `ds4_server.c` 中植入 5 处 `custom_*` 函数调用** — 用于初始化、路由、统计、缓存管理、清理
4. **在上游添加空 stub 声明** — 当不使自定义模式时编译为 `static inline` 空函数
5. **修改 Makefile** — 将 `ds4-server` 的编译源从 `ds4_server.c` 改为 `ds4_server_custom.c`
6. **添加 `dashboard_html.h` 自动生成规则** — `xxd -i dashboard.html`
7. **确保测试路径不受影响** — `ds4_test.o` 直接依赖 `ds4_server.c`

## 3. 性能 Benchmark 汇总

### 3.1 量化方案对比

| 指标 | Q2 (IQ2XXS) | Q4 (custom-Q4KExperts) | Q8 (Q8-all) |
|------|-------------|----------------------|-------------|
| 文件大小 | ~87 GB | ~165 GB | ~303 GB |
| mmap 内存 | 81 GiB | 153 GiB | 282 GiB |
| 预热时间 | 28 s | 53 s | 131 s |
| 状态 | **可用** | **可用** | **不可用**（BOS 退化） |

### 3.2 Prefill 速度（tokens/s）

| Context | Q2 | Q4 | Q8 |
|---------|-------|-------|-------|
| 2,048 | 522 | 530 | 602 |
| 4,096 | 476 | 472 | 530 |
| 8,192 | 465 | 461 | 505 |
| 16,384 | 442 | 438 | 487 |
| 24,576 | 420 | 417 | 462 |
| 32,768 | 397 | 397 | 437 |

Q8 在预填阶段比 Q2/Q4 快约 **10-15%**，因为 Q8_0 的 Metal 计算内核走的是更简单的通用路径。

### 3.3 Generation 速度（tokens/s）

| Context | Q2 | Q4 | Q8 |
|---------|-------|-------|-------|
| 2,048 | 26.6 | 26.3 | 26.9 |
| 4,096 | 26.9 | 26.0 | 27.0 |
| 8,192 | 26.5 | 25.8 | 26.0 |
| 16,384 | 26.1 | 25.5 | 26.3 |
| 24,576 | 25.9 | 25.1 | 26.0 |
| 32,768 | 25.4 | 24.6 | 25.7 |

> **关键发现：** Generation 速度在三种量化间差异极小（<4%）。因为 decode 瓶颈在于 Metal kernel dispatch 开销（43 层 × 15 阶段 = 645 次 dispatch/步），而非内存带宽。M3 Ultra 的 800 GB/s 带宽足够宽，Q8 的 2x 数据量并未成为瓶颈。

### 3.4 典型负载下的速度

在实际生产负载（TOOLS + 30-120K ctx）下：

| 模式 | 速度 | 说明 |
|------|------|------|
| THINKING, ctx≈0（无工具） | ~30 t/s | 最佳场景 |
| TOOLS, ctx 30-120K | ~21-24 t/s | 典型负载 |
| Quality + MTP | **无提升** | Quality 强制严格验证，丢弃 MTP 草稿 |

### 3.5 Q8 质量退化分析

Q8-all（303 GB）模型可以完成 benchmark 但生成退化到重复循环。根因追踪过程：

```
排查路径：
1. 检查 tensor offset → blk.1.attn_norm.weight MD5 不匹配（blk.0 匹配）
   → 根因：patch_gguf_rope.py 旧版错误计算小 tensor 对齐
   → 第一个 unpadded F32 小 tensor（12 字节）被错误对齐到 32 字节
   → 后续所有 tensor offset 累积偏移了 20 字节
   
2. patch 修复后重新生成 → 依然 BOS 退化

3. 检查权重完整性 →
   - 345 个 Q8_0 非 expert 张量：Q8 vs Q4 全部 MD5 匹配 ✓
   - routed expert Q8_0 vs Q4_K 对比：MSE ~2e-6，max_diff ~0.008 ✓
   → 权重本身正确

4. 结论：Q8_0 routed expert Metal 内核 (kernel_mul_mv_id_q8_0_f32) 存在 bug
   → routed expert 计算不正确导致模型在生成首 token 后就失去连贯性
```

### 3.6 Q8_0 vs Q4_K 反向对比

在后续测试中，将 Q8_0 用于非 expert 部分、expert 保持 Q4_K 时：

| Context | Q4_K 预填 | Q8_0 预填 | Q4_K 生成 | Q8_0 生成 |
|---------|-----------|-----------|-----------|-----------|
| 2,048 | 504 | **607** | 26.3 | **27.6** |
| 4,096 | 482 | **539** | 26.8 | **27.6** |
| 8,192 | 475 | **528** | 26.6 | **27.3** |
| 16,384 | 449 | **499** | 26.1 | **26.8** |

Q8_0 不仅精度更高，预填速度快约 **15%**，生成速度快约 **4-5%**。说明 Q8_0 的 Metal 内核（非 expert 路径）针对 M3 Ultra 的带宽特性做了更好的优化。

## 4. Thinking 模式质量对比

### 4.1 封闭式推理（数学/逻辑）

| 题目 | Q2 | Q4 |
|------|------|------|
| "Alice 有几个姐妹" | 正确（4） | 正确（4） |
| "Bat and Ball"（$1.10） | 正确（$0.05） | 正确（$0.05） |
| "照片里的人"经典谜题 | 正确（his son） | 正确（his son） |

在有确定答案的逻辑/数学题上，Q2 和 Q4 **没有质量差异**。思考过程都清晰完整。

### 4.2 开放域推理（策略/教学）

以"象棋三原则"开放式策略题为例：

| 维度 | Q2 | Q4 |
|------|------|------|
| 三原则 | Activate pieces, Control center, King safety | Develop before attack, Control center, Watch opponent's threats |
| 思考聚焦度 | 基础但正确 | 更深入、更实用 |
| 输出结构 | 口语化 | 更结构化 |
| 第三原则差异 | "王的安全"（基础） | "先看对手威胁"（实用） |

> **关键发现：** Q2 和 Q4 在封闭推理上完全等价，差异只在开放域任务中显现——Q4 的思考更聚焦、输出更结构化、建议更实用。但 Q2 在答案正确性上不输 Q4，只是"润色度"差一些。

## 5. 初步结论与建议

### 5.1 量化选择建议

| 场景 | 推荐量化 | 理由 |
|------|----------|------|
| 日常使用 | **Q2 (81 GiB)** | 性价比最高，速度与 Q4 无差别，仅开放域任务有微小质量差距 |
| 追求最大化质量 | **Q4 (153 GiB)** | 开放任务更 polished，但推理正确率与 Q2 无本质差异 |
| 不推荐 | **Q8 (282 GiB)** | Metal MoE 内核 bug 导致生成退化，即使权重正确也无法正常工作 |

### 5.2 监控面板适用性

- **单机本地服务**场景下足够轻量，无额外依赖
- unity build 机制有效地隔离了自定义代码与上游，本次 main merge 的 81 个提交中只有 `ds4_server.c` 的 `kv_cache_evict` 函数产生了真正的冲突，且通过 `protected_sha` 参数平滑合并
- `stats.ndjson` 持久化方案在重启场景下表现良好
- 未来可以考虑增加 prefill 速度监控和更细粒度的阶段性能分析（需要 `DS4_METAL_DECODE_STAGE_PROFILE` 支持）

### 5.3 性能调优总结

| 参数 | 推荐值 | 效果 |
|------|--------|------|
| Prefill Chunk | 4096 | 预填速度 +24%（对比 2048） |
| Context | 393216 | 允许 Think Max，实际 P99=164K |
| Max Tokens | 8192 | 避免单请求阻塞过长 |
| KV Cache Entries | 50 | 多客户端场景足够 |
| MTP | 禁用 | 无实际收益 |
| Quality | 启用 | 保证生成正确性 |

---

*测试环境：Apple Mac Studio M3 Ultra, 512 GB, macOS*
*测试模型：DeepSeek V4 Flash (Q2 IQ2XXS / Q4 custom-Q4KExperts)*
*文档更新：2026-05-16*
