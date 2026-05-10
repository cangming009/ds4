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
    const char *html =
        "<!DOCTYPE html>"
        "<html lang=\"zh-CN\"><head>"
        "<meta charset=\"UTF-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>ds4-server Dashboard</title>"
        "<style>"
        "*{margin:0;padding:0;box-sizing:border-box}"
        "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#0d1117;color:#c9d1d9;padding:20px}"
        "h1{color:#58a6ff;font-size:24px;display:flex;align-items:center;gap:10px;flex-wrap:wrap}"
        ".top-bar{display:flex;align-items:center;justify-content:space-between;flex-wrap:wrap;gap:12px;margin-bottom:20px}"
        ".tabs{display:flex;gap:4px;background:#161b22;padding:3px;border-radius:6px;border:1px solid #30363d}"
        ".tabs button{background:none;border:none;color:#8b949e;padding:4px 14px;border-radius:4px;cursor:pointer;font-size:12px;font-weight:500;transition:all .15s}"
        ".tabs button.active{background:#30363d;color:#f0f6fc}"
        ".tabs button:hover{color:#f0f6fc}"
        ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:12px;margin-bottom:20px}"
        ".card{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:14px}"
        ".card h2{font-size:11px;color:#8b949e;text-transform:uppercase;letter-spacing:.5px;margin-bottom:8px}"
        ".card .val{font-size:26px;font-weight:600;color:#f0f6fc}"
        ".card .sub{font-size:11px;color:#8b949e;margin-top:4px}"
        ".card .sub .hl{color:#58a6ff}"
        ".cfg-grid{display:grid;grid-template-columns:auto 1fr;gap:4px 16px;font-size:13px}"
        ".cfg-grid dt{color:#8b949e;white-space:nowrap}"
        ".cfg-grid dd{color:#f0f6fc;word-break:break-all}"
        "table{width:100%;border-collapse:collapse;font-size:12px;font-variant-numeric:tabular-nums}"
        "th{text-align:left;padding:6px 8px;color:#8b949e;border-bottom:1px solid #30363d;font-size:10px;text-transform:uppercase;white-space:nowrap}"
        "td{padding:6px 8px;border-bottom:1px solid #21262d;white-space:nowrap}"
        "tr:hover td{background:#1c2128}"
        ".badge{display:inline-block;padding:1px 6px;border-radius:10px;font-size:10px;font-weight:600}"
        ".badge.ok{background:#3fb95022;color:#3fb950}"
        ".badge.warn{background:#d2992222;color:#d29922}"
        ".badge.err{background:#f8514922;color:#f85149}"
        ".badge.info{background:#58a6ff22;color:#58a6ff}"
        ".badge.tool{background:#bc8c2222;color:#bc8c22}"
        ".dot{display:inline-block;width:8px;height:8px;border-radius:50%;background:#3fb950;animation:pulse 2s infinite;vertical-align:middle;margin-right:8px}"
        "@keyframes pulse{0%{opacity:1}50%{opacity:.4}100%{opacity:1}}"
        ".modal{display:none;position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,.6);z-index:1000;align-items:center;justify-content:center}"
        ".modal.open{display:flex}"
        ".modal-box{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:20px;max-width:500px;width:90%;max-height:80vh;overflow-y:auto}"
        ".modal-box h3{margin-bottom:12px;color:#58a6ff}"
        ".modal-box .row{display:flex;justify-content:space-between;padding:6px 0;border-bottom:1px solid #21262d;font-size:13px}"
        ".modal-box .row .lbl{color:#8b949e}"
        ".modal-box .row .val{color:#f0f6fc;text-align:right}"
        ".modal-close{float:right;background:none;border:none;color:#8b949e;font-size:20px;cursor:pointer;padding:0 4px}"
        ".modal-close:hover{color:#f0f6fc}"
        "tr{cursor:pointer}"
        "button.as-link{background:none;border:none;color:#58a6ff;cursor:pointer;padding:0 2px;font-size:13px;text-decoration:none}"
        "button.as-link:hover{color:#79c0ff}"
        "</style></head><body>"
        "<div class=\"top-bar\">"
        "<h1><span class=\"dot\"></span>ds4-server Dashboard</h1>"
        "<div class=\"tabs\" id=\"range-tabs\">"
        "<button data-r=\"d1\" class=\"active\">1天</button>"
        "<button data-r=\"d7\">7天</button>"
        "<button data-r=\"d30\">30天</button>"
        "</div></div>"

        "<div class=\"grid\" id=\"cards\">"
        "<div class=\"card\"><h2>请求 / Requests</h2><div class=\"val\" id=\"s-req\">0</div><div class=\"sub\" id=\"s-req-sub\"></div></div>"
        "<div class=\"card\"><h2>首字时延 / TTFT</h2><div class=\"val\" id=\"s-ttft\">0</div><div class=\"sub\" id=\"s-ttft-sub\">ms</div></div>"
        "<div class=\"card\"><h2>生成速度 / Speed</h2><div class=\"val\" id=\"s-spd\">0</div><div class=\"sub\" id=\"s-spd-sub\">tok/s</div></div>"
        "<div class=\"card\"><h2>缓存命中 / Cache Hit</h2><div class=\"val\" id=\"s-hit\">0</div><div class=\"sub\" id=\"s-hit-sub\"></div></div>"
        "<div class=\"card\"><h2>延迟 / Latency</h2><div class=\"val\" id=\"s-lat\">0</div><div class=\"sub\">s / 请求</div></div>"
        "<div class=\"card\"><h2>Token</h2><div class=\"val\" id=\"s-tok\">0</div><div class=\"sub\" id=\"s-tok-d\"></div></div>"
        "<div class=\"card\"><h2>错误 / Errors</h2><div class=\"val\" id=\"s-err\">0</div><div class=\"sub\" id=\"s-err-r\"></div></div>"
        "<div class=\"card\"><h2>显存 / Memory</h2><div class=\"val\" id=\"s-mem\">0</div><div class=\"sub\">GiB</div></div>"
        "</div>"

        "<div class=\"card\" style=\"margin-bottom:20px\">"
        "<h2>配置 / Configuration</h2>"
        "<dl class=\"cfg-grid\" id=\"cfg\"></dl></div>"

        "<div class=\"card\">"
        "<h2>近期请求 / Recent Requests <span id=\"rcnt\" style=\"font-weight:400;font-size:11px\"></span></h2>"
        "<div style=\"overflow-x:auto\"><table><thead><tr>"
        "<th>时间 / Time</th><th>类型 / Type</th><th>提示 / Prompt</th><th>缓存 / Cache</th><th>生成 / Gen</th>"
        "<th>TTFT / ms</th><th>速度 / Spd</th><th>延迟 / Lat</th><th>结束 / End</th><th>标志 / Flag</th>"
        "</tr></thead><tbody id=\"rt\"></tbody></table></div></div>"

        "<!-- request detail modal -->"
        "<div class=\"modal\" id=\"detail-modal\"><div class=\"modal-box\">"
        "<button class=\"modal-close\" onclick=\"closeDetail()\">&times;</button>"
        "<h3>请求详情 / Request Detail</h3>"
        "<div id=\"detail-body\"></div>"
        "</div></div>"

        "<script>"
        "function T(t){var d=new Date(t*1e3);return ('0'+d.getHours()).slice(-2)+':'+('0'+d.getMinutes()).slice(-2)+':'+('0'+d.getSeconds()).slice(-2)}"
        "function B(t,c){return'<span class=\"badge '+c+'\">'+t+'</span>'}"
        "var R='d1',_re=[];"

        "function showDetail(i){"
        "var e=_re[i];if(!e)return;"
        "var fb=e.er?'错误 error':e.f==='stop'?'停止 stop':e.f==='length'?'超长 length':e.f==='tool_calls'?'tool':e.f;"
        "var tp=e.k===0?'聊天 / Chat':'补全 / Completion';"
        "var fl=[];if(e.tl)fl.push('tools');if(e.th)fl.push('思考 / Think');"
        "document.getElementById('detail-body').innerHTML="
        "'<div class=\"row\"><span class=\"lbl\">时间 / Time</span><span class=\"val\">'+T(e.t)+'</span></div>'"
        "+'<div class=\"row\"><span class=\"lbl\">类型 / Type</span><span class=\"val\">'+tp+'</span></div>'"
        "+'<div class=\"row\"><span class=\"lbl\">提示 Token / Prompt</span><span class=\"val\">'+e.p.toLocaleString()+'</span></div>'"
        "+'<div class=\"row\"><span class=\"lbl\">缓存 Token / Cached</span><span class=\"val\">'+e.c.toLocaleString()+'</span></div>'"
        "+'<div class=\"row\"><span class=\"lbl\">生成 Token / Gen</span><span class=\"val\">'+e.g.toLocaleString()+'</span></div>'"
        "+'<div class=\"row\"><span class=\"lbl\">首字时延 / TTFT</span><span class=\"val\">'+(e.ttft?e.ttft.toFixed(1)+' ms':'—')+'</span></div>'"
        "+'<div class=\"row\"><span class=\"lbl\">生成速度 / Speed</span><span class=\"val\">'+e.s.toFixed(1)+' tok/s</span></div>'"
        "+'<div class=\"row\"><span class=\"lbl\">总延迟 / Latency</span><span class=\"val\">'+(e.e?e.e.toFixed(2)+' s':'—')+'</span></div>'"
        "+'<div class=\"row\"><span class=\"lbl\">结束原因 / Finish</span><span class=\"val\">'+fb+'</span></div>'"
        "+(fl.length?'<div class=\"row\"><span class=\"lbl\">标志 / Flags</span><span class=\"val\">'+fl.join(', ')+'</span></div>':'');"
        "document.getElementById('detail-modal').classList.add('open');"
        "}"
        "function closeDetail(){document.getElementById('detail-modal').classList.remove('open')}"
        "async function openCache(){try{await fetch('/kv-cache/open')}catch(e){}}"
        "async function clearCache(){if(!confirm('确定清空 KV 缓存？/ Clear KV cache?'))return;"
        "try{var r=await fetch('/kv-cache/clear',{method:'POST'});"
        "if(r.ok)location.reload();else alert('Clear failed')}catch(e){alert('Clear failed')}}"

        "function updCards(r,s){"
        "document.getElementById('s-req').textContent=r.req;"
        "document.getElementById('s-ttft').textContent=r.ttft.toFixed(1);"
        "document.getElementById('s-ttft-sub').textContent=r.ttft.toFixed(1)+' ms avg | all: '+s.avg_ttft_ms.toFixed(1)+' ms';"
        "document.getElementById('s-spd').textContent=r.tps.toFixed(1);"
        "document.getElementById('s-spd-sub').textContent=r.tps.toFixed(1)+' tok/s avg | all: '+s.avg_tokens_per_sec.toFixed(1);"
        "document.getElementById('s-hit').textContent=r.hit.toFixed(1)+'%';"
        "document.getElementById('s-lat').textContent=r.lat.toFixed(2);"
        "var tt=s.total_prompt_tokens+s.total_gen_tokens;document.getElementById('s-tok').textContent=tt.toLocaleString();"
        "document.getElementById('s-tok-d').textContent='prompt: '+s.total_prompt_tokens.toLocaleString()+' | gen: '+s.total_gen_tokens.toLocaleString();"
        "document.getElementById('s-err').textContent=r.err;"
        "document.getElementById('s-err-r').textContent=r.req>0?r.err+' / '+(r.err/r.req*100).toFixed(1)+'%':'';"
        "document.getElementById('s-mem').textContent=(s.context_memory_mib/1024).toFixed(2);"
        "}"

        "function update(d){"
        "var s=d.stats,c=d.config,re=d.recent||[];"
        "var r=s[R];"
        "if(!r)return;"
        "updCards(r,s);"

        "var h='';var p=["
        "['模型 / Model',c.model_path],['上下文 / Context',c.ctx_size],['最大 Token / Max Tokens',c.max_tokens],"
        "['地址 / Host',c.host],['端口 / Port',c.port],['预热权重 / Warm Weights',c.warm_weights?'✓':'✗'],"
        "['精确内核 / Quality',c.quality?'✓':'✗'],['KV 缓存 / KV Cache',c.kv_cache?'✓ ('+c.kv_cache_entries+'/'+(c.kv_cache_max_entries||'∞')+', '+(c.kv_cache_size_mib>1024?(c.kv_cache_size_mib/1024).toFixed(1)+' GiB':c.kv_cache_size_mib.toFixed(0)+' MiB')+') <button class=as-link onclick=\"event.stopPropagation();openCache()\">📂</button> <button class=as-link onclick=\"event.stopPropagation();clearCache()\">🗑</button>':'✗'],['MTP',c.mtp?'✓':'✗']"
        "];for(var i=0;i<p.length;i++)h+='<dt>'+p[i][0]+'</dt><dd>'+p[i][1]+'</dd>';"
        "document.getElementById('cfg').innerHTML=h;"
        "document.getElementById('rcnt').textContent='('+s.ring_size+' 条记录 shown)';"

        "_re=re;var b='';for(var i=re.length-1;i>=0;i--){var e=re[i];"
        "var fb=e.er?B('错误 error','err'):e.f==='stop'?B('停止 stop','info'):e.f==='length'?B('超长 length','warn'):e.f==='tool_calls'?B('tool','tool'):B(e.f,'ok');"
        "var fl='';if(e.tl)fl+=B('tools','tool')+' ';if(e.th)fl+=B('思考 think','ok')+' ';"
        "b+='<tr onclick=\"showDetail('+i+')\"><td>'+T(e.t)+'</td><td>'+(e.k===0?'聊天 chat':'完成 cmp')+'</td><td>'+e.p+'</td><td>'+e.c+'</td><td>'+e.g+'</td><td>'+(e.ttft?e.ttft.toFixed(0):'—')+'</td><td>'+e.s.toFixed(1)+'</td><td>'+(e.e?e.e.toFixed(2):'—')+'</td><td>'+fb+'</td><td>'+fl+'</td></tr>'"
        "}"
        "document.getElementById('rt').innerHTML=b;"
        "}"

        "document.getElementById('range-tabs').addEventListener('click',function(ev){"
        "var b=ev.target.closest('button');if(!b||!b.dataset.r)return;"
        "R=b.dataset.r;"
        "document.querySelectorAll('#range-tabs button').forEach(function(x){x.classList.toggle('active',x.dataset.r===R)});"
        "});"

        "setInterval(async function(){"
        "try{var r=await fetch('/stats'),d=await r.json();"
        "if(!d.stats.d1)return;"
        "d.stats.context_memory_mib=d.config.context_memory_mib;"
        "update(d);"
        "}catch(e){}},2000)"
        "</script></body></html>";

    return http_response(fd, 200, "text/html; charset=utf-8", html);
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
