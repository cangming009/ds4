---
name: deepseek-opt
description: "Optimization and debugging guide for ds4-server (DeepSeek V4 Flash inference engine). Covers build system, C string pitfalls in HTML/JS generation, stats dashboard architecture, and common troubleshooting patterns."
---

# ds4-server 优化与调试指南

This skill captures debugging experience, known pitfalls, and optimization patterns for the ds4-server project.

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

## CRITICAL: C String Pitfall in HTML/JS Generation

The dashboard HTML and JavaScript are embedded in C string literals in `send_dashboard()` (ds4_server.c). This creates a persistent trap:

### The Bug

**Adjacent C string literals** that cross a boundary where JavaScript uses `+` concatenation produce broken output:

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

### Prevention Checklist

When modifying any string in `send_dashboard()` that involves the `b+=` table-row builder or any `"...""..."` adjacent C string pattern:
1. Verify every `+` between JS string literals is OUTSIDE the quotes: `'a' + 'b'` not `'a+'b'`
2. Prefer merging adjacent column strings into a single string like `'</td><td>'`
3. After editing, compile, restart the server, and visually verify the dashboard in a browser
4. A silent `catch(e){}` in the JS `setInterval` means there's a JS syntax error — check the console

## Stats Dashboard Architecture

### Data Flow

```
generate_job() → stats_entry (ring buffer, 200 entries)
               → stats_day_bucket (31 rolling days)
               → stats.ndjson (file, appended per request)
                                        ↓
server restart → stats_file_load() reads stats.ndjson → rebuilds totals + day buckets
                                        ↓
/stats endpoint → append_stats_json() → JSON with d1/d7/d30 + ring buffer
                                        ↓
send_dashboard() → HTML+JS → browser (fetches /stats every 2s)
```

### Stats File Format

`stats.ndjson` — one JSON object per line:
```
{"t":<timestamp>,"p":<prompt>,"c":<cached>,"g":<gen>,"e":<elapsed>,"s":<speed>,"ttft":<ttft>,"f":"<finish>","tl":<0/1>,"th":<0/1>,"er":<0/1>,"k":<kind>}
```

The file is appended to on every request (`fflush` after each write). On startup, `sscanf` parses each line to rebuild aggregates. The ring buffer starts empty and fills as new requests come in.

### Day Bucket Aggregation

- `stats_day_bucket_for(rs)` uses `time(NULL)` (wall clock) to compute `day_id = timestamp / 86400`
- **Important**: `now_sec()` uses `CLOCK_MONOTONIC` — do NOT use monotonic time for day bucket keys
- `accumulate_day_buckets()` sums buckets within [today-N+1, today] for d1/d7/d30
- Maximum 31 buckets stored, oldest evicted via FIFO replacement

### Key Stats Fields

| Field | Type | Description |
|-------|------|-------------|
| `total_requests` | uint64_t | Cumulative request count |
| `total_cached_tokens` | uint64_t | Tokens served from KV cache |
| `total_ttft_ms` | double | Sum of all TTFTs (for avg computation) |
| `cache_hit_rate` | double % | `cached_tokens / prompt_tokens * 100` |
| `d1/d7/d30` | object | Pre-aggregated by `accumulate_day_buckets()` |

## Dashboard Features

### Stat Cards (top grid, 8 cards)
1. 请求/Requests — count (range-aware)
2. 首字时延/TTFT — avg ms (range-aware)
3. 生成速度/Speed — tok/s (range-aware)
4. 缓存命中/Cache Hit — % (range-aware)
5. 延迟/Latency — s/req
6. Token — prompt + gen totals
7. 错误/Errors — count + rate%
8. 显存/Memory — MiB

### Range Tabs
- 1天/7天/30天 tabs in the top bar
- All data loaded in one `/stats` fetch, switching tabs is instant (no network request)
- Default: d1

### Request Table
Columns: Time, Type, Prompt, Cache, Gen, TTFT(ms), Speed, Latency, Finish, Flags
200 most recent requests in ring buffer.

## Stats Persistence

- File: `stats.ndjson` in the working directory
- Written: after each request's stats are recorded in memory (append, fflush)
- Loaded: on server start via `stats_file_load()`
- Reset: delete `stats.ndjson` to start fresh
- Recovery: if the file is missing or corrupted, stats start from zero (graceful)
- Performance: file write is < 1ms on local SSD, no impact on request latency (single-threaded worker)

## Common Mistakes

### 1. C String Concatenation in HTML/JS (REPEATED BUG)
**Symptom**: Dashboard shows no data, `setInterval try/catch` silently fails.
**Cause**: Adjacent C string literals breaking JS `+` operators (see CRITICAL section above).
**Fix**: Never split a JS `+` concatenation across C string boundaries. Use merged strings like `'</td><td>'`.

### 2. Monotonic vs Wall Clock
**Symptom**: Day buckets (d1/d7/d30) always show 0 even though total_requests > 0.
**Cause**: Using `now_sec()` (CLOCK_MONOTONIC) for day bucket keys instead of `time(NULL)`.
**Fix**: Always use `time(NULL)` for calendar/day calculations.

### 3. Server Restart Loses Data (Now Fixed)
**Symptom**: Stats reset to zero after server restart.
**Fix**: Stats are now persisted to `stats.ndjson` and loaded on startup.

### 4. Think Max Mode
- Requires ctx >= 393216
- Automatically enabled via `ds4_think_mode_for_context()`
- Uses `<think>` prefix token injection

## Quick Reference

```sh
# Start with optimal defaults
./start_server.sh

# Start with LAN access
./start_server.sh --host 0.0.0.0

# Reset stats
rm stats.ndjson && pkill ds4-server && ./start_server.sh

# Build after changes
make ds4-server && pkill ds4-server && ./start_server.sh
```
