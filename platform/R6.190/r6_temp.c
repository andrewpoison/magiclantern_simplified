/*
 * R6.190 M14.1: the camera temperature (spec m14/m141 spec_m141.md Rev. 2, 2.2 / 4.2 / 5.2; docs/m14_plan.md E3 b, E4, E6 c,
 * approved 2026-10-15, "M14.1 starten" 2026-10-16). Way A: only the newest poll of r6_tlog_task (Tor 0g, read only there);
 * nothing of Canon's is read, called or written here.
 *  - Sensor A in whole degC as Canon rounds it for EXIF: floor((t16 + 8) / 16) (48 of 48 values in the M14.0 log).
 *  - Valid: A read, -40 <= a <= 99, the poll at most R6TB_STALE_MS old.
 *  - Warning: Canon's status [0x484C] != 0 (always, no hysteresis), or A >= "Warn at" T (on from t16 >= 16 T - 8, off below
 *    16 T - 16: 0.5 degC hysteresis). The threshold state is advanced only by r6_temp_poll, once per poll from
 *    r6_tlog_task (skeptic M8); the box, the menu and the log only read it.
 *  - Display > "Temperature": the box text, Show in LV, Warn at, Canon's status, the 8 sensors in 0.1 degC (r6tl_c).
 *  - The box (A 37 degC) is drawn by r6_grid.c on the LCD shooting screen (Global Draw), never during ML's own picture.
 * The degree sign is SYM_DEGREE (ML's font), never UTF-8.
 */
#include "dryos.h"
#include "menu.h"
#include "config.h"
#include "bmp.h"                /* SYM_DEGREE */
#include "r6_tlog.h"
#include "r6_temp.h"
#include "r6_interval.h"        /* r6_iv_in_pic */

#ifdef CONFIG_R6_TEMP
#if !defined(CONFIG_R6_TLOG) || !defined(CONFIG_R6_GRID)
#error "r6_temp.c: CONFIG_R6_TEMP needs CONFIG_R6_TLOG (the poll) and CONFIG_R6_GRID (the box)"
#endif

extern int get_global_draw_setting(void);   /* src/zebra.c */

static CONFIG_INT("temp.lv",   temp_lv, 0);       /* Show in LV */
static CONFIG_INT("temp.warn", temp_warn, 45);    /* Warn at, degC */

static int r6tb_hyst;                             /* the threshold state; written by r6_temp_poll only */
static uint32_t r6tb_polls, r6tb_ons, r6tb_offs;  /* r6_temp_poll's counters */
static uint32_t r6tb_draws, r6tb_erases;          /* r6_grid.c's counters */
static int r6tb_shown;                            /* the box is drawn */
static char r6tb_last[24];                        /* the last box text drawn */

static int r6tb_warn_at(void) { return COERCE(temp_warn, 30, 70); }

int r6_temp_deg(int t16)
{
    int x = t16 + 8, q = x / 16;
    if (x < 0 && q * 16 != x) q--;               /* floor, not the truncation of / */
    return q;
}

int r6_temp_poll(int t16, int ok, uint32_t st, int st_ok, int * hyst)
{
    const int T = r6tb_warn_at(), a = r6_temp_deg(t16), was = r6tb_hyst;
    if (!ok || a < -40 || a > 99) r6tb_hyst = 0;
    else if (t16 >= 16 * T - 8) r6tb_hyst = 1;
    else if (t16 < 16 * T - 16) r6tb_hyst = 0;
    r6tb_polls++;
    r6tb_ons += r6tb_hyst && !was;
    r6tb_offs += !r6tb_hyst && was;
    *hyst = r6tb_hyst;
    return r6tb_hyst || (st_ok && st);
}

/* the newest poll as the box shows it: 1 = sensor A valid; *a, *warn, the status (st_ok && st != 0 -> *st, else 0) */
struct r6tb_v { int got, valid, a, warn, st, age; struct r6tl_now n; };
static void r6tb_get(struct r6tb_v * v)
{
    memset(v, 0, sizeof(*v));
    v->got = r6tl_now(&v->n);
    if (!v->got) return;
    v->age = get_ms_clock() - v->n.t;
    v->a = r6_temp_deg(v->n.t16[3]);
    v->valid = (v->n.ok & 8) && v->a >= -40 && v->a <= 99 && v->age <= R6TB_STALE_MS;
    v->st = v->n.st_ok && v->n.st ? (v->n.st <= 99 ? (int) v->n.st : 100) : 0;
    v->warn = (v->valid && v->n.hyst) || v->st;
}

/* "A 37 degC", "A 45 degC!", " S2" with a status, "A -- degC" invalid */
static void r6tb_text(char * buf, int n, const struct r6tb_v * v)
{
    char s[8] = "";
    if (v->st) snprintf(s, sizeof(s), v->st < 100 ? " S%d" : " S?", v->st);
    if (v->valid) snprintf(buf, n, "A %d" SYM_DEGREE "C%s%s", v->a, v->warn ? "!" : "", s);
    else snprintf(buf, n, "A --" SYM_DEGREE "C%s", s);
}

int r6_temp_lv_wanted(void) { return temp_lv != 0; }

int r6_temp_lv_text(char * buf, int n, int * warn)
{
    struct r6tb_v v;
    if (!temp_lv || r6_iv_in_pic()) return 0;    /* nothing while ML's picture runs */
    r6tb_get(&v);
    r6tb_text(buf, n, &v);
    *warn = v.warn;
    return 1;
}

uint32_t r6_temp_lv_count(int draw)
{
    r6tb_shown = draw;
    if (draw) return ++r6tb_draws;
    return ++r6tb_erases;
}

/* the log (r6tl_ast): copies only */
void r6_temp_ast(struct r6tl_out * o)
{
    struct r6tb_v v;
    char t[24];
    r6tb_get(&v);
    r6tb_text(t, sizeof(t), &v);
    r6tl_i(o, "tb.cfg=", temp_lv);
    r6tl_i(o, ",", r6tb_warn_at());
    r6tl_i(o, "\ntb.now=", v.valid);
    r6tl_i(o, ",", v.n.t16[3]);
    r6tl_i(o, ",", v.a);
    r6tl_i(o, ",", v.n.st_ok);
    r6tl_u(o, ",", v.n.st);
    r6tl_i(o, ",", v.got ? v.age : -1);
    r6tl_i(o, ",", v.warn);
    r6tl_i(o, ",", v.n.hyst);
    r6tl_u(o, ",", r6tb_polls);
    r6tl_u(o, ",", r6tb_ons);
    r6tl_u(o, ",", r6tb_offs);
    r6tl_i(o, "\ntb.lv=", r6tb_shown);
    r6tl_u(o, ",", r6tb_draws);
    r6tl_u(o, ",", r6tb_erases);
    snprintf(r6tb_last, sizeof(r6tb_last), "%s", t);
    r6tl_pf(o, ",%s\n", r6tb_last);
}

/* ---- menu (Display > Temperature) ---- */
static MENU_UPDATE_FUNC(r6tb_head_update)
{
    struct r6tb_v v;
    char t[24];
    r6tb_get(&v);
    r6tb_text(t, sizeof(t), &v);
    MENU_SET_VALUE("%s", t);
    if (v.warn)
        MENU_SET_WARNING(MENU_WARN_ADVICE, v.st ? "Canon reports over-temperature (status %d)." : "Sensor A at or above %d" SYM_DEGREE "C.",
                         v.st ? v.st : r6tb_warn_at());
    else if (temp_lv && !get_global_draw_setting())
        MENU_SET_WARNING(MENU_WARN_ADVICE, "Global Draw is OFF: no box in LV.");
}

static MENU_UPDATE_FUNC(r6tb_warn_update)
{
    MENU_SET_VALUE("%d" SYM_DEGREE "C", r6tb_warn_at());
}

static MENU_UPDATE_FUNC(r6tb_status_update)
{
    struct r6tb_v v;
    r6tb_get(&v);
    if (!v.got || !v.n.st_ok) MENU_SET_VALUE("invalid");
    else if (!v.n.st) MENU_SET_VALUE("0 (normal)");
    else if (v.n.st <= 9999) MENU_SET_VALUE("%d (Canon warning level)", (int) v.n.st);
    else MENU_SET_VALUE("0x%08x (Canon warning level)", (int) v.n.st);
    if (v.got && v.age > R6TB_STALE_MS) MENU_SET_RINFO("(old)");
}

/* the sensors: the menu names (A first) and their index in Canon's table */
static const char * const r6tb_nm[8] = { "A (EXIF)", "DRAM", "MIMIL", "MIMIR", "TFT (display)", "SH (shutter)", "IISA (stabilizer)",
                                         "IISB (stabilizer)" };
static const uint8_t r6tb_idx[8] = { 3, 0, 1, 2, 4, 5, 6, 7 };

static MENU_UPDATE_FUNC(r6tb_sensor_update)
{
    struct r6tb_v v;
    char c[12];
    int k = 0;
    for (int i = 0; i < 8; i++) if (!strcmp(entry->name, r6tb_nm[i])) k = r6tb_idx[i];
    r6tb_get(&v);
    if (!v.got || !(v.n.ok & (1 << k))) { MENU_SET_VALUE("invalid"); return; }
    int a = r6_temp_deg(v.n.t16[k]);
    MENU_SET_VALUE("%s" SYM_DEGREE "C%s%s", r6tl_c(c, v.n.t16[k]), a < -40 || a > 99 ? " ?" : "", v.age > R6TB_STALE_MS ? " (old)" : "");
}

#define R6TB_SENSOR(nm, h) { .name = nm, .update = r6tb_sensor_update, .icon_type = IT_ALWAYS_ON, .help = h }

static struct menu_entry r6tb_menu[] = {
    {
        .name   = "Temperature",
        .select = menu_open_submenu,
        .update = r6tb_head_update,
        .help   = "Camera temperature sensor A (= EXIF) and Canon's other sensors.",
        .help2  = "M14.1: read only, from Canon's sensor table (once per second).",
        .children = (struct menu_entry[]) {
            {
                .name    = "Show in LV",
                .priv    = &temp_lv,
                .max     = 1,
                .depends_on = DEP_GLOBAL_DRAW,
                .help    = "A box with sensor A (whole " SYM_DEGREE "C, as in EXIF) on the LCD shooting screen (Global Draw).",
                .help2   = "Not in the EVF, not during an exposure. Saved.",
            },
            {
                .name    = "Warn at",
                .priv    = &temp_warn,
                .min     = 30,
                .max     = 70,
                .update  = r6tb_warn_update,
                .help    = "Warning colour from this value on (sensor A), or when Canon reports over-temperature.",
                .help2   = "Off again 0.5 " SYM_DEGREE "C below.",
            },
            {
                .name      = "Canon status",
                .update    = r6tb_status_update,
                .icon_type = IT_ALWAYS_ON,
                .help      = "Canon's over-temperature level (PROP_TEMP_STATUS). Any value other than 0 sets the warning colour.",
            },
            R6TB_SENSOR("A (EXIF)", "Canon's camera temperature, written to EXIF at the shutter press."),
            R6TB_SENSOR("DRAM", "Memory temperature (from Canon's image processor)."),
            R6TB_SENSOR("MIMIL", "Image processor, left (Canon's name)."),
            R6TB_SENSOR("MIMIR", "Image processor, right (Canon's name)."),
            R6TB_SENSOR("TFT (display)", "Near the rear display."),
            R6TB_SENSOR("SH (shutter)", "Shutter unit; Canon picks its burst tables by it."),
            R6TB_SENSOR("IISA (stabilizer)", "In-body stabilizer, sensor 1."),
            R6TB_SENSOR("IISB (stabilizer)", "In-body stabilizer, sensor 2."),
            MENU_EOL
        },
    },
};

static void r6tb_init()
{
    menu_add("Display", r6tb_menu, COUNT(r6tb_menu));
}

INIT_FUNC("r6_temp", r6tb_init);

#endif /* CONFIG_R6_TEMP */
