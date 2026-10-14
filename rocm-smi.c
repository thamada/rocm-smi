/*
 * rocm-smi — ROCm System Management Interface (C)
 *
 * Command-line compatible with the ROCm rocm-smi tool (CLI 4.x):
 * the same flags, the same log layout, and the same concise table.
 * Telemetry and controls go through libamd_smi, with sysfs used where
 * the library does not expose a value.
 */
#include <amd_smi/amdsmi.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define SMI_VERSION "4.0.0+c"
#define APP_WIDTH 90
#define MAX_DEV 32
#define CLOCK_JSON_VERSION 1

/* DRM_IOW('d', DRM_COMMAND_BASE + DRM_AMDGPU_INFO, 32) — wakes runtime PM. */
#define DRM_IOCTL_AMDGPU_INFO_WAKE 0x40206445u

static const char *HEADER = " ROCm System Management Interface ";
static const char *FOOTER = " End of ROCm SMI Log ";

static int g_json;
static int g_csv;
static int g_loglevel = 30; /* WARNING */
static int g_retcode;
static const char *g_autorespond;

typedef struct {
    amdsmi_processor_handle h;
    uint32_t card;
    uint32_t render;
    int have_enum;
} Dev;

static Dev g_devs[MAX_DEV];
static int g_ndev;

typedef struct {
    char key[192];
    char val[512];
} Kv;

typedef struct {
    char name[32];
    Kv *items;
    int n, cap;
} JObj;

static JObj g_jobs[MAX_DEV + 1];
static int g_njobs;

/* -------------------------------------------------------------------------- */
/* small utilities                                                            */
/* -------------------------------------------------------------------------- */

static int ok(amdsmi_status_t st) { return st == AMDSMI_STATUS_SUCCESS; }

static void die_oom(void) {
    fprintf(stderr, "ERROR: out of memory\n");
    exit(1);
}

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) die_oom();
    return p;
}

static void trim(char *s) {
    char *e;
    while (*s && isspace((unsigned char)*s)) memmove(s, s + 1, strlen(s));
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
}

static int read_file(const char *path, char *buf, size_t n) {
    FILE *f = fopen(path, "r");
    size_t r;
    if (!f) return -1;
    r = fread(buf, 1, n - 1, f);
    fclose(f);
    buf[r] = 0;
    trim(buf);
    return (int)r;
}

static int read_u64_file(const char *path, uint64_t *out) {
    char buf[128];
    char *end = NULL;
    unsigned long long v;
    if (read_file(path, buf, sizeof buf) < 0) return -1;
    errno = 0;
    v = strtoull(buf, &end, 0);
    if (errno || end == buf) return -1;
    *out = (uint64_t)v;
    return 0;
}

static int str_eq(const char *a, const char *b) {
    return a && b && strcmp(a, b) == 0;
}

static int str_ieq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

static const char *na_u16(uint16_t v, char *buf, size_t n) {
    if (v == 0xFFFF) return "N/A";
    snprintf(buf, n, "%u", v);
    return buf;
}

static const char *na_u32(uint32_t v, char *buf, size_t n) {
    if (v == 0xFFFFFFFFu) return "N/A";
    snprintf(buf, n, "%u", v);
    return buf;
}

static const char *na_u64(uint64_t v, char *buf, size_t n) {
    if (v == UINT64_MAX) return "N/A";
    snprintf(buf, n, "%" PRIu64, v);
    return buf;
}

/* -------------------------------------------------------------------------- */
/* JSON store                                                                 */
/* -------------------------------------------------------------------------- */

static JObj *jobj_get(const char *name, int create) {
    int i;
    for (i = 0; i < g_njobs; i++)
        if (strcmp(g_jobs[i].name, name) == 0) return &g_jobs[i];
    if (!create) return NULL;
    if (g_njobs >= MAX_DEV + 1) return NULL;
    memset(&g_jobs[g_njobs], 0, sizeof g_jobs[g_njobs]);
    snprintf(g_jobs[g_njobs].name, sizeof g_jobs[g_njobs].name, "%s", name);
    return &g_jobs[g_njobs++];
}

static void json_add_name(const char *name, const char *key, const char *val) {
    JObj *o = jobj_get(name, 1);
    Kv *k;
    int i;
    if (!o || !key) return;
    for (i = 0; i < o->n; i++) {
        if (strcmp(o->items[i].key, key) == 0) {
            snprintf(o->items[i].val, sizeof o->items[i].val, "%s", val ? val : "");
            return;
        }
    }
    if (o->n == o->cap) {
        o->cap = o->cap ? o->cap * 2 : 16;
        o->items = realloc(o->items, (size_t)o->cap * sizeof(Kv));
        if (!o->items) die_oom();
    }
    k = &o->items[o->n++];
    snprintf(k->key, sizeof k->key, "%s", key);
    snprintf(k->val, sizeof k->val, "%s", val ? val : "");
}

static void json_add(int dev, const char *key, const char *val) {
    char name[32];
    if (dev < 0) snprintf(name, sizeof name, "system");
    else snprintf(name, sizeof name, "card%d", dev);
    json_add_name(name, key, val);
}

static void json_escape(const char *s, char *out, size_t n) {
    size_t j = 0;
    for (; s && *s && j + 2 < n; s++) {
        if (*s == '"' || *s == '\\') {
            if (j + 3 >= n) break;
            out[j++] = '\\';
        }
        if ((unsigned char)*s < 0x20) continue;
        out[j++] = *s;
    }
    out[j] = 0;
}

static void json_dump(void) {
    int i, k, first_obj = 1;
    printf("{");
    for (i = 0; i < g_njobs; i++) {
        char ek[256], ev[640];
        if (g_jobs[i].n == 0) continue;
        if (!first_obj) printf(",");
        first_obj = 0;
        json_escape(g_jobs[i].name, ek, sizeof ek);
        printf("\"%s\": {", ek);
        for (k = 0; k < g_jobs[i].n; k++) {
            json_escape(g_jobs[i].items[k].key, ek, sizeof ek);
            json_escape(g_jobs[i].items[k].val, ev, sizeof ev);
            printf("%s\"%s\": \"%s\"", k ? ", " : "", ek, ev);
        }
        printf("}");
    }
    printf("}\n");
}

static void csv_dump(const int *devs, int nd) {
    int i, k, h;
    int sys = 0;
    JObj *system = jobj_get("system", 0);
    const char *headers[512];
    int nh = 0;

    for (i = 0; i < g_njobs; i++)
        if (strcmp(g_jobs[i].name, "system") == 0 && g_jobs[i].n) sys = 1;

    if (sys && system && nd == 0) {
        printf("name, value\n");
        for (k = 0; k < system->n; k++)
            printf("\"%s\", \"%s\"\n", system->items[k].key, system->items[k].val);
        return;
    }
    printf("device");
    for (i = 0; i < nd; i++) {
        char name[32];
        JObj *o;
        snprintf(name, sizeof name, "card%d", devs[i]);
        o = jobj_get(name, 0);
        if (!o) continue;
        for (k = 0; k < o->n; k++) {
            int seen = 0;
            for (h = 0; h < nh; h++)
                if (strcmp(headers[h], o->items[k].key) == 0) seen = 1;
            if (!seen && nh < 512) headers[nh++] = o->items[k].key;
        }
    }
    for (h = 0; h < nh; h++) printf(",%s", headers[h]);
    printf("\n");
    for (i = 0; i < nd; i++) {
        char name[32];
        JObj *o;
        snprintf(name, sizeof name, "card%d", devs[i]);
        o = jobj_get(name, 0);
        printf("card%d", devs[i]);
        for (h = 0; h < nh; h++) {
            const char *v = "N/A";
            if (o) {
                for (k = 0; k < o->n; k++)
                    if (strcmp(o->items[k].key, headers[h]) == 0) v = o->items[k].val;
            }
            printf(",");
            for (; *v; v++)
                if (*v != ',') putchar(*v);
        }
        printf("\n");
    }
}

/* -------------------------------------------------------------------------- */
/* log formatting (matches rocm_smi.py printLog / printLogSpacer)             */
/* -------------------------------------------------------------------------- */

static void print_spacer(const char *display, const char *fill, int fit) {
    int width = fit > 0 ? fit : APP_WIDTH;
    int i, pad, dlen;
    char shown[256];
    if (g_json) return;
    if (width % 2) width++;
    if (!fill) fill = "=";
    if (display && display[0]) {
        snprintf(shown, sizeof shown, "%s", display);
        dlen = (int)strlen(shown);
        if (dlen % 2) {
            if (dlen + 1 < (int)sizeof shown) {
                shown[dlen] = fill[0];
                shown[dlen + 1] = 0;
                dlen++;
            }
        }
        pad = (width - dlen) / 2;
        if (pad < 0) pad = 0;
        for (i = 0; i < pad; i++) putchar(fill[0]);
        fputs(shown, stdout);
        for (i = 0; i < width - dlen - pad; i++) putchar(fill[0]);
        putchar('\n');
    } else {
        for (i = 0; i < width; i++) putchar(fill[0]);
        putchar('\n');
    }
}

static void print_log(int dev, const char *metric, const char *value) {
    int has_val = value && value[0];
    if (g_json) {
        if (metric && has_val) json_add(dev, metric, value);
        return;
    }
    if (dev < 0) {
        if (metric && has_val) printf("%s: %s\n", metric, value);
        else printf("%s\n", metric ? metric : (value ? value : ""));
        return;
    }
    if (!metric)
        printf("GPU[%d]\t\t: %s\n", dev, value ? value : "");
    else if (has_val)
        printf("GPU[%d]\t\t: %s: %s\n", dev, metric, value);
    else
        printf("GPU[%d]\t\t: %s\n", dev, metric);
    fflush(stdout);
}

static void print_logf(int dev, const char *metric, const char *fmt, ...) {
    char buf[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    print_log(dev, metric, buf);
}

static void log_err(const char *fmt, ...) {
    va_list ap;
    if (g_loglevel > 40) return;
    fputs("ERROR: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void log_warn(const char *fmt, ...) {
    va_list ap;
    if (g_loglevel > 30) return;
    fputs("WARNING: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* -------------------------------------------------------------------------- */
/* device discovery                                                           */
/* -------------------------------------------------------------------------- */

static int driver_live(void) {
    char buf[64];
    if (access("/sys/module/amdgpu", F_OK) != 0) return 0;
    if (access("/sys/module/amdgpu/initstate", F_OK) != 0) return 1;
    if (read_file("/sys/module/amdgpu/initstate", buf, sizeof buf) < 0) return 0;
    return strstr(buf, "live") != NULL;
}

static void wake_card(uint32_t card, uint32_t render) {
    char path[64];
    int fd;
    unsigned char req[32];
    if (render) snprintf(path, sizeof path, "/dev/dri/renderD%u", render);
    else snprintf(path, sizeof path, "/dev/dri/card%u", card);
    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) return;
    memset(req, 0, sizeof req);
    ioctl(fd, DRM_IOCTL_AMDGPU_INFO_WAKE, req);
    close(fd);
}

static int discover(void) {
    uint32_t sc = 0, i, j;
    amdsmi_socket_handle *socks;
    amdsmi_status_t st;

    st = amdsmi_init(AMDSMI_INIT_AMD_GPUS);
    if (!ok(st)) st = amdsmi_init(AMDSMI_INIT_AMD_APUS);
    if (!ok(st)) return -1;

    if (!ok(amdsmi_get_socket_handles(&sc, NULL)) || sc == 0) {
        /* single-socket / host path: ask by type with a NULL socket */
        uint32_t pc = MAX_DEV;
        amdsmi_processor_handle hs[MAX_DEV];
        if (ok(amdsmi_get_processor_handles_by_type(NULL, AMDSMI_PROCESSOR_TYPE_AMD_GPU, hs, &pc))) {
            for (i = 0; i < pc && g_ndev < MAX_DEV; i++) {
                g_devs[g_ndev].h = hs[i];
                g_ndev++;
            }
        }
    } else {
        socks = xmalloc(sc * sizeof *socks);
        if (!ok(amdsmi_get_socket_handles(&sc, socks))) {
            free(socks);
            return g_ndev ? 0 : -1;
        }
        for (i = 0; i < sc; i++) {
            uint32_t pc = 0;
            amdsmi_processor_handle *hs;
            if (!ok(amdsmi_get_processor_handles(socks[i], &pc, NULL)) || pc == 0) continue;
            hs = xmalloc(pc * sizeof *hs);
            if (!ok(amdsmi_get_processor_handles(socks[i], &pc, hs))) {
                free(hs);
                continue;
            }
            for (j = 0; j < pc && g_ndev < MAX_DEV; j++) {
                amdsmi_processor_type_t t = AMDSMI_PROCESSOR_TYPE_UNKNOWN;
                amdsmi_get_processor_type(hs[j], &t);
                if (t != AMDSMI_PROCESSOR_TYPE_AMD_GPU && t != AMDSMI_PROCESSOR_TYPE_AMD_APU &&
                    t != AMDSMI_PROCESSOR_TYPE_UNKNOWN)
                    continue;
                g_devs[g_ndev].h = hs[j];
                g_ndev++;
            }
            free(hs);
        }
        free(socks);
    }

    for (i = 0; i < (uint32_t)g_ndev; i++) {
        amdsmi_enumeration_info_t en;
        memset(&en, 0, sizeof en);
        if (ok(amdsmi_get_gpu_enumeration_info(g_devs[i].h, &en))) {
            g_devs[i].card = en.drm_card;
            g_devs[i].render = en.drm_render;
            g_devs[i].have_enum = 1;
            wake_card(en.drm_card, en.drm_render);
        }
    }
    return g_ndev > 0 ? 0 : -1;
}

static Dev *D(int id) {
    if (id < 0 || id >= g_ndev) return NULL;
    return &g_devs[id];
}

/* -------------------------------------------------------------------------- */
/* queries                                                                    */
/* -------------------------------------------------------------------------- */

static const char *perf_name(amdsmi_dev_perf_level_t p) {
    switch (p) {
    case AMDSMI_DEV_PERF_LEVEL_AUTO: return "auto";
    case AMDSMI_DEV_PERF_LEVEL_LOW: return "low";
    case AMDSMI_DEV_PERF_LEVEL_HIGH: return "high";
    case AMDSMI_DEV_PERF_LEVEL_MANUAL: return "manual";
    case AMDSMI_DEV_PERF_LEVEL_STABLE_STD: return "stable_std";
    case AMDSMI_DEV_PERF_LEVEL_STABLE_PEAK: return "stable_peak";
    case AMDSMI_DEV_PERF_LEVEL_STABLE_MIN_MCLK: return "stable_min_mclk";
    case AMDSMI_DEV_PERF_LEVEL_STABLE_MIN_SCLK: return "stable_min_sclk";
    case AMDSMI_DEV_PERF_LEVEL_DETERMINISM: return "determinism";
    default: return NULL;
    }
}

static int perf_from_name(const char *s, amdsmi_dev_perf_level_t *out) {
    static const struct { const char *n; amdsmi_dev_perf_level_t v; } t[] = {
        {"auto", AMDSMI_DEV_PERF_LEVEL_AUTO},
        {"low", AMDSMI_DEV_PERF_LEVEL_LOW},
        {"high", AMDSMI_DEV_PERF_LEVEL_HIGH},
        {"manual", AMDSMI_DEV_PERF_LEVEL_MANUAL},
        {"stable_std", AMDSMI_DEV_PERF_LEVEL_STABLE_STD},
        {"stable_peak", AMDSMI_DEV_PERF_LEVEL_STABLE_PEAK},
        {"stable_min_mclk", AMDSMI_DEV_PERF_LEVEL_STABLE_MIN_MCLK},
        {"stable_min_sclk", AMDSMI_DEV_PERF_LEVEL_STABLE_MIN_SCLK},
        {"determinism", AMDSMI_DEV_PERF_LEVEL_DETERMINISM},
        {NULL, 0}
    };
    int i;
    for (i = 0; t[i].n; i++)
        if (str_ieq(s, t[i].n)) { *out = t[i].v; return 0; }
    return -1;
}

struct clkmap { const char *name; amdsmi_clk_type_t type; };
static const struct clkmap CLKS[] = {
    {"dcef", AMDSMI_CLK_TYPE_DCEF},
    {"dclk0", AMDSMI_CLK_TYPE_DCLK0},
    {"dclk1", AMDSMI_CLK_TYPE_DCLK1},
    {"fclk", AMDSMI_CLK_TYPE_DF},
    {"mclk", AMDSMI_CLK_TYPE_MEM},
    {"pcie", AMDSMI_CLK_TYPE_PCIE},
    {"sclk", AMDSMI_CLK_TYPE_SYS},
    {"socclk", AMDSMI_CLK_TYPE_SOC},
    {"vclk0", AMDSMI_CLK_TYPE_VCLK0},
    {"vclk1", AMDSMI_CLK_TYPE_VCLK1},
};
static const int NCLKS = (int)(sizeof CLKS / sizeof CLKS[0]);

static int clk_lookup(const char *name, amdsmi_clk_type_t *out) {
    int i;
    for (i = 0; i < NCLKS; i++)
        if (str_ieq(name, CLKS[i].name) ||
            (str_ieq(name, "soc") && CLKS[i].type == AMDSMI_CLK_TYPE_SOC) ||
            (str_ieq(name, "df") && CLKS[i].type == AMDSMI_CLK_TYPE_DF)) {
            *out = CLKS[i].type;
            return 0;
        }
    return -1;
}

static double temp_c(int dev, amdsmi_temperature_type_t t, int *okf) {
    int64_t v = 0;
    if (!ok(amdsmi_get_temp_metric(D(dev)->h, t, AMDSMI_TEMP_CURRENT, &v))) {
        if (okf) *okf = 0;
        return 0;
    }
    if (okf) *okf = 1;
    if (v > 1000 || v < -1000) return (double)v / 1000.0;
    return (double)v;
}

static int current_clk_mhz(int dev, amdsmi_clk_type_t t, double *mhz, int *level, int *deep) {
    amdsmi_frequencies_t f;
    memset(&f, 0, sizeof f);
    if (!ok(amdsmi_get_clk_freq(D(dev)->h, t, &f))) return -1;
    if (f.num_supported == 0 || f.current >= f.num_supported) return -1;
    *mhz = (double)f.frequency[f.current] / 1e6;
    *level = (int)f.current;
    *deep = f.has_deep_sleep ? 1 : 0;
    return 0;
}

static void fmt_level(int level, int deep, char *buf, size_t n) {
    if (deep && level == 0) snprintf(buf, n, "S");
    else if (deep) snprintf(buf, n, "%d", level - 1);
    else snprintf(buf, n, "%d", level);
}

/* Python str(float): 22 -> "22.0", 22.5 -> "22.5". */
static void fmt_py_num(char *buf, size_t n, double v) {
    char tmp[64];
    char *dot, *e;
    snprintf(tmp, sizeof tmp, "%.10f", v);
    dot = strchr(tmp, '.');
    if (dot) {
        e = tmp + strlen(tmp) - 1;
        while (e > dot && *e == '0') *e-- = 0;
        if (e == dot) {
            e[1] = '0';
            e[2] = 0;
        }
    }
    snprintf(buf, n, "%s", tmp);
}

static int util_value_ok(uint64_t v) {
    return v != UINT64_MAX && v != 0xFFFFFFFFu;
}

static int mem_busy_pct(int dev, uint32_t *pct) {
    char path[128];
    uint64_t v;
    snprintf(path, sizeof path, "/sys/class/drm/card%u/device/mem_busy_percent", D(dev)->card);
    if (read_u64_file(path, &v) != 0) return -1;
    *pct = (uint32_t)v;
    return 0;
}

static int power_watts(int dev, double *w, const char **ptype, const char **label) {
    amdsmi_power_info_t p;
    memset(&p, 0, sizeof p);
    *w = 0;
    *ptype = "INVALID_POWER_TYPE";
    *label = "(Avg)";
    if (!ok(amdsmi_get_power_info(D(dev)->h, &p))) return -1;
    if (p.average_socket_power != 0xFFFFFFFFu) {
        *w = (double)p.average_socket_power;
        *ptype = "AVERAGE";
        *label = "(Avg)";
        return 0;
    }
    if (p.current_socket_power != 0xFFFFFFFFu) {
        *w = (double)p.current_socket_power;
        *ptype = "CURRENT SOCKET";
        *label = "(Socket)";
        return 0;
    }
    if (p.socket_power != UINT64_MAX && p.socket_power != 0) {
        if (p.socket_power > 100000) *w = (double)p.socket_power / 1e6;
        else *w = (double)p.socket_power;
        *ptype = "AVERAGE";
        *label = "(Avg)";
        return 0;
    }
    return -1;
}

static int fan_level_pct(int dev, int64_t *level, int *pct, int64_t *rpm) {
    int64_t lv = 0, rp = 0;
    uint64_t mx = 0;
    amdsmi_status_t st = amdsmi_get_gpu_fan_speed(D(dev)->h, 0, &lv);
    if (!ok(st)) return (int)st;
    if (!ok(amdsmi_get_gpu_fan_speed_max(D(dev)->h, 0, &mx)) || mx == 0) mx = 255;
    *level = lv;
    *pct = (int)((lv * 100 + (int64_t)mx / 2) / (int64_t)mx);
    if (ok(amdsmi_get_gpu_fan_rpms(D(dev)->h, 0, &rp))) *rpm = rp;
    else *rpm = -1;
    return 0;
}

static int gpu_busy(int dev, uint32_t *pct) {
    if (ok(amdsmi_get_gpu_busy_percent(D(dev)->h, pct))) return 0;
    return -1;
}

static int mem_bytes(int dev, amdsmi_memory_type_t t, uint64_t *used, uint64_t *total) {
    int uok = 0, tok = 0;
    *used = *total = 0;
    if (ok(amdsmi_get_gpu_memory_usage(D(dev)->h, t, used))) uok = 1;
    if (ok(amdsmi_get_gpu_memory_total(D(dev)->h, t, total))) tok = 1;
    return (uok && tok) ? 0 : -1;
}

static int vram_pct(int dev) {
    uint64_t u, t;
    if (mem_bytes(dev, AMDSMI_MEM_TYPE_VRAM, &u, &t) != 0 || t == 0) return -1;
    return (int)((100.0 * (double)u) / (double)t);
}

static int power_cap_w(int dev, double *w) {
    amdsmi_power_cap_info_t c;
    memset(&c, 0, sizeof c);
    if (!ok(amdsmi_get_power_cap_info(D(dev)->h, 0, &c))) return -1;
    if (c.power_cap > 100000) *w = (double)c.power_cap / 1e6;
    else *w = (double)c.power_cap;
    return 0;
}

static void device_id_str(int dev, char *buf, size_t n) {
    amdsmi_asic_info_t a;
    memset(&a, 0, sizeof a);
    if (!ok(amdsmi_get_gpu_asic_info(D(dev)->h, &a)) || a.device_id == 0 ||
        a.device_id == 0xFFFFFFFFFFFFFFFFull)
        snprintf(buf, n, "N/A");
    else if (a.device_id <= 0xFFFF)
        snprintf(buf, n, "0x%llx", (unsigned long long)a.device_id);
    else
        snprintf(buf, n, "0x%llx", (unsigned long long)a.device_id);
}

static void guid_str(int dev, char *buf, size_t n) {
    amdsmi_kfd_info_t k;
    memset(&k, 0, sizeof k);
    if (ok(amdsmi_get_gpu_kfd_info(D(dev)->h, &k)) && k.kfd_id != UINT64_MAX) {
        snprintf(buf, n, "%" PRIu64, k.kfd_id);
        return;
    }
    snprintf(buf, n, "N/A");
}

static uint32_t node_id(int dev) {
    amdsmi_kfd_info_t k;
    memset(&k, 0, sizeof k);
    if (ok(amdsmi_get_gpu_kfd_info(D(dev)->h, &k)) && k.node_id != 0xFFFFFFFFu)
        return k.node_id;
    return (uint32_t)dev;
}

/* rsmi_dev_unique_id_get: 64-bit KFD unique_id, printed as 0x... */
static void unique_id_str(int dev, char *buf, size_t n) {
    char path[160], raw[128], *end = NULL;
    unsigned long long u = 0;
    snprintf(path, sizeof path, "/sys/class/drm/card%u/device/unique_id", D(dev)->card);
    if (read_file(path, raw, sizeof raw) >= 0 && raw[0]) {
        u = strtoull(raw, &end, 16);
        if (u && end && *end == 0) {
            snprintf(buf, n, "0x%llx", u);
            return;
        }
    }
    snprintf(path, sizeof path, "/sys/class/kfd/kfd/topology/nodes/%u/properties", node_id(dev));
    {
        FILE *f = fopen(path, "r");
        char line[256];
        if (f) {
            while (fgets(line, sizeof line, f)) {
                if (sscanf(line, "unique_id %llu", &u) == 1 && u) {
                    snprintf(buf, n, "0x%llx", u);
                    fclose(f);
                    return;
                }
            }
            fclose(f);
        }
    }
    snprintf(buf, n, "N/A");
}

static const char *vbios_ver(const amdsmi_vbios_info_t *v) {
    if (v->part_number[0]) return v->part_number;
    if (v->version[0]) return v->version;
    return "N/A";
}

static const char *part_name(amdsmi_accelerator_partition_type_t t) {
    switch (t) {
    case AMDSMI_ACCELERATOR_PARTITION_SPX: return "SPX";
    case AMDSMI_ACCELERATOR_PARTITION_DPX: return "DPX";
    case AMDSMI_ACCELERATOR_PARTITION_TPX: return "TPX";
    case AMDSMI_ACCELERATOR_PARTITION_QPX: return "QPX";
    case AMDSMI_ACCELERATOR_PARTITION_CPX: return "CPX";
    default: return "N/A";
    }
}

static void mem_part(int dev, char *buf, size_t n) {
    if (!ok(amdsmi_get_gpu_memory_partition(D(dev)->h, buf, (uint32_t)n)) || !buf[0])
        snprintf(buf, n, "N/A");
}

static void comp_part(int dev, char *buf, size_t n) {
    amdsmi_accelerator_partition_profile_t p;
    uint32_t id[8];
    memset(&p, 0, sizeof p);
    memset(id, 0, sizeof id);
    if (ok(amdsmi_get_gpu_accelerator_partition_profile(D(dev)->h, &p, id))) {
        snprintf(buf, n, "%s", part_name(p.profile_type));
        return;
    }
    snprintf(buf, n, "N/A");
}

static const char *part_id_str(int dev, char *buf, size_t n) {
    amdsmi_kfd_info_t k;
    memset(&k, 0, sizeof k);
    if (ok(amdsmi_get_gpu_kfd_info(D(dev)->h, &k)) && k.current_partition_id != 0xFFFFFFFFu)
        snprintf(buf, n, "%u", k.current_partition_id);
    else
        snprintf(buf, n, "0");
    return buf;
}

static void bdf_str(int dev, char *buf, size_t n) {
    amdsmi_bdf_t b;
    memset(&b, 0, sizeof b);
    if (!ok(amdsmi_get_gpu_device_bdf(D(dev)->h, &b))) {
        snprintf(buf, n, "N/A");
        return;
    }
    snprintf(buf, n, "%04" PRIX64 ":%02" PRIX64 ":%02" PRIX64 ".%" PRIX64,
             (uint64_t)b.bdf.domain_number, (uint64_t)b.bdf.bus_number,
             (uint64_t)b.bdf.device_number, (uint64_t)b.bdf.function_number);
}

static const char *ras_state_name(amdsmi_ras_err_state_t s) {
    switch (s) {
    case AMDSMI_RAS_ERR_STATE_NONE: return "NONE";
    case AMDSMI_RAS_ERR_STATE_DISABLED: return "DISABLED";
    case AMDSMI_RAS_ERR_STATE_PARITY: return "PARITY";
    case AMDSMI_RAS_ERR_STATE_SING_C: return "SING_C";
    case AMDSMI_RAS_ERR_STATE_MULT_UC: return "MULT_UC";
    case AMDSMI_RAS_ERR_STATE_POISON: return "POISON";
    case AMDSMI_RAS_ERR_STATE_ENABLED: return "ENABLED";
    default: return "N/A";
    }
}

struct rasblk { const char *name; amdsmi_gpu_block_t bit; };
static const struct rasblk RAS[] = {
    {"UMC", AMDSMI_GPU_BLOCK_UMC},
    {"SDMA", AMDSMI_GPU_BLOCK_SDMA},
    {"GFX", AMDSMI_GPU_BLOCK_GFX},
    {"MMHUB", AMDSMI_GPU_BLOCK_MMHUB},
    {"ATHUB", AMDSMI_GPU_BLOCK_ATHUB},
    {"PCIE_BIF", AMDSMI_GPU_BLOCK_PCIE_BIF},
    {"HDP", AMDSMI_GPU_BLOCK_HDP},
    {"XGMI_WAFL", AMDSMI_GPU_BLOCK_XGMI_WAFL},
    {"DF", AMDSMI_GPU_BLOCK_DF},
    {"SMN", AMDSMI_GPU_BLOCK_SMN},
    {"SEM", AMDSMI_GPU_BLOCK_SEM},
    {"MP0", AMDSMI_GPU_BLOCK_MP0},
    {"MP1", AMDSMI_GPU_BLOCK_MP1},
    {"FUSE", AMDSMI_GPU_BLOCK_FUSE},
    {"MCA", AMDSMI_GPU_BLOCK_MCA},
    {"VCN", AMDSMI_GPU_BLOCK_VCN},
    {"JPEG", AMDSMI_GPU_BLOCK_JPEG},
};
static const int NRAS = (int)(sizeof RAS / sizeof RAS[0]);

static const char *fw_name(amdsmi_fw_block_t id) {
    switch (id) {
    case AMDSMI_FW_ID_SMU: return "SMC";
    case AMDSMI_FW_ID_CP_CE: return "CE";
    case AMDSMI_FW_ID_CP_PFP: return "PFP";
    case AMDSMI_FW_ID_CP_ME: return "ME";
    case AMDSMI_FW_ID_CP_MEC_JT1: return "MEC JT1";
    case AMDSMI_FW_ID_CP_MEC_JT2: return "MEC JT2";
    case AMDSMI_FW_ID_CP_MEC1: return "MEC";
    case AMDSMI_FW_ID_CP_MEC2: return "MEC2";
    case AMDSMI_FW_ID_RLC: return "RLC";
    case AMDSMI_FW_ID_SDMA0: return "SDMA0";
    case AMDSMI_FW_ID_SDMA1: return "SDMA1";
    case AMDSMI_FW_ID_SDMA2: return "SDMA2";
    case AMDSMI_FW_ID_SDMA3: return "SDMA3";
    case AMDSMI_FW_ID_SDMA4: return "SDMA4";
    case AMDSMI_FW_ID_SDMA5: return "SDMA5";
    case AMDSMI_FW_ID_SDMA6: return "SDMA6";
    case AMDSMI_FW_ID_SDMA7: return "SDMA7";
    case AMDSMI_FW_ID_VCN: return "VCN";
    case AMDSMI_FW_ID_UVD: return "UVD";
    case AMDSMI_FW_ID_VCE: return "VCE";
    case AMDSMI_FW_ID_MC: return "MC";
    case AMDSMI_FW_ID_ASD: return "ASD";
    case AMDSMI_FW_ID_TA_RAS: return "TA RAS";
    case AMDSMI_FW_ID_TA_XGMI: return "TA XGMI";
    case AMDSMI_FW_ID_CP_MES: return "MES";
    case AMDSMI_FW_ID_MES_KIQ: return "MES KIQ";
    case AMDSMI_FW_ID_PSP_SOSDRV: return "SOS";
    default: return NULL;
    }
}

static int fw_is_hex(const char *n) {
    return str_eq(n, "VCN") || str_eq(n, "VCE") || str_eq(n, "UVD") || str_eq(n, "SOS") ||
           str_eq(n, "ASD") || str_eq(n, "MES") || str_eq(n, "MES KIQ");
}
static int fw_is_dot(const char *n) {
    return str_eq(n, "TA XGMI") || str_eq(n, "TA RAS") || str_eq(n, "SMC");
}

static void fmt_fw(const char *name, uint64_t ver, char *buf, size_t n) {
    unsigned hexv = (unsigned)(ver & 0xFFFFFFFFu);
    if (fw_is_hex(name))
        snprintf(buf, n, "0x%08x", hexv);
    else if (fw_is_dot(name))
        snprintf(buf, n, "%02u.%02u.%02u.%02u",
                 (hexv >> 24) & 0xFF, (hexv >> 16) & 0xFF, (hexv >> 8) & 0xFF, hexv & 0xFF);
    else
        snprintf(buf, n, "%" PRIu64, ver);
}

static const char *link_type_name(amdsmi_link_type_t t) {
    switch (t) {
    case AMDSMI_LINK_TYPE_PCIE: return "PCIE";
    case AMDSMI_LINK_TYPE_XGMI: return "XGMI";
    case AMDSMI_LINK_TYPE_INTERNAL: return "INTERNAL";
    case AMDSMI_LINK_TYPE_NUMA: return "NUMA";
    default: return "XXXX";
    }
}

static const char *profile_name(uint64_t mask) {
    switch (mask) {
    case AMDSMI_PWR_PROF_PRST_CUSTOM_MASK: return "CUSTOM";
    case AMDSMI_PWR_PROF_PRST_VIDEO_MASK: return "VIDEO";
    case AMDSMI_PWR_PROF_PRST_POWER_SAVING_MASK: return "POWER SAVING";
    case AMDSMI_PWR_PROF_PRST_COMPUTE_MASK: return "COMPUTE";
    case AMDSMI_PWR_PROF_PRST_VR_MASK: return "VR";
    case AMDSMI_PWR_PROF_PRST_3D_FULL_SCR_MASK: return "3D FULL SCREEN";
    case AMDSMI_PWR_PROF_PRST_BOOTUP_DEFAULT: return "BOOTUP DEFAULT";
    default: return "UNKNOWN";
    }
}

static int confirm_danger(void) {
    char line[64];
    fputs("\n"
          "          ******WARNING******\n"
          "\n"
          "          Operating your AMD GPU outside of official AMD specifications or outside of\n"
          "          factory settings, including but not limited to the conducting of overclocking,\n"
          "          over-volting or under-volting (including use of this interface software,\n"
          "          even if such software has been directly or indirectly provided by AMD or otherwise\n"
          "          affiliated in any way with AMD), may cause damage to your AMD GPU, system components\n"
          "          and/or result in system failure, as well as cause other problems.\n"
          "          DAMAGES CAUSED BY USE OF YOUR AMD GPU OUTSIDE OF OFFICIAL AMD SPECIFICATIONS OR\n"
          "          OUTSIDE OF FACTORY SETTINGS ARE NOT COVERED UNDER ANY AMD PRODUCT WARRANTY AND\n"
          "          MAY NOT BE COVERED BY YOUR BOARD OR SYSTEM MANUFACTURER'S WARRANTY.\n"
          "          Use this utility with caution.\n"
          "\n"
          "Do you accept these terms? [y/N] ",
          stdout);
    fflush(stdout);
    if (g_autorespond) {
        printf("%s\n", g_autorespond);
        return g_autorespond[0] == 'y' || g_autorespond[0] == 'Y';
    }
    if (!fgets(line, sizeof line, stdin)) return 0;
    return line[0] == 'y' || line[0] == 'Y';
}

static void status_line(int dev, amdsmi_status_t st, const char *what) {
    const char *s = NULL;
    if (ok(st)) {
        print_log(dev, what, NULL);
        return;
    }
    amdsmi_status_code_to_string(st, &s);
    if (st == AMDSMI_STATUS_NOT_SUPPORTED)
        print_log(dev, "Not supported on the given system", NULL);
    else if (st == AMDSMI_STATUS_NO_PERM)
        print_log(dev, "Permission denied", NULL);
    else
        print_logf(dev, "Failed", "%s (%s)", what, s ? s : "error");
    g_retcode = 1;
}

/* -------------------------------------------------------------------------- */
/* show commands                                                              */
/* -------------------------------------------------------------------------- */

/* Left-align s to width and append it. Padding never exceeds the column buffer. */
static void append_padded(char *line, size_t line_n, int width, const char *s) {
    char col[320];
    size_t sl = strlen(s);
    size_t room = sizeof col - 1;
    size_t pad = 0;
    size_t used;
    if (sl > room) sl = room;
    if (width > (int)sl) {
        pad = (size_t)width - sl;
        if (pad > room - sl) pad = room - sl;
    }
    memcpy(col, s, sl);
    if (pad) memset(col + sl, ' ', pad);
    col[sl + pad] = 0;
    used = strlen(line);
    if (line_n > used + 1)
        strncat(line, col, line_n - used - 1);
}

static void show_version(void) {
    amdsmi_version_t v;
    memset(&v, 0, sizeof v);
    printf("ROCM-SMI version: %s\n", SMI_VERSION);
    if (ok(amdsmi_get_lib_version(&v)))
        printf("ROCM-SMI-LIB version: %u.%u.%u\n", v.major, v.minor, v.release);
}

static void show_concise(const int *devs, int nd) {
    const char *headers[] = {"Device", "Node", "IDs", "", "Temp", "Power", "Partitions",
                             "SCLK", "MCLK", "Fan", "Perf", "PwrCap", "VRAM%", "GPU%"};
    char subs[14][64];
    char cells[MAX_DEV][14][128];
    int widths[14];
    int i, c, wsum = 0;
    char temp_label[32] = "(Edge)";
    const char *power_label = "(Avg)";
    char line[2048];
    int good = 0;
    double tc;

    if (g_json) {
        printf("NOT_SUPPORTED: Cannot print JSON/CSV output for concise output\n");
        exit(1);
    }
    if (nd > 0) {
        tc = temp_c(devs[0], AMDSMI_TEMPERATURE_TYPE_EDGE, &good);
        if (!good) {
            tc = temp_c(devs[0], AMDSMI_TEMPERATURE_TYPE_JUNCTION, &good);
            snprintf(temp_label, sizeof temp_label, good ? "(Junction)" : "(Edge)");
            (void)tc;
            if (!good) {
                good = 0;
                tc = temp_c(devs[0], AMDSMI_TEMPERATURE_TYPE_VRAM, &good);
                if (good) snprintf(temp_label, sizeof temp_label, "(Memory)");
            }
        }
        {
            double w;
            const char *pt, *lb;
            if (power_watts(devs[0], &w, &pt, &lb) == 0) power_label = lb;
        }
    }
    subs[0][0] = 0;
    subs[1][0] = 0;
    snprintf(subs[2], sizeof subs[2], "(DID,");
    snprintf(subs[3], sizeof subs[3], "GUID)");
    snprintf(subs[4], sizeof subs[4], "%s", temp_label);
    snprintf(subs[5], sizeof subs[5], "%s", power_label);
    snprintf(subs[6], sizeof subs[6], "(Mem, Compute, ID)");
    for (c = 7; c < 14; c++) subs[c][0] = 0;

    for (c = 0; c < 14; c++) {
        int hs = (int)strlen(headers[c]);
        int ss = (int)strlen(subs[c]);
        if (hs > ss) {
            memset(subs[c] + ss, ' ', (size_t)(hs - ss));
            subs[c][hs] = 0;
        }
    }
    for (c = 0; c < 14; c++) widths[c] = (int)strlen(headers[c]) + 2;

    for (i = 0; i < nd; i++) {
        int d = devs[i];
        char did[32], guid[32], mp[32], cp[32], pid[32], perf[32] = "N/A";
        char sclk[32] = "N/A", mclk[32] = "N/A";
        double mhz;
        int level, deep, pct, vp;
        int64_t fl = 0, rpm = 0;
        uint32_t busy = 0;
        double watts = 0, cap = 0;
        const char *pt, *lb;
        amdsmi_dev_perf_level_t pl;
        int tg = 0;
        amdsmi_temperature_type_t ts = AMDSMI_TEMPERATURE_TYPE_EDGE;
        if (strstr(temp_label, "Junction")) ts = AMDSMI_TEMPERATURE_TYPE_JUNCTION;
        else if (strstr(temp_label, "Memory")) ts = AMDSMI_TEMPERATURE_TYPE_VRAM;

        device_id_str(d, did, sizeof did);
        guid_str(d, guid, sizeof guid);
        mem_part(d, mp, sizeof mp);
        comp_part(d, cp, sizeof cp);
        part_id_str(d, pid, sizeof pid);
        tc = temp_c(d, ts, &tg);
        if (current_clk_mhz(d, AMDSMI_CLK_TYPE_SYS, &mhz, &level, &deep) == 0)
            snprintf(sclk, sizeof sclk, "%.0fMhz", mhz);
        if (current_clk_mhz(d, AMDSMI_CLK_TYPE_MEM, &mhz, &level, &deep) == 0)
            snprintf(mclk, sizeof mclk, "%.0fMhz", mhz);
        if (fan_level_pct(d, &fl, &pct, &rpm) == 0)
            snprintf(cells[i][9], sizeof cells[i][9], "%d%%", pct);
        else
            snprintf(cells[i][9], sizeof cells[i][9], "N/A");
        if (ok(amdsmi_get_gpu_perf_level(D(d)->h, &pl)) && perf_name(pl))
            snprintf(perf, sizeof perf, "%s", perf_name(pl));
        vp = vram_pct(d);
        snprintf(cells[i][0], sizeof cells[i][0], "%d", d);
        snprintf(cells[i][1], sizeof cells[i][1], "%u", node_id(d));
        snprintf(cells[i][2], sizeof cells[i][2], "%s, ", did);
        snprintf(cells[i][3], sizeof cells[i][3], "%s", guid);
        if (tg) snprintf(cells[i][4], sizeof cells[i][4], "%.1f\xC2\xB0" "C", tc);
        else snprintf(cells[i][4], sizeof cells[i][4], "N/A");
        if (power_watts(d, &watts, &pt, &lb) == 0 && watts != 0)
            snprintf(cells[i][5], sizeof cells[i][5], "%.1fW", watts);
        else
            snprintf(cells[i][5], sizeof cells[i][5], "N/A");
        snprintf(cells[i][6], sizeof cells[i][6], "%s, %s, %s", mp, cp, pid);
        snprintf(cells[i][7], sizeof cells[i][7], "%s", sclk);
        snprintf(cells[i][8], sizeof cells[i][8], "%s", mclk);
        snprintf(cells[i][10], sizeof cells[i][10], "%s", perf);
        if (power_cap_w(d, &cap) == 0) snprintf(cells[i][11], sizeof cells[i][11], "%.1fW", cap);
        else snprintf(cells[i][11], sizeof cells[i][11], "N/A");
        if (vp >= 0) snprintf(cells[i][12], sizeof cells[i][12], "%d%%", vp);
        else snprintf(cells[i][12], sizeof cells[i][12], "N/A");
        if (gpu_busy(d, &busy) == 0) snprintf(cells[i][13], sizeof cells[i][13], "%u%%", busy);
        else snprintf(cells[i][13], sizeof cells[i][13], "N/A");
        for (c = 0; c < 14; c++) {
            int L = (int)strlen(cells[i][c]) + 2;
            if (L > widths[c]) widths[c] = L;
        }
    }
    /* header/subheader may have grown */
    {
        char hdrpad[14][64];
        for (c = 0; c < 14; c++) {
            snprintf(hdrpad[c], sizeof hdrpad[c], "%s", headers[c]);
            if ((int)strlen(subs[c]) > (int)strlen(hdrpad[c])) {
                int extra = (int)strlen(subs[c]) - (int)strlen(hdrpad[c]);
                int hl = (int)strlen(hdrpad[c]);
                memset(hdrpad[c] + hl, ' ', (size_t)extra);
                hdrpad[c][hl + extra] = 0;
            }
            if ((int)strlen(hdrpad[c]) + 2 > widths[c]) widths[c] = (int)strlen(hdrpad[c]) + 2;
        }
        line[0] = 0;
        for (c = 0; c < 14; c++)
            append_padded(line, sizeof line, widths[c], hdrpad[c]);
        wsum = (int)strlen(line);
        print_spacer(HEADER, "=", wsum);
        print_spacer(" Concise Info ", "=", wsum);
        printf("%s\n", line);
        line[0] = 0;
        for (c = 0; c < 14; c++)
            append_padded(line, sizeof line, widths[c], subs[c]);
        printf("\033[3m%s\033[0m\n", line);
        print_spacer(NULL, "=", wsum);
        for (i = 0; i < nd; i++) {
            line[0] = 0;
            for (c = 0; c < 14; c++)
                append_padded(line, sizeof line, widths[c], cells[i][c]);
            printf("%s\n", line);
        }
        print_spacer(NULL, "=", wsum);
        print_spacer(FOOTER, "=", wsum);
    }
}

static void show_hw(const int *devs, int nd) {
    const char *headers[] = {"GPU", "NODE", "DID", "GUID", "GFX VER", "GFX RAS", "SDMA RAS",
                             "UMC RAS", "VBIOS", "BUS", "PARTITION ID"};
    char cells[MAX_DEV][11][AMDSMI_MAX_STRING_LENGTH];
    int widths[11];
    int i, c;
    char line[2048];
    int wsum;
    if (g_json) {
        printf("NOT_SUPPORTED: Cannot print JSON/CSV output for concise hardware output\n");
        exit(1);
    }
    for (c = 0; c < 11; c++) widths[c] = (int)strlen(headers[c]) + 2;
    for (i = 0; i < nd; i++) {
        int d = devs[i];
        amdsmi_asic_info_t a;
        amdsmi_vbios_info_t vb;
        amdsmi_ras_err_state_t st;
        char gfx[32] = "N/A";
        memset(&a, 0, sizeof a);
        memset(&vb, 0, sizeof vb);
        amdsmi_get_gpu_asic_info(D(d)->h, &a);
        amdsmi_get_gpu_vbios_info(D(d)->h, &vb);
        snprintf(cells[i][0], sizeof cells[i][0], "%d", d);
        snprintf(cells[i][1], sizeof cells[i][1], "%u", node_id(d));
        device_id_str(d, cells[i][2], sizeof cells[i][2]);
        guid_str(d, cells[i][3], sizeof cells[i][3]);
        if (a.target_graphics_version != UINT64_MAX && a.target_graphics_version != 0)
            snprintf(gfx, sizeof gfx, "gfx%" PRIx64, a.target_graphics_version);
        snprintf(cells[i][4], sizeof cells[i][4], "%s", gfx);
        if (ok(amdsmi_get_gpu_ecc_status(D(d)->h, AMDSMI_GPU_BLOCK_GFX, &st)))
            snprintf(cells[i][5], sizeof cells[i][5], "%s", ras_state_name(st));
        else snprintf(cells[i][5], sizeof cells[i][5], "N/A");
        if (ok(amdsmi_get_gpu_ecc_status(D(d)->h, AMDSMI_GPU_BLOCK_SDMA, &st)))
            snprintf(cells[i][6], sizeof cells[i][6], "%s", ras_state_name(st));
        else snprintf(cells[i][6], sizeof cells[i][6], "N/A");
        if (ok(amdsmi_get_gpu_ecc_status(D(d)->h, AMDSMI_GPU_BLOCK_UMC, &st)))
            snprintf(cells[i][7], sizeof cells[i][7], "%s", ras_state_name(st));
        else snprintf(cells[i][7], sizeof cells[i][7], "N/A");
        snprintf(cells[i][8], sizeof cells[i][8], "%s", vbios_ver(&vb));
        bdf_str(d, cells[i][9], sizeof cells[i][9]);
        part_id_str(d, cells[i][10], sizeof cells[i][10]);
        for (c = 0; c < 11; c++) {
            int L = (int)strlen(cells[i][c]) + 2;
            if (L > widths[c]) widths[c] = L;
        }
    }
    line[0] = 0;
    for (c = 0; c < 11; c++)
        append_padded(line, sizeof line, widths[c], headers[c]);
    wsum = (int)strlen(line);
    print_spacer(" Concise Hardware Info ", "=", wsum);
    printf("%s\n", line);
    for (i = 0; i < nd; i++) {
        line[0] = 0;
        for (c = 0; c < 11; c++)
            append_padded(line, sizeof line, widths[c], cells[i][c]);
        printf("%s\n", line);
    }
    print_spacer(NULL, "=", wsum);
}

static void show_id(const int *devs, int nd) {
    int i;
    print_spacer(" ID ", "=", 0);
    for (i = 0; i < nd; i++) {
        int d = devs[i];
        amdsmi_asic_info_t a;
        char did[32], rev[16], sub[16], guid[40];
        memset(&a, 0, sizeof a);
        amdsmi_get_gpu_asic_info(D(d)->h, &a);
        device_id_str(d, did, sizeof did);
        if (a.rev_id != 0xFFFFFFFFu) snprintf(rev, sizeof rev, "0x%02x", a.rev_id);
        else snprintf(rev, sizeof rev, "N/A");
        if (a.subsystem_id) snprintf(sub, sizeof sub, "0x%04x", a.subsystem_id);
        else snprintf(sub, sizeof sub, "N/A");
        guid_str(d, guid, sizeof guid);
        print_logf(d, "Device Name", "\t\t%s", a.market_name[0] ? a.market_name : "N/A");
        print_logf(d, "Device ID", "\t\t%s", did);
        print_logf(d, "Device Rev", "\t\t%s", rev);
        print_logf(d, "Subsystem ID", "\t%s", sub);
        print_logf(d, "GUID", "\t\t%s", guid);
    }
    print_spacer(NULL, "=", 0);
}

static void show_vbios(const int *devs, int nd) {
    int i;
    print_spacer(" VBIOS ", "=", 0);
    for (i = 0; i < nd; i++) {
        amdsmi_vbios_info_t v;
        memset(&v, 0, sizeof v);
        if (ok(amdsmi_get_gpu_vbios_info(D(devs[i])->h, &v)))
            print_log(devs[i], "VBIOS version", vbios_ver(&v));
        else
            print_log(devs[i], "VBIOS version", "N/A");
    }
    print_spacer(NULL, "=", 0);
}

static void show_driver(const int *devs, int nd) {
    amdsmi_driver_info_t info;
    const char *ver = "N/A";
    print_spacer(" Version of System Component ", "=", 0);
    memset(&info, 0, sizeof info);
    if (nd > 0 && ok(amdsmi_get_gpu_driver_info(D(devs[0])->h, &info)) && info.driver_version[0])
        ver = info.driver_version;
    else {
        char buf[128];
        if (read_file("/sys/module/amdgpu/version", buf, sizeof buf) >= 0) ver = buf;
    }
    if (g_json) json_add(-1, "Driver version", ver);
    else printf("Driver version: %s\n", ver);
    print_spacer(NULL, "=", 0);
    (void)nd;
}

static void show_temp(const int *devs, int nd) {
    static const struct { const char *n; amdsmi_temperature_type_t t; } s[] = {
        {"edge", AMDSMI_TEMPERATURE_TYPE_EDGE},
        {"junction", AMDSMI_TEMPERATURE_TYPE_JUNCTION},
        {"memory", AMDSMI_TEMPERATURE_TYPE_VRAM},
        {"hbm0", AMDSMI_TEMPERATURE_TYPE_HBM_0},
        {"hbm1", AMDSMI_TEMPERATURE_TYPE_HBM_1},
        {"hbm2", AMDSMI_TEMPERATURE_TYPE_HBM_2},
        {"hbm3", AMDSMI_TEMPERATURE_TYPE_HBM_3},
    };
    int i, k;
    print_spacer(" Temperature ", "=", 0);
    for (i = 0; i < nd; i++) {
        for (k = 0; k < (int)(sizeof s / sizeof s[0]); k++) {
            int good = 0;
            double t = temp_c(devs[i], s[k].t, &good);
            char m[64];
            snprintf(m, sizeof m, "Temperature (Sensor %s) (C)", s[k].n);
            if (good) print_logf(devs[i], m, "%.1f", t);
        }
    }
    print_spacer(NULL, "=", 0);
}

static void show_fan(const int *devs, int nd) {
    int i;
    print_spacer(" Current Fan Metric ", "=", 0);
    for (i = 0; i < nd; i++) {
        int64_t lv = 0, rpm = -1;
        int pct = 0;
        int st = fan_level_pct(devs[i], &lv, &pct, &rpm);
        if (st == AMDSMI_STATUS_NOT_SUPPORTED) {
            print_log(devs[i], "Not supported", NULL);
            continue;
        }
        if (st != 0 || lv == 0 || pct == 0) {
            print_logf(devs[i], NULL, "Unable to detect fan speed for GPU %d", devs[i]);
            continue;
        }
        if (g_json) {
            print_logf(devs[i], "Fan speed (level)", "%" PRId64, lv);
            print_logf(devs[i], "Fan speed (%)", "%d", pct);
        } else {
            print_logf(devs[i], "Fan Level", "%" PRId64 " (%d%%)", lv, pct);
        }
        if (rpm >= 0) print_logf(devs[i], "Fan RPM", "%" PRId64, rpm);
    }
    print_spacer(NULL, "=", 0);
}

static void show_power(const int *devs, int nd) {
    int i;
    print_spacer(" Power Consumption ", "=", 0);
    for (i = 0; i < nd; i++) {
        double w;
        const char *pt, *lb;
        char metric[96];
        char num[32];
        if (power_watts(devs[i], &w, &pt, &lb) != 0) continue;
        if (str_eq(pt, "CURRENT SOCKET"))
            snprintf(metric, sizeof metric, "Current Socket Graphics Package Power (W)");
        else
            snprintf(metric, sizeof metric, "Average Graphics Package Power (W)");
        fmt_py_num(num, sizeof num, w);
        print_log(devs[i], metric, num);
    }
    print_spacer(NULL, "=", 0);
}

static void show_use(const int *devs, int nd) {
    int i;
    print_spacer(" % time GPU is busy ", "=", 0);
    for (i = 0; i < nd; i++) {
        uint32_t p = 0;
        uint64_t ts = 0;
        amdsmi_utilization_counter_t u;
        if (gpu_busy(devs[i], &p) == 0) print_logf(devs[i], "GPU use (%)", "%u", p);
        else print_log(devs[i], "GPU use Unsupported", NULL);
        memset(&u, 0, sizeof u);
        u.type = AMDSMI_COARSE_GRAIN_GFX_ACTIVITY;
        if (ok(amdsmi_get_utilization_count(D(devs[i])->h, &u, 1, &ts)) && util_value_ok(u.value))
            print_logf(devs[i], "GFX Activity", "%" PRIu64, u.value);
        else
            print_log(devs[i], "GFX Activity", "N/A");
    }
    print_spacer(NULL, "=", 0);
}

static void show_memuse(const int *devs, int nd) {
    int i;
    print_spacer(" Current Memory Use ", "=", 0);
    for (i = 0; i < nd; i++) {
        uint32_t busy = 0;
        uint64_t ts = 0;
        amdsmi_utilization_counter_t u;
        if (mem_busy_pct(devs[i], &busy) == 0)
            print_logf(devs[i], "GPU memory use (%)", "%u", busy);
        else
            print_log(devs[i], "GPU memory use Unsupported", NULL);
        memset(&u, 0, sizeof u);
        u.type = AMDSMI_COARSE_GRAIN_MEM_ACTIVITY;
        if (ok(amdsmi_get_utilization_count(D(devs[i])->h, &u, 1, &ts)) && util_value_ok(u.value))
            print_logf(devs[i], "Memory Activity", "%" PRIu64, u.value);
        else
            print_log(devs[i], "Memory Activity", "N/A");
    }
    print_spacer(NULL, "=", 0);
}

static void show_voltage(const int *devs, int nd) {
    int i;
    print_spacer(" Current voltage ", "=", 0);
    for (i = 0; i < nd; i++) {
        int64_t mv = 0;
        if (ok(amdsmi_get_gpu_volt_metric(D(devs[i])->h, AMDSMI_VOLT_TYPE_VDDGFX,
                                          AMDSMI_VOLT_CURRENT, &mv)))
            print_logf(devs[i], "Voltage (mV)", "%" PRId64, mv);
    }
    print_spacer(NULL, "=", 0);
}

static void show_clocks_current(const int *devs, int nd, const char *only) {
    int i, k;
    if (!only) print_spacer(" Current clock frequencies ", "=", 0);
    for (i = 0; i < nd; i++) {
        for (k = 0; k < NCLKS; k++) {
            double mhz;
            int level, deep;
            char lv[16], metric[64];
            if (only && !str_ieq(only, CLKS[k].name) &&
                !(str_ieq(only, "sclk") && CLKS[k].type == AMDSMI_CLK_TYPE_SYS))
                continue;
            if (CLKS[k].type == AMDSMI_CLK_TYPE_PCIE) {
                amdsmi_pcie_bandwidth_t bw;
                memset(&bw, 0, sizeof bw);
                if (!ok(amdsmi_get_gpu_pci_bandwidth(D(devs[i])->h, &bw))) continue;
                if (bw.transfer_rate.current >= bw.transfer_rate.num_supported) continue;
                print_logf(devs[i], "pcie clock level",
                           "%u (%.1fGT/s x%u)", bw.transfer_rate.current,
                           (double)bw.transfer_rate.frequency[bw.transfer_rate.current] / 1e9,
                           bw.lanes[bw.transfer_rate.current]);
                continue;
            }
            if (current_clk_mhz(devs[i], CLKS[k].type, &mhz, &level, &deep) != 0) continue;
            fmt_level(level, deep, lv, sizeof lv);
            if (only) {
                snprintf(metric, sizeof metric, "%s clock level", CLKS[k].name);
                print_logf(devs[i], metric, "%s (%.0fMhz)", lv, mhz);
            } else if (g_json) {
                char a[64], b[64];
                snprintf(a, sizeof a, "%s clock speed:", CLKS[k].name);
                snprintf(b, sizeof b, "%s clock level:", CLKS[k].name);
                print_logf(devs[i], a, "(%.0fMhz)", mhz);
                print_log(devs[i], b, lv);
            } else {
                char a[80];
                snprintf(a, sizeof a, "%s clock level: %s", CLKS[k].name, lv);
                print_logf(devs[i], a, "(%.0fMhz)", mhz);
            }
        }
    }
    if (!only) print_spacer(NULL, "=", 0);
}

static void show_clocks_supported(const int *devs, int nd) {
    int i, k, j;
    print_spacer(" Supported clock frequencies ", "=", 0);
    for (i = 0; i < nd; i++) {
        for (k = 0; k < NCLKS; k++) {
            amdsmi_frequencies_t f;
            if (CLKS[k].type == AMDSMI_CLK_TYPE_PCIE) {
                amdsmi_pcie_bandwidth_t bw;
                memset(&bw, 0, sizeof bw);
                if (!ok(amdsmi_get_gpu_pci_bandwidth(D(devs[i])->h, &bw))) continue;
                print_logf(devs[i], NULL, "Supported PCIe frequencies on GPU%d", devs[i]);
                for (j = 0; j < (int)bw.transfer_rate.num_supported; j++) {
                    char idx[16], fr[64];
                    snprintf(idx, sizeof idx, "%d", j);
                    snprintf(fr, sizeof fr, "%.1fGT/s x%u%s",
                             (double)bw.transfer_rate.frequency[j] / 1e9, bw.lanes[j],
                             j == (int)bw.transfer_rate.current ? " *" : "");
                    print_log(devs[i], idx, fr);
                }
                print_log(devs[i], "", NULL);
                continue;
            }
            memset(&f, 0, sizeof f);
            if (!ok(amdsmi_get_clk_freq(D(devs[i])->h, CLKS[k].type, &f)) || f.num_supported == 0)
                continue;
            print_logf(devs[i], NULL, "Supported %s frequencies on GPU%d", CLKS[k].name, devs[i]);
            for (j = 0; j < (int)f.num_supported; j++) {
                char idx[16], fr[64];
                int deep = f.has_deep_sleep ? 1 : 0;
                fmt_level(j, deep, idx, sizeof idx);
                snprintf(fr, sizeof fr, "%.0fMhz%s", (double)f.frequency[j] / 1e6,
                         j == (int)f.current ? " *" : "");
                print_log(devs[i], idx, fr);
            }
            print_log(devs[i], "", NULL);
        }
        if (i + 1 < nd) print_spacer(NULL, "-", 0);
    }
    print_spacer(NULL, "=", 0);
}

static void show_perf(const int *devs, int nd) {
    int i;
    print_spacer(" Show Performance Level ", "=", 0);
    for (i = 0; i < nd; i++) {
        amdsmi_dev_perf_level_t p;
        if (ok(amdsmi_get_gpu_perf_level(D(devs[i])->h, &p)) && perf_name(p))
            print_log(devs[i], "Performance Level", perf_name(p));
        else
            print_log(devs[i], "Performance Level Unsupported", NULL);
    }
    print_spacer(NULL, "=", 0);
}

static void show_overdrive(const int *devs, int nd, const char *which) {
    int i;
    print_spacer(" OverDrive Level ", "=", 0);
    for (i = 0; i < nd; i++) {
        uint32_t od = 0;
        amdsmi_status_t st;
        if (str_eq(which, "mclk"))
            st = amdsmi_get_gpu_mem_overdrive_level(D(devs[i])->h, &od);
        else
            st = amdsmi_get_gpu_overdrive_level(D(devs[i])->h, &od);
        if (!ok(st)) continue;
        print_logf(devs[i], str_eq(which, "mclk") ? "GPU Memory OverDrive value (%)"
                                                   : "GPU OverDrive value (%)",
                   "%u", od);
    }
    print_spacer(NULL, "=", 0);
}

static void show_maxpower(const int *devs, int nd) {
    int i;
    print_spacer(" Power Cap ", "=", 0);
    for (i = 0; i < nd; i++) {
        double w;
        if (power_cap_w(devs[i], &w) == 0)
            print_logf(devs[i], "Max Graphics Package Power (W)", "%.1f", w);
        else
            print_log(devs[i], "Max Graphics Package Power Unsupported", NULL);
    }
    print_spacer(NULL, "=", 0);
}

static void show_bus(const int *devs, int nd) {
    int i;
    print_spacer(" PCI Bus ID ", "=", 0);
    for (i = 0; i < nd; i++) {
        char b[32];
        bdf_str(devs[i], b, sizeof b);
        print_log(devs[i], "PCI Bus", b);
    }
    print_spacer(NULL, "=", 0);
}

static void show_meminfo(const int *devs, int nd, char **types, int nt) {
    static const struct { const char *n; amdsmi_memory_type_t t; } all[] = {
        {"VRAM", AMDSMI_MEM_TYPE_VRAM},
        {"VIS_VRAM", AMDSMI_MEM_TYPE_VIS_VRAM},
        {"GTT", AMDSMI_MEM_TYPE_GTT},
    };
    int i, k, t;
    print_spacer(" Memory Usage (Bytes) ", "=", 0);
    for (i = 0; i < nd; i++) {
        for (t = 0; t < (nt ? nt : 3); t++) {
            const char *name = nt ? types[t] : all[t].n;
            amdsmi_memory_type_t mt = AMDSMI_MEM_TYPE_VRAM;
            uint64_t u = 0, tot = 0;
            int found = 0;
            char up[64];
            for (k = 0; k < 3; k++)
                if (str_ieq(name, all[k].n) || str_ieq(name, "all")) {
                    if (str_ieq(name, "all")) {
                        /* handled by expanding below */
                    }
                    mt = all[k].t;
                    found = 1;
                    name = all[k].n;
                    break;
                }
            if (str_ieq(types && nt ? types[0] : "", "all") && nt == 1) {
                /* fall through to all types once */
            }
            if (!found && !(nt == 1 && str_ieq(types[0], "all"))) {
                print_logf(devs[i], "Invalid memory type", "%s", types[t]);
                continue;
            }
            if (nt == 1 && str_ieq(types[0], "all")) {
                int a;
                for (a = 0; a < 3; a++) {
                    mem_bytes(devs[i], all[a].t, &u, &tot);
                    snprintf(up, sizeof up, "%s Total Memory (B)", all[a].n);
                    print_logf(devs[i], up, "%" PRIu64, tot);
                    snprintf(up, sizeof up, "%s Total Used Memory (B)", all[a].n);
                    print_logf(devs[i], up, "%" PRIu64, u);
                }
                break;
            }
            mem_bytes(devs[i], mt, &u, &tot);
            snprintf(up, sizeof up, "%s Total Memory (B)", name);
            print_logf(devs[i], up, "%" PRIu64, tot);
            snprintf(up, sizeof up, "%s Total Used Memory (B)", name);
            print_logf(devs[i], up, "%" PRIu64, u);
        }
    }
    print_spacer(NULL, "=", 0);
}

static void show_memvendor(const int *devs, int nd) {
    int i;
    print_spacer(" Memory Vendor ", "=", 0);
    for (i = 0; i < nd; i++) {
        char brand[256] = {0};
        if (ok(amdsmi_get_gpu_vram_vendor(D(devs[i])->h, brand, sizeof brand)) && brand[0])
            print_log(devs[i], "GPU memory vendor", brand);
    }
    print_spacer(NULL, "=", 0);
}

static void show_product(const int *devs, int nd) {
    int i;
    print_spacer(" Product Info ", "=", 0);
    for (i = 0; i < nd; i++) {
        int d = devs[i];
        amdsmi_asic_info_t a;
        amdsmi_vbios_info_t v;
        char did[32], rev[16], sub[16], guid[40], gfx[32] = "N/A", sku[64] = "N/A";
        memset(&a, 0, sizeof a);
        memset(&v, 0, sizeof v);
        amdsmi_get_gpu_asic_info(D(d)->h, &a);
        amdsmi_get_gpu_vbios_info(D(d)->h, &v);
        device_id_str(d, did, sizeof did);
        if (a.rev_id != 0xFFFFFFFFu) snprintf(rev, sizeof rev, "0x%02x", a.rev_id);
        else snprintf(rev, sizeof rev, "N/A");
        if (a.subsystem_id) snprintf(sub, sizeof sub, "0x%04x", a.subsystem_id);
        else snprintf(sub, sizeof sub, "N/A");
        guid_str(d, guid, sizeof guid);
        if (a.target_graphics_version && a.target_graphics_version != UINT64_MAX)
            snprintf(gfx, sizeof gfx, "gfx%" PRIx64, a.target_graphics_version);
        {
            const char *part = v.part_number[0] ? v.part_number : v.version;
            char *p1 = strchr(part, '-');
            if (p1 && strchr(p1 + 1, '-')) {
                char *p2 = strchr(p1 + 1, '-');
                size_t L = (size_t)(p2 - (p1 + 1));
                if (L > 1 && L < sizeof sku) {
                    memcpy(sku, p1 + 1, L);
                    sku[L] = 0;
                }
            }
        }
        print_logf(d, "Card Series", "\t\t%s", a.market_name[0] ? a.market_name : "N/A");
        print_logf(d, "Card Model", "\t\t%s", did);
        print_logf(d, "Card Vendor", "\t\t%s", a.vendor_name[0] ? a.vendor_name : "N/A");
        print_logf(d, "Card SKU", "\t\t%s", sku);
        print_logf(d, "Subsystem ID", "\t%s", sub);
        print_logf(d, "Device Rev", "\t\t%s", rev);
        print_logf(d, "Node ID", "\t\t%u", node_id(d));
        print_logf(d, "GUID", "\t\t%s", guid);
        print_logf(d, "GFX Version", "\t\t%s", gfx);
    }
    print_spacer(NULL, "=", 0);
}

static void show_serial(const int *devs, int nd) {
    int i;
    print_spacer(" Serial Number ", "=", 0);
    for (i = 0; i < nd; i++) {
        amdsmi_asic_info_t a;
        amdsmi_board_info_t b;
        const char *sn = "N/A";
        memset(&a, 0, sizeof a);
        memset(&b, 0, sizeof b);
        amdsmi_get_gpu_asic_info(D(devs[i])->h, &a);
        amdsmi_get_gpu_board_info(D(devs[i])->h, &b);
        if (b.product_serial[0] && strcmp(b.product_serial, "N/A") != 0) sn = b.product_serial;
        print_log(devs[i], "Serial Number", sn);
        (void)a;
    }
    print_spacer(NULL, "=", 0);
}

static void show_uid(const int *devs, int nd) {
    int i;
    print_spacer(" Unique ID ", "=", 0);
    for (i = 0; i < nd; i++) {
        char buf[80];
        unique_id_str(devs[i], buf, sizeof buf);
        print_log(devs[i], "Unique ID", buf);
    }
    print_spacer(NULL, "=", 0);
}

static void show_fw(const int *devs, int nd, char **blocks, int nb) {
    int i, k;
    print_spacer(" Firmware Information ", "=", 0);
    for (i = 0; i < nd; i++) {
        amdsmi_fw_info_t fw;
        memset(&fw, 0, sizeof fw);
        if (!ok(amdsmi_get_fw_info(D(devs[i])->h, &fw))) continue;
        for (k = 0; k < fw.num_fw_info && k < AMDSMI_FW_ID__MAX; k++) {
            const char *nm = fw_name(fw.fw_info_list[k].fw_id);
            char ver[64], metric[80];
            int want = (nb == 0);
            int b;
            if (!nm || fw.fw_info_list[k].fw_version == 0) continue;
            for (b = 0; b < nb; b++)
                if (str_ieq(blocks[b], "all") || str_ieq(blocks[b], nm)) want = 1;
            if (!want) continue;
            fmt_fw(nm, fw.fw_info_list[k].fw_version, ver, sizeof ver);
            snprintf(metric, sizeof metric, "%s firmware version", nm);
            if (strlen(nm) <= 2) print_logf(devs[i], metric, "\t\t%s", ver);
            else print_logf(devs[i], metric, "\t%s", ver);
        }
    }
    print_spacer(NULL, "=", 0);
}

static void show_range(const int *devs, int nd, const char *which) {
    int i;
    char title[64];
    snprintf(title, sizeof title, " Show Valid %s Range ", which);
    print_spacer(title, "=", 0);
    for (i = 0; i < nd; i++) {
        amdsmi_od_volt_freq_data_t od;
        memset(&od, 0, sizeof od);
        if (!ok(amdsmi_get_gpu_od_volt_info(D(devs[i])->h, &od))) {
            print_logf(devs[i], NULL, "Unable to display %s range", which);
            continue;
        }
        if (str_eq(which, "sclk") || str_eq(which, "mclk")) {
            amdsmi_range_t *r = str_eq(which, "sclk") ? &od.curr_sclk_range : &od.curr_mclk_range;
            uint64_t lo = r->lower_bound, hi = r->upper_bound;
            if (lo > 1000000) lo /= 1000000;
            if (hi > 1000000) hi /= 1000000;
            if (lo == UINT64_MAX || hi == UINT64_MAX)
                print_logf(devs[i], NULL, "Unable to display %s range", which);
            else
                print_logf(devs[i], NULL, "Valid %s range: %" PRIu64 "Mhz - %" PRIu64 "Mhz",
                           which, lo, hi);
        } else {
            uint32_t nr = od.num_regions;
            amdsmi_freq_volt_region_t *regs;
            uint32_t j;
            if (nr == 0) {
                print_log(devs[i], "Voltage curve regions unsupported.", NULL);
                continue;
            }
            regs = xmalloc(nr * sizeof *regs);
            if (ok(amdsmi_get_gpu_od_volt_curve_regions(D(devs[i])->h, &nr, regs))) {
                for (j = 0; j < nr; j++)
                    print_logf(devs[i], NULL,
                               "Region %u: Valid voltage range: %" PRIu64 "mV - %" PRIu64 "mV",
                               j, regs[j].volt_range.lower_bound, regs[j].volt_range.upper_bound);
            }
            free(regs);
        }
    }
    print_spacer(NULL, "=", 0);
}

static void show_vc(const int *devs, int nd) {
    int i, p;
    print_spacer(" Voltage Curve Points ", "=", 0);
    for (i = 0; i < nd; i++) {
        amdsmi_od_volt_freq_data_t od;
        memset(&od, 0, sizeof od);
        if (!ok(amdsmi_get_gpu_od_volt_info(D(devs[i])->h, &od))) continue;
        for (p = 0; p < AMDSMI_NUM_VOLTAGE_CURVE_POINTS; p++) {
            uint64_t hz = od.curve.vc_points[p].frequency;
            uint64_t mv = od.curve.vc_points[p].voltage;
            if (hz > 1000000) hz /= 1000000;
            print_logf(devs[i], NULL, "Voltage point %d: %" PRIu64 "Mhz %" PRIu64 "mV", p, hz, mv);
        }
    }
    print_spacer(NULL, "=", 0);
}

static void show_clkvolt(const int *devs, int nd) {
    int i;
    if (g_json) return;
    print_spacer(" GPU Memory clock frequencies and voltages ", "=", 0);
    for (i = 0; i < nd; i++) {
        amdsmi_od_volt_freq_data_t od;
        uint64_t s0, s1, m0, m1;
        memset(&od, 0, sizeof od);
        if (!ok(amdsmi_get_gpu_od_volt_info(D(devs[i])->h, &od))) continue;
        s0 = od.curr_sclk_range.lower_bound;
        s1 = od.curr_sclk_range.upper_bound;
        m0 = od.curr_mclk_range.lower_bound;
        m1 = od.curr_mclk_range.upper_bound;
        if (s0 > 1000000) s0 /= 1000000;
        if (s1 > 1000000) s1 /= 1000000;
        if (m0 > 1000000) m0 /= 1000000;
        if (m1 > 1000000) m1 /= 1000000;
        print_log(devs[i], "OD_SCLK:", NULL);
        print_logf(devs[i], NULL, "0: %" PRIu64 "Mhz", s0);
        print_logf(devs[i], NULL, "1: %" PRIu64 "Mhz", s1);
        print_log(devs[i], "OD_MCLK:", NULL);
        print_logf(devs[i], NULL, "0: %" PRIu64 "Mhz", m0);
        print_logf(devs[i], NULL, "1: %" PRIu64 "Mhz", m1);
        if (od.sclk_freq_limits.upper_bound || od.mclk_freq_limits.upper_bound)
            print_log(devs[i], "OD_RANGE:", NULL);
        if (od.sclk_freq_limits.upper_bound) {
            uint64_t a = od.sclk_freq_limits.lower_bound, b = od.sclk_freq_limits.upper_bound;
            if (a > 1000000) a /= 1000000;
            if (b > 1000000) b /= 1000000;
            print_logf(devs[i], NULL, "SCLK:     %" PRIu64 "Mhz        %" PRIu64 "Mhz", a, b);
        }
        if (od.mclk_freq_limits.upper_bound) {
            uint64_t a = od.mclk_freq_limits.lower_bound, b = od.mclk_freq_limits.upper_bound;
            if (a > 1000000) a /= 1000000;
            if (b > 1000000) b /= 1000000;
            print_logf(devs[i], NULL, "MCLK:     %" PRIu64 "Mhz        %" PRIu64 "Mhz", a, b);
        }
    }
    print_spacer(NULL, "=", 0);
}

static void show_profile(const int *devs, int nd) {
    int i, bit;
    if (g_json) return;
    print_spacer(" Show Power Profiles ", "=", 0);
    for (i = 0; i < nd; i++) {
        amdsmi_power_profile_status_t st;
        int shown = 0;
        memset(&st, 0, sizeof st);
        if (!ok(amdsmi_get_gpu_power_profile_presets(D(devs[i])->h, 0, &st))) continue;
        for (bit = 0; bit < 7; bit++) {
            uint64_t mask = 1ull << bit;
            if (st.available_profiles & mask) {
                shown++;
                print_logf(devs[i], NULL, "%d. Available power profile (#%d of 7): %s%s",
                           shown, bit + 1, profile_name(mask),
                           (st.current == mask) ? "*" : "");
            }
        }
    }
    print_spacer(NULL, "=", 0);
}

static void show_bw(const int *devs, int nd) {
    int i;
    print_spacer(" Measured PCIe Bandwidth ", "=", 0);
    for (i = 0; i < nd; i++) {
        amdsmi_pcie_info_t p;
        uint64_t sent = 0, recv = 0, maxsz = 0;
        memset(&p, 0, sizeof p);
        if (ok(amdsmi_get_pcie_info(D(devs[i])->h, &p)) && p.pcie_metric.pcie_bandwidth)
            print_logf(devs[i], "Current PCIe bandwidth (MB/s)", "%.3f",
                       (double)p.pcie_metric.pcie_bandwidth / 8.0);
        else if (ok(amdsmi_get_gpu_pci_throughput(D(devs[i])->h, &sent, &recv, &maxsz))) {
            double bw = ((double)(sent + recv) * (double)maxsz) / 1024.0 / 1024.0;
            print_logf(devs[i], "Estimated maximum PCIe bandwidth over the last second (MB/s)",
                       "%.3f", bw);
        }
    }
    print_spacer(NULL, "=", 0);
}

static void show_replay(const int *devs, int nd) {
    int i;
    print_spacer(" PCIe Replay Counter ", "=", 0);
    for (i = 0; i < nd; i++) {
        uint64_t c = 0;
        if (ok(amdsmi_get_gpu_pci_replay_counter(D(devs[i])->h, &c)))
            print_logf(devs[i], "PCIe Replay Count", "%" PRIu64, c);
    }
    print_spacer(NULL, "=", 0);
}

static void show_energy(const int *devs, int nd) {
    int i;
    print_spacer(" Consumed Energy ", "=", 0);
    for (i = 0; i < nd; i++) {
        uint64_t e = 0, ts = 0;
        float res = 0;
        if (ok(amdsmi_get_energy_count(D(devs[i])->h, &e, &res, &ts))) {
            print_logf(devs[i], "Energy counter", "%" PRIu64, e);
            print_logf(devs[i], "Accumulated Energy (uJ)", "%.2f", (double)e * (double)res);
        }
    }
    print_spacer(NULL, "=", 0);
}

static void show_pages(const int *devs, int nd, const char *which) {
    int i;
    print_spacer(" Pages Info ", "=", 0);
    for (i = 0; i < nd; i++) {
        uint32_t n = 0, r;
        amdsmi_retired_page_record_t *recs;
        int any = 0;
        if (!ok(amdsmi_get_gpu_memory_reserved_pages(D(devs[i])->h, &n, NULL))) continue;
        if (n == 0) continue;
        recs = xmalloc(n * sizeof *recs);
        if (!ok(amdsmi_get_gpu_memory_reserved_pages(D(devs[i])->h, &n, recs))) {
            free(recs);
            continue;
        }
        printf("\nGPU[%d]: \t%s PAGES INFO\n", devs[i], which ? which : "ALL");
        printf("    Page address   Page size    Status\n");
        for (r = 0; r < n; r++) {
            const char *st = "reserved";
            if (recs[r].status == AMDSMI_MEM_PAGE_STATUS_PENDING) st = "pending";
            else if (recs[r].status == AMDSMI_MEM_PAGE_STATUS_UNRESERVABLE) st = "unreservable";
            if (which && !str_eq(which, "all") && !str_eq(which, st)) continue;
            printf("    0x%" PRIx64 "   0x%" PRIx64 "    %s\n",
                   recs[r].page_address, recs[r].page_size, st);
            any = 1;
        }
        (void)any;
        free(recs);
    }
    print_spacer(NULL, "=", 0);
}

static void show_ras(const int *devs, int nd, char **blocks, int nb) {
    int i, k;
    print_spacer(" RAS Info ", "=", 0);
    for (i = 0; i < nd; i++) {
        for (k = 0; k < NRAS; k++) {
            int want = (nb == 0);
            int b;
            amdsmi_ras_err_state_t st;
            amdsmi_error_count_t ec;
            for (b = 0; b < nb; b++)
                if (str_ieq(blocks[b], "all") || str_ieq(blocks[b], RAS[k].name)) want = 1;
            if (!want) continue;
            if (!ok(amdsmi_get_gpu_ecc_status(D(devs[i])->h, RAS[k].bit, &st))) continue;
            print_logf(devs[i], NULL, "Block %s ECC: %s", RAS[k].name, ras_state_name(st));
            memset(&ec, 0, sizeof ec);
            if (ok(amdsmi_get_gpu_ecc_count(D(devs[i])->h, RAS[k].bit, &ec))) {
                print_logf(devs[i], NULL, "Block %s correctable: %" PRIu64, RAS[k].name,
                           ec.correctable_count);
                print_logf(devs[i], NULL, "Block %s uncorrectable: %" PRIu64, RAS[k].name,
                           ec.uncorrectable_count);
            }
        }
    }
    print_spacer(NULL, "=", 0);
}

static void show_xgmi(const int *devs, int nd) {
    int i;
    print_spacer(" XGMI Error Status ", "=", 0);
    for (i = 0; i < nd; i++) {
        amdsmi_xgmi_status_t st;
        if (!ok(amdsmi_gpu_xgmi_error_status(D(devs[i])->h, &st))) continue;
        print_log(devs[i], "XGMI Error status",
                  st == AMDSMI_XGMI_STATUS_NO_ERRORS ? "No errors" :
                  st == AMDSMI_XGMI_STATUS_ERROR ? "Error" : "Multiple errors");
    }
    print_spacer(NULL, "=", 0);
}

static void show_partitions(const int *devs, int nd, int which) {
    int i;
    print_spacer(which ? " Memory Partition " : " Compute Partition ", "=", 0);
    for (i = 0; i < nd; i++) {
        char buf[64];
        if (which) {
            mem_part(devs[i], buf, sizeof buf);
            print_log(devs[i], "Memory Partition", buf);
        } else {
            comp_part(devs[i], buf, sizeof buf);
            print_log(devs[i], "Compute Partition", buf);
        }
    }
    print_spacer(NULL, "=", 0);
}

static void topo_header(const int *devs, int nd) {
    int i;
    printf("      ");
    for (i = 0; i < nd; i++) printf("%-12s ", " ");
    /* reprint properly */
    printf("\r      ");
    for (i = 0; i < nd; i++) printf("GPU%-9d ", devs[i]);
    printf("\n");
}

static void show_topo_weight(const int *devs, int nd) {
    int a, b;
    print_spacer(" Weight between two GPUs ", "=", 0);
    if (!g_json) topo_header(devs, nd);
    for (a = 0; a < nd; a++) {
        if (!g_json) printf("GPU%-3d ", devs[a]);
        for (b = 0; b < nd; b++) {
            uint64_t w = 0;
            char cell[32] = "0";
            if (a != b) {
                if (ok(amdsmi_topo_get_link_weight(D(devs[a])->h, D(devs[b])->h, &w)))
                    snprintf(cell, sizeof cell, "%" PRIu64, w);
                else
                    snprintf(cell, sizeof cell, "N/A");
            }
            if (g_json) {
                char key[96];
                snprintf(key, sizeof key, "(Topology) Weight between DRM devices %d and %d",
                         devs[a], devs[b]);
                json_add(-1, key, cell);
            } else
                printf("%-12s ", cell);
        }
        if (!g_json) printf("\n");
    }
}

static void show_topo_hops(const int *devs, int nd) {
    int a, b;
    print_spacer(" Hops between two GPUs ", "=", 0);
    if (!g_json) topo_header(devs, nd);
    for (a = 0; a < nd; a++) {
        if (!g_json) printf("GPU%-3d ", devs[a]);
        for (b = 0; b < nd; b++) {
            uint64_t hops = 0;
            amdsmi_link_type_t ty = AMDSMI_LINK_TYPE_UNKNOWN;
            char cell[32] = "0";
            if (a != b) {
                if (ok(amdsmi_topo_get_link_type(D(devs[a])->h, D(devs[b])->h, &hops, &ty)))
                    snprintf(cell, sizeof cell, "%" PRIu64, hops);
                else
                    snprintf(cell, sizeof cell, "N/A");
            }
            if (g_json) {
                char key[96];
                snprintf(key, sizeof key, "(Topology) Hops between DRM devices %d and %d",
                         devs[a], devs[b]);
                json_add(-1, key, cell);
            } else
                printf("%-12s ", cell);
        }
        if (!g_json) printf("\n");
    }
}

static void show_topo_type(const int *devs, int nd) {
    int a, b;
    print_spacer(" Link Type between two GPUs ", "=", 0);
    if (!g_json) topo_header(devs, nd);
    for (a = 0; a < nd; a++) {
        if (!g_json) printf("GPU%-3d ", devs[a]);
        for (b = 0; b < nd; b++) {
            uint64_t hops = 0;
            amdsmi_link_type_t ty = AMDSMI_LINK_TYPE_UNKNOWN;
            const char *cell = "0";
            char store[32];
            if (a != b) {
                if (ok(amdsmi_topo_get_link_type(D(devs[a])->h, D(devs[b])->h, &hops, &ty)))
                    cell = link_type_name(ty);
                else
                    cell = "XXXX";
            }
            snprintf(store, sizeof store, "%s", cell);
            if (g_json) {
                char key[96];
                snprintf(key, sizeof key, "(Topology) Link type between DRM devices %d and %d",
                         devs[a], devs[b]);
                json_add(-1, key, store);
            } else
                printf("%-12s ", store);
        }
        if (!g_json) printf("\n");
    }
}

static void show_topo_access(const int *devs, int nd) {
    int a, b;
    print_spacer(" Link accessibility between GPUs ", "=", 0);
    if (!g_json) topo_header(devs, nd);
    for (a = 0; a < nd; a++) {
        if (!g_json) printf("GPU%-3d ", devs[a]);
        for (b = 0; b < nd; b++) {
            bool acc = false;
            const char *cell = "true";
            if (a != b) {
                if (!ok(amdsmi_is_P2P_accessible(D(devs[a])->h, D(devs[b])->h, &acc)))
                    cell = "N/A";
                else
                    cell = acc ? "true" : "false";
            }
            if (g_json) {
                char key[96];
                snprintf(key, sizeof key, "(Topology) Access between DRM devices %d and %d",
                         devs[a], devs[b]);
                json_add(-1, key, cell);
            } else
                printf("%-12s ", cell);
        }
        if (!g_json) printf("\n");
    }
}

static void show_numa(const int *devs, int nd) {
    int i;
    print_spacer(" Numa Nodes ", "=", 0);
    for (i = 0; i < nd; i++) {
        uint32_t n = 0;
        int32_t aff = 0;
        if (ok(amdsmi_topo_get_numa_node_number(D(devs[i])->h, &n)))
            print_logf(devs[i], "(Topology) Numa Node", "%u", n);
        if (ok(amdsmi_get_gpu_topo_numa_affinity(D(devs[i])->h, &aff)))
            print_logf(devs[i], "(Topology) Numa Affinity", "%d", aff);
    }
}

static void show_nodes_bw(const int *devs, int nd) {
    int a, b;
    print_spacer(" Bandwidth table (GB/s) ", "=", 0);
    if (!g_json) topo_header(devs, nd);
    for (a = 0; a < nd; a++) {
        if (!g_json) printf("GPU%-3d ", devs[a]);
        for (b = 0; b < nd; b++) {
            uint64_t mn = 0, mx = 0;
            char cell[32] = "0";
            if (a != b &&
                ok(amdsmi_get_minmax_bandwidth_between_processors(D(devs[a])->h, D(devs[b])->h, &mn, &mx)))
                snprintf(cell, sizeof cell, "%" PRIu64, mx);
            else if (a != b)
                snprintf(cell, sizeof cell, "N/A");
            if (!g_json) printf("%-12s ", cell);
            else {
                char key[96];
                snprintf(key, sizeof key, "(Topology) Bandwidth between %d and %d", devs[a], devs[b]);
                json_add(-1, key, cell);
            }
        }
        if (!g_json) printf("\n");
    }
}

static void proc_name(uint32_t pid, char *buf, size_t n) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%u/comm", pid);
    if (read_file(path, buf, n) < 0) snprintf(buf, n, "UNKNOWN");
}

static void show_pids(const char *verbose) {
    uint32_t maxp = 0, i;
    amdsmi_proc_info_t *list = NULL;
    amdsmi_processor_handle hs[MAX_DEV];
    int d;
    print_spacer(" KFD Processes ", "=", 0);
    for (d = 0; d < g_ndev; d++) hs[d] = g_devs[d].h;
    if (!ok(amdsmi_get_gpu_process_list(g_devs[0].h, &maxp, NULL)) || maxp == 0) {
        print_log(-1, "No KFD PIDs currently running", NULL);
        print_spacer(NULL, "=", 0);
        return;
    }
    list = xmalloc(maxp * sizeof *list);
    if (!ok(amdsmi_get_gpu_process_list(g_devs[0].h, &maxp, list))) {
        free(list);
        print_log(-1, "No KFD PIDs currently running", NULL);
        print_spacer(NULL, "=", 0);
        return;
    }
    print_log(-1, "KFD process information:", NULL);
    if (verbose && str_eq(verbose, "details"))
        printf("%-8s %-16s %-8s %-12s %-12s %s\n", "PID", "PROCESS NAME", "GPU",
               "VRAM USED", "SDMA USED", "CU OCCUPANCY");
    else
        printf("%-8s %-16s %-8s %-12s %-12s %s\n", "PID", "PROCESS NAME", "GPU(s)",
               "VRAM USED", "SDMA USED", "CU OCCUPANCY");
    for (i = 0; i < maxp; i++) {
        char name[64];
        proc_name(list[i].pid, name, sizeof name);
        printf("%-8u %-16s %-8d %-12" PRIu64 " %-12" PRIu64 " %u\n",
               list[i].pid, name, g_ndev, list[i].memory_usage.vram_mem, list[i].sdma_usage,
               list[i].cu_occupancy == 0xFFFFFFFFu ? 0 : list[i].cu_occupancy);
    }
    free(list);
    (void)hs;
    print_spacer(NULL, "=", 0);
}

static void show_pidgpus(char **pids, int np) {
    uint32_t maxp = 0, i, p;
    amdsmi_proc_info_by_pid_t *procs;
    amdsmi_processor_handle hs[MAX_DEV];
    print_spacer(" GPUs Indexed by PID ", "=", 0);
    for (i = 0; i < (uint32_t)g_ndev; i++) hs[i] = g_devs[i].h;
    if (!ok(amdsmi_get_gpu_process_list_by_pid(hs, (uint32_t)g_ndev, NULL, &maxp)) || maxp == 0) {
        print_log(-1, "No KFD PIDs currently running", NULL);
        print_spacer(NULL, "=", 0);
        return;
    }
    procs = xmalloc(maxp * sizeof *procs);
    if (!ok(amdsmi_get_gpu_process_list_by_pid(hs, (uint32_t)g_ndev, procs, &maxp))) {
        free(procs);
        print_log(-1, "Unable to get list of KFD PIDs. A kernel update may be needed", NULL);
        print_spacer(NULL, "=", 0);
        return;
    }
    for (p = 0; p < maxp; p++) {
        int want = (np == 0);
        int a;
        char line[256];
        int pos = 0;
        for (a = 0; a < np; a++)
            if ((uint32_t)atoi(pids[a]) == procs[p].pid) want = 1;
        if (!want) continue;
        pos = snprintf(line, sizeof line, "PID %u is using %u DRM device(s):", procs[p].pid,
                       procs[p].num_gpus);
        for (i = 0; i < procs[p].num_gpus && pos < (int)sizeof line - 8; i++)
            pos += snprintf(line + pos, sizeof line - (size_t)pos, " %u", procs[p].gpus[i].gpu_index);
        print_log(-1, line, NULL);
    }
    free(procs);
    print_spacer(NULL, "=", 0);
}

static void u16_list(const uint16_t *v, int n, char *buf, size_t nb) {
    int i;
    buf[0] = '[';
    buf[1] = 0;
    for (i = 0; i < n; i++) {
        char t[32];
        if (v[i] == 0xFFFF) snprintf(t, sizeof t, "N/A");
        else snprintf(t, sizeof t, "%u", v[i]);
        if (i) strncat(buf, ", ", nb - strlen(buf) - 1);
        strncat(buf, t, nb - strlen(buf) - 1);
    }
    strncat(buf, "]", nb - strlen(buf) - 1);
}

static void u64_list(const uint64_t *v, int n, char *buf, size_t nb) {
    int i;
    buf[0] = '[';
    buf[1] = 0;
    for (i = 0; i < n; i++) {
        char t[32];
        if (v[i] == UINT64_MAX) snprintf(t, sizeof t, "N/A");
        else snprintf(t, sizeof t, "%" PRIu64, v[i]);
        if (i) strncat(buf, ", ", nb - strlen(buf) - 1);
        strncat(buf, t, nb - strlen(buf) - 1);
    }
    strncat(buf, "]", nb - strlen(buf) - 1);
}

static void show_metrics(const int *devs, int nd) {
    int i;
    print_spacer(" GPU Metrics ", "=", 0);
    for (i = 0; i < nd; i++) {
        amdsmi_gpu_metrics_t *m = xmalloc(sizeof *m);
        char b1[32], b2[32], list[512];
        memset(m, 0, sizeof *m);
        if (!ok(amdsmi_get_gpu_metrics_info(D(devs[i])->h, m))) {
            free(m);
            continue;
        }
        print_logf(devs[i], "Metric Version and Size (Bytes)", "%u.%u %u",
                   m->common_header.format_revision, m->common_header.content_revision,
                   m->common_header.structure_size);
#define PM(name, unit, expr)                                                      \
        do {                                                                      \
            char _vb[64];                                                         \
            const char *_s = (expr);                                              \
            char _metric[96];                                                     \
            snprintf(_metric, sizeof _metric, "%s (%s)", name, unit);             \
            print_log(devs[i], _metric, _s);                                      \
            (void)_vb;                                                            \
        } while (0)
        PM("temperature_edge", "C", na_u16(m->temperature_edge, b1, sizeof b1));
        PM("temperature_hotspot", "C", na_u16(m->temperature_hotspot, b1, sizeof b1));
        PM("temperature_mem", "C", na_u16(m->temperature_mem, b1, sizeof b1));
        PM("temperature_vrgfx", "C", na_u16(m->temperature_vrgfx, b1, sizeof b1));
        PM("temperature_vrsoc", "C", na_u16(m->temperature_vrsoc, b1, sizeof b1));
        PM("temperature_vrmem", "C", na_u16(m->temperature_vrmem, b1, sizeof b1));
        PM("average_gfx_activity", "%", na_u16(m->average_gfx_activity, b1, sizeof b1));
        PM("average_umc_activity", "%", na_u16(m->average_umc_activity, b1, sizeof b1));
        PM("average_mm_activity", "%", na_u16(m->average_mm_activity, b1, sizeof b1));
        PM("average_socket_power", "W", na_u16(m->average_socket_power, b1, sizeof b1));
        PM("energy_accumulator", "15.259uJ (2^-16)", na_u64(m->energy_accumulator, b1, sizeof b1));
        PM("system_clock_counter", "ns", na_u64(m->system_clock_counter, b1, sizeof b1));
        PM("average_gfxclk_frequency", "MHz", na_u16(m->average_gfxclk_frequency, b1, sizeof b1));
        PM("average_socclk_frequency", "MHz", na_u16(m->average_socclk_frequency, b1, sizeof b1));
        PM("average_uclk_frequency", "MHz", na_u16(m->average_uclk_frequency, b1, sizeof b1));
        PM("average_vclk0_frequency", "MHz", na_u16(m->average_vclk0_frequency, b1, sizeof b1));
        PM("average_dclk0_frequency", "MHz", na_u16(m->average_dclk0_frequency, b1, sizeof b1));
        PM("average_vclk1_frequency", "MHz", na_u16(m->average_vclk1_frequency, b1, sizeof b1));
        PM("average_dclk1_frequency", "MHz", na_u16(m->average_dclk1_frequency, b1, sizeof b1));
        PM("current_gfxclk", "MHz", na_u16(m->current_gfxclk, b1, sizeof b1));
        PM("current_socclk", "MHz", na_u16(m->current_socclk, b1, sizeof b1));
        PM("current_uclk", "MHz", na_u16(m->current_uclk, b1, sizeof b1));
        PM("current_vclk0", "MHz", na_u16(m->current_vclk0, b1, sizeof b1));
        PM("current_dclk0", "MHz", na_u16(m->current_dclk0, b1, sizeof b1));
        PM("current_vclk1", "MHz", na_u16(m->current_vclk1, b1, sizeof b1));
        PM("current_dclk1", "MHz", na_u16(m->current_dclk1, b1, sizeof b1));
        PM("throttle_status", "N/A", na_u32(m->throttle_status, b1, sizeof b1));
        PM("current_fan_speed", "rpm", na_u16(m->current_fan_speed, b1, sizeof b1));
        PM("pcie_link_width", "Lanes", na_u16(m->pcie_link_width, b1, sizeof b1));
        PM("pcie_link_speed", "0.1 GT/s", na_u16(m->pcie_link_speed, b1, sizeof b1));
        PM("voltage_soc", "mV", na_u16(m->voltage_soc, b1, sizeof b1));
        PM("voltage_gfx", "mV", na_u16(m->voltage_gfx, b1, sizeof b1));
        PM("voltage_mem", "mV", na_u16(m->voltage_mem, b1, sizeof b1));
        PM("current_socket_power", "W", na_u16(m->current_socket_power, b1, sizeof b1));
        u16_list(m->vcn_activity, AMDSMI_MAX_NUM_VCN, list, sizeof list);
        print_log(devs[i], "vcn_activity (%)", list);
        PM("xgmi_link_width", "GB/s", na_u16(m->xgmi_link_width, b1, sizeof b1));
        PM("xgmi_link_speed", "Gbps", na_u16(m->xgmi_link_speed, b1, sizeof b1));
        PM("pcie_bandwidth_acc", "GB/s", na_u64(m->pcie_bandwidth_acc, b1, sizeof b1));
        PM("pcie_bandwidth_inst", "GB/s", na_u64(m->pcie_bandwidth_inst, b1, sizeof b1));
        PM("pcie_replay_count_acc", "Count", na_u64(m->pcie_replay_count_acc, b1, sizeof b1));
        u64_list(m->xgmi_read_data_acc, AMDSMI_MAX_NUM_XGMI_LINKS, list, sizeof list);
        print_log(devs[i], "xgmi_read_data_acc (kB)", list);
        u64_list(m->xgmi_write_data_acc, AMDSMI_MAX_NUM_XGMI_LINKS, list, sizeof list);
        print_log(devs[i], "xgmi_write_data_acc (kB)", list);
        u16_list(m->current_gfxclks, AMDSMI_MAX_NUM_GFX_CLKS, list, sizeof list);
        print_log(devs[i], "current_gfxclks (MHz)", list);
        u16_list(m->current_socclks, AMDSMI_MAX_NUM_CLKS, list, sizeof list);
        print_log(devs[i], "current_socclks (MHz)", list);
        u16_list(m->jpeg_activity, 8, list, sizeof list);
        print_log(devs[i], "jpeg_activity (%)", list);
        PM("vram_max_bandwidth", "GB/s", na_u64(m->vram_max_bandwidth, b1, sizeof b1));
        PM("num_partition", "N/A", na_u16(m->num_partition, b1, sizeof b1));
        (void)b2;
#undef PM
        free(m);
    }
    print_spacer(NULL, "=", 0);
}

static void show_events(const int *devs, int nd, char **types, int nt) {
    static const char *names[] = {
        "VMFAULT", "THERMAL_THROTTLE", "GPU_PRE_RESET", "GPU_POST_RESET",
        "MIGRATE_START", "MIGRATE_END", "PAGE_FAULT_START", "PAGE_FAULT_END",
        "QUEUE_EVICTION", "QUEUE_RESTORE", "UNMAP_FROM_GPU", "PROCESS_START", "PROCESS_END"
    };
    uint64_t mask = 0;
    int i, k;
    print_spacer(" Show Events ", "=", 0);
    print_log(-1, "press 'q' and then Enter to quit", NULL);
    if (nt == 0) mask = (1ull << 13) - 1;
    else {
        for (k = 0; k < nt; k++) {
            int found = 0;
            for (i = 0; i < 13; i++)
                if (str_ieq(types[k], names[i])) { mask |= 1ull << i; found = 1; }
            if (!found) fprintf(stderr, "Ignoring unrecognized event type %s\n", types[k]);
        }
        if (!mask) mask = (1ull << 13) - 1;
    }
    printf("DEVICE\tTIME\tTYPE\tDESCRIPTION\n");
    for (i = 0; i < nd; i++) {
        amdsmi_init_gpu_event_notification(D(devs[i])->h);
        amdsmi_set_gpu_event_notification_mask(D(devs[i])->h, mask);
    }
    for (;;) {
        amdsmi_evt_notification_data_t data[8];
        uint32_t n = 8;
        fd_set rfds;
        struct timeval tv;
        int sel;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        sel = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);
        if (sel > 0) {
            char line[16];
            if (fgets(line, sizeof line, stdin) && (line[0] == 'q' || line[0] == 3)) break;
        }
        n = 8;
        if (ok(amdsmi_get_gpu_event_notification(1000, &n, data)) && n) {
            uint32_t e;
            for (e = 0; e < n; e++) {
                int di;
                const char *nm = "UNKNOWN";
                for (di = 0; di < g_ndev; di++)
                    if (g_devs[di].h == data[e].processor_handle) break;
                if (data[e].event >= 1 && data[e].event <= 13) nm = names[data[e].event - 1];
                printf("\rGPU[%d]:\t%s\t%s\n", di < g_ndev ? di : 0, nm, data[e].message);
            }
        }
    }
    for (i = 0; i < nd; i++) amdsmi_stop_gpu_event_notification(D(devs[i])->h);
    print_spacer(NULL, "=", 0);
}

static void show_tempgraph(const int *devs, int nd) {
    int cycle;
    print_spacer(" Temperature Graph ", "=", 0);
    print_log(-1, "press Ctrl-C to quit (showing 30 samples)", NULL);
    for (cycle = 0; cycle < 30; cycle++) {
        int i;
        int cols = 60;
        for (i = 0; i < nd; i++) {
            int good = 0;
            double t = temp_c(devs[i], AMDSMI_TEMPERATURE_TYPE_EDGE, &good);
            int pct, k;
            if (!good) t = temp_c(devs[i], AMDSMI_TEMPERATURE_TYPE_JUNCTION, &good);
            pct = good ? (int)t : 0;
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            printf("GPU[%d] Temp %3d\xC2\xB0" "C |", devs[i], pct);
            for (k = 0; k < cols; k++) putchar(k < (pct * cols) / 100 ? '#' : ' ');
            printf("|\n");
        }
        fflush(stdout);
        usleep(500000);
    }
    print_spacer(NULL, "=", 0);
}

/* -------------------------------------------------------------------------- */
/* set / reset                                                                */
/* -------------------------------------------------------------------------- */

static void set_perf(const int *devs, int nd, const char *level) {
    amdsmi_dev_perf_level_t p;
    int i;
    print_spacer(" Set Performance Level ", "=", 0);
    if (perf_from_name(level, &p) != 0) {
        print_logf(-1, "Invalid performance level", "%s", level);
        print_spacer(NULL, "=", 0);
        return;
    }
    for (i = 0; i < nd; i++) {
        amdsmi_status_t st = amdsmi_set_gpu_perf_level(D(devs[i])->h, p);
        if (ok(st)) print_logf(devs[i], NULL, "Successfully set performance level to %s", level);
        else status_line(devs[i], st, "set performance level");
    }
    print_spacer(NULL, "=", 0);
}

static void set_fan(const int *devs, int nd, const char *level) {
    int i;
    print_spacer(" Set Fan Speed ", "=", 0);
    for (i = 0; i < nd; i++) {
        uint64_t mx = 255, speed;
        char tmp[64];
        snprintf(tmp, sizeof tmp, "%s", level);
        if (!ok(amdsmi_get_gpu_fan_speed_max(D(devs[i])->h, 0, &mx)) || mx == 0) mx = 255;
        if (tmp[0] && tmp[strlen(tmp) - 1] == '%') {
            tmp[strlen(tmp) - 1] = 0;
            speed = (uint64_t)(atof(tmp) * (double)mx / 100.0);
        } else {
            speed = (uint64_t)strtoull(tmp, NULL, 0);
        }
        status_line(devs[i], amdsmi_set_gpu_fan_speed(D(devs[i])->h, 0, speed),
                    "Fan speed set");
        if (ok(amdsmi_get_gpu_fan_speed(D(devs[i])->h, 0, (int64_t *)&speed)))
            print_logf(devs[i], NULL, "Successfully set fan speed to %s", level);
    }
    print_spacer(NULL, "=", 0);
}

static void reset_fans(const int *devs, int nd) {
    int i;
    print_spacer(" Reset Fans ", "=", 0);
    for (i = 0; i < nd; i++) {
        amdsmi_status_t st = amdsmi_reset_gpu_fan(D(devs[i])->h, 0);
        if (ok(st)) print_log(devs[i], "Successfully reset fan speed to driver control", NULL);
        else status_line(devs[i], st, "reset fan");
    }
    print_spacer(NULL, "=", 0);
}

static void set_clocks(const int *devs, int nd, const char *type, int *levels, int nl) {
    amdsmi_clk_type_t ct;
    int i, k;
    uint64_t mask = 0;
    print_spacer(" Set Clock Frequency ", "=", 0);
    if (clk_lookup(type, &ct) != 0) {
        print_logf(-1, "Unsupported clock type", "%s", type);
        print_spacer(NULL, "=", 0);
        return;
    }
    for (k = 0; k < nl; k++)
        if (levels[k] >= 0 && levels[k] < 64) mask |= 1ull << levels[k];
    for (i = 0; i < nd; i++) {
        amdsmi_status_t st = amdsmi_set_clk_freq(D(devs[i])->h, ct, mask);
        if (ok(st)) print_logf(devs[i], NULL, "Successfully set %s frequency mask", type);
        else status_line(devs[i], st, "set clock");
    }
    print_spacer(NULL, "=", 0);
}

static void set_power_od(const int *devs, int nd, const char *watts, int reset) {
    int i;
    print_spacer(reset ? " Reset Power OverDrive " : " Set Power OverDrive ", "=", 0);
    if (!reset && !confirm_danger()) {
        print_log(-1, "Confirmation not given. Exiting without setting value", NULL);
        print_spacer(NULL, "=", 0);
        exit(1);
    }
    for (i = 0; i < nd; i++) {
        amdsmi_power_cap_info_t c;
        uint64_t cap;
        memset(&c, 0, sizeof c);
        amdsmi_get_power_cap_info(D(devs[i])->h, 0, &c);
        if (reset) cap = c.default_power_cap ? c.default_power_cap : c.power_cap;
        else {
            double w = atof(watts);
            cap = (c.power_cap > 100000) ? (uint64_t)(w * 1000000.0) : (uint64_t)w;
        }
        if (ok(amdsmi_set_power_cap(D(devs[i])->h, 0, cap)))
            print_logf(devs[i], NULL, "Successfully set power cap to %s", reset ? "default" : watts);
        else
            status_line(devs[i], AMDSMI_STATUS_NOT_SUPPORTED, "set power cap");
    }
    print_spacer(NULL, "=", 0);
}

static void set_overdrive(const int *devs, int nd, const char *pct) {
    int i;
    uint32_t od = (uint32_t)atoi(pct);
    print_spacer(" Set OverDrive ", "=", 0);
    if (!confirm_danger()) {
        print_log(-1, "Confirmation not given. Exiting without setting value", NULL);
        print_spacer(NULL, "=", 0);
        exit(1);
    }
    for (i = 0; i < nd; i++) {
        amdsmi_status_t st = amdsmi_set_gpu_overdrive_level(D(devs[i])->h, od);
        if (ok(st)) print_logf(devs[i], NULL, "Successfully set OverDrive to %u%%", od);
        else status_line(devs[i], st, "set overdrive");
    }
    print_spacer(NULL, "=", 0);
}

static void set_extremum(const int *devs, int nd, const char *which, const char *clk, const char *mhz) {
    amdsmi_clk_type_t ct;
    amdsmi_clk_limit_type_t lim;
    int i;
    print_spacer(" Set Valid sclk Extremum ", "=", 0);
    if (!confirm_danger()) {
        print_log(-1, "Confirmation not given. Exiting without setting value", NULL);
        exit(1);
    }
    if (clk_lookup(clk, &ct) != 0) {
        print_logf(-1, "Unsupported clock", "%s", clk);
        return;
    }
    lim = str_ieq(which, "min") ? AMDSMI_CLK_LIMIT_MIN : AMDSMI_CLK_LIMIT_MAX;
    for (i = 0; i < nd; i++) {
        amdsmi_status_t st = amdsmi_set_gpu_clk_limit(D(devs[i])->h, ct, lim, (uint64_t)atoi(mhz));
        if (ok(st))
            print_logf(devs[i], NULL, "Successfully set %s %s to %s(MHz)", which, clk, mhz);
        else
            status_line(devs[i], st, "set extremum");
    }
    print_spacer(NULL, "=", 0);
}

static void set_range(const int *devs, int nd, const char *clk, const char *mn, const char *mx) {
    amdsmi_clk_type_t ct = str_ieq(clk, "mclk") ? AMDSMI_CLK_TYPE_MEM : AMDSMI_CLK_TYPE_SYS;
    int i;
    print_spacer(" Set Clock Range ", "=", 0);
    if (!confirm_danger()) exit(1);
    for (i = 0; i < nd; i++) {
        amdsmi_status_t s1 = amdsmi_set_gpu_od_clk_info(D(devs[i])->h, AMDSMI_FREQ_IND_MIN,
                                                        (uint64_t)atoi(mn), ct);
        amdsmi_status_t s2 = amdsmi_set_gpu_od_clk_info(D(devs[i])->h, AMDSMI_FREQ_IND_MAX,
                                                        (uint64_t)atoi(mx), ct);
        if (ok(s1) && ok(s2))
            print_logf(devs[i], NULL, "Successfully set %s range to %s-%s MHz", clk, mn, mx);
        else
            status_line(devs[i], ok(s1) ? s2 : s1, "set range");
    }
    print_spacer(NULL, "=", 0);
}

static void set_vc(const int *devs, int nd, const char *pt, const char *mhz, const char *mv) {
    int i;
    print_spacer(" Set Voltage Curve ", "=", 0);
    if (!confirm_danger()) exit(1);
    for (i = 0; i < nd; i++) {
        amdsmi_status_t st = amdsmi_set_gpu_od_volt_info(D(devs[i])->h, (uint32_t)atoi(pt),
                                                         (uint64_t)atoi(mhz), (uint64_t)atoi(mv));
        if (ok(st)) print_logf(devs[i], NULL, "Successfully set voltage curve point %s", pt);
        else status_line(devs[i], st, "set voltage curve");
    }
    print_spacer(NULL, "=", 0);
}

static void set_slevel(const int *devs, int nd, const char *clk, const char *lvl,
                       const char *mhz, const char *mv) {
    (void)lvl;
    (void)mv;
    /* Level+voltage OD writes are exposed as range endpoints on this library. */
    set_range(devs, nd, clk, mhz, mhz);
}

static void reset_clocks(const int *devs, int nd) {
    int i;
    print_spacer(" Reset Clocks ", "=", 0);
    for (i = 0; i < nd; i++) {
        amdsmi_set_gpu_overdrive_level(D(devs[i])->h, 0);
        print_log(devs[i], "OverDrive set to 0", NULL);
        if (ok(amdsmi_set_gpu_perf_level(D(devs[i])->h, AMDSMI_DEV_PERF_LEVEL_AUTO)))
            print_log(devs[i], "Performance level set to auto", NULL);
    }
    print_spacer(NULL, "=", 0);
}

static void set_profile(const int *devs, int nd, const char *spec) {
    int i;
    uint64_t mask = 0;
    print_spacer(" Set Power Profile ", "=", 0);
    if (str_ieq(spec, "CUSTOM")) mask = AMDSMI_PWR_PROF_PRST_CUSTOM_MASK;
    else if (str_ieq(spec, "VIDEO")) mask = AMDSMI_PWR_PROF_PRST_VIDEO_MASK;
    else if (str_ieq(spec, "POWER SAVING") || str_ieq(spec, "POWER_SAVING"))
        mask = AMDSMI_PWR_PROF_PRST_POWER_SAVING_MASK;
    else if (str_ieq(spec, "COMPUTE")) mask = AMDSMI_PWR_PROF_PRST_COMPUTE_MASK;
    else if (str_ieq(spec, "VR")) mask = AMDSMI_PWR_PROF_PRST_VR_MASK;
    else if (str_ieq(spec, "3D FULL SCREEN") || str_ieq(spec, "3D_FULL_SCREEN"))
        mask = AMDSMI_PWR_PROF_PRST_3D_FULL_SCR_MASK;
    else if (str_ieq(spec, "BOOTUP DEFAULT") || str_ieq(spec, "BOOTUP_DEFAULT"))
        mask = AMDSMI_PWR_PROF_PRST_BOOTUP_DEFAULT;
    else {
        int n = atoi(spec);
        if (n >= 1 && n <= 7) mask = 1ull << (n - 1);
    }
    for (i = 0; i < nd; i++) {
        amdsmi_status_t st;
        if (!mask) {
            print_log(devs[i], "Unrecognized profile", NULL);
            continue;
        }
        st = amdsmi_set_gpu_power_profile(D(devs[i])->h, 0,
                                           (amdsmi_power_profile_preset_masks_t)mask);
        if (ok(st)) print_logf(devs[i], NULL, "Successfully set power profile to %s", spec);
        else status_line(devs[i], st, "set profile");
    }
    print_spacer(NULL, "=", 0);
}

static void reset_profile(const int *devs, int nd) {
    set_profile(devs, nd, "BOOTUP DEFAULT");
}

static void set_perfdet(const int *devs, int nd, uint64_t mhz) {
    int i;
    print_spacer(" Set Performance Determinism ", "=", 0);
    for (i = 0; i < nd; i++) {
        amdsmi_status_t st = amdsmi_set_gpu_perf_determinism_mode(D(devs[i])->h, mhz);
        if (ok(st)) print_logf(devs[i], NULL, "Successfully set performance determinism to %" PRIu64 " MHz", mhz);
        else status_line(devs[i], st, "set perf determinism");
    }
    print_spacer(NULL, "=", 0);
}

static void reset_perfdet(const int *devs, int nd) {
    set_perf(devs, nd, "auto");
}

static int mempart_from_name(const char *s, amdsmi_memory_partition_type_t *o) {
    if (str_ieq(s, "NPS1")) { *o = AMDSMI_MEMORY_PARTITION_NPS1; return 0; }
    if (str_ieq(s, "NPS2")) { *o = AMDSMI_MEMORY_PARTITION_NPS2; return 0; }
    if (str_ieq(s, "NPS4")) { *o = AMDSMI_MEMORY_PARTITION_NPS4; return 0; }
    if (str_ieq(s, "NPS8")) { *o = AMDSMI_MEMORY_PARTITION_NPS8; return 0; }
    return -1;
}

static int comppart_from_name(const char *s, amdsmi_accelerator_partition_type_t *o) {
    if (str_ieq(s, "SPX")) { *o = AMDSMI_ACCELERATOR_PARTITION_SPX; return 0; }
    if (str_ieq(s, "DPX")) { *o = AMDSMI_ACCELERATOR_PARTITION_DPX; return 0; }
    if (str_ieq(s, "TPX")) { *o = AMDSMI_ACCELERATOR_PARTITION_TPX; return 0; }
    if (str_ieq(s, "QPX")) { *o = AMDSMI_ACCELERATOR_PARTITION_QPX; return 0; }
    if (str_ieq(s, "CPX")) { *o = AMDSMI_ACCELERATOR_PARTITION_CPX; return 0; }
    return -1;
}

static void set_mempart(const int *devs, int nd, const char *name) {
    amdsmi_memory_partition_type_t m;
    int i;
    print_spacer(" Set Memory Partition ", "=", 0);
    if (mempart_from_name(name, &m) != 0) {
        print_logf(-1, "Invalid memory partition", "%s", name);
        print_spacer(NULL, "=", 0);
        return;
    }
    if (!confirm_danger()) exit(1);
    for (i = 0; i < nd; i++) {
        amdsmi_status_t st = amdsmi_set_gpu_memory_partition(D(devs[i])->h, m);
        if (ok(st)) print_logf(devs[i], NULL, "Successfully set memory partition to %s", name);
        else status_line(devs[i], st, "set memory partition");
    }
    print_spacer(NULL, "=", 0);
}

static void set_comppart(const int *devs, int nd, const char *name) {
    amdsmi_accelerator_partition_type_t want;
    int i;
    print_spacer(" Set Compute Partition ", "=", 0);
    if (comppart_from_name(name, &want) != 0) {
        print_logf(-1, "Invalid compute partition", "%s", name);
        print_spacer(NULL, "=", 0);
        return;
    }
    for (i = 0; i < nd; i++) {
        amdsmi_accelerator_partition_profile_config_t *cfg = xmalloc(sizeof *cfg);
        uint32_t p, chosen = UINT32_MAX;
        amdsmi_status_t st;
        memset(cfg, 0, sizeof *cfg);
        st = amdsmi_get_gpu_accelerator_partition_profile_config(D(devs[i])->h, cfg);
        if (ok(st)) {
            for (p = 0; p < cfg->num_profiles && p < AMDSMI_MAX_ACCELERATOR_PROFILE; p++)
                if (cfg->profiles[p].profile_type == want) chosen = cfg->profiles[p].profile_index;
        }
        free(cfg);
        if (chosen == UINT32_MAX) {
            print_log(devs[i], "Not supported on the given system", NULL);
            continue;
        }
        st = amdsmi_set_gpu_accelerator_partition_profile(D(devs[i])->h, chosen);
        if (ok(st)) print_logf(devs[i], NULL, "Successfully set compute partition to %s", name);
        else status_line(devs[i], st, "set compute partition");
    }
    print_spacer(NULL, "=", 0);
}

static void do_reset_gpu(int dev) {
    print_spacer(" GPU Reset ", "=", 0);
    if (ok(amdsmi_reset_gpu(D(dev)->h)))
        print_log(dev, "Successfully reset GPU", NULL);
    else
        print_log(dev, "GPU reset failed", NULL);
    print_spacer(NULL, "=", 0);
}

static void reset_xgmi(const int *devs, int nd) {
    int i;
    print_spacer(" Reset XGMI Error ", "=", 0);
    for (i = 0; i < nd; i++) {
        if (ok(amdsmi_reset_gpu_xgmi_error(D(devs[i])->h)))
            print_log(devs[i], "Successfully reset XGMI error count", NULL);
        else
            print_log(devs[i], "Not supported on the given system", NULL);
    }
    print_spacer(NULL, "=", 0);
}

static void save_settings(const int *devs, int nd, const char *path) {
    FILE *f;
    int i, k;
    print_spacer(" Save Settings ", "=", 0);
    if (access(path, F_OK) == 0) {
        print_logf(-1, NULL, "%s already exists. Settings not saved", path);
        print_spacer(NULL, "=", 0);
        exit(1);
    }
    f = fopen(path, "w");
    if (!f) {
        print_logf(-1, NULL, "Cannot write %s", path);
        print_spacer(NULL, "=", 0);
        return;
    }
    fputs("{", f);
    for (i = 0; i < nd; i++) {
        int d = devs[i];
        amdsmi_dev_perf_level_t pl;
        int64_t lv = 0, rpm = 0;
        int pct = 0;
        uint32_t od = 0;
        const char *perf = "Unsupported";
        fprintf(f, "%s\"card%d\": {", i ? ", " : "", d);
        fprintf(f, "\"vJson\": %d, \"clocks\": {", CLOCK_JSON_VERSION);
        for (k = 0; k < NCLKS; k++) {
            amdsmi_frequencies_t fr;
            memset(&fr, 0, sizeof fr);
            fprintf(f, "%s\"%s\": \"%u\"", k ? ", " : "", CLKS[k].name,
                    ok(amdsmi_get_clk_freq(D(d)->h, CLKS[k].type, &fr)) ? fr.current : 0);
        }
        fputs("}", f);
        fan_level_pct(d, &lv, &pct, &rpm);
        amdsmi_get_gpu_overdrive_level(D(d)->h, &od);
        if (ok(amdsmi_get_gpu_perf_level(D(d)->h, &pl)) && perf_name(pl)) perf = perf_name(pl);
        fprintf(f, ", \"fan\": %" PRId64 ", \"overdrivesclk\": \"%u\", \"overdrivemclk\": \"0\"",
                lv, od);
        fprintf(f, ", \"profile\": \"1\", \"perflevel\": \"%s\"}", perf);
    }
    fputs("}\n", f);
    fclose(f);
    print_logf(-1, "Current settings successfully saved to", "%s", path);
    print_spacer(NULL, "=", 0);
}

static const char *json_find(const char *s, const char *key, char *out, size_t n) {
    char pat[128];
    const char *p, *e;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    p = strstr(s, pat);
    if (!p) return NULL;
    p = strchr(p + strlen(pat), ':');
    if (!p) return NULL;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') {
        p++;
        e = strchr(p, '"');
        if (!e) return NULL;
        if ((size_t)(e - p) >= n) return NULL;
        memcpy(out, p, (size_t)(e - p));
        out[e - p] = 0;
        return e;
    }
    e = p;
    while (*e && *e != ',' && *e != '}' && !isspace((unsigned char)*e)) e++;
    if ((size_t)(e - p) >= n) return NULL;
    memcpy(out, p, (size_t)(e - p));
    out[e - p] = 0;
    return e;
}

static void load_settings(const char *path) {
    FILE *f;
    char *buf;
    long sz;
    const char *p;
    print_spacer(" Load Settings ", "=", 0);
    f = fopen(path, "r");
    if (!f) {
        print_logf(-1, NULL, "No settings file found at %s", path);
        print_spacer(NULL, "=", 0);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = xmalloc((size_t)sz + 1);
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        free(buf);
        return;
    }
    buf[sz] = 0;
    fclose(f);
    p = buf;
    while ((p = strstr(p, "\"card")) != NULL) {
        int dev = atoi(p + 5);
        char fan[32] = {0}, perf[32] = {0}, od[32] = {0}, ver[32] = {0};
        const char *card = p;
        const char *end = strchr(p, '}');
        char slice[4096];
        if (dev < 0 || dev >= g_ndev || !end) { p += 5; continue; }
        if ((size_t)(end - card) >= sizeof slice) { p = end; continue; }
        memcpy(slice, card, (size_t)(end - card));
        slice[end - card] = 0;
        json_find(slice, "vJson", ver, sizeof ver);
        if (ver[0] && atoi(ver) != CLOCK_JSON_VERSION) {
            print_logf(-1, NULL, "Unable to load legacy clock file - file v%s != current v%d",
                       ver, CLOCK_JSON_VERSION);
            break;
        }
        if (json_find(slice, "fan", fan, sizeof fan) && fan[0] && strcmp(fan, "0") != 0)
            set_fan(&dev, 1, fan);
        if (json_find(slice, "overdrivesclk", od, sizeof od) && od[0] && strcmp(od, "0") != 0)
            set_overdrive(&dev, 1, od);
        if (json_find(slice, "perflevel", perf, sizeof perf) && perf[0] &&
            strcmp(perf, "Unsupported") != 0)
            set_perf(&dev, 1, perf);
        print_logf(dev, NULL, "Successfully loaded values from %s", path);
        p = end;
    }
    free(buf);
    print_spacer(NULL, "=", 0);
}

/* -------------------------------------------------------------------------- */
/* arguments                                                                  */
/* -------------------------------------------------------------------------- */

static void usage(void) {
    printf("usage: rocm-smi [-h] [-V] [-d DEVICE [DEVICE ...]] [--alldevices] [--showhw] [-a] [-i] [-v]\n"
           "                [-e [EVENT ...]] [--showdriverversion] [--showtempgraph] [--showfwinfo [BLOCK ...]]\n"
           "                [--showmclkrange] [--showmemvendor] [--showsclkrange] [--showproductname]\n"
           "                [--showserial] [--showuniqueid] [--showvoltagerange] [--showbus] [--showpagesinfo]\n"
           "                [--showpendingpages] [--showretiredpages] [--showunreservablepages] [-f] [-P] [-t]\n"
           "                [-u] [--showmemuse] [--showvoltage] [-b] [-c] [-g] [-l] [-M] [-m] [-o] [-p] [-S] [-s]\n"
           "                [--showmeminfo TYPE [TYPE ...]] [--showpids [VERBOSE]] [--showpidgpus [PID ...]]\n"
           "                [--showreplaycount] [--showrasinfo [BLOCK ...]] [--showvc] [--showxgmierr]\n"
           "                [--showtopo] [--showtopoaccess] [--showtopoweight] [--showtopohops] [--showtopotype]\n"
           "                [--showtoponuma] [--showenergycounter] [--shownodesbw] [--showcomputepartition]\n"
           "                [--showmemorypartition] [--showmetrics] [-r] [--resetfans] [--resetprofile]\n"
           "                [--resetpoweroverdrive] [--resetxgmierr] [--resetperfdeterminism]\n"
           "                [--resetcomputepartition] [--resetmemorypartition] [--setclock TYPE LEVEL]\n"
           "                [--setsclk LEVEL ...] [--setmclk LEVEL ...] [--setpcie LEVEL ...] [--setslevel L MHZ MV]\n"
           "                [--setmlevel L MHZ MV] [--setvc POINT MHZ MV] [--setsrange MIN MAX]\n"
           "                [--setextremum min|max sclk|mclk CLK] [--setmrange MIN MAX] [--setfan LEVEL]\n"
           "                [--setperflevel LEVEL] [--setoverdrive %%] [--setmemoverdrive %%]\n"
           "                [--setpoweroverdrive WATTS] [--setprofile PROFILE] [--setperfdeterminism SCLK]\n"
           "                [--setcomputepartition {CPX,SPX,DPX,TPX,QPX}] [--setmemorypartition {NPS1,NPS2,NPS4,NPS8}]\n"
           "                [--rasenable BLOCK ERRTYPE] [--rasdisable BLOCK ERRTYPE] [--rasinject BLOCK]\n"
           "                [--gpureset] [--load FILE | --save FILE] [--autorespond RESPONSE]\n"
           "                [--loglevel LEVEL] [--json] [--csv]\n"
           "\n"
           "AMD ROCm System Management Interface | ROCM-SMI version: %s\n"
           "\n"
           "optional arguments:\n"
           "  -h, --help                    show this help message and exit\n"
           "  --gpureset                    Reset specified GPU (One GPU must be specified)\n"
           "  --load FILE                   Load Clock, Fan, Performance and Profile settings from FILE\n"
           "  --save FILE                   Save Clock, Fan, Performance and Profile settings to FILE\n"
           "  -V, --version                 Show version information\n"
           "  -d, --device DEVICE...        Execute command on specified device\n"
           "\n"
           "Display Options:\n"
           "  --alldevices\n"
           "  --showhw                      Show Hardware details\n"
           "  -a, --showallinfo             Show Temperature, Fan and Clock values\n"
           "\n"
           "Topology:\n"
           "  -i, --showid                  Show DEVICE IDs\n"
           "  -v, --showvbios               Show VBIOS version\n"
           "  -e, --showevents [EVENT ...]  Show event list\n"
           "  --showdriverversion           Show kernel driver version\n"
           "  --showtempgraph               Show Temperature Graph\n"
           "  --showfwinfo [BLOCK ...]      Show FW information\n"
           "  --showmclkrange               Show mclk range\n"
           "  --showmemvendor               Show GPU memory vendor\n"
           "  --showsclkrange               Show sclk range\n"
           "  --showproductname             Show product details\n"
           "  --showserial                  Show GPU's Serial Number\n"
           "  --showuniqueid                Show GPU's Unique ID\n"
           "  --showvoltagerange            Show voltage range\n"
           "  --showbus                     Show PCI bus number\n"
           "\n"
           "Pages information:\n"
           "  --showpagesinfo               Show retired, pending and unreservable pages\n"
           "  --showpendingpages            Show pending retired pages\n"
           "  --showretiredpages            Show retired pages\n"
           "  --showunreservablepages       Show unreservable pages\n"
           "\n"
           "Hardware-related information:\n"
           "  -f, --showfan                 Show current fan speed\n"
           "  -P, --showpower               Show current average or instant socket graphics package power\n"
           "  -t, --showtemp                Show current temperature\n"
           "  -u, --showuse                 Show current GPU use\n"
           "  --showmemuse                  Show current GPU memory used\n"
           "  --showvoltage                 Show current GPU voltage\n"
           "\n"
           "Software-related/controlled information:\n"
           "  -b, --showbw                  Show estimated PCIe use\n"
           "  -c, --showclocks              Show current clock frequencies\n"
           "  -g, --showgpuclocks           Show current GPU clock frequencies\n"
           "  -l, --showprofile             Show Compute Profile attributes\n"
           "  -M, --showmaxpower            Show maximum graphics package power this GPU will consume\n"
           "  -m, --showmemoverdrive        Show current GPU Memory Clock OverDrive level\n"
           "  -o, --showoverdrive           Show current GPU Clock OverDrive level\n"
           "  -p, --showperflevel           Show current DPM Performance Level\n"
           "  -S, --showclkvolt             Show supported GPU and Memory Clocks and Voltages\n"
           "  -s, --showclkfrq              Show supported GPU and Memory Clock\n"
           "  --showmeminfo TYPE...         Show Memory usage information for given block(s) TYPE\n"
           "  --showpids [VERBOSE]          Show current running KFD PIDs\n"
           "  --showpidgpus [PID ...]       Show GPUs used by specified KFD PIDs (all if no arg given)\n"
           "  --showreplaycount             Show PCIe Replay Count\n"
           "  --showrasinfo [BLOCK ...]     Show RAS enablement information and error counts\n"
           "  --showvc                      Show voltage curve\n"
           "  --showxgmierr                 Show XGMI error information since last read\n"
           "  --showtopo                    Show hardware topology information\n"
           "  --showtopoaccess              Shows the link accessibility between GPUs\n"
           "  --showtopoweight              Shows the relative weight between GPUs\n"
           "  --showtopohops                Shows the number of hops between GPUs\n"
           "  --showtopotype                Shows the link type between GPUs\n"
           "  --showtoponuma                Shows the numa nodes\n"
           "  --showenergycounter           Energy accumulator that stores amount of energy consumed\n"
           "  --shownodesbw                 Shows bandwidth between numa nodes\n"
           "  --showcomputepartition        Shows current compute partitioning\n"
           "  --showmemorypartition         Shows current memory partition\n"
           "  --showmetrics                 Show current gpu metric data\n"
           "\n"
           "Set options:\n"
           "  --setclock TYPE LEVEL         Set Clock Frequency Level(s) for specified clock\n"
           "  --setsclk LEVEL...            Set GPU Clock Frequency Level(s)\n"
           "  --setmclk LEVEL...            Set GPU Memory Clock Frequency Level(s)\n"
           "  --setpcie LEVEL...            Set PCIE Clock Frequency Level(s)\n"
           "  --setslevel L MHZ MV          Change GPU Clock frequency (MHz) and Voltage (mV)\n"
           "  --setmlevel L MHZ MV          Change GPU Memory clock frequency (MHz) and Voltage (mV)\n"
           "  --setvc POINT MHZ MV          Change SCLK Voltage Curve (MHz mV) for a specific point\n"
           "  --setsrange MIN MAX           Set min and max SCLK speed\n"
           "  --setextremum min|max sclk|mclk CLK\n"
           "                                Set min/max of SCLK/MCLK speed\n"
           "  --setmrange MIN MAX           Set min and max MCLK speed\n"
           "  --setfan LEVEL                Set GPU Fan Speed (Level or %%)\n"
           "  --setperflevel LEVEL          Set Performance Level\n"
           "  --setoverdrive %%              Set GPU OverDrive level\n"
           "  --setmemoverdrive %%           Set GPU Memory Overclock OverDrive level\n"
           "  --setpoweroverdrive WATTS     Set the maximum GPU power using Power OverDrive in Watts\n"
           "  --setprofile PROFILE          Specify Power Profile level (#) or a quoted string\n"
           "  --setperfdeterminism SCLK     Set clock frequency limit for minimal performance variation\n"
           "  --setcomputepartition MODE    Set compute partition\n"
           "  --setmemorypartition MODE     Set memory partition\n"
           "  --rasenable BLOCK ERRTYPE     Enable RAS for specified block and error type\n"
           "  --rasdisable BLOCK ERRTYPE    Disable RAS for specified block and error type\n"
           "  --rasinject BLOCK             Inject RAS poison for specified block (ONLY WORKS ON UNSECURED BOARDS)\n"
           "\n"
           "Reset options:\n"
           "  -r, --resetclocks             Reset clocks and OverDrive to default\n"
           "  --resetfans                   Reset fans to automatic (driver) control\n"
           "  --resetprofile                Reset Power Profile back to default\n"
           "  --resetpoweroverdrive         Set the maximum GPU power back to the device default state\n"
           "  --resetxgmierr                Reset XGMI error count\n"
           "  --resetperfdeterminism        Disable performance determinism\n"
           "  --resetcomputepartition       Resets to boot compute partition state\n"
           "  --resetmemorypartition        Resets to boot memory partition state\n"
           "\n"
           "Auto-response options:\n"
           "  --autorespond RESPONSE        Response to automatically provide for all prompts (NOT RECOMMENDED)\n"
           "\n"
           "Output options:\n"
           "  --loglevel LEVEL              One of debug/info/warning/error/critical\n"
           "  --json                        Print output in JSON format\n"
           "  --csv                         Print output in CSV format\n",
           SMI_VERSION);
}

typedef struct {
    int help, version, alldevices, showhw, showall, showid, showvbios, showdriver, showtempgraph;
    int showmclkrange, showmemvendor, showsclkrange, showproduct, showserial, showuid, showvrange;
    int showbus, showpages, showpending, showretired, showunres;
    int showfan, showpower, showtemp, showuse, showmemuse, showvoltage;
    int showbw, showclocks, showgpuclocks, showprofile, showmaxpower, showmod, showod, showperf;
    int showclkvolt, showclkfrq, showreplay, showvc, showxgmi, showtopo, showaccess, showweight;
    int showhops, showtype, shownuma, showenergy, shownodes, showcpart, showmpart, showmetrics;
    int resetclocks, resetfans, resetprofile, resetpower, resetxgmi, resetperfdet;
    int resetcpart, resetmpart, gpureset;
    int devs[MAX_DEV], ndev;
    int dev_given;
    char *events[32]; int nevents; int have_events;
    char *fw[32]; int nfw; int have_fw;
    char *meminfo[8]; int nmem;
    char *ras[32]; int nras; int have_ras;
    char *pidgpus[32]; int npidgpus; int have_pidgpus;
    int have_pids; char *pids_verbose;
    char *setclock_type, *setclock_level;
    int setsclk[64], nsetsclk, setmclk[64], nsetmclk, setpcie[64], nsetpcie;
    char *setslevel[3], *setmlevel[3], *setvc[3], *setsrange[2], *setmrange[2], *setextremum[3];
    char *setfan, *setperf, *setod, *setmod, *setpower, *setprofile, *setcpart, *setmpart;
    int setperfdet;
    uint64_t perfdet_mhz;
    char *ras_en[2], *ras_dis[2], *ras_inj;
    char *load_path, *save_path;
} Args;

static int is_opt(const char *s) { return s && s[0] == '-'; }

static int take_ints(int argc, char **argv, int *i, int *out, int maxn) {
    int n = 0;
    while (*i + 1 < argc && !is_opt(argv[*i + 1]) && n < maxn) {
        (*i)++;
        out[n++] = atoi(argv[*i]);
    }
    return n;
}

static int take_strs(int argc, char **argv, int *i, char **out, int maxn) {
    int n = 0;
    while (*i + 1 < argc && !is_opt(argv[*i + 1]) && n < maxn) {
        (*i)++;
        out[n++] = argv[*i];
    }
    return n;
}

static int need_n(int argc, char **argv, int *i, int n, char **dst) {
    int k;
    for (k = 0; k < n; k++) {
        if (*i + 1 >= argc || is_opt(argv[*i + 1])) {
            fprintf(stderr, "usage: rocm-smi: expected %d argument(s)\n", n);
            exit(2);
        }
        (*i)++;
        dst[k] = argv[*i];
    }
    return 0;
}

static int parse_args(int argc, char **argv, Args *a) {
    int i;
    memset(a, 0, sizeof *a);
    a->pids_verbose = "summary";
    for (i = 1; i < argc; i++) {
        const char *s = argv[i];
        if (str_eq(s, "-h") || str_eq(s, "--help")) a->help = 1;
        else if (str_eq(s, "-V") || str_eq(s, "--version")) a->version = 1;
        else if (str_eq(s, "--alldevices")) a->alldevices = 1;
        else if (str_eq(s, "--showhw")) a->showhw = 1;
        else if (str_eq(s, "-a") || str_eq(s, "--showallinfo")) a->showall = 1;
        else if (str_eq(s, "-i") || str_eq(s, "--showid")) a->showid = 1;
        else if (str_eq(s, "-v") || str_eq(s, "--showvbios")) a->showvbios = 1;
        else if (str_eq(s, "--showdriverversion")) a->showdriver = 1;
        else if (str_eq(s, "--showtempgraph")) a->showtempgraph = 1;
        else if (str_eq(s, "--showmclkrange")) a->showmclkrange = 1;
        else if (str_eq(s, "--showmemvendor")) a->showmemvendor = 1;
        else if (str_eq(s, "--showsclkrange")) a->showsclkrange = 1;
        else if (str_eq(s, "--showproductname")) a->showproduct = 1;
        else if (str_eq(s, "--showserial")) a->showserial = 1;
        else if (str_eq(s, "--showuniqueid")) a->showuid = 1;
        else if (str_eq(s, "--showvoltagerange")) a->showvrange = 1;
        else if (str_eq(s, "--showbus")) a->showbus = 1;
        else if (str_eq(s, "--showpagesinfo")) a->showpages = 1;
        else if (str_eq(s, "--showpendingpages")) a->showpending = 1;
        else if (str_eq(s, "--showretiredpages")) a->showretired = 1;
        else if (str_eq(s, "--showunreservablepages")) a->showunres = 1;
        else if (str_eq(s, "-f") || str_eq(s, "--showfan")) a->showfan = 1;
        else if (str_eq(s, "-P") || str_eq(s, "--showpower")) a->showpower = 1;
        else if (str_eq(s, "-t") || str_eq(s, "--showtemp")) a->showtemp = 1;
        else if (str_eq(s, "-u") || str_eq(s, "--showuse")) a->showuse = 1;
        else if (str_eq(s, "--showmemuse")) a->showmemuse = 1;
        else if (str_eq(s, "--showvoltage")) a->showvoltage = 1;
        else if (str_eq(s, "-b") || str_eq(s, "--showbw")) a->showbw = 1;
        else if (str_eq(s, "-c") || str_eq(s, "--showclocks")) a->showclocks = 1;
        else if (str_eq(s, "-g") || str_eq(s, "--showgpuclocks")) a->showgpuclocks = 1;
        else if (str_eq(s, "-l") || str_eq(s, "--showprofile")) a->showprofile = 1;
        else if (str_eq(s, "-M") || str_eq(s, "--showmaxpower")) a->showmaxpower = 1;
        else if (str_eq(s, "-m") || str_eq(s, "--showmemoverdrive")) a->showmod = 1;
        else if (str_eq(s, "-o") || str_eq(s, "--showoverdrive")) a->showod = 1;
        else if (str_eq(s, "-p") || str_eq(s, "--showperflevel")) a->showperf = 1;
        else if (str_eq(s, "-S") || str_eq(s, "--showclkvolt")) a->showclkvolt = 1;
        else if (str_eq(s, "-s") || str_eq(s, "--showclkfrq")) a->showclkfrq = 1;
        else if (str_eq(s, "--showreplaycount")) a->showreplay = 1;
        else if (str_eq(s, "--showvc")) a->showvc = 1;
        else if (str_eq(s, "--showxgmierr")) a->showxgmi = 1;
        else if (str_eq(s, "--showtopo")) a->showtopo = 1;
        else if (str_eq(s, "--showtopoaccess")) a->showaccess = 1;
        else if (str_eq(s, "--showtopoweight")) a->showweight = 1;
        else if (str_eq(s, "--showtopohops")) a->showhops = 1;
        else if (str_eq(s, "--showtopotype")) a->showtype = 1;
        else if (str_eq(s, "--showtoponuma")) a->shownuma = 1;
        else if (str_eq(s, "--showenergycounter")) a->showenergy = 1;
        else if (str_eq(s, "--shownodesbw")) a->shownodes = 1;
        else if (str_eq(s, "--showcomputepartition")) a->showcpart = 1;
        else if (str_eq(s, "--showmemorypartition")) a->showmpart = 1;
        else if (str_eq(s, "--showmetrics")) a->showmetrics = 1;
        else if (str_eq(s, "-r") || str_eq(s, "--resetclocks")) a->resetclocks = 1;
        else if (str_eq(s, "--resetfans")) a->resetfans = 1;
        else if (str_eq(s, "--resetprofile")) a->resetprofile = 1;
        else if (str_eq(s, "--resetpoweroverdrive")) a->resetpower = 1;
        else if (str_eq(s, "--resetxgmierr")) a->resetxgmi = 1;
        else if (str_eq(s, "--resetperfdeterminism")) a->resetperfdet = 1;
        else if (str_eq(s, "--resetcomputepartition")) a->resetcpart = 1;
        else if (str_eq(s, "--resetmemorypartition")) a->resetmpart = 1;
        else if (str_eq(s, "--gpureset")) a->gpureset = 1;
        else if (str_eq(s, "--json")) a->help = a->help, g_json = 1;
        else if (str_eq(s, "--csv")) { g_csv = 1; g_json = 1; }
        else if (str_eq(s, "-d") || str_eq(s, "--device")) {
            a->ndev = take_ints(argc, argv, &i, a->devs, MAX_DEV);
            a->dev_given = 1;
            if (a->ndev == 0) { fprintf(stderr, "rocm-smi: --device requires a device id\n"); exit(2); }
        }
        else if (str_eq(s, "-e") || str_eq(s, "--showevents")) {
            a->have_events = 1;
            a->nevents = take_strs(argc, argv, &i, a->events, 32);
        }
        else if (str_eq(s, "--showfwinfo")) {
            a->have_fw = 1;
            a->nfw = take_strs(argc, argv, &i, a->fw, 32);
        }
        else if (str_eq(s, "--showmeminfo")) {
            a->nmem = take_strs(argc, argv, &i, a->meminfo, 8);
            if (!a->nmem) { fprintf(stderr, "rocm-smi: --showmeminfo requires TYPE\n"); exit(2); }
        }
        else if (str_eq(s, "--showpids")) {
            a->have_pids = 1;
            if (i + 1 < argc && !is_opt(argv[i + 1])) a->pids_verbose = argv[++i];
        }
        else if (str_eq(s, "--showpidgpus")) {
            a->have_pidgpus = 1;
            a->npidgpus = take_strs(argc, argv, &i, a->pidgpus, 32);
        }
        else if (str_eq(s, "--showrasinfo")) {
            a->have_ras = 1;
            a->nras = take_strs(argc, argv, &i, a->ras, 32);
        }
        else if (str_eq(s, "--setclock")) {
            char *p[2];
            need_n(argc, argv, &i, 2, p);
            a->setclock_type = p[0];
            a->setclock_level = p[1];
        }
        else if (str_eq(s, "--setsclk")) a->nsetsclk = take_ints(argc, argv, &i, a->setsclk, 64);
        else if (str_eq(s, "--setmclk")) a->nsetmclk = take_ints(argc, argv, &i, a->setmclk, 64);
        else if (str_eq(s, "--setpcie")) a->nsetpcie = take_ints(argc, argv, &i, a->setpcie, 64);
        else if (str_eq(s, "--setslevel")) need_n(argc, argv, &i, 3, a->setslevel);
        else if (str_eq(s, "--setmlevel")) need_n(argc, argv, &i, 3, a->setmlevel);
        else if (str_eq(s, "--setvc")) need_n(argc, argv, &i, 3, a->setvc);
        else if (str_eq(s, "--setsrange")) need_n(argc, argv, &i, 2, a->setsrange);
        else if (str_eq(s, "--setmrange")) need_n(argc, argv, &i, 2, a->setmrange);
        else if (str_eq(s, "--setextremum")) need_n(argc, argv, &i, 3, a->setextremum);
        else if (str_eq(s, "--setfan")) {
            if (i + 1 >= argc) exit(2);
            a->setfan = argv[++i];
        }
        else if (str_eq(s, "--setperflevel")) {
            if (i + 1 >= argc) exit(2);
            a->setperf = argv[++i];
        }
        else if (str_eq(s, "--setoverdrive")) {
            if (i + 1 >= argc) exit(2);
            a->setod = argv[++i];
        }
        else if (str_eq(s, "--setmemoverdrive")) {
            if (i + 1 >= argc) exit(2);
            a->setmod = argv[++i];
        }
        else if (str_eq(s, "--setpoweroverdrive")) {
            if (i + 1 >= argc) exit(2);
            a->setpower = argv[++i];
        }
        else if (str_eq(s, "--setprofile")) {
            if (i + 1 >= argc) exit(2);
            a->setprofile = argv[++i];
        }
        else if (str_eq(s, "--setperfdeterminism")) {
            if (i + 1 >= argc) exit(2);
            a->setperfdet = 1;
            a->perfdet_mhz = (uint64_t)strtoull(argv[++i], NULL, 0);
        }
        else if (str_eq(s, "--setcomputepartition")) {
            if (i + 1 >= argc) exit(2);
            a->setcpart = argv[++i];
        }
        else if (str_eq(s, "--setmemorypartition")) {
            if (i + 1 >= argc) exit(2);
            a->setmpart = argv[++i];
        }
        else if (str_eq(s, "--rasenable")) need_n(argc, argv, &i, 2, a->ras_en);
        else if (str_eq(s, "--rasdisable")) need_n(argc, argv, &i, 2, a->ras_dis);
        else if (str_eq(s, "--rasinject")) {
            if (i + 1 >= argc) exit(2);
            a->ras_inj = argv[++i];
        }
        else if (str_eq(s, "--load")) {
            if (i + 1 >= argc) exit(2);
            a->load_path = argv[++i];
        }
        else if (str_eq(s, "--save")) {
            if (i + 1 >= argc) exit(2);
            a->save_path = argv[++i];
        }
        else if (str_eq(s, "--autorespond")) {
            if (i + 1 >= argc) exit(2);
            g_autorespond = argv[++i];
        }
        else if (str_eq(s, "--loglevel")) {
            const char *lv;
            if (i + 1 >= argc) exit(2);
            lv = argv[++i];
            if (str_ieq(lv, "debug")) g_loglevel = 10;
            else if (str_ieq(lv, "info")) g_loglevel = 20;
            else if (str_ieq(lv, "warning")) g_loglevel = 30;
            else if (str_ieq(lv, "error")) g_loglevel = 40;
            else if (str_ieq(lv, "critical")) g_loglevel = 50;
            else { fprintf(stderr, "rocm-smi: invalid log level %s\n", lv); exit(2); }
        }
        else {
            fprintf(stderr, "usage: rocm-smi: unrecognized arguments: %s\n", s);
            exit(2);
        }
    }
    return 0;
}

static int wants_sudo(const Args *a) {
    return a->setsclk[0] || a->nsetsclk || a->nsetmclk || a->nsetpcie || a->resetfans || a->setfan ||
           a->setperf || a->load_path || a->resetclocks || a->setprofile || a->resetprofile ||
           a->setod || a->setmod || a->setpower || a->resetpower || a->ras_en[0] || a->ras_dis[0] ||
           a->ras_inj || a->gpureset || a->setperfdet || a->setslevel[0] || a->setmlevel[0] ||
           a->setvc[0] || a->setsrange[0] || a->setextremum[0] || a->setmrange[0] ||
           a->setclock_type || a->setcpart || a->setmpart || a->resetcpart || a->resetmpart;
}

static int any_action(const Args *a) {
    return a->showhw || a->showall || a->showid || a->showvbios || a->showdriver || a->showtempgraph ||
           a->showmclkrange || a->showmemvendor || a->showsclkrange || a->showproduct || a->showserial ||
           a->showuid || a->showvrange || a->showbus || a->showpages || a->showpending || a->showretired ||
           a->showunres || a->showfan || a->showpower || a->showtemp || a->showuse || a->showmemuse ||
           a->showvoltage || a->showbw || a->showclocks || a->showgpuclocks || a->showprofile ||
           a->showmaxpower || a->showmod || a->showod || a->showperf || a->showclkvolt || a->showclkfrq ||
           a->showreplay || a->showvc || a->showxgmi || a->showtopo || a->showaccess || a->showweight ||
           a->showhops || a->showtype || a->shownuma || a->showenergy || a->shownodes || a->showcpart ||
           a->showmpart || a->showmetrics || a->have_events || a->have_fw || a->nmem || a->have_ras ||
           a->have_pids || a->have_pidgpus || a->resetclocks || a->resetfans || a->resetprofile ||
           a->resetpower || a->resetxgmi || a->resetperfdet || a->gpureset || a->setfan || a->setperf ||
           a->nsetsclk || a->nsetmclk || a->nsetpcie || a->setclock_type || a->setod || a->setmod ||
           a->setpower || a->setprofile || a->load_path || a->save_path || a->setcpart || a->setmpart ||
           a->setslevel[0] || a->setmlevel[0] || a->setvc[0] || a->setsrange[0] || a->setmrange[0] ||
           a->setextremum[0] || a->setperfdet || a->ras_en[0] || a->ras_dis[0] || a->ras_inj ||
           a->resetcpart || a->resetmpart;
}

static int concise_requested(int argc, const Args *a) {
    int extra = a->alldevices + (g_json || g_csv);
    if (any_action(a)) return 0;
    if (argc == 1) return 1;
    if (!a->dev_given && extra && argc <= 3) return 1;
    return 0;
}

int main(int argc, char **argv) {
    Args a;
    int devs[MAX_DEV], nd = 0, i;
    int concise;

    parse_args(argc, argv, &a);
    if (a.help) { usage(); return 0; }

    if (wants_sudo(&a) && geteuid() != 0) {
        char **nv = xmalloc((size_t)(argc + 2) * sizeof *nv);
        nv[0] = "sudo";
        for (i = 0; i < argc; i++) nv[i + 1] = argv[i];
        nv[argc + 1] = NULL;
        execvp("sudo", nv);
        log_err("root privileges are required (sudo exec failed: %s)", strerror(errno));
        return 1;
    }

    if (!driver_live()) {
        log_err("Driver not initialized (amdgpu not found in modules)");
        return 0;
    }
    if (discover() != 0) {
        log_err("ROCm SMI returned %s (the expected value is 0)", "no devices");
        if (g_ndev == 0) {
            print_log(-1, "ERROR: No DRM devices available. Exiting", NULL);
            return 1;
        }
    }

    if (a.version) {
        show_version();
        amdsmi_shut_down();
        return 0;
    }

    if (a.dev_given) {
        for (i = 0; i < a.ndev; i++) {
            if (a.devs[i] < 0 || a.devs[i] >= g_ndev) {
                log_warn("No such device card%d", a.devs[i]);
                return 0;
            }
            devs[nd++] = a.devs[i];
        }
    } else {
        for (i = 0; i < g_ndev; i++) devs[nd++] = i;
    }
    if (nd == 0) {
        print_log(-1, "ERROR: No DRM devices available. Exiting", NULL);
        return 1;
    }

    if (a.showall) {
        a.showid = a.showvbios = a.showdriver = a.showfan = a.showpower = a.showtemp = 1;
        a.showuse = a.showmemuse = a.showvoltage = a.showclocks = a.showmaxpower = 1;
        a.showod = a.showmod = a.showperf = a.showproduct = a.showserial = a.showuid = 1;
        a.showbus = a.showpages = a.showmemvendor = a.showsclkrange = a.showmclkrange = 1;
        a.showvrange = a.showvc = a.showenergy = a.showreplay = a.showcpart = a.showmpart = 1;
        a.showmetrics = a.have_fw = a.have_pids = a.have_pidgpus = 1;
        a.nfw = 0;
        if (!g_json) { a.showprofile = a.showclkfrq = a.showclkvolt = 1; }
    }

    concise = concise_requested(argc, &a);
    if (!g_json) printf("\n");
    if (!concise) print_spacer(HEADER, "=", 0);

    if (a.gpureset) {
        if (!a.dev_given || a.ndev != 1) {
            log_err("No device specified. One device must be specified for GPU reset");
            print_spacer(NULL, "=", 0);
            return 1;
        }
        do_reset_gpu(devs[0]);
        amdsmi_shut_down();
        return 0;
    }

    if (concise) show_concise(devs, nd);
    if (a.showhw) show_hw(devs, nd);
    if (a.showdriver) show_driver(devs, nd);
    if (a.showtempgraph) show_tempgraph(devs, nd);
    if (a.showid) show_id(devs, nd);
    if (a.showuid) show_uid(devs, nd);
    if (a.showvbios) show_vbios(devs, nd);
    if (a.have_events) show_events(devs, nd, a.events, a.nevents);
    if (a.resetclocks) reset_clocks(devs, nd);
    if (a.showtemp) show_temp(devs, nd);
    if (a.showclocks) show_clocks_current(devs, nd, NULL);
    if (a.showgpuclocks) show_clocks_current(devs, nd, "sclk");
    if (a.showfan) show_fan(devs, nd);
    if (a.showperf) show_perf(devs, nd);
    if (a.showod) show_overdrive(devs, nd, "sclk");
    if (a.showmod) show_overdrive(devs, nd, "mclk");
    if (a.showmaxpower) show_maxpower(devs, nd);
    if (a.showprofile) show_profile(devs, nd);
    if (a.showpower) show_power(devs, nd);
    if (a.showclkfrq) show_clocks_supported(devs, nd);
    if (a.showuse) show_use(devs, nd);
    if (a.showmemuse) show_memuse(devs, nd);
    if (a.showmemvendor) show_memvendor(devs, nd);
    if (a.showbw) show_bw(devs, nd);
    if (a.showreplay) show_replay(devs, nd);
    if (a.showserial) show_serial(devs, nd);
    if (a.have_pids) show_pids(a.pids_verbose);
    if (a.have_pidgpus) show_pidgpus(a.pidgpus, a.npidgpus);
    if (a.showclkvolt) show_clkvolt(devs, nd);
    if (a.showvoltage) show_voltage(devs, nd);
    if (a.showbus) show_bus(devs, nd);
    if (a.nmem) show_meminfo(devs, nd, a.meminfo, a.nmem);
    if (a.have_ras) show_ras(devs, nd, a.ras, a.nras);
    if (a.have_fw) show_fw(devs, nd, a.fw, a.nfw);
    if (a.showproduct) show_product(devs, nd);
    if (a.showxgmi) show_xgmi(devs, nd);
    if (a.shownodes) show_nodes_bw(devs, nd);
    if (a.showtopo) {
        show_topo_weight(devs, nd);
        if (!g_json) printf("\n");
        show_topo_hops(devs, nd);
        if (!g_json) printf("\n");
        show_topo_type(devs, nd);
        if (!g_json) printf("\n");
        show_numa(devs, nd);
    }
    if (a.showaccess) show_topo_access(devs, nd);
    if (a.showweight) show_topo_weight(devs, nd);
    if (a.showhops) show_topo_hops(devs, nd);
    if (a.showtype) show_topo_type(devs, nd);
    if (a.shownuma) show_numa(devs, nd);
    if (a.showpages) show_pages(devs, nd, "all");
    if (a.showretired) show_pages(devs, nd, "reserved");
    if (a.showpending) show_pages(devs, nd, "pending");
    if (a.showunres) show_pages(devs, nd, "unreservable");
    if (a.showsclkrange) show_range(devs, nd, "sclk");
    if (a.showmclkrange) show_range(devs, nd, "mclk");
    if (a.showvrange) show_range(devs, nd, "voltage");
    if (a.showvc) show_vc(devs, nd);
    if (a.showenergy) show_energy(devs, nd);
    if (a.showcpart) show_partitions(devs, nd, 0);
    if (a.showmpart) show_partitions(devs, nd, 1);
    if (a.showmetrics) show_metrics(devs, nd);

    if (a.setclock_type) {
        int lv = atoi(a.setclock_level);
        set_clocks(devs, nd, a.setclock_type, &lv, 1);
    }
    if (a.nsetsclk) set_clocks(devs, nd, "sclk", a.setsclk, a.nsetsclk);
    if (a.nsetmclk) set_clocks(devs, nd, "mclk", a.setmclk, a.nsetmclk);
    if (a.nsetpcie) set_clocks(devs, nd, "pcie", a.setpcie, a.nsetpcie);
    if (a.setslevel[0]) set_slevel(devs, nd, "sclk", a.setslevel[0], a.setslevel[1], a.setslevel[2]);
    if (a.setmlevel[0]) set_slevel(devs, nd, "mclk", a.setmlevel[0], a.setmlevel[1], a.setmlevel[2]);
    if (a.resetfans) reset_fans(devs, nd);
    if (a.setfan) set_fan(devs, nd, a.setfan);
    if (a.setperf) set_perf(devs, nd, a.setperf);
    if (a.setod) set_overdrive(devs, nd, a.setod);
    if (a.setmod) {
        print_spacer(" Set Memory OverDrive ", "=", 0);
        print_log(-1, "GPU Memory OverDrive is not supported on this system", NULL);
        print_spacer(NULL, "=", 0);
    }
    if (a.setpower) set_power_od(devs, nd, a.setpower, 0);
    if (a.resetpower) set_power_od(devs, nd, NULL, 1);
    if (a.setprofile) set_profile(devs, nd, a.setprofile);
    if (a.setvc[0]) set_vc(devs, nd, a.setvc[0], a.setvc[1], a.setvc[2]);
    if (a.setextremum[0]) set_extremum(devs, nd, a.setextremum[0], a.setextremum[1], a.setextremum[2]);
    if (a.setsrange[0]) set_range(devs, nd, "sclk", a.setsrange[0], a.setsrange[1]);
    if (a.setmrange[0]) set_range(devs, nd, "mclk", a.setmrange[0], a.setmrange[1]);
    if (a.setperfdet) set_perfdet(devs, nd, a.perfdet_mhz);
    if (a.setcpart) set_comppart(devs, nd, a.setcpart);
    if (a.setmpart) set_mempart(devs, nd, a.setmpart);
    if (a.resetprofile) reset_profile(devs, nd);
    if (a.resetxgmi) reset_xgmi(devs, nd);
    if (a.resetperfdet) reset_perfdet(devs, nd);
    if (a.resetcpart) {
        print_spacer(" Reset Compute Partition ", "=", 0);
        print_log(-1, "Reset to boot compute partition is not supported on the given system", NULL);
        print_spacer(NULL, "=", 0);
    }
    if (a.resetmpart) {
        print_spacer(" Reset Memory Partition ", "=", 0);
        print_log(-1, "Reset to boot memory partition is not supported on the given system", NULL);
        print_spacer(NULL, "=", 0);
    }
    if (a.ras_en[0] || a.ras_dis[0] || a.ras_inj) {
        print_spacer(" RAS ", "=", 0);
        print_log(-1, "RAS enable/disable/inject is not supported on the given system", NULL);
        print_spacer(NULL, "=", 0);
    }
    if (a.load_path) load_settings(a.load_path);
    if (a.save_path) save_settings(devs, nd, a.save_path);

    if (g_loglevel >= 30) g_retcode = 0;

    if (g_json && !concise) {
        int empty = 1;
        for (i = 0; i < g_njobs; i++)
            if (g_jobs[i].n) empty = 0;
        if (empty) {
            log_warn("No JSON data to report");
        } else if (!g_csv) {
            json_dump();
        } else {
            csv_dump(devs, nd);
        }
    }

    if (!concise) print_spacer(FOOTER, "=", 0);
    amdsmi_shut_down();
    return g_retcode;
}
