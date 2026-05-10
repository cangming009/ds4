/* ds4_server_custom.c — Custom extensions for ds4-server.
 *
 * This file is #included at the end of ds4_server.c (unity build) and
 * provides the stats dashboard, KV cache management UI, and related hooks.
 *
 * By keeping all custom additions in a separate file, merging upstream
 * changes to ds4_server.c produces far fewer conflicts — the custom code
 * lives entirely here.
 *
 * ===== Hook interface (forward-declared in ds4_server.c) =====
 *   custom_kv_cache_evict_max_entries(kc)
 *   custom_record_stats(s, j, ttft_ms, t0, final_finish, cached, completion, thinking_inside)
 *   custom_handle_route(s, fd, hr)         — hr is void * (http_request *)
 *   custom_server_init(s, model_path, host, port, ctx_size, warm_weights, quality)
 *   custom_server_close(s)
 *
 * The server struct has a single opaque field: void *custom_ctx.
 * All custom state lives in this file's custom_context struct.
 * ============================================================ */

#include "dashboard_html.h"

/* ======================== Custom context ======================== */

/* Import types from ds4_server.c — available because this file is
 * #included at the end of ds4_server.c (unity build). */

/* ======================== Stats types ======================== */

#define CUSTOM_STATS_RING_SIZE 200

typedef struct {
    double timestamp;
    int prompt_tokens;
    int cached_tokens;
    int gen_tokens;
    double elapsed_sec;
    double tokens_per_sec;
    double ttft_ms;
    char finish_reason[20];
    bool has_tools;
    bool has_thinking;
    bool is_error;
    int kind; /* 0=chat, 1=completion */
} custom_stats_entry;

#define CUSTOM_STATS_DAY_BUCKETS 31

typedef struct {
    int day_id; /* time(NULL) / 86400 */
    uint64_t total_requests;
    uint64_t total_prompt_tokens;
    uint64_t total_cached_tokens;
    uint64_t total_gen_tokens;
    double total_elapsed_sec;
    double total_ttft_ms;
    uint64_t total_errors;
} custom_stats_day_bucket;

typedef struct {
    uint64_t total_requests;
    uint64_t total_prompt_tokens;
    uint64_t total_cached_tokens;
    uint64_t total_gen_tokens;
    double total_elapsed_sec;
    uint64_t total_errors;
    double total_ttft_ms;
    custom_stats_day_bucket days[CUSTOM_STATS_DAY_BUCKETS];
    int day_count;
    custom_stats_entry ring[CUSTOM_STATS_RING_SIZE];
    int ring_write;
    int ring_count;
} custom_request_stats;

static custom_stats_day_bucket *custom_stats_day_bucket_for(custom_request_stats *rs) {
    int day_id = (int)(time(NULL) / 86400);
    for (int i = 0; i < rs->day_count; i++) {
        if (rs->days[i].day_id == day_id) return &rs->days[i];
    }
    if (rs->day_count < CUSTOM_STATS_DAY_BUCKETS) {
        custom_stats_day_bucket *b = &rs->days[rs->day_count++];
        memset(b, 0, sizeof(*b));
        b->day_id = day_id;
        return b;
    }
    int oldest = 0;
    for (int i = 1; i < CUSTOM_STATS_DAY_BUCKETS; i++) {
        if (rs->days[i].day_id < rs->days[oldest].day_id) oldest = i;
    }
    memset(&rs->days[oldest], 0, sizeof(rs->days[oldest]));
    rs->days[oldest].day_id = day_id;
    return &rs->days[oldest];
}

/* ======================== Custom context ======================== */

typedef struct {
    custom_request_stats stats;
    char cfg_model_path[256];
    char cfg_host[64];
    int cfg_port;
    int cfg_ctx_size;
    bool cfg_warm_weights;
    bool cfg_quality;
    char stats_path[512];
    FILE *stats_fp;
} custom_context;

static custom_context *cctx(server *s) { return (custom_context *)s->custom_ctx; }

/* ======================== Stats file I/O ======================== */

static void custom_stats_file_append(server *s, custom_stats_entry *e) {
    custom_context *c = cctx(s);
    if (!c->stats_fp) return;
    fprintf(c->stats_fp,
        "{\"t\":%.3f,\"p\":%d,\"c\":%d,\"g\":%d,\"e\":%.3f,\"s\":%.1f,"
        "\"ttft\":%.1f,\"f\":\"%s\",\"tl\":%d,\"th\":%d,\"er\":%d,\"k\":%d}\n",
        e->timestamp, e->prompt_tokens, e->cached_tokens, e->gen_tokens,
        e->elapsed_sec, e->tokens_per_sec, e->ttft_ms,
        e->finish_reason,
        e->has_tools ? 1 : 0, e->has_thinking ? 1 : 0,
        e->is_error ? 1 : 0, e->kind);
    fflush(c->stats_fp);
}

static void custom_stats_file_load(server *s) {
    custom_context *c = cctx(s);
    FILE *fp = fopen(c->stats_path, "r");
    if (!fp) return;
    custom_request_stats *rs = &c->stats;
    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        double ts; int p = 0, c = 0, g = 0;
        double e = 0, sp = 0, ttft = 0;
        char f[20] = ""; int tl = 0, th = 0, er = 0, k = 0;
        if (sscanf(line,
            " {\"t\":%lf,\"p\":%d,\"c\":%d,\"g\":%d,\"e\":%lf,\"s\":%lf,"
            "\"ttft\":%lf,\"f\":\"%19[^\"]\",\"tl\":%d,\"th\":%d,\"er\":%d,\"k\":%d}",
            &ts, &p, &c, &g, &e, &sp, &ttft, f, &tl, &th, &er, &k) < 12) continue;
        rs->total_requests++;
        rs->total_prompt_tokens += (uint64_t)p;
        rs->total_cached_tokens += (uint64_t)c;
        rs->total_gen_tokens += (uint64_t)g;
        rs->total_elapsed_sec += e;
        rs->total_ttft_ms += ttft;
        if (er) rs->total_errors++;
        custom_stats_day_bucket *db = custom_stats_day_bucket_for(rs);
        db->total_requests++;
        db->total_prompt_tokens += (uint64_t)p;
        db->total_cached_tokens += (uint64_t)c;
        db->total_gen_tokens += (uint64_t)g;
        db->total_elapsed_sec += e;
        db->total_ttft_ms += ttft;
        if (er) db->total_errors++;
    }
    fclose(fp);
    server_log(DS4_LOG_OK, "stats: loaded %llu entries from %s",
               (unsigned long long)rs->total_requests, c->stats_path);
}

/* ======================== KV cache management ======================== */

static void kv_cache_refresh(kv_disk_cache *kc);

static void custom_kv_cache_clear_files(kv_disk_cache *kc) {
    if (!kc->enabled || !kc->dir) return;
    kv_cache_clear(kc);
    DIR *d = opendir(kc->dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        char sha[41];
        if (!sha_hex_name(de->d_name, sha)) continue;
        char *path = path_join(kc->dir, de->d_name);
        unlink(path);
        free(path);
    }
    closedir(d);
    kv_cache_refresh(kc);
}

/* ======================== Dashboard JSON builders ======================== */

static void custom_accumulate_day_buckets(custom_request_stats *rs, int days,
    double *out_req, double *out_ptok, double *out_ctok, double *out_gtok,
    double *out_sec, double *out_ttft, double *out_err)
{
    int today = (int)(time(NULL) / 86400);
    uint64_t req = 0, ptok = 0, ctok = 0, gtok = 0, err = 0;
    double sec = 0.0, ttft = 0.0;
    for (int i = 0; i < rs->day_count; i++) {
        if (rs->days[i].day_id > today || rs->days[i].day_id <= today - days) continue;
        req += rs->days[i].total_requests;
        ptok += rs->days[i].total_prompt_tokens;
        ctok += rs->days[i].total_cached_tokens;
        gtok += rs->days[i].total_gen_tokens;
        sec += rs->days[i].total_elapsed_sec;
        ttft += rs->days[i].total_ttft_ms;
        err += rs->days[i].total_errors;
    }
    *out_req = (double)req;
    *out_ptok = (double)ptok;
    *out_ctok = (double)ctok;
    *out_gtok = (double)gtok;
    *out_sec = sec;
    *out_ttft = ttft;
    *out_err = (double)err;
}

static void custom_append_stats_json(buf *b, server *s) {
    custom_context *c = cctx(s);
    custom_request_stats *rs = &c->stats;
    ds4_context_memory mem = ds4_context_memory_estimate(DS4_BACKEND_METAL, c->cfg_ctx_size);
    double avg_tps = rs->total_elapsed_sec > 0.0
        ? (double)rs->total_gen_tokens / rs->total_elapsed_sec : 0.0;
    double avg_latency = rs->total_requests > 0
        ? rs->total_elapsed_sec / (double)rs->total_requests : 0.0;
    double avg_ttft = rs->total_requests > 0
        ? rs->total_ttft_ms / (double)rs->total_requests : 0.0;
    double hit_rate = rs->total_prompt_tokens > 0
        ? (double)rs->total_cached_tokens / (double)rs->total_prompt_tokens * 100.0 : 0.0;

    double d1_req, d1_pt, d1_ct, d1_gt, d1_sec, d1_ttft, d1_err;
    double d7_req, d7_pt, d7_ct, d7_gt, d7_sec, d7_ttft, d7_err;
    double d30_req, d30_pt, d30_ct, d30_gt, d30_sec, d30_ttft, d30_err;
    custom_accumulate_day_buckets(rs, 1, &d1_req, &d1_pt, &d1_ct, &d1_gt, &d1_sec, &d1_ttft, &d1_err);
    custom_accumulate_day_buckets(rs, 7, &d7_req, &d7_pt, &d7_ct, &d7_gt, &d7_sec, &d7_ttft, &d7_err);
    custom_accumulate_day_buckets(rs, 30, &d30_req, &d30_pt, &d30_ct, &d30_gt, &d30_sec, &d30_ttft, &d30_err);

    double d1_tps = d1_sec > 0.0 ? d1_gt / d1_sec : 0.0;
    double d1_lat = d1_req > 0.0 ? d1_sec / d1_req : 0.0;
    double d1_attft = d1_req > 0.0 ? d1_ttft / d1_req : 0.0;
    double d1_hit = d1_pt > 0.0 ? d1_ct / d1_pt * 100.0 : 0.0;
    double d7_tps = d7_sec > 0.0 ? d7_gt / d7_sec : 0.0;
    double d7_lat = d7_req > 0.0 ? d7_sec / d7_req : 0.0;
    double d7_attft = d7_req > 0.0 ? d7_ttft / d7_req : 0.0;
    double d7_hit = d7_pt > 0.0 ? d7_ct / d7_pt * 100.0 : 0.0;
    double d30_tps = d30_sec > 0.0 ? d30_gt / d30_sec : 0.0;
    double d30_lat = d30_req > 0.0 ? d30_sec / d30_req : 0.0;
    double d30_attft = d30_req > 0.0 ? d30_ttft / d30_req : 0.0;
    double d30_hit = d30_pt > 0.0 ? d30_ct / d30_pt * 100.0 : 0.0;

    double kv_cache_total_mib = 0;
    if (s->kv.enabled) {
        for (int i = 0; i < s->kv.len; i++)
            kv_cache_total_mib += (double)s->kv.entry[i].file_size / (1024.0 * 1024.0);
    }

    buf_printf(b, "{"
        "\"config\":{"
            "\"model_path\":\"%s\","
            "\"mtp\":%s,"
            "\"ctx_size\":%d,"
            "\"max_tokens\":%d,"
            "\"host\":\"%s\","
            "\"port\":%d,"
            "\"warm_weights\":%s,"
            "\"quality\":%s,"
            "\"kv_cache\":%s,"
            "\"kv_cache_entries\":%d,"
            "\"kv_cache_max_entries\":%d,"
            "\"kv_cache_size_mib\":%.1f,"
            "\"kv_cache_dir\":\"%s\","
            "\"context_memory_mib\":%.0f"
        "},"
        "\"stats\":{"
            "\"total_requests\":%llu,"
            "\"total_prompt_tokens\":%llu,"
            "\"total_cached_tokens\":%llu,"
            "\"total_gen_tokens\":%llu,"
            "\"total_time_sec\":%.3f,"
            "\"avg_tokens_per_sec\":%.2f,"
            "\"avg_latency_sec\":%.3f,"
            "\"avg_ttft_ms\":%.1f,"
            "\"cache_hit_rate\":%.1f,"
            "\"total_errors\":%llu,"
            "\"ring_size\":%d,"
            "\"d1\":{\"req\":%.0f,\"tps\":%.2f,\"lat\":%.3f,\"ttft\":%.1f,\"hit\":%.1f,\"err\":%.0f},"
            "\"d7\":{\"req\":%.0f,\"tps\":%.2f,\"lat\":%.3f,\"ttft\":%.1f,\"hit\":%.1f,\"err\":%.0f},"
            "\"d30\":{\"req\":%.0f,\"tps\":%.2f,\"lat\":%.3f,\"ttft\":%.1f,\"hit\":%.1f,\"err\":%.0f}"
        "},"
        "\"recent\":[",
        c->cfg_model_path,
        ds4_engine_has_mtp(s->engine) ? "true" : "false",
        c->cfg_ctx_size,
        s->default_tokens,
        c->cfg_host,
        c->cfg_port,
        c->cfg_warm_weights ? "true" : "false",
        c->cfg_quality ? "true" : "false",
        s->kv.enabled ? "true" : "false",
        s->kv.enabled ? s->kv.len : 0,
        s->kv.enabled ? s->kv.opt.max_entries : 0,
        kv_cache_total_mib,
        s->kv.enabled && s->kv.dir ? s->kv.dir : "",
        (double)mem.total_bytes / (1024.0 * 1024.0),
        (unsigned long long)rs->total_requests,
        (unsigned long long)rs->total_prompt_tokens,
        (unsigned long long)rs->total_cached_tokens,
        (unsigned long long)rs->total_gen_tokens,
        rs->total_elapsed_sec,
        avg_tps,
        avg_latency,
        avg_ttft,
        hit_rate,
        (unsigned long long)rs->total_errors,
        rs->ring_count,
        d1_req, d1_tps, d1_lat, d1_attft, d1_hit, d1_err,
        d7_req, d7_tps, d7_lat, d7_attft, d7_hit, d7_err,
        d30_req, d30_tps, d30_lat, d30_attft, d30_hit, d30_err);

    int start = rs->ring_count < CUSTOM_STATS_RING_SIZE ? 0 : rs->ring_write;
    int count = rs->ring_count < CUSTOM_STATS_RING_SIZE ? rs->ring_count : CUSTOM_STATS_RING_SIZE;
    for (int i = 0; i < count; i++) {
        custom_stats_entry *e = &rs->ring[(start + i) % CUSTOM_STATS_RING_SIZE];
        if (i > 0) buf_putc(b, ',');
        buf_printf(b,
            "{\"t\":%.3f,\"p\":%d,\"c\":%d,\"g\":%d,\"e\":%.3f,\"s\":%.1f,"
            "\"ttft\":%.1f,\"f\":\"%s\",\"tl\":%s,\"th\":%s,\"er\":%s,\"k\":%d}",
            e->timestamp, e->prompt_tokens, e->cached_tokens, e->gen_tokens,
            e->elapsed_sec, e->tokens_per_sec, e->ttft_ms,
            e->finish_reason,
            e->has_tools ? "true" : "false",
            e->has_thinking ? "true" : "false",
            e->is_error ? "true" : "false",
            e->kind);
    }
    buf_puts(b, "]}");
}

static bool custom_send_stats_json(server *s, int fd) {
    buf b = {0};
    custom_append_stats_json(&b, s);
    buf_putc(&b, '\n');
    bool ok = http_response(fd, 200, "application/json", b.ptr);
    buf_free(&b);
    return ok;
}

static bool custom_send_dashboard(server *s, int fd) {
    (void)s;
    return http_response(fd, 200, "text/html; charset=utf-8", (const char *)dashboard_html);
}

/* ======================== Hook implementations ======================== */

/* Evict LRU entries when count exceeds max_entries */
static void custom_kv_cache_evict_max_entries(kv_disk_cache *kc) {
    if (kc->opt.max_entries <= 0) return;
    while (kc->len > kc->opt.max_entries) {
        int victim = 0;
        uint64_t oldest = kc->entry[0].last_used;
        for (int i = 1; i < kc->len; i++) {
            if (kc->entry[i].last_used < oldest) {
                oldest = kc->entry[i].last_used;
                victim = i;
            }
        }
        kv_entry e = kc->entry[victim];
        if (unlink(e.path) == 0) {
            server_log(DS4_LOG_KVCACHE,
                       "ds4-server: kv cache max entries evicted tokens=%u hits=%u size=%.2f MiB",
                       e.tokens, e.hits, (double)e.file_size / (1024.0 * 1024.0));
        }
        kv_entry_free(&e);
        memmove(kc->entry + victim, kc->entry + victim + 1,
                (size_t)(kc->len - victim - 1) * sizeof(kc->entry[0]));
        kc->len--;
    }
}

/* Record stats after a job completes */
static void custom_record_stats(server *s, job *j, double ttft_ms, double t0,
                                const char *final_finish, int cached, int completion,
                                int thinking_inside)
{
    custom_context *c = cctx(s);
    custom_request_stats *rs = &c->stats;
    custom_stats_entry *e = &rs->ring[rs->ring_write];
    e->timestamp = (double)time(NULL);
    e->prompt_tokens = j->req.prompt.len;
    e->cached_tokens = cached;
    e->gen_tokens = completion;
    e->elapsed_sec = now_sec() - t0;
    e->tokens_per_sec = e->elapsed_sec > 0.0 ? (double)completion / e->elapsed_sec : 0.0;
    e->ttft_ms = ttft_ms;
    e->has_tools = j->req.has_tools;
    e->has_thinking = thinking_inside;
    e->is_error = !strcmp(final_finish, "error");
    e->kind = (int)j->req.kind;
    snprintf(e->finish_reason, sizeof(e->finish_reason), "%s", final_finish);

    rs->total_requests++;
    rs->total_prompt_tokens += (uint64_t)j->req.prompt.len;
    rs->total_cached_tokens += (uint64_t)cached;
    rs->total_gen_tokens += (uint64_t)completion;
    rs->total_elapsed_sec += e->elapsed_sec;
    rs->total_ttft_ms += ttft_ms;
    if (e->is_error) rs->total_errors++;

    /* Day bucket */
    custom_stats_day_bucket *db = custom_stats_day_bucket_for(rs);
    db->total_requests++;
    db->total_prompt_tokens += (uint64_t)j->req.prompt.len;
    db->total_cached_tokens += (uint64_t)cached;
    db->total_gen_tokens += (uint64_t)completion;
    db->total_elapsed_sec += e->elapsed_sec;
    db->total_ttft_ms += ttft_ms;
    if (e->is_error) db->total_errors++;

    custom_stats_file_append(s, e);

    rs->ring_write = (rs->ring_write + 1) % CUSTOM_STATS_RING_SIZE;
    if (rs->ring_count < CUSTOM_STATS_RING_SIZE) rs->ring_count++;
}

/* Handle custom HTTP routes. Returns true if route was handled. */
static bool custom_handle_route(server *s, int fd, void *vhr) {
    http_request *hr = (http_request *)vhr;
    if (!strcmp(hr->method, "GET") && !strcmp(hr->path, "/stats")) {
        custom_send_stats_json(s, fd);
        return true;
    }
    if (!strcmp(hr->method, "GET") && (!strcmp(hr->path, "/") || !strcmp(hr->path, "/dashboard"))) {
        custom_send_dashboard(s, fd);
        return true;
    }
    if (!strcmp(hr->method, "GET") && !strcmp(hr->path, "/kv-cache/open")) {
        if (s->kv.enabled && s->kv.dir) {
            char cmd[4096];
            snprintf(cmd, sizeof(cmd), "open \"%s\"", s->kv.dir);
            (void)system(cmd);
            http_response(fd, 200, "text/plain", "OK");
        } else {
            http_error(fd, 400, "cache not enabled");
        }
        return true;
    }
    if (!strcmp(hr->method, "POST") && !strcmp(hr->path, "/kv-cache/clear")) {
        custom_kv_cache_clear_files(&s->kv);
        http_response(fd, 200, "text/plain", "OK");
        return true;
    }
    return false;
}

/* Initialize custom subsystems */
static void custom_server_init(server *s, const char *model_path, const char *host,
                                int port, int ctx_size, bool warm_weights, bool quality)
{
    custom_context *c = (custom_context *)xmalloc(sizeof(*c));
    memset(c, 0, sizeof(*c));
    snprintf(c->cfg_model_path, sizeof(c->cfg_model_path), "%s", model_path ? model_path : "");
    snprintf(c->cfg_host, sizeof(c->cfg_host), "%s", host ? host : "");
    c->cfg_port = port;
    c->cfg_ctx_size = ctx_size;
    c->cfg_warm_weights = warm_weights;
    c->cfg_quality = quality;
    snprintf(c->stats_path, sizeof(c->stats_path), "stats.ndjson");
    s->custom_ctx = c;
    custom_stats_file_load(s);
    c->stats_fp = fopen(c->stats_path, "a");
    if (!c->stats_fp)
        server_log(DS4_LOG_WARNING, "stats: cannot open %s for append", c->stats_path);
}

/* Clean up custom resources */
static void custom_server_close(server *s) {
    custom_context *c = cctx(s);
    if (!c) return;
    if (c->stats_fp) {
        fclose(c->stats_fp);
        c->stats_fp = NULL;
    }
    free(c);
    s->custom_ctx = NULL;
}
