---
name: deepseek-opt
description: "Optimization and debugging guide for ds4-server (DeepSeek V4 Flash inference engine). Covers build system, performance tuning, stats dashboard, known pitfalls, and deployment patterns."
---

# ds4-server Optimization & Debug Guide

Optimization and debugging guide for the ds4-server (DeepSeek V4 Flash inference engine).

## Build System

```sh
make              # Build ds4 CLI + ds4-server (Metal GPU backend)
make ds4          # Build CLI only
make ds4-server   # Build server only
make ds4_native   # Build CPU-only CLI (no Metal, for debug — kernel VM bug on macOS)
make test         # Build and run ds4_test --all
make clean        # Remove binaries and object files
```

The build is strict C99 with `-Wall -Wextra`. Link with: `-lm -pthread -framework Foundation -framework Metal`. No external dependencies.

### Dashboard HTML Embedding

The stats dashboard HTML/CSS/JS lives in `dashboard.html` as a standalone file. At build time, `xxd -i` converts it to a C byte array in `dashboard_html.h` (auto-generated, gitignored):

```makefile
dashboard_html.h: dashboard.html
	xxd -i $< | sed -e '1s/unsigned char/static const unsigned char/' \
	                -e '$$d' -e 's/^};$$/, 0x00/' > $@
	echo '};' >> $@
```

The `ds4_server.o` build rule depends on `dashboard_html.h`. To modify the dashboard, edit `dashboard.html` and rebuild — no C string escaping needed.

### Custom Build Architecture (ds4_server_custom.c)

The server is compiled via `ds4_server_custom.c` as the entry point (NOT `ds4_server.c` directly). This file serves as a custom overlay:

```makefile
ds4_server.o: ds4_server_custom.c ds4_server.c dashboard_html.h ds4.h rax.h
	$(CC) $(CFLAGS) -D DS4_CUSTOM_ENTRY -c -o $@ ds4_server_custom.c
```

**Architecture**:
- `ds4_server_custom.c` forward-declares custom hooks → `#include "ds4_server.c"` (upstream) → custom hook implementations
- Upstream `ds4_server.c` is compiled as part of the custom file, with guards (`#ifndef DS4_CUSTOM_ENTRY`) to skip its own forward declarations and `#include "ds4_server_custom.c"` when compiled via this path
- Tests still compile `ds4_server.c` directly (no `DS4_CUSTOM_ENTRY`), so the old unity-build path still works

**Scope**: `ds4_server_custom.c` only handles monitoring/stats hooks — it wraps upstream `ds4_server.c` without modifying it. All other files should remain untouched.

**When syncing upstream `ds4_server.c` changes, only `ds4_server_custom.c` may need updating**:

1. **Hook signatures** — check if upstream changed the 5 hook calls (`custom_kv_cache_evict_max_entries`, `custom_record_stats`, `custom_handle_route`, `custom_server_init`, `custom_server_close`). If so, update their forward declarations in `ds4_server_custom.c`.

2. **Types used by hooks** — if upstream renamed types (`server`, `job`, `kv_disk_cache`, etc.), update the `struct` tag forward declarations at the top of `ds4_server_custom.c`.

3. **Custom implementation** — if upstream behavior changed in areas your hooks touch (stats, KV cache, routing), adapt the implementations accordingly.

No other files need changes — the guards in `ds4_server.c` (`#ifndef DS4_CUSTOM_ENTRY`) and Makefile are one-time setup and don't need updating.

## CRITICAL: C String Pitfall in HTML/JS Generation (Legacy)

**This bug no longer applies to the current codebase** — the dashboard HTML was extracted to `dashboard.html` and is embedded via `xxd -i` at build time. The section below is kept for reference in case inline HTML strings reappear.

### The Bug (inline C strings only)

Adjacent C string literals that cross a boundary where JavaScript uses `+` concatenation produce broken output:

```c
// BROKEN — two C strings, the `+` ends up INSIDE the first string
"...e.g+'</td>"
"+'<td>'+(e.ttft?..."
```

C concatenates these into: `"...e.g+'</td>+'<td>'+(e.ttft?..."` where `'</td>+'` is one string and `<td>` is no longer a string — JavaScript syntax error.

### The Fix

Use `'</td><td>'` as ONE combined substring — don't split across C string boundaries:

```c
// CORRECT — single C string, no boundary issue
"...e.g+'</td><td>'+(e.ttft?..."
```

## Performance Tuning

### Reference Machine: Apple M3 Ultra (512 GB)

Production deployment specs:

| Spec | Value |
|------|-------|
| CPU | Apple M3 Ultra (32-core) |
| GPU | 80-core unified GPU |
| Memory | 512 GB unified memory |
| Memory bandwidth | ~800 GB/s |
| Platform | macOS (Darwin), Metal backend |

### Model Metrics (from startup logs)

| Metric | Value |
|--------|-------|
| Model mmap size | 80.76 GiB |
| MTP model mmap size | 3.55 GiB |
| Context buffers | 10,610 MiB (chunk=2048) / 12,318 MiB (chunk=4096) @ ctx=393,216 |
| Prefill chunk | 2,048 tokens (default), 4,096 (DS4_METAL_PREFILL_CHUNK=4096) |
| Raw KV rows (SWA) | 2,304 |
| Compressed KV rows (MLA) | 98,306 (= ctx/4 for ratio-4 layers) |
| Weight warm time | ~5.8 s |
| Metal residency setup | ~480 ms (main) + ~475 ms (MTP) |
| **Training ctx (native)** | **65,536** (from GGUF `deepseek4.context_length`) |
| **YaRN RoPE scaling** | **16×** (`deepseek4.rope.scaling.factor`), freq_base=10000 |
| **Max extended ctx** | **1,048,576** (= 64K × 16) |

### Context Usage Analysis (from production logs)

扫描 **17 个日志文件、477 次请求** 得到的真实使用分布：

| 分位 | 实际 Context | 增量 tokens/请求 | 生成 tokens/请求 |
|------|------------|----------------|----------------|
| P50 | **79,584** | **160** | **400** |
| P75 | **113,348** | — | — |
| P90 | **131,954** | — | **4,400** |
| P95 | **143,099** | — | — |
| P99 | **164,156** | — | **9,450** |
| Max | **168,267** | **72,620** | **10,850** |
| 平均 | **79,560** | **1,729** | **1,375** |

**结论**：`-c 393216` 对实际使用 (P99=164K) 有 2.3× 余量，足够 28+ 轮工具调用不截断。不需要更大的 ctx。

### Decode Speed by Mode (实测数据)

从实际日志测量，M3 Ultra 上的真实速度（TOOLS 工具调用负载）：

| Scenario | tok/s | Condition |
|----------|-------|-----------|
| THINKING, ctx=0 (no tools) | 30 t/s | best case |
| TOOLS + DSML_START, ctx=29K | 23 t/s | quality on, mtp-draft=1 |
| TOOLS + DSML_START, ctx=82K | 21 t/s | quality on, mtp-draft=1 |
| TOOLS + DSML_START, ctx=84K | **13 t/s** | quality off, mtp-draft=5 |
| TOOLS + DSML_START, ctx=97K | 24 t/s | no quality, mtp-draft=2 |
| TOOLS + DSML_START, ctx=122K | **22.3 t/s** | **no quality, no MTP, chunk=4096** |
| TOOLS + DSML_START, ctx=124K | **22.3 t/s** | **no quality, no MTP, chunk=4096** (2nd round) |

### Greedy Decode Throughput (ds4-bench, chunk=2048)

纯 greedy decode 基准测试（2025-05-11），`ds4-bench --prompt-file bench/promessi_sposi.txt --gen-tokens 128 --step-incr 2048`：

| ctx | Prefill (t/s) | Gen (t/s) |
|-----|--------------|-----------|
| 2,048 | 544.55 | 27.07 |
| 4,096 | 484.59 | 26.94 |
| 8,192 | 471.72 | 26.76 |
| 16,384 | 447.46 | 26.19 |
| 24,576 | 425.90 | 25.76 |
| 32,768 | 404.15 | 25.47 |

**说明**：
- 这是纯 GPU decode 理论上限（greedy, no HTTP, no DSML, no sampling）
- Prefill 从 545 → 404 t/s（-26%），decoder 从 27.07 → 25.47 t/s（-6%），MLA 衰减极小
- 生产环境工具调用速度（~22 t/s）比 benchmark 低 ~3-5 t/s，原因是 HTTP 流式、JSON 组包、DSML_START 检测、采样等 CPU 开销
- 设 `DS4_METAL_PREFILL_CHUNK=4096` 可将 prefill 提升 ~24%

**关键发现：**

- **MTP 无实际收益** — 带 MTP (draft=2, 97K): 24 t/s；不带 MTP (122K): 22.3 t/s。差距 ~1.7 t/s 不值得多占 3.55 GiB
- **`--quality` 在 >30K context 时无影响** — quality on/off 都约 21-22 t/s
- **MTP draft 过高在工具调用时降速** — draft=5 + DSML_START → 13 t/s
- **MLA 压缩 KV cache 消除 O(n²)** — ctx=29K 23 t/s → ctx=122K 22.3 t/s，仅 ~3% 退化
- **实测最高速 ~30 t/s**（空 context + 无工具）
- **256K+ ctx 衰减估算**: MLA O(n) attention 每步跟全量压缩行计算，推算 500K ctx → ~20 t/s，1M ctx → ~18 t/s

### Decode Stage Profile (DS4_METAL_DECODE_STAGE_PROFILE=1)

Per-layer, per-stage timing breakdown (43 layers × 15 stages = 645 dispatches/step):

| Stage | Share | Time/layer | Note |
|-------|-------|-----------|------|
| Attention (q_path + attn_output) | 42% | ~41ms | 瓶颈所在 |
| MoE (routed_moe) | 28% | ~17ms | 256 experts, 6 active + 1 shared |
| HC + Norms | 18% | ~12ms | Hyper-Connection + layer norms |
| Other (embed, lm_head, rope, etc.) | 12% | ~8ms | |

**关键发现**：
- Summed stage time ≈ 166ms/step，但 wall-clock 每 token 仅 ~42ms (24 t/s)
- 说明 Metal GPU 的 pipeline parallelism 让大部分 stage 重叠执行
- 瓶颈在 **Metal kernel dispatch 开销**（645 次 dispatch/step）而非计算本身

### 工具调用时 MTP 降速原因

DSML_START 产生结构化 JSON 输出，token 概率分布极度尖锐。MTP 快速验证器即使宽松（基于 margin threshold），也难以接受任何 draft token。`--mtp-draft=5` 时每步浪费 5 次小模型前向，实测从 21 t/s 跌至 13 t/s。

建议工具调用场景使用 `--mtp-draft 2`（容错好、开销低），或完全不用 MTP。

### Prefill Speed

实测对比 (M3 Ultra, TOOLS 负载, 122K 全量 prefill)：

| Chunk Size | 4K ctx | 65K ctx | 122K ctx | 122K avg | 总耗时 |
|-----------|--------|---------|----------|----------|-------|
| 2048 (default) | ~270 t/s | ~290 t/s | ~270 t/s | ~270 t/s | ~450s |
| 4096 (env var) | **485 t/s** | **333 t/s** | **213 t/s** | **336 t/s** | **363s** |
| 提升 | **+80%** | **+15%** | **-21%** | **+24%** | **-20%** |

关键发现：
- **小 context 提升巨大**：4K ctx 时 485 vs 270 t/s (+80%)，因为 attention mask 构造开销被均摊
- **大 context 仍有收益**：122K ctx 最终 chunk 降到 213 t/s（不如 2048 基线...），但整体平均仍快 24%
- **总耗时节省 ~20%**：122K 全量 prefill 从 ~450s 降到 363s
- **KV cache 不兼容**：切换 chunk 大小会使已有 KV cache 条目失效（graph chunk layout mismatch），适用场景是首次预填或重启后
- **增量 prefill 不受影响**：命中 KV cache 后（如多轮工具调用），增量只有几十个 token，chunk size 无差异

| Scenario | Speed | Notes |
|----------|-------|-------|
| Fresh prefill (chunk=2048) | 270-460 tok/s | First-time prompt processing, varies with ctx |
| Fresh prefill (chunk=4096) | **213-485 tok/s** | **~24% faster avg, set via DS4_METAL_PREFILL_CHUNK=4096** |
| Partial after KV cache reload | 240-350 tok/s | Disk cache load then continue |
| Small prompt (< 512 tokens) | 90-200 tok/s | Overhead dominated by graph setup |
| KV cache load from disk | 26-77 ms | For 20K-61K cached tokens |
| KV cache save to disk | 25-170 ms | For 10K-71K tokens |

## Critical: `--quality` + `--mtp` Cancel Each Other

**This is the single biggest performance issue on this deployment.**

When `--quality` is set, MTP strict verification is forced on (`ds4.c:16208`):

```c
const bool strict_mtp = e->quality || getenv("DS4_MTP_STRICT") != NULL;
```

The strict MTP path (`metal_graph_verify_decode2_exact`) verifies each draft token against an exact decode pass. The code explicitly comments:

> *"It is not a speed win — it is a correctness guarantee."*

**Effect**: The MTP draft model runs (3.55 GiB memory, extra compute per step) but provides zero speedup. Every draft output is discarded and recomputed by the exact decoder.

### MTP Recommendation (Updated)

**基于实测，MTP 不推荐用于任何场景。**

| Use case | ~tok/s | Memory | 结论 |
|----------|--------|--------|------|
| No MTP, no quality | 22.3 t/s | 0 extra | **推荐** |
| MTP draft=2, no quality | 24 t/s (97K ctx) | +3.55 GiB | 只快 1.7 t/s，不值得 |
| MTP draft=5, no quality | 13 t/s (DSML_START) | +3.55 GiB | 严重降速 |
| MTP + quality | 21 t/s | +3.55 GiB | quality 强制严格验证，零收益 |

**原因总结**：
1. 工具调用（JSON 输出）token 概率分布尖锐 → MTP draft 几乎全被拒
2. 即使 draft=2 在普通文本上有收益，3.55 GiB 换 1.7 t/s 性价比太低
3. quality 强制 strict_mtp，draft 永远被丢弃（**"It is not a speed win — it is a correctness guarantee."**）

## Critical: `-n` Default Blocks the Server

**Default**: `-n 393216` (= full context size)

Since the worker thread processes requests serially (FIFO queue), a single long generation blocks all other requests:

| Gen length | Time @ 21 tok/s |
|-----------|-----------------|
| 1,000 tokens | 48 s |
| 5,000 tokens | 4.0 min |
| 10,000 tokens | 7.9 min |
| 50,000 tokens | 40 min |

**Fix**: Set `-n 8192` or `-n 16384`. Clients can request more via API `max_tokens`.

## KV Cache Tuning

### From Log Analysis

With `--kv-cache-max-entries 10` in multi-turn tool-calling workflows (ctx grows 0 -> 73K across 30+ turns):

- Entries are evicted via LRU before they can be reused
- Many evicted entries show `hits=0` (never reused before eviction)
- Typical eviction pattern: evict ~100K+ token entry, create new ~50K entry, repeat

### Parameters

| Parameter | Default | Recommended | Reason |
|-----------|---------|-------------|--------|
| `--kv-cache-max-entries` | 10 | **25-100** | Multi-turn workflows need more slots |
| `--kv-disk-space-mb` | 4096 | 4096-16384 | 512 GB machine: disk space is not the bottleneck |

### Eviction Reasons (from logs)

- `reason=evict`: LRU eviction when max_entries exceeded
- `reason=continued`: Periodic save of the active session (at `continued_interval` = 10K tokens)
- `reason=cold`: First save of a cold prompt prefix (<= `cold_max_tokens` = 30K)

### Chunk=4096 Eviction Pattern

With chunk=4096:
- KV entries saved every **12K tokens** (3 chunks × 4096)
- Each entry ~184 MiB (12K) → ~1.6 GiB (122K)
- With max_entries=50, LRU starts evicting after accumulating ~600 MiB per saved chunk
- Typical: save 3 new entries → evict 2 oldest entries → repeat
- All evictions show `hits=0` (multi-turn conversations rarely revisit old prefixes)

### Access Quantization

Log shows `quant=2` for disk cache entries. This is controlled by the cache implementation, not a user-configurable flag.

## Stats Dashboard

### Data Flow

```
generate_job() -> stats_entry (ring buffer, 200 entries)
               -> stats_day_bucket (31 rolling days)
               -> stats.ndjson (file, appended per request)
                                        |
server restart -> stats_file_load() reads stats.ndjson -> rebuilds totals + day buckets
                                        |
/stats endpoint -> append_stats_json() -> JSON with d1/d7/d30 + ring buffer
                                        |
dashboard.html (browser) -> fetch /stats every 2s -> update UI
```

### Stat Cards

1. Requests - count (range-aware: d1/d7/d30)
2. TTFT - avg ms (range-aware)
3. Speed - tok/s (range-aware)
4. Cache Hit - % (range-aware)
5. Latency - s/req
6. Token - prompt + gen totals
7. Errors - count + rate%
8. Memory - GiB

### Range Tabs

1天/7天/30天 tabs at top. All data loaded in single `/stats` fetch, switching tabs is instant.

### Stats Persistence

- File: `stats.ndjson` in working directory
- Written: after each request (append, fflush)
- Loaded: on startup via `stats_file_load()`
- Reset: delete `stats.ndjson`
- Recovery: missing/corrupted file -> graceful zero start

## Startup Log Reference

Example startup log for reference:

```
ds4: warming mapped tensor pages: 80.76 GiB
ds4: warmed tensor pages in 5.929s (checksum=660464255)
ds4: Metal model views created in 1.615 ms, residency requested in 520.877 ms, warmup 6.553 ms
ds4: Metal backend initialized for graph diagnostics
ds4: context buffers 12317.74 MiB (ctx=393216, backend=metal, prefill_chunk=4096, raw_kv_rows=4352, compressed_kv_rows=98306)
ds4: KV disk cache ./kv-cache (budget=16384 MiB, cross-quant=accept, min=512, cold_max=30000, continued=10000, trim=32, align=2048)
ds4: listening on http://0.0.0.0:8000
```

## Common Mistakes

### 1. MTP + Quality (No Speedup)
**Symptom**: Decode speed stuck at ~22 tok/s even with MTP enabled.
**Cause**: `--quality` forces strict MTP verification, discarding draft results.
**Fix**: Use `--mtp` without `--quality` (or use `--quality` without `--mtp`).

### 2. Monotonic vs Wall Clock
**Symptom**: Day buckets (d1/d7/d30) always show 0 even though total_requests > 0.
**Cause**: Using `now_sec()` (CLOCK_MONOTONIC) for day bucket keys instead of `time(NULL)`.
**Fix**: Always use `time(NULL)` for calendar/day calculations.

### 3. Server Restart Loses Data (Now Fixed)
**Symptom**: Stats reset to zero after server restart.
**Fix**: Stats are persisted to `stats.ndjson` and loaded on startup.

### 4. Think Max Misconception
**Symptom**: Worrying that `-c 393216` forces Think Max mode and slows down inference.
**Reality**: `-c 393216` only **allows** Think Max. It is NOT forced:

```c
ds4_think_mode ds4_think_mode_for_context(ds4_think_mode mode, int ctx_size) {
    if (mode == DS4_THINK_MAX && ctx_size < DS4_THINK_MAX_MIN_CONTEXT)
        return DS4_THINK_HIGH;  // downgrade if ctx too small
    return mode;                // otherwise keep original
}
```

- **Think Max 只在客户端主动请求 reasoning_effort="max" 时触发**
- 普通对话/工具调用 `think_mode=DS4_THINK_NONE`，ctx 大小完全不参与决策
- ctx=393216 的唯一作用：允许模式从 HIGH 升级到 MAX，不是降级到 HIGH 到 MAX
- **速度也跟 ctx cap 无关** — prefill/decode 只取决于实际使用的 token 数，不是分配的上限

## Startup Configs

### Recommended: Chat / API / Tool Calling

```sh
# Via start_server.sh (推荐)
./start_server.sh
```

```sh
# 等价手动命令
DS4_METAL_PREFILL_CHUNK=4096 ./ds4-server -m ds4flash.gguf -c 393216 -n 8192 \
  --warm-weights --quality \
  --kv-disk-dir ./kv-cache --kv-disk-space-mb 16384 \
  --kv-cache-max-entries 50
```

注意：
- **Prefill chunk=4096** — 提升 ~24% 首次预填速度
- **-n 8192** — 防止单请求阻塞队列（客户端可通过 API `max_tokens` 覆盖）
- **kv-cache-max-entries 50** — 多轮工具调用需要更多 KV 槽位
- **Quality 启用** — 使用精确内核（注：>30K context 下 quality on/off 速度基本无差异）
- **ctx=393216** — 覆盖 P99=164K 使用量，2.3× 余量；只允许 Think Max 不强制开启

### Memory-Constrained (< 64 GB)

```sh
DS4_METAL_PREFILL_CHUNK=4096 ./ds4-server -m ds4flash.gguf -c 131072 -n 4096 \
  --warm-weights \
  --kv-disk-dir ./kv-cache --kv-disk-space-mb 2048 \
  --kv-cache-max-entries 25
```

## Quick Reference

```sh
# Start with optimal defaults (prefill_chunk=4096, ctx=393216, kv-cache=50, quality=on)
./start_server.sh

# Start with LAN access
./start_server.sh --host 0.0.0.0

# Custom prefill chunk (smaller mem, slower prefill)
./start_server.sh --prefill-chunk 2048

# Reset stats
rm stats.ndjson && pkill -f "ds4-server" && ./start_server.sh

# Build after changes
make ds4-server && pkill ds4-server && ./start_server.sh

# Stage profiling (per-layer decode breakdown)
DS4_METAL_DECODE_STAGE_PROFILE=1 ./ds4-server ...

# Common: check real context usage
grep "chat ctx=" logs/*.log | grep "prompt start" | sed 's/.*ctx=//' | cut -d: -f1 | sort -n | tail -5
```
