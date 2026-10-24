/*
 * R6.190 M16.1 (spec m16/m161 spec_m161.md Rev. 2a 2, 3.2, 4.1, 5, 6, 7.1; plan docs/m16_plan.md E5..E8, rules 1..27, K1..K9; K3
 * answer F1 (a) 2026-10-23; CONFIG_R6_SCRIPT): the script facade. lua.mo controls the camera only through these functions, and
 * they only use the camera-tested R6 paths:
 *  - pictures: the script job "picture" of r6_iv_task (r6_interval.c, command 7: every Interval check, iv_shoot through the two
 *    shutter callbacks); asynchronous from GUI tasks (2000, r6_last_shot);
 *  - Tv: the script job "Tv" (command 8, the runner of Tor 1's context "script", r6_propw.c); this file never names _r6pw_*;
 *  - the R6 menu commands: the entries' select functions, run in GuiMainTask (ML event MLEV_R6_SCRIPT, the GUI slot) with the
 *    origin mark R6GO_SCRIPT, never while an ML picture, a Tor 1 window or an ETTR run is on (the intent mark of r6_interval.c);
 *    stops never through a toggle; menu.set on R6 entries fail-closed per menu; the Astro red hulls (origin + start check);
 *  - E8 in the caller's task: SetGUIRequestMode (camera.gui.*, lv.start / stop), key presses with their release, REC, magnify
 *    (GUI_Control with FAKE_BTN / fake_simple_button), waiting while an ML picture / Tv write / ETTR run is on.
 * Canon: SetGUIRequestMode and gui_enqueue_message (through GUI_Control / fake_simple_button), nothing else; read only: the
 * settings sheet [cc+0x34FC] (Tor 0 a), Canon's job count, TempSvc (Tor 0g), sys_mem (Tor 0h). No property write, no MMIO, no
 * call(). Exported (R6_190.sym): exactly the functions of r6_api_abi.h; everything else is static or "_" (A10).
 */
#include "internals.h"                  /* the switches only (no token): without CONFIG_R6_SCRIPT this file is empty (G2) */
#ifdef CONFIG_R6_SCRIPT
#include "dryos.h"
#include "bmp.h"
#include "menu.h"
#include "config.h"
#include "property.h"
#include "propvalues.h"
#include "lens.h"
#include "shoot.h"
#include "module.h"
#include "tasks.h"
#include "gui-common.h"
#include "state-object.h"                /* wait_lv_frames (r6_modsup.c) */
#include "r6_interval.h"
#include "r6_faceyuv.h"

#include "r6_modsup.h"
#include "r6_api.h"
#ifdef CONFIG_R6_TLOG
#include "r6_tlog.h"
#endif
#ifdef CONFIG_R6_TEMP
#include "r6_temp.h"
#endif
#ifdef CONFIG_R6_ASTRO
#include "r6_astro.h"
#endif
#ifdef CONFIG_R6_ETTR
#include "r6_ettr.h"
#endif

#if !defined(CONFIG_R6_MODSUP) || !defined(CONFIG_R6_IV_SCRIPT)
#error "r6_api.c: CONFIG_R6_SCRIPT needs CONFIG_R6_MODSUP and CONFIG_R6_IV_SCRIPT"
#endif
#ifdef CONFIG_PROP_REQUEST_CHANGE
#error "r6_api.c: property writes stay off"
#endif

_Static_assert(R6API_PASS != INT_MIN, "MB-5: R6API_PASS must not be upstream's 'not found'");
_Static_assert(R6API_PASS != R6API_OK && R6API_PASS != R6API_TASK && R6API_PASS != R6API_TIMEOUT && R6API_PASS != R6API_BUSY
    && R6API_PASS != R6API_GUITASK && R6API_PASS != R6API_CANCELLED && R6API_PASS != R6API_EARLY && R6API_PASS != R6API_UNRES
    && R6API_PASS != R6API_LATE && R6API_PASS != R6API_NOSET && R6API_PASS != R6API_ARG && R6API_PASS != R6API_STACK,
    "MB-5: R6API_PASS differs from every other code");
_Static_assert(MLEV_R6_SCRIPT != MLEV_HIJACK_FORMAT_DIALOG_BOX && MLEV_R6_SCRIPT != MLEV_TURN_ON_DISPLAY
    && MLEV_R6_SCRIPT != MLEV_TURN_OFF_DISPLAY && MLEV_R6_SCRIPT != MLEV_LCD_SENSOR_START && MLEV_R6_SCRIPT != MLEV_REDRAW
    && MLEV_R6_SCRIPT != MLEV_BV_DISABLE && MLEV_R6_SCRIPT != MLEV_BV_ENABLE && MLEV_R6_SCRIPT != MLEV_BV_AUTO_UPDATE
    && MLEV_R6_SCRIPT != MLEV_MENU_OPEN && MLEV_R6_SCRIPT != MLEV_MENU_CLOSE && MLEV_R6_SCRIPT != MLEV_MENU_REDRAW
    && MLEV_R6_SCRIPT != MLEV_AV_SHORT && MLEV_R6_SCRIPT != MLEV_AV_LONG && MLEV_R6_SCRIPT != MLEV_TRIGGER_ZEBRAS_FOR_PLAYBACK
    && MLEV_R6_SCRIPT != MLEV_JOYSTICK_LONG, "MLEV_R6_SCRIPT is free (src/gui-common.h)");

#define R6API_AS_GREY  4        /* = R6AS_PAL_GREY (r6_astro.c:91, r6_astro_pal bit 2: Full is on; the stage gate compares) */
#define R6API_GUIMAIN  "GuiMainTask"

/* ---- small helpers ---- */
static void r6api_inc(volatile uint32_t * p) { uint32_t v; do v = *p; while (!r6_cas32(p, v, v + 1)); }
static int r6api_down(void) { return ml_shutdown_requested || _r6iv_scr_shutdown(); }
static int r6api_why(char * why, int n, const char * fmt, const char * s)
{
    if (why && n > 0) snprintf(why, n, fmt, s ? s : "");
    return 0;
}
static int r6api_set(char * why, int n, int res, const char * s) { r6api_why(why, n, "%s", s); return res; }
static int r6api_streq(const char * a, const char * b) { return a && b && streq(a, b); }

/* ======== 2.2: task classes ======== */
static int r6api_class_of(const char * t)
{
    if (r6api_streq(t, R6API_GUIMAIN) || r6api_streq(t, "menu_task") || r6api_streq(t, "menu_redraw_task")) return R6TC_ASYNC;
    if (r6api_streq(t, "PropMgr") || r6api_streq(t, "r6_iv_task")) return R6TC_REFUSE;
    return R6TC_SYNC;
}

int r6_script_task_class(const char * task_name) { return r6api_class_of(task_name ? task_name : get_current_task_name()); }

static int r6api_class(void) { return r6api_class_of(get_current_task_name()); }
static int r6api_guimain(void) { return r6api_streq(get_current_task_name(), R6API_GUIMAIN); }

/* ======== scr.org, scr.cnt (2.3, 5.5) ======== */
#define R6API_ORG_RING 32
static struct r6api_org r6api_orgs[R6API_ORG_RING];
static volatile uint32_t r6api_org_n = 0;
static volatile uint32_t r6api_cnt[8];           /* menu, script, fake, key, refused; late, gui_jobs, ast_ref */
static int r6api_tvw_last = -1;                  /* "Tv writes" at the last script end */
static uint32_t r6api_hint_gen = 0;              /* rule 7: the WLAN / BT / USB hint once per script run */

static int r6api_task_code(void)
{
    const char * t = get_current_task_name();
    if (r6api_streq(t, R6API_GUIMAIN)) return 1;
    if (r6api_streq(t, "menu_task")) return 2;
    if (t && t[0] == 'l' && t[1] == 'u' && t[2] == 'a') return 3;
    if (r6api_streq(t, "r6_iv_task")) return 4;
    return 0;
}

static void r6api_org_add(int org, int cmd, int arg, int res)
{
    uint32_t k;
    do k = r6api_org_n; while (!r6_cas32(&r6api_org_n, k, k + 1));
    struct r6api_org * e = &r6api_orgs[k % R6API_ORG_RING];
    e->t = get_ms_clock();
    e->task = r6api_task_code();
    e->org = org;
    e->cmd = cmd;
    e->pad = 0;
    e->arg = arg;
    e->res = res;
    if (org == R6GO_SCRIPT) r6api_inc(&r6api_cnt[1]);
    else if (org == R6GO_FAKE) r6api_inc(&r6api_cnt[2]);
    else if (org == R6GO_KEY) r6api_inc(&r6api_cnt[3]);
    else r6api_inc(&r6api_cnt[0]);
    if (res && res != R6API_QUEUED) r6api_inc(&r6api_cnt[4]);
}

void _r6api_org_note(int what) { r6api_org_add(_r6_gui_org(), what, 0, 0); }

int _r6api_org_ring(int i, struct r6api_org * e)
{
    uint32_t n = r6api_org_n;
    int cnt = n < R6API_ORG_RING ? (int) n : R6API_ORG_RING;
    if (e && i >= 0 && i < cnt) *e = r6api_orgs[(n - cnt + i) % R6API_ORG_RING];
    return cnt;
}

void _r6api_counts(uint32_t c[8]) { for (int i = 0; i < 8; i++) c[i] = r6api_cnt[i]; }
int _r6api_tvw_end(void) { return r6api_tvw_last; }
uint32_t _r6api_ast_ref(void) { return r6api_cnt[7]; }

static void r6api_hint(uint32_t gen)
{
    if (!gen || gen == r6api_hint_gen) return;
    r6api_hint_gen = gen;
    NotifyBox(2000, "Lua: WLAN/BT/USB should be off for script pictures and Tv writes");
}

/* ======== 3.2: pictures ======== */
/* the wait for a job of the script slot (2.5): until ACTIVE at most limit_ms from t_req, then cancelled (R6API_TIMEOUT); after
 * ACTIVE until DONE, the backstop limit_ms again (R6API_LATE, never expected). 1 = done (*o), else the code */
static int r6api_wait_job(uint32_t seq, int limit_ms, struct r6_iv_scr_out * o)
{
    int t0 = get_ms_clock(), t_act = 0;
    for (;;)
    {
        if (_r6iv_scr_take(seq, o)) return 1;
        int s = _r6iv_scr_state(seq), now = get_ms_clock();
        if (s < 0) return R6API_CANCELLED;                       /* freed by someone else (script end, Interval Start) */
        if (s == R6IV_SS_ACTIVE || s == R6IV_SS_ABANDONED)
        {
            if (!t_act) t_act = now | 1;
            if (now - (t_act & ~1) >= limit_ms && _r6iv_scr_cancel(seq, 1) == 2) { r6api_inc(&r6api_cnt[5]); return R6API_LATE; }
        }
        else if (now - t0 >= limit_ms && _r6iv_scr_cancel(seq, 0)) return R6API_TIMEOUT;   /* POSTED / TAKEN: nothing happened */
        msleep(R6API_POLL_MS);
    }
}

int r6_take_a_pic_ex(uint32_t gen, int should_af, int timeout_ms, uint32_t * seq, char * why, int n)
{
    uint32_t q = 0;
    if (seq) *seq = 0;
    int cls = r6api_class();
    if (cls == R6TC_REFUSE) return r6api_set(why, n, R6API_TASK, "wrong task");
    if (should_af == AF_ENABLE) return r6api_set(why, n, R6API_CAM + R6IV_CAM_FOCUS, r6_iv_cam_name(R6IV_CAM_FOCUS));   /* K5 */
    if (r6api_down()) return r6api_set(why, n, R6API_BUSY, "shutting down");     /* SL-10 */
    int async = cls == R6TC_ASYNC;
    if (_r6iv_scr_post(R6IV_SK_SHOT, 0, gen, async, &q)) return r6api_set(why, n, R6API_BUSY, "another script job is open");
    if (seq) *seq = q;
    r6api_hint(gen);
    if (async) return r6api_set(why, n, R6API_QUEUED, "queued");                  /* r6_last_shot gets the result (SL-13) */
    struct r6_iv_scr_out o;
    int r = r6api_wait_job(q, timeout_ms > 0 ? timeout_ms : _r6iv_scr_limit_ms(), &o);
    if (r == 1) return r6api_set(why, n, o.res, o.why);
    return r6api_set(why, n, r, r == R6API_TIMEOUT ? "timeout (no picture)" : r == R6API_LATE ? "late (see r6.last_shot)" : "cancelled");
}

int r6_script_burst(uint32_t gen, int n_pics, int * done, char * why, int n)
{
    int r = 0;
    if (done) *done = 0;
    if (r6api_class() != R6TC_SYNC) return r6api_set(why, n, r6api_class() == R6TC_REFUSE ? R6API_TASK : R6API_GUITASK, "not from a GUI task");
    for (int i = 0; i < n_pics; i++)
    {
        if (i) msleep(200);                                     /* >= 100 ms without a job: the Trash press finds a gap (R-14) */
        r = r6_take_a_pic_ex(gen, AF_DISABLE, 0, 0, why, n);
        if (r) return r;
        if (done) (*done)++;
    }
    return r6api_set(why, n, R6API_OK, "ok");
}

/* camera.bulb(s): only in B with Canon's bulb timer = s (+- 0.5 s) between 31 s and 30 min: the D1 way of M9.2 */
int r6_script_bulb(uint32_t gen, int seconds, char * why, int n)
{
    if (r6api_class() != R6TC_SYNC) return r6api_set(why, n, r6api_class() == R6TC_REFUSE ? R6API_TASK : R6API_GUITASK, "not from a GUI task");
    struct r6_iv_cc s;
    r6_iv_cc_read(&s);
    int t = r6_iv_bulb_t(&s);
    if (shooting_mode != SHOOTMODE_BULB || t < r6_iv_bulb_min_s() || t > r6_iv_bulb_max_s() || t != seconds)
    {
        if (why && n > 0) snprintf(why, n, "set Canon's bulb timer to %d s in B (holding needs M16.2)", seconds);
        return R6API_ARG;
    }
    return r6_take_a_pic_ex(gen, AF_DISABLE, 0, 0, why, n);
}

int r6_script_wait_jobs(int timeout_ms)
{
    if (r6api_class() != R6TC_SYNC) return r6api_class() == R6TC_REFUSE ? R6API_TASK : R6API_GUITASK;
    int t0 = get_ms_clock();
    for (;;)
    {
        if (!_r6iv_scr_busy() && !r6_iv_in_pic() && *(volatile uint32_t *) R6IV_RAM_JOBCNT == 0) return R6API_OK;
        if (get_ms_clock() - t0 >= timeout_ms) return R6API_TIMEOUT;
        msleep(R6API_POLL_MS);
    }
}

int r6_last_shot(struct r6_shot_res * r)
{
    struct r6_iv_scr_out o;
    uint32_t gen = 0;
    int a = 0, c = 0;
    memset(r, 0, sizeof(*r));
    int k = _r6iv_scr_last(&o, &gen, &a, &c);
    if (k <= 0) return k;
    r->seq = o.seq; r->gen = gen; r->t_req = o.t_req; r->t_done = o.t_done; r->res = o.res; r->async = a; r->cancelled = c;
    memcpy(r->why, o.why, sizeof(r->why));
    r->why[sizeof(r->why) - 1] = 0;
    return 1;
}

/* ======== 4.1: Tv ======== */
int r6_script_tv(uint32_t gen, int code768, char * why, int n)
{
    uint32_t q = 0;
    int cls = r6api_class();
    if (cls == R6TC_REFUSE) return r6api_set(why, n, R6API_TASK, "wrong task");
    if (cls == R6TC_ASYNC) return r6api_set(why, n, R6API_GUITASK, "not from a GUI task (use task.create)");
    if (r6api_down()) return r6api_set(why, n, R6API_BUSY, "shutting down");
    if (_r6iv_scr_post(R6IV_SK_TV, code768, gen, 0, &q)) return r6api_set(why, n, R6API_BUSY, "another script job is open");
    r6api_hint(gen);
    struct r6_iv_scr_out o;
    int r = r6api_wait_job(q, R6API_TV_MS, &o);
    if (r == 1) return r6api_set(why, n, o.res, o.why);
    return r6api_set(why, n, r, r == R6API_TIMEOUT ? "timeout (not written)" : r == R6API_LATE ? "late (see scr.tv)" : "cancelled");
}

/* 4.5: Canon's settings sheet p = [cc+0x34FC] (Tor 0 a): Tv [p+0x61E], Av [p+0x61C], Sv [p+0x510], compensation [p+0x4A0], Code768 */
int r6_script_expo(struct r6_expo * e)
{
    struct r6_iv_cc s;
    memset(e, 0, sizeof(*e));
    r6_iv_cc_read(&s);
    if (!(s.valid & R6IV_V_CC) || !(s.valid & R6IV_V_P) || !r6_fy_ram_ok(s.p + 0x4A0, 0x180)) return 0;
    uint32_t w61c = *(volatile uint32_t *) (s.p + 0x61C), w510 = *(volatile uint32_t *) (s.p + 0x510), w4a0 = *(volatile uint32_t *) (s.p + 0x4A0);
    e->tv = (int16_t) (w61c >> 16);
    e->av = (int16_t) (w61c & 0xFFFF);
    e->sv = (int16_t) (w510 & 0xFFFF);
    e->comp = (int16_t) (w4a0 & 0xFFFF);
    e->valid = 0x0F;
    return 1;
}

/* ======== the small readers (5.6, 7.2, 7.4) ======== */
int r6_script_sysmem(struct r6_sysmem * m)
{
    struct r6_sysq q;
    memset(m, 0, sizeof(*m));
    int ok = lp_sys_query(&q);
    m->ok = ok;
    m->largest = q.largest;
    m->free = q.free;
    m->used = q.used;
    m->peak = q.peak;
    snprintf(m->why, sizeof(m->why), "%s", r6sq_why_name(q.why));
    return ok;
}

int r6_script_prop_state(uint32_t id) { return _r6_prop_state(id); }

/* camera.temperature: sensor A in whole degC as Canon rounds it for EXIF (r6tl_now + r6_temp_deg), *deci_c = 10 x that */
int r6_script_temp(int * deci_c)
{
#if defined(CONFIG_R6_TLOG) && defined(CONFIG_R6_TEMP)
    struct r6tl_now o;
    if (!r6tl_now(&o) || !(o.ok & (1 << 3))) return 0;
    *deci_c = 10 * r6_temp_deg(o.t16[3]);
    return 1;
#else
    return 0;
#endif
}

int r6_script_dst(void)
{
#ifdef CONFIG_R6_ACLOCK
    return _r6_ac_dst();
#else
    return -1;
#endif
}

int r6_lv_running(void)
{
    int t_menu = 0, t_lv = -1, in_pic = 0;
    _r6iv_times(&t_menu, &t_lv, &in_pic);
    return t_lv >= 0;
}

/* ======== 5: the R6 menus (the nine areas, fail-closed per area, 5.2 / MB-6) ======== */
enum { A_IV, A_MD, A_ETTR, A_AST, A_TEMP, A_ACLK, A_GRID, A_HIST, A_LP, A_N };
static const struct { const char * menu, * head; } r6api_areas[A_N] = {
    { "Shoot", "Interval" }, { "Shoot", "Motion trigger" }, { "Shoot", "ETTR" }, { "Display", "Astro red" },
    { "Display", "Temperature" }, { "Display", "Astro clock" }, { "Overlay", "Grids & masks" }, { "Overlay", "Histogram (Canon)" },
    { "Debug", "LV probe" },
};
/* the R6 submenus (a lookup that fails there is UNRES, never upstream's path) */
static const char * const r6api_submenus[] = { "Interval", "Motion trigger", "ETTR", "Astro red", "Temperature", "Astro clock",
    "Tonight", "Now", "Polar scope", "LV probe" };

/* the kinds of an R6 entry: an action (once), Start (ON starts, OFF stops), Tv writes, Astro Mode, a setting without a select
 * (range-checked priv write), a setting with a select (the select, shortest direction), a switch with consequences (its select),
 * not settable, a submenu head */
enum { RT_ACT = 1, RT_START, RT_TVW, RT_ASTMODE, RT_SET, RT_SETSEL, RT_SWITCH, RT_NO, RT_SUB };
struct r6api_ent { uint8_t area, kind, cmd, pad; const char * menu, * name; };
/* the table: every entry of the nine menu sources, with the same #ifdef (G9 compares the names with gcc -E of those sources) */
static const struct r6api_ent r6api_tab[] = {
#ifdef CONFIG_R6_INTERVAL
    { A_IV, RT_SUB, 0, 0, "Shoot", "Interval" },
    { A_IV, RT_START, R6SC_IV_START, 0, "Interval", "Start" },
    { A_IV, RT_ACT, R6SC_IV_TEST, 0, "Interval", "Test: 1 picture" },
    { A_IV, RT_SET, 0, 0, "Interval", "Interval" },
    { A_IV, RT_SET, 0, 0, "Interval", "Pictures" },
    { A_IV, RT_SET, 0, 0, "Interval", "Start delay" },
    { A_IV, RT_SET, 0, 0, "Interval", "Mode" },
    { A_IV, RT_SET, 0, 0, "Interval", "Bracket shots" },
    { A_IV, RT_SET, 0, 0, "Interval", "Bracket step" },
    { A_IV, RT_SET, 0, 0, "Interval", "Bracket order" },
    { A_IV, RT_NO, 0, 0, "Interval", "Bracket Tv" },
    { A_IV, RT_TVW, R6SC_TVW, 0, "Interval", "Tv writes" },
    { A_IV, RT_ACT, R6SC_TVBACK, 0, "Interval", "Tv back to" },
    { A_IV, RT_NO, 0, 0, "Interval", "Camera" },
    { A_IV, RT_NO, 0, 0, "Interval", "Canon" },
    { A_IV, RT_NO, 0, 0, "Interval", "Last series" },
    { A_IV, RT_NO, 0, 0, "Interval", "Auto power off" },
#endif
#ifdef CONFIG_R6_MOTION
    { A_MD, RT_SUB, 0, 0, "Shoot", "Motion trigger" },
    { A_MD, RT_START, R6SC_MD_START, 0, "Motion trigger", "Start" },
    { A_MD, RT_SETSEL, 0, 0, "Motion trigger", "Mode" },
    { A_MD, RT_SET, 0, 0, "Motion trigger", "Trigger" },
    { A_MD, RT_SET, 0, 0, "Motion trigger", "Area" },
    { A_MD, RT_SET, 0, 0, "Motion trigger", "Position" },
    { A_MD, RT_SET, 0, 0, "Motion trigger", "Level" },
    { A_MD, RT_SET, 0, 0, "Motion trigger", "Confirm" },
    { A_MD, RT_SETSEL, 0, 0, "Motion trigger", "Pictures" },
    { A_MD, RT_SET, 0, 0, "Motion trigger", "Wait after picture" },
    { A_MD, RT_SET, 0, 0, "Motion trigger", "Start delay" },
    { A_MD, RT_SET, 0, 0, "Motion trigger", "Max duration" },
    { A_MD, RT_SET, 0, 0, "Motion trigger", "Display" },
    { A_MD, RT_NO, 0, 0, "Motion trigger", "Detector" },
    { A_MD, RT_NO, 0, 0, "Motion trigger", "Camera" },
    { A_MD, RT_NO, 0, 0, "Motion trigger", "Last run" },
#endif
#ifdef CONFIG_R6_ETTR
    { A_ETTR, RT_SUB, 0, 0, "Shoot", "ETTR" },
    { A_ETTR, RT_SET, 0, 0, "ETTR", "Mode" },
    { A_ETTR, RT_ACT, R6SC_ETTR_APPLY, 0, "ETTR", "Apply once" },
    { A_ETTR, RT_SET, 0, 0, "ETTR", "Target" },
    { A_ETTR, RT_SET, 0, 0, "ETTR", "Highlight ignore" },
    { A_ETTR, RT_SET, 0, 0, "ETTR", "Longest Tv" },
    { A_ETTR, RT_SET, 0, 0, "ETTR", "Shortest Tv" },
    { A_ETTR, RT_SET, 0, 0, "ETTR", "Show in LV" },
    { A_ETTR, RT_NO, 0, 0, "ETTR", "Proposal" },
    { A_ETTR, RT_NO, 0, 0, "ETTR", "Measurement" },
#endif
#ifdef CONFIG_R6_ASTRO
    { A_AST, RT_SUB, 0, 0, "Display", "Astro red" },
    { A_AST, RT_ASTMODE, R6SC_AST_MODE, 0, "Astro red", "Mode" },
    { A_AST, RT_SET, 0, 0, "Astro red", "LCD level" },
    { A_AST, RT_SET, 0, 0, "Astro red", "EVF level" },
    { A_AST, RT_ACT, R6SC_AST_TEST, 0, "Astro red", "Test: 10 s" },
    { A_AST, RT_NO, 0, 0, "Astro red", "Status" },
    { A_AST, RT_NO, 0, 0, "Astro red", "Palette" },
#endif
#ifdef CONFIG_R6_TEMP
    { A_TEMP, RT_SUB, 0, 0, "Display", "Temperature" },
    { A_TEMP, RT_SET, 0, 0, "Temperature", "Show in LV" },
    { A_TEMP, RT_SET, 0, 0, "Temperature", "Warn at" },
    { A_TEMP, RT_NO, 0, 0, "Temperature", "Canon status" },
    { A_TEMP, RT_NO, 0, 0, "Temperature", "A (EXIF)" },          /* the eight sensor rows (R6TB_SENSOR): read only */
    { A_TEMP, RT_NO, 0, 0, "Temperature", "DRAM" },
    { A_TEMP, RT_NO, 0, 0, "Temperature", "MIMIL" },
    { A_TEMP, RT_NO, 0, 0, "Temperature", "MIMIR" },
    { A_TEMP, RT_NO, 0, 0, "Temperature", "TFT (display)" },
    { A_TEMP, RT_NO, 0, 0, "Temperature", "SH (shutter)" },
    { A_TEMP, RT_NO, 0, 0, "Temperature", "IISA (stabilizer)" },
    { A_TEMP, RT_NO, 0, 0, "Temperature", "IISB (stabilizer)" },
#endif
#ifdef CONFIG_R6_ACLOCK
    { A_ACLK, RT_SUB, 0, 0, "Display", "Astro clock" },
    { A_ACLK, RT_SUB, 0, 0, "Astro clock", "Tonight" },
    { A_ACLK, RT_NO, 0, 0, "Tonight", "Night of" },
    { A_ACLK, RT_NO, 0, 0, "Tonight", "Sun" },
    { A_ACLK, RT_NO, 0, 0, "Tonight", "Civil twilight" },
    { A_ACLK, RT_NO, 0, 0, "Tonight", "Nautical" },
    { A_ACLK, RT_NO, 0, 0, "Tonight", "Astro night" },
    { A_ACLK, RT_NO, 0, 0, "Tonight", "Moon" },
    { A_ACLK, RT_NO, 0, 0, "Tonight", "Milky Way" },
    { A_ACLK, RT_SUB, 0, 0, "Astro clock", "Now" },
    { A_ACLK, RT_NO, 0, 0, "Now", "Local time" },
    { A_ACLK, RT_NO, 0, 0, "Now", "Sun altitude" },
    { A_ACLK, RT_NO, 0, 0, "Now", "Moon" },
    { A_ACLK, RT_NO, 0, 0, "Now", "Moon age" },
    { A_ACLK, RT_NO, 0, 0, "Now", "Next full moon" },
    { A_ACLK, RT_NO, 0, 0, "Now", "Next new moon" },
    { A_ACLK, RT_NO, 0, 0, "Now", "Galactic centre" },
    { A_ACLK, RT_SUB, 0, 0, "Astro clock", "Polar scope" },
    { A_ACLK, RT_NO, 0, 0, "Polar scope", "Polaris HA" },
    { A_ACLK, RT_NO, 0, 0, "Polar scope", "Polaris clock" },
    { A_ACLK, RT_SET, 0, 0, "Polar scope", "Clock view" },
    { A_ACLK, RT_SET, 0, 0, "Astro clock", "Latitude" },
    { A_ACLK, RT_SET, 0, 0, "Astro clock", "Longitude" },
    { A_ACLK, RT_SET, 0, 0, "Astro clock", "Time zone" },
    { A_ACLK, RT_SET, 0, 0, "Astro clock", "Manual offset" },
    { A_ACLK, RT_SET, 0, 0, "Astro clock", "MW: centre above" },
    { A_ACLK, RT_SET, 0, 0, "Astro clock", "MW: sun below" },
    { A_ACLK, RT_SET, 0, 0, "Astro clock", "Show in LV" },
    { A_ACLK, RT_NO, 0, 0, "Astro clock", "Status" },
#endif
#ifdef CONFIG_R6_GRID
    { A_GRID, RT_SET, 0, 0, "Overlay", "Grids & masks" },
    { A_GRID, RT_SET, 0, 0, "Grids & masks", "Grid" },
    { A_GRID, RT_SET, 0, 0, "Grids & masks", "Center mark" },
    { A_GRID, RT_SET, 0, 0, "Grids & masks", "Aspect frame" },
    { A_GRID, RT_SET, 0, 0, "Grids & masks", "Aspect style" },
    { A_GRID, RT_SET, 0, 0, "Grids & masks", "Safe areas" },
    { A_GRID, RT_SET, 0, 0, "Grids & masks", "Color" },
    { A_GRID, RT_SET, 0, 0, "Grids & masks", "Line" },
#endif
#ifdef FEATURE_R6_CANON_HISTOGRAM
    { A_HIST, RT_SET, 0, 0, "Overlay", "Histogram (Canon)" },
#endif
#ifdef FEATURE_LV_BUFFER_PROBE
    { A_LP, RT_SUB, 0, 0, "Debug", "LV probe" },
    { A_LP, RT_SWITCH, 0, 0, "LV probe", "LV flag follows Canon" },
    { A_LP, RT_SWITCH, 0, 0, "LV probe", "Status line" },
    { A_LP, RT_ACT, R6SC_SELECT, 0, "LV probe", "Log snapshot" },
#ifdef CONFIG_R6_ASTRO
    { A_LP, RT_ACT, R6SC_SELECT, 0, "LV probe", "Log snapshot in 10 s" },
#ifdef CONFIG_R6_SNAPMEM
    { A_LP, RT_NO, 0, 0, "LV probe", "Snapshot memory" },
#ifdef CONFIG_R6_TASKMEM
    { A_LP, RT_NO, 0, 0, "LV probe", "Task memory" },
#endif
#ifdef CONFIG_R6_TLOG
    { A_LP, RT_SET, 0, 0, "LV probe", "Skip temp/clock log" },
#endif
#endif
    { A_LP, RT_SWITCH, 0, 0, "LV probe", "Display regs" },
    { A_LP, RT_ACT, R6SC_SELECT, 0, "LV probe", "Key test" },
#endif
    { A_LP, RT_ACT, R6SC_SELECT, 0, "LV probe", "FaceYuv sample" },
    { A_LP, RT_SWITCH, 0, 0, "LV probe", "Expo log" },
#endif
};
#define R6API_NTAB ((int) (sizeof(r6api_tab) / sizeof(r6api_tab[0])))

static struct menu_entry * r6api_ents[R6API_NTAB];   /* resolved once (r6api_init) */
static struct menu_entry * r6api_heads[A_N];
static uint8_t r6api_unres[A_N];                     /* 1 = an expected entry of this area is missing: the whole area UNRES */
static int r6api_resolved = 0;
static int r6api_ast_ok = 0;                         /* the two Astro hulls are in place (key.press needs them, 5.3) */

/* the top entry of e (its parent chain, as menu_add builds it) and its area (A_*), -1 = not one of the nine */
static int r6api_area_of(struct menu_entry * e)
{
    for (int i = 0; e && e->parent && i < 8; i++) e = e->parent;
    if (!e || !e->parent_menu || !e->name) return -1;
    for (int a = 0; a < A_N; a++)
        if (r6api_streq(e->name, r6api_areas[a].head) && r6api_streq(e->parent_menu->name, r6api_areas[a].menu)) return a;
    return -1;
}

int _r6api_menu_r6(struct menu_entry * e) { return r6api_area_of(e) >= 0; }

static int r6api_is_r6_menu(const char * menu)
{
    for (int i = 0; i < (int) COUNT(r6api_submenus); i++) if (r6api_streq(menu, r6api_submenus[i])) return 1;
    for (int a = 0; a < A_N; a++) if (r6api_streq(menu, r6api_areas[a].head)) return 1;
    return 0;
}

/* ======== 5.3: the Astro red hulls ======== */
static void (*r6api_ast_mode_orig)(void * priv, int delta) = 0;
static void (*r6api_ast_test_orig)(void * priv, int delta) = 0;
static volatile int r6api_hull_ref = 0;              /* the newest refusal of a hull: 1 origin, 2 the start check */

static void r6api_hull_refuse(int why)
{
    r6api_hull_ref = why;
    r6api_inc(&r6api_cnt[7]);
    if (r6api_guimain()) NotifyBox(2000, why == 2 ? "Astro red: wait for the start check" : "Astro red: not from here");
}

/* the hull's own prediction from *priv and delta (the formula of r6as_mode_select, astro_gate 14g): Full -> origin and start check */
static void _r6api_ast_mode_hull(void * priv, int delta)
{
    int org = _r6_gui_org();
    if (!r6api_guimain() || org == R6GO_NONE || !priv || !r6api_ast_mode_orig) { r6api_hull_refuse(1); return; }
    int m = COERCE(*(int *) priv, 0, 2);
    int full = (m + (delta < 0 ? 2 : 1)) % 3 == 2;
    if (full && org != R6GO_KEY && get_ms_clock() < R6API_T2_BOOT_MS) { r6api_hull_refuse(2); return; }   /* 5.4 (MB-2) */
    r6api_hull_ref = 0;
    if (org != R6GO_SCRIPT) r6api_org_add(org, R6SC_AST_MODE, delta, 0);   /* scr.org: a key's select (the facade logs its own, MB-8) */
    r6api_ast_mode_orig(priv, delta);
}

static void _r6api_ast_test_hull(void * priv, int delta)
{
    int org = _r6_gui_org();
    if (!r6api_guimain() || org == R6GO_NONE || !r6api_ast_test_orig) { r6api_hull_refuse(1); return; }
    if (org != R6GO_KEY && get_ms_clock() < R6API_T2_BOOT_MS) { r6api_hull_refuse(2); return; }   /* 5.4: red without Full is Tor 2 too */
    r6api_hull_ref = 0;
    if (org != R6GO_SCRIPT) r6api_org_add(org, R6SC_AST_TEST, delta, 0);   /* scr.org: a key's select (MB-8) */
    r6api_ast_test_orig(priv, delta);
}

/* once, after the last R6 menu_add (r6_api.o is the last R6 object: .init_funcs in link order) and before module_load_task */
static void r6api_init(void * unused)
{
    for (int a = 0; a < A_N; a++) r6api_heads[a] = _r6_menu_entry(r6api_areas[a].menu, r6api_areas[a].head);
    for (int i = 0; i < R6API_NTAB; i++)
    {
        const struct r6api_ent * t = &r6api_tab[i];
        struct menu_entry * e = _r6_menu_entry(t->menu, t->name);
        if (!e || r6api_area_of(e) != t->area || !r6api_heads[t->area]) { r6api_unres[t->area] = 1; e = 0; }
        r6api_ents[i] = e;
        if (!e) printf("[R6] r6_api: %s -> %s not resolved\n", t->menu, t->name);
    }
#ifdef CONFIG_R6_ASTRO
    int mi = -1, ti = -1;
    for (int i = 0; i < R6API_NTAB; i++)
    {
        if (r6api_tab[i].kind == RT_ASTMODE) mi = i;
        if (r6api_tab[i].cmd == R6SC_AST_TEST) ti = i;
    }
    if (mi >= 0 && ti >= 0 && r6api_ents[mi] && r6api_ents[ti] && r6api_ents[mi]->select && r6api_ents[ti]->select && !r6api_unres[A_AST])
    {
        r6api_ast_mode_orig = r6api_ents[mi]->select;
        r6api_ast_test_orig = r6api_ents[ti]->select;
        r6_barrier();
        r6api_ents[mi]->select = _r6api_ast_mode_hull;
        r6api_ents[ti]->select = _r6api_ast_test_hull;
        r6api_ast_ok = 1;
    }
    else r6api_unres[A_AST] = 1;
#endif
    r6api_resolved = 1;
}

/* the INIT_FUNC by hand: static, so the record is not exported (INIT_FUNC's task_create_* object would be) */
static struct task_create __attribute__((section(".init_funcs"), used)) r6api_init_rec = { .name = "r6_api", .entry = r6api_init };

static int r6api_tab_find(int cmd)
{
    for (int i = 0; i < R6API_NTAB; i++) if (r6api_tab[i].cmd == cmd && r6api_tab[i].kind != RT_SUB) return i;
    return -1;
}

/* ======== 5.1: running an R6 command (GuiMainTask, mark R6GO_SCRIPT around it) ======== */
/* one job: a command (cmd, arg) or an entry (i: the table index, target) */
struct r6api_job { int cmd, arg, i, target; };

static int r6api_sel(struct menu_entry * e, int delta)
{
    if (!e || !e->select) return 0;
    e->select(e->priv, delta);
    return 1;
}

/* a priv value with a select: the select in the shorter direction until *priv == target, at most max - min steps */
static int r6api_step_to(struct menu_entry * e, int target, char * why, int n)
{
    if (!e || !e->select || !IS_ML_PTR(e->priv)) return r6api_set(why, n, R6API_NOSET, "not settable from scripts");
    int lo = e->min, hi = e->max, range = hi - lo + 1;
    if (hi < lo || target < lo || target > hi) return r6api_set(why, n, R6API_ARG, "value out of range");
    for (int k = 0; k <= hi - lo && range > 0; k++)
    {
        int cur = *(int *) e->priv;
        if (cur == target) return r6api_set(why, n, R6API_OK, "ok");
        int up = ((target - cur) % range + range) % range;
        e->select(e->priv, up <= range - up ? 1 : -1);
        if (*(int *) e->priv == cur) return r6api_set(why, n, R6API_BUSY, "not changed (refused)");
    }
    return r6api_set(why, n, *(int *) e->priv == target ? R6API_OK : R6API_BUSY, *(int *) e->priv == target ? "ok" : "not reached");
}

/* Astro red Mode (5.1, A4, MB-7): exactly one select in the direction of the target; afterwards the result is checked */
static int r6api_ast_mode(struct menu_entry * e, int arg, char * why, int n)
{
    if (!e || !IS_ML_PTR(e->priv) || !r6api_ast_ok) return r6api_set(why, n, R6API_UNRES, "R6 menu entry not resolved");
    if (arg < 0 || arg > 2) return r6api_set(why, n, R6API_ARG, "mode: Off, ML only or Full (0, 1, 2)");
    int m = COERCE(*(int *) e->priv, 0, 2);
    if (m == arg)
    {
#ifdef CONFIG_R6_ASTRO
        if (arg == 2 && !(r6_astro_pal() & R6API_AS_GREY)) return r6api_set(why, n, R6API_BUSY, "pending (start path)");
#endif
        return r6api_set(why, n, R6API_OK, "ok");
    }
    int delta = (m + 1) % 3 == arg ? 1 : -1;
    if ((m + (delta < 0 ? 2 : 1)) % 3 == 2 && get_ms_clock() < R6API_T2_BOOT_MS) return r6api_set(why, n, R6API_EARLY, "too early (start check)");
    r6api_hull_ref = 0;
    r6api_sel(e, delta);
    if (r6api_hull_ref) return r6api_set(why, n, r6api_hull_ref == 2 ? R6API_EARLY : R6API_BUSY, r6api_hull_ref == 2 ? "too early (start check)" : "not from here");
    int m2 = COERCE(*(int *) e->priv, 0, 2);
    if (m2 == arg) return r6api_set(why, n, R6API_OK, "ok");
    if (m2 == 2) r6api_sel(e, arg == 0 ? 1 : -1);          /* TOCTOU: Full although not wanted: one step to the target */
    else r6api_sel(e, (m2 + 1) % 3 == m ? 1 : -1);         /* Full refused (H5 / no memory): back to the start value, never over Full */
    return r6api_set(why, n, COERCE(*(int *) e->priv, 0, 2) == arg ? R6API_OK : R6API_BUSY, COERCE(*(int *) e->priv, 0, 2) == arg ? "ok" : "mode not reached");
}

/* the refusal ring of r6_interval.c (iv.ref) after a select: a new entry of src -> R6API_CAM + its code. Review R2-5: every entry
 * since n0 (at most the ring), the newest of src first; r6_iv_task may have written its own entry meanwhile */
static int r6api_ref_after(uint32_t n0, int src, int src2, char * why, int n)
{
    struct r6_iv_ref r;
    uint32_t k = r6_iv_refusal_n() - n0;
    if (!k) return r6api_set(why, n, R6API_OK, "ok");
    int c = r6_iv_refusal(0, &r);
    for (int i = c - 1; i >= 0 && i >= c - (int) MIN(k, (uint32_t) c); i--)
    {
        if (!r6_iv_refusal(i, &r) || (r.src != src && r.src != src2)) continue;
#ifdef CONFIG_R6_ETTR
        if (src == R6IV_REF_ETTR) return r6api_set(why, n, R6API_CAM + r.cam, r6_ettr_a_name(r.cam));
#endif
        return r6api_set(why, n, R6API_CAM + r.cam, r6_iv_cam_name(r.cam));
    }
    return r6api_set(why, n, R6API_OK, "ok");
}

/* the commands of 5.1 and the entry actions of 5.2; GuiMainTask only (the caller set the mark and holds the intent mark) */
static int r6api_run(const struct r6api_job * j, char * why, int n)
{
    int cmd = j->cmd, arg = j->arg;
    struct menu_entry * e = j->i >= 0 ? r6api_ents[j->i] : 0;
    if (j->i < 0 && cmd != R6SC_IV_STOP && cmd != R6SC_MD_STOP)
    {
        int i = r6api_tab_find(cmd);
        if (i < 0 || !r6api_ents[i] || r6api_unres[r6api_tab[i].area]) return r6api_set(why, n, R6API_UNRES, "R6 menu entry not resolved");
        e = r6api_ents[i];
    }
    if (j->i >= 0 && (!e || r6api_unres[r6api_tab[j->i].area])) return r6api_set(why, n, R6API_UNRES, "R6 menu entry not resolved");
    uint32_t n0 = r6_iv_refusal_n();
    switch (cmd)
    {
        case R6SC_IV_START:
            /* review R2-5 / R3-3: an open script job would take the menu's cancel branch (no start, no refusal entry, "ok") */
            if (_r6iv_scr_busy()) return r6api_set(why, n, R6API_BUSY, "script job running");
            if (!_r6iv_scr_can_start(0) || r6_iv_md_running()) return r6api_set(why, n, R6API_BUSY, r6_iv_running() ? "busy: Interval runs" : "busy");
            r6api_sel(e, 1);
            return r6api_ref_after(n0, R6IV_REF_START, R6IV_REF_START, why, n);
        case R6SC_IV_STOP:
            return _r6iv_scr_iv_stop() ? r6api_set(why, n, R6API_OK, "ok") : r6api_set(why, n, R6API_OK, "nothing to stop");
        case R6SC_IV_TEST:
            if (!_r6iv_scr_can_start(0)) return r6api_set(why, n, R6API_BUSY, r6_iv_running() ? "busy: Interval runs" : "busy");
            r6api_sel(e, 1);
            return r6api_ref_after(n0, R6IV_REF_TEST, R6IV_REF_TEST, why, n);
        case R6SC_TVW:
            if (arg != 0 && arg != 1) return r6api_set(why, n, R6API_ARG, "Tv writes: 0 or 1");
            /* review R3-8: OFF while a restore is pending would make it fail at row 1 (and "Tv back" needs the switch, D7) */
            if (!arg && _r6iv_tvw_on() && _r6iv_scr_pw_pend()) return r6api_set(why, n, R6API_BUSY, "Tv restore pending");
            if (_r6iv_tvw_on() != arg) r6api_sel(e, 1);
            return _r6iv_tvw_on() == arg ? r6api_set(why, n, R6API_OK, "ok") : r6api_set(why, n, R6API_BUSY, "not changed (refused)");
        case R6SC_TVBACK:
        {
            r6api_sel(e, 1);
            const char * w = _r6iv_scr_back_why();
            return w ? r6api_set(why, n, R6API_BUSY, w) : r6api_set(why, n, R6API_OK, "ok");
        }
#ifdef CONFIG_R6_MOTION
        case R6SC_MD_START:
            if (r6_iv_md_running() || !_r6iv_scr_can_start(1)) return r6api_set(why, n, R6API_BUSY, "busy");
            r6api_sel(e, 1);
            return r6_iv_md_refused() ? r6api_set(why, n, R6API_CAM + r6_iv_md_refused(), r6_iv_cam_name(r6_iv_md_refused())) : r6api_set(why, n, R6API_OK, "ok");
        case R6SC_MD_STOP:
            r6_iv_md_stop();                                    /* stops only a motion run, never starts (MB-4) */
            return r6api_set(why, n, R6API_OK, "ok");
#endif
#ifdef CONFIG_R6_ETTR
        case R6SC_ETTR_APPLY:
            r6api_sel(e, 1);
            return r6api_ref_after(n0, R6IV_REF_ETTR, R6IV_REF_ETTR, why, n);
#endif
        case R6SC_AST_MODE:
            return r6api_ast_mode(e, arg, why, n);
        case R6SC_AST_TEST:
            if (get_ms_clock() < R6API_T2_BOOT_MS) return r6api_set(why, n, R6API_EARLY, "too early (start check)");
            r6api_hull_ref = 0;
            r6api_sel(e, 1);
            return r6api_hull_ref ? r6api_set(why, n, r6api_hull_ref == 2 ? R6API_EARLY : R6API_BUSY, r6api_hull_ref == 2 ? "too early (start check)" : "not from here")
                : r6api_set(why, n, R6API_OK, "ok");
        case R6SC_SELECT:                                       /* an entry of the table: its kind decides */
        {
            if (j->i < 0) return r6api_set(why, n, R6API_ARG, "no entry");
            int k = r6api_tab[j->i].kind;
            if (k == RT_ACT) { r6api_sel(e, 1); return r6api_set(why, n, R6API_OK, "ok"); }
            if (k == RT_SETSEL || k == RT_SWITCH) return r6api_step_to(e, j->target, why, n);
            if (k == RT_SET)
            {
                if (e->select || !IS_ML_PTR(e->priv)) return r6api_set(why, n, R6API_NOSET, "not settable from scripts");
                if (e->max > e->min && (j->target < e->min || j->target > e->max)) return r6api_set(why, n, R6API_ARG, "value out of range");
                *(int *) e->priv = j->target;                   /* as upstream menu_set_value_from_script (range checked) */
                return r6api_set(why, n, R6API_OK, "ok");
            }
            return r6api_set(why, n, R6API_NOSET, "not settable from scripts");
        }
    }
    return r6api_set(why, n, R6API_ARG, "unknown command");
}

/* the stops are no toggles and run also while a picture / Tv window / ETTR run is on (2.6) */
static int r6api_is_stop(const struct r6api_job * j) { return j->cmd == R6SC_IV_STOP || j->cmd == R6SC_MD_STOP; }

/* GuiMainTask: one job with the origin mark R6GO_SCRIPT, the decisive busy check under the intent mark right before it (MB-1) */
static int r6api_exec(const struct r6api_job * j, char * why, int n)
{
    int res;
    if (r6api_down()) return r6api_set(why, n, R6API_BUSY, "shutting down");
    int stop = r6api_is_stop(j);
    if (!stop && _r6iv_scr_intent_begin()) return r6api_set(why, n, R6API_BUSY, "busy: ML picture / Tv write / ETTR running");
    int org = _r6_gui_org_set(R6GO_SCRIPT);
    res = r6api_run(j, why, n);
    _r6_gui_org_set(org);
    if (!stop) _r6iv_scr_intent_end(0);
    r6api_org_add(R6GO_SCRIPT, j->cmd, j->i >= 0 ? j->i : j->arg, res);
    return res;
}

/* ---- the GUI slot (2.5, MB-9): a Lua task posts, GuiMainTask runs it on MLEV_R6_SCRIPT (obj = seq) ---- */
#define GJ_W(q, s) ((uint32_t) (q) << 8 | (uint32_t) (s))
static struct {
    volatile uint32_t w;
    uint32_t gen, tid;
    struct r6api_job j;
    int res;
    char why[48];
} r6api_gj;
static volatile uint32_t r6api_gj_seqn = 0;

static uint32_t r6api_tid_gm = 0, r6api_tid_clk = 0;   /* LVP tsk.hw (MK8) */
void _r6api_tids(uint32_t t[3]) { t[0] = r6api_tid_gm; t[1] = r6api_tid_clk; t[2] = _r6iv_scr_task_id(); }

int _r6api_gui_event(struct event * event)
{
    if (!r6api_tid_gm) r6api_tid_gm = get_current_task_id();
    if (!event || event->param != MLEV_R6_SCRIPT) return 0;
    uint32_t q = (uint32_t) event->obj, w = r6api_gj.w;
    if (!q || w != GJ_W(q, R6IV_SS_POSTED) || !r6_cas32(&r6api_gj.w, w, GJ_W(q, R6IV_SS_TAKEN))) return 1;   /* stale: nothing */
    char t[48];
    int res = r6api_exec(&r6api_gj.j, t, sizeof(t));
    r6api_gj.res = res;
    memcpy(r6api_gj.why, t, sizeof(t));
    r6_barrier();
    if (!r6_cas32(&r6api_gj.w, GJ_W(q, R6IV_SS_TAKEN), GJ_W(q, R6IV_SS_DONE)))
        r6_cas32(&r6api_gj.w, GJ_W(q, R6IV_SS_ABANDONED), GJ_W(q, R6IV_SS_FREE));   /* the poster gave up (backstop) */
    return 1;
}

/* a Lua task: wait (<= 3 s) for "not busy" unless a stop, post, the ML event, wait for RUNNING (3 s), then DONE (backstop 3 s) */
static int r6api_gui_job(uint32_t gen, const struct r6api_job * j, char * why, int n)
{
    int t0 = get_ms_clock();
    if (!r6api_is_stop(j))
        while (_r6iv_gui_busy())
        {
            if (get_ms_clock() - t0 >= R6API_GUI_MS) return r6api_set(why, n, R6API_BUSY, "busy: ML picture / Tv write / ETTR running");
            msleep(R6API_POLL_MS);
        }
    uint32_t o, q, w = r6api_gj.w;
    if ((w & 0xFF) != R6IV_SS_FREE) return r6api_set(why, n, R6API_BUSY, "another GUI job is open");
    do { o = r6api_gj_seqn; q = (o + 1) & 0xFFFFFF; if (!q) q = 1; } while (!r6_cas32(&r6api_gj_seqn, o, q));
    if (!r6_cas32(&r6api_gj.w, w, GJ_W(q, R6IV_SS_FILL))) return r6api_set(why, n, R6API_BUSY, "another GUI job is open");
    r6api_gj.gen = gen;
    r6api_gj.tid = get_current_task_id();
    r6api_gj.j = *j;
    r6api_gj.res = 0;
    r6api_gj.why[0] = 0;
    r6_barrier();
    t0 = get_ms_clock();                                     /* review R2-6 / R3-7: the 3 s until RUNNING count from t_req (2.5) */
    r6api_gj.w = GJ_W(q, R6IV_SS_POSTED);
    r6_barrier();
    r6api_inc(&r6api_cnt[6]);
    GUI_Control(MLEV_R6_SCRIPT, (int) q, FAKE_BTN, 0);       /* after POSTED (SL-3); an ML event never reaches Canon */
    int t_run = 0;
    for (;;)
    {
        msleep(R6API_POLL_MS);
        w = r6api_gj.w;
        int now = get_ms_clock();
        if (w == GJ_W(q, R6IV_SS_DONE))
        {
            r6_barrier();
            int res = r6api_gj.res;
            r6api_why(why, n, "%s", r6api_gj.why);
            r6_barrier();
            r6_cas32(&r6api_gj.w, w, GJ_W(q, R6IV_SS_FREE));
            return res;
        }
        if (w == GJ_W(q, R6IV_SS_POSTED) && now - t0 >= R6API_GUI_MS && r6_cas32(&r6api_gj.w, w, GJ_W(q, R6IV_SS_FREE)))
            return r6api_set(why, n, R6API_TIMEOUT, "timeout: GuiMainTask did not run it (nothing done)");
        if (w == GJ_W(q, R6IV_SS_TAKEN))
        {
            if (!t_run) t_run = now | 1;
            if (now - (t_run & ~1) >= R6API_GUI_MS && r6_cas32(&r6api_gj.w, w, GJ_W(q, R6IV_SS_ABANDONED)))
            {
                r6api_inc(&r6api_cnt[5]);
                return r6api_set(why, n, R6API_LATE, "late: still running in GuiMainTask");
            }
        }
        /* review R2-1 / R3-1: r6api_gj_end (the script's real end) keeps the seq: POSTED -> FREE, TAKEN -> ABANDONED (-> FREE), DONE -> FREE;
         * this waiter's own FREE / ABANDONED returned above, so these are a cancel from outside */
        if ((w >> 8) != q || (w & 0xFF) == R6IV_SS_FREE || (w & 0xFF) == R6IV_SS_ABANDONED) return r6api_set(why, n, R6API_CANCELLED, "cancelled");
    }
}

/* the way of every R6 command: GuiMainTask directly (stack >= 4 KB), a Lua task through the GUI slot, other GUI tasks refuse */
static int r6api_submit(uint32_t gen, const struct r6api_job * j, char * why, int n)
{
    if (r6api_down()) return r6api_set(why, n, R6API_BUSY, "shutting down");
    if (!r6api_resolved) return r6api_set(why, n, R6API_UNRES, "R6 menu entry not resolved");
    int cls = r6api_class();
    if (cls == R6TC_REFUSE) return r6api_set(why, n, R6API_TASK, "wrong task");
    if (r6api_guimain())
    {
        int left = r6_stack_left();
        if (left >= 0 && left < R6API_STACK_MIN) return r6api_set(why, n, R6API_STACK, "stack: use task.create");   /* SB-11 */
        if (left < 0) return r6api_set(why, n, R6API_STACK, "C stack unknown: use task.create");
        if (!r6api_is_stop(j) && _r6iv_gui_busy()) return r6api_set(why, n, R6API_BUSY, "busy: ML picture / Tv write / ETTR running");
        return r6api_exec(j, why, n);
    }
    if (cls == R6TC_ASYNC) return r6api_set(why, n, R6API_GUITASK, "not from menu_task (use task.create)");
    return r6api_gui_job(gen, j, why, n);
}

int r6_script_cmd(uint32_t gen, int cmd, int arg, char * why, int n)
{
    if (cmd <= R6SC_NONE || cmd >= R6SC_N || cmd == R6SC_SELECT) return r6api_set(why, n, R6API_ARG, "unknown command");
    struct r6api_job j = { cmd, arg, -1, 0 };
    return r6api_submit(gen, &j, why, n);
}

/* ======== 5.2: menu.set (MB-5): R6API_PASS = not an R6 entry, the upstream path ======== */
static int r6api_parse(const struct menu_entry * e, const char * str, int ival, int * v)
{
    if (!str) { *v = ival == INT_MIN ? 1 : ival; return 1; }          /* no value: "ON" */
    if (streq(str, "ON")) { *v = 1; return 1; }
    if (streq(str, "OFF")) { *v = 0; return 1; }
    if (e && e->choices && e->max >= e->min && e->max - e->min < 256)
        for (int i = e->min; i <= e->max; i++)
            if (e->choices[i - e->min] && streq(str, e->choices[i - e->min])) { *v = i; return 1; }
    char * end = 0;
    long x = strtol(str, &end, 10);
    if (end && end != str && *end == 0) { *v = (int) x; return 1; }
    return 0;
}

int r6_script_menu_set(uint32_t gen, const char * menu, const char * entry, const char * str, int ival, char * why, int n)
{
    struct menu_entry * e = _r6_menu_entry(menu, entry);
    if (!e) return r6api_is_r6_menu(menu) ? r6api_set(why, n, R6API_UNRES, "R6 menu entry not resolved") : R6API_PASS;
    int a = r6api_area_of(e);
    if (a < 0) return R6API_PASS;
    if (!r6api_resolved || r6api_unres[a]) return r6api_set(why, n, R6API_UNRES, "R6 menu entry not resolved");
    int i = -1;
    for (int k = 0; k < R6API_NTAB; k++) if (r6api_ents[k] == e) { i = k; break; }
    if (i < 0) return r6api_set(why, n, R6API_UNRES, "R6 menu entry not resolved");   /* in the area, not in the table */
    int kind = r6api_tab[i].kind, v = 0;
    if (kind == RT_SUB) return r6api_set(why, n, R6API_OK, "submenu: nothing");
    if (kind == RT_NO) return r6api_set(why, n, R6API_NOSET, "not settable from scripts");
    if (kind == RT_ASTMODE && str && (streq(str, "Off") || streq(str, "ML only") || streq(str, "Full")))
        v = streq(str, "Off") ? 0 : streq(str, "ML only") ? 1 : 2;
    else if (!r6api_parse(e, str, ival, &v)) return r6api_set(why, n, R6API_ARG, "unknown value");
    struct r6api_job j = { R6SC_SELECT, 0, i, v };
    if (kind == RT_START) { if (v != 0 && v != 1) return r6api_set(why, n, R6API_ARG, "ON or OFF"); j.cmd = v ? r6api_tab[i].cmd : (r6api_tab[i].cmd == R6SC_IV_START ? R6SC_IV_STOP : R6SC_MD_STOP); j.i = -1; }
    else if (kind == RT_ACT) { if (!v) return r6api_set(why, n, R6API_OK, "OFF: nothing"); j.cmd = r6api_tab[i].cmd; j.i = r6api_tab[i].cmd == R6SC_SELECT ? i : -1; }
    else if (kind == RT_TVW) { j.cmd = R6SC_TVW; j.arg = v; j.i = -1; }
    else if (kind == RT_ASTMODE) { j.cmd = R6SC_AST_MODE; j.arg = v; j.i = -1; }
    return r6api_submit(gen, &j, why, n);
}

/* ======== 6: E8 in the caller's task ======== */
/* the release code of a press (src/../gui.h: buttons press = even, release = + 1; the multi-controller -> 0x76); -1 = none (dials) */
static int r6api_release(int c)
{
    if (c == BGMT_PRESS_SET || c == BGMT_MENU || c == BGMT_INFO || c == BGMT_PLAY || c == BGMT_TRASH || c == BGMT_PRESS_ZOOM_IN
        || c == BGMT_Q || c == BGMT_RATE) return c + 1;
    if (c >= BGMT_PRESS_UP && c <= BGMT_JOY_CENTER) return BGMT_UNPRESS_UDLR;
    return -1;
}

static int r6api_wait_intent(int limit, char * why, int n)
{
    int t0 = get_ms_clock();
    while (_r6iv_scr_intent_begin())                          /* the mark is down again while waiting (V2a-10) */
    {
        if (r6api_down()) return r6api_set(why, n, R6API_BUSY, "shutting down");
        if (get_ms_clock() - t0 >= limit) return r6api_set(why, n, R6API_BUSY, "busy: ML picture / Tv write / ETTR running");
        msleep(R6API_POLL_MS);
    }
    return 0;
}

/* SetGUIRequestMode under the intent mark (only this call between + 1 and - 1), then the settle time (SL-6) */
static int r6api_gui_mode(int mode, int limit, char * why, int n)
{
    int r = r6api_wait_intent(limit, why, n);
    if (r) return r;
    SetGUIRequestMode(mode);
    _r6iv_scr_intent_end(R6API_GUI_SETTLE_MS);
    return 0;
}

static int r6api_wait_cond(int (*f)(void), int want, int limit)
{
    int t0 = get_ms_clock();
    while (!!f() != !!want)
    {
        if (get_ms_clock() - t0 >= limit) return 0;
        msleep(50);
    }
    return 1;
}

static void r6api_pair(int press)
{
    fake_simple_button(press);
    int rel = r6api_release(press);
    if (rel >= 0) fake_simple_button(rel);
}

/* 6.3: both indicators of the mode dial at Video (review R3-5: AND; r6_iv_cam_check's OR is the refusing direction) */
static int r6api_movie_ok(void) { return is_movie_mode() && lv_movie_select == LVMS_ENABLE_MOVIE; }

int r6_script_gui(uint32_t gen, int op, int arg, int timeout_ms, char * why, int n)
{
    int cls = r6api_class();
    if (cls == R6TC_REFUSE) return r6api_set(why, n, R6API_TASK, "wrong task");
    if (r6api_down()) return r6api_set(why, n, R6API_BUSY, "shutting down");     /* SL-10: no SetGUIRequestMode, no GUI_Control */
    if (op != R6SG_KEY && cls == R6TC_ASYNC) return r6api_set(why, n, R6API_GUITASK, "not from a GUI task (use task.create)");   /* A3 */
    int lim = timeout_ms > 0 ? timeout_ms : R6API_E8_MS, r = 0;
    switch (op)
    {
        case R6SG_MODE:
            r = r6api_gui_mode(arg, lim, why, n);
            if (r) return r;
            msleep(500);                                       /* as upstream's camera.gui.mode */
            return r6api_set(why, n, get_gui_mode() == arg ? R6API_OK : R6API_TIMEOUT, get_gui_mode() == arg ? "ok" : "not reached");
        case R6SG_PLAY:
        case R6SG_MENU:
        {
            int on = arg != 0, mode = on ? (op == R6SG_PLAY ? 0x2002 : 0x2003) : 0;
            r = r6api_gui_mode(mode, lim, why, n);
            if (r) return r;
            int ok = r6api_wait_cond(op == R6SG_PLAY ? is_play_mode : is_menu_mode, on, 2000);
            return r6api_set(why, n, ok ? R6API_OK : R6API_TIMEOUT, ok ? "ok" : "not reached");
        }
        case R6SG_LV_START:
        {
            int t0 = get_ms_clock();
            r = r6api_gui_mode(0, lim, why, n);
            if (r) return r;
            int ok = r6api_wait_cond(r6_lv_running, 1, MAX(R6API_E8_MS - (get_ms_clock() - t0), 1));
            if (ok) ok = wait_lv_frames(3);
            return r6api_set(why, n, ok ? R6API_OK : R6API_TIMEOUT, ok ? "ok" : "no live view after 5 s");
        }
        case R6SG_LV_STOP:
        {
            r = r6api_gui_mode(0x2002, lim, why, n);
            if (r) return r;
            int ok = r6api_wait_cond(r6_lv_running, 0, R6API_E8_MS);
            return r6api_set(why, n, ok ? R6API_OK : R6API_TIMEOUT, ok ? "ok" : "live view still running after 5 s");
        }
        case R6SG_ZOOM:
        {
            if (arg != 1 && arg != 5 && arg != 10) return r6api_set(why, n, R6API_ARG, "zoom: 1, 5 or 10");
            int t0 = get_ms_clock();
            for (int k = 0; k < 3 && lv_dispsize != arg; k++)
            {
                /* review R2-4 / R3-4: an E8 switch (2.6): the intent mark around each pair, then the settle time; the wait outside it */
                r = r6api_wait_intent(MAX(lim - (get_ms_clock() - t0), 1), why, n);
                if (r) return r;
                r6api_pair(BGMT_PRESS_ZOOM_IN);
                _r6iv_scr_intent_end(R6API_GUI_SETTLE_MS);
                msleep(300);                                 /* GuiMainTask works the pair off within 300 ms */
            }
            if (why && n > 0) snprintf(why, n, "zoom %d", lv_dispsize);
            return lv_dispsize == arg ? R6API_OK : R6API_TIMEOUT;
        }
        case R6SG_REC:
            if (!r6api_movie_ok()) return r6api_set(why, n, R6API_ARG, "movie: set the mode dial to Video");
            if (gui_menu_shown()) return r6api_set(why, n, R6API_ARG, "movie: close the ML menu");
            r = r6api_wait_intent(lim, why, n);
            if (r) return r;
            fake_simple_button(0x2D);                          /* movie REC press / release (gui.h); no wait, state unknown (A2) */
            fake_simple_button(0x2E);
            _r6iv_scr_intent_end(R6API_GUI_SETTLE_MS);
            return r6api_set(why, n, R6API_OK, "sent, state unknown");
        case R6SG_KEY:
        {
            if (!r6api_ast_ok) return r6api_set(why, n, R6API_UNRES, "R6 menu entry not resolved");   /* 5.3: no FAKE SET past the hulls */
            int c = module_translate_key(arg, MODULE_KEY_CANON);
            if (arg == MODULE_KEY_PRESS_HALFSHUTTER || arg == MODULE_KEY_UNPRESS_HALFSHUTTER || arg == MODULE_KEY_UNPRESS_FULLSHUTTER
                || arg == MODULE_KEY_UNPRESS_SET || arg == MODULE_KEY_UNPRESS_UDLR)
                return r6api_set(why, n, R6API_ARG, "not analysed yet (M16.2)");
            if (arg == MODULE_KEY_PRESS_FULLSHUTTER) return r6api_set(why, n, R6API_ARG, "full press: camera.shoot");
            if (arg == MODULE_KEY_REC) return r6_script_gui(gen, R6SG_REC, 0, timeout_ms, why, n);
            if (c == 0 && arg != MODULE_KEY_WHEEL_UP) return r6api_set(why, n, R6API_ARG, "unknown key");   /* A14 */
            if (c < 0) return r6api_set(why, n, R6API_ARG, "unknown key");
            r6api_pair(c);                                     /* R6GO_FAKE in GuiMainTask; dials without a release */
            return r6api_set(why, n, R6API_OK, "ok");
        }
    }
    return r6api_set(why, n, R6API_ARG, "unknown GUI op");
}

/* ======== 2.5 / 8.1: the ends of a script (SL-2) ======== */
static void r6api_gj_end(uint32_t gen, uint32_t tid, int all)
{
    uint32_t w = r6api_gj.w;
    int s = w & 0xFF;
    if (!gen || s == R6IV_SS_FREE || s == R6IV_SS_FILL) return;
    r6_barrier();
    if (r6api_gj.gen != gen || (!all && r6api_gj.tid != tid)) return;
    if (s == R6IV_SS_POSTED) r6_cas32(&r6api_gj.w, w, (w & ~0xFFu) | R6IV_SS_FREE);
    else if (s == R6IV_SS_TAKEN) r6_cas32(&r6api_gj.w, w, (w & ~0xFFu) | R6IV_SS_ABANDONED);
    else if (s == R6IV_SS_DONE) r6_cas32(&r6api_gj.w, w, (w & ~0xFFu) | R6IV_SS_FREE);
}

void r6_script_task_end(uint32_t gen)
{
    uint32_t tid = get_current_task_id();
    _r6iv_scr_end_owner(gen, tid, 0);
    r6api_gj_end(gen, tid, 0);
}

void r6_script_end(uint32_t gen, int sent_rec, int set_tvw)
{
    _r6iv_scr_end_owner(gen, 0, 1);
    r6api_gj_end(gen, 0, 1);
    r6api_tvw_last = _r6iv_tvw_on();
    if (sent_rec) NotifyBox(5000, "Lua: movie may still be recording - stop it with REC");   /* R-4: no REC from ML (A2) */
    else if (set_tvw && _r6iv_tvw_on()) NotifyBox(3000, "Lua: Tv writes still ON (set by the script)");   /* K4 (A17) */
}

/* ======== 7.1: events from seconds_clock_task (at most one call per second and kind) ======== */
static int r6api_iv_n = 0;
static uint32_t r6api_pics = 0;
void _r6api_second(void)
{
    if (!r6api_tid_clk) r6api_tid_clk = get_current_task_id();
    int c = get_interval_count();
    uint32_t p = _r6iv_scr_shots();
    if (c > r6api_iv_n) module_exec_cbr(CBR_INTERVALOMETER);
    if (c > r6api_iv_n || p != r6api_pics) module_exec_cbr(CBR_POST_SHOOT);
    r6api_iv_n = c;
    r6api_pics = p;
}

#endif /* CONFIG_R6_SCRIPT */
