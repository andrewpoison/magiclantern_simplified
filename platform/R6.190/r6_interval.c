/*
 * R6.190 M8.0: interval release, ML menu Shoot > "R6 Interval".
 *
 * Approved by the user on 2026-10-02: ML may take pictures with
 * call("Release") (option B in docs/m7_plan.md), nothing else new: no
 * property write (CONFIG_PROP_REQUEST_CHANGE stays off, also for auto power
 * off and AF), no other new Canon call, no MMIO, no file. Analysis (ROM,
 * checked by skeptics): docs/m8_plan.md. Not upstream's intervalometer
 * (FEATURE_INTERVALOMETER): that one also calls SetGUIRequestMode(0), blinks
 * the card LED (MMIO), autostarts at boot from the saved setting, writes
 * script files and ignores the return value of the release.
 *
 * "Release" on R6 1.9.0: handler 0xE0292AB6 in Canon's factory (FA) eventproc
 * table (entry 0xE0CD2414, registered by FA_Initialize 0xE0109DF0 at every
 * boot). Up to 3 attempts of FaSyRelease 0xE01FB152, 200 ms apart: SW1 on,
 * SW2 on (data 10), SW2 off, SW1 off through RemoteSwChange 0xE06D32E8 ->
 * CameraConductor handlers 0xE029661A / 0xE029665C (they run in the caller's
 * task), each step waits for Canon's reply (SW1 5 s, SW2 1 s), then up to
 * 10 s for the finished job (FACTORY_PostShootJob 0xE01FAD1C gives the
 * semaphore). Returns 0 only after that, 1 after three failed attempts, -1
 * if the name is not registered. Nothing waits unbounded (worst case ~67 s).
 * The shot enters Canon's normal release entry (ActRelStart 0xE031DB94) with
 * ShutterMode 10, which marks the job as a factory job (CreateJob flag 0x8).
 * No property / flash / file write in the chain itself (heuristic closure);
 * "FA_Release" (writes FA_ADJUST_FLAG) is never called.
 *
 * Hazards and what this file does about them:
 *  - a late Canon reply (after a reply timeout) writes into a stale stack
 *    frame of the calling task and gives a deleted semaphore = Canon assert =
 *    hang: only MF (the SW1 reply waits for AF), only M with Tv <= 1 s (Tv
 *    mode: safety shift may lengthen it; Tv = PROP_SHUTTER as delivered, own
 *    handler, lens.c drops it in Av / P), single shot; one
 *    long-lived task (r6_iv_task, never ends) makes every call, never two at
 *    once, never from the GUI, a property handler or with an ML lock held
 *  - exposures near the 10 s job wait cause retries (extra pictures): Tv <=
 *    1 s; a call that takes R6IV_SLOW_MS or more stops the series
 *  - the factory mark may leave Canon's job open (not confirmed): after every
 *    picture ML waits until Canon's job count [0x9DC0] is 0 again, else the
 *    series stops; at most R6IV_MAX_PICS pictures per series
 *  - factory side paths: no picture while Canon's factory log writes a file
 *    (mode [0x9B40] / handle [0x9B54]), before FA_Initialize set up what the
 *    finished job uses ([0xE708], [0x9B3C], FA-Neo list [0x180D0] and
 *    semaphore [0x180BC]: NULL there = write at 4 / give NULL = hang), while a
 *    Release is in flight ([0x9C40]) or before Canon registered its remote
 *    switch handlers ([0x108DC] / [0x108E0])
 *  - Canon's screen saver / eco mode (GUI mode 0xE08E / 0xE08F) end a series:
 *    ML cannot wake the camera without a new Canon input
 *  - no ML retry: any return value != 0, no new picture or more than one new
 *    picture (PROP_RELEASE_COUNTER / file number) stops the series
 * Canon's auto power off and screen off cannot be held off without a write:
 * the user sets them to Disable / the longest time; an ML shutdown stops the
 * series. Stop conditions are checked every 50 ms, the waits only at a slot. Status in the
 * ML menu only; details in Debug > LV probe > Log snapshot (refused while a
 * series runs). Nothing is saved except interval, count and start delay; a
 * series never starts by itself. RAM reads only, besides call("Release").
 */

#include "dryos.h"
#include "bmp.h"
#include "menu.h"
#include "config.h"
#include "property.h"
#include "propvalues.h"
#include "lens.h"
#include "fio-ml.h"
#include "tasks.h"
#include "r6_interval.h"

#ifdef CONFIG_R6_INTERVAL

#ifdef FEATURE_INTERVALOMETER
#error "R6 M8.0: upstream's intervalometer is not approved (SetGUIRequestMode(0), LED MMIO, boot autostart, script files)"
#endif
#ifdef CONFIG_PROP_REQUEST_CHANGE
#error "R6 M8.0: property writes stay off"
#endif

extern int lv_action_raw_get(uint32_t * value, int * len);   /* propvalues.c */

#define R6IV_EARLY_MS      20000   /* no picture in the first 20 s after boot */
#define R6IV_LV_MS          2000   /* Canon LV must run for this long */
#define R6IV_MENU_HOLD_MS   3000   /* no picture until this long after the ML menu closed */
#define R6IV_GUI_GRACE_MS   3000   /* Canon's GUI mode follows the ML menu (0x2003) with a delay */
#define R6IV_LATE_MS        2000   /* a picture this late after its slot is skipped (no catch-up) */
#define R6IV_WAIT_MAX_MS   60000   /* camera not ready this long (ML menu excluded): stop */
#define R6IV_SLOW_MS        9000   /* the release's own job wait is 10 s */
#define R6IV_PIC_MS        15000   /* new picture expected within this time after the call */
#define R6IV_JOB_MS        30000   /* Canon's job count must be 0 again within this time */
#define R6IV_SETTLE_MS       300   /* after the job: late counter updates (extra pictures) */
#define R6IV_MAX_PICS         50   /* per series, until a long test showed no leak */
#define R6IV_RAW_TV_MIN       56   /* 1 s (raw2shutter_ms: 2^((56 - raw) / 8) s) */
#define R6IV_RAW_TV_MAX      168   /* 1/16000 s */
#define R6IV_GUI_REVIEW   0x4033   /* Canon's quick review (SetGUIMode 0xE0BD35E4) */
#define R6IV_GUI_SR       0x4034   /* LvReview's SR mode (GUI_RequestSR from action_None_NotifySR 0xE0B9BD1A) */
#define R6IV_GUI_SAVER    0xE08E   /* START_SCREEN_SAVER (GuiUtilTftPowerSaveController 0xE0BA6F74 / 0xE0BA6ECA) */
#define R6IV_GUI_ECO      0xE08F   /* START_ECO_MODE (0xE0BA6CFA) */

static const int iv_intervals[] = { 3, 5, 10, 15, 20, 30, 45, 60, 90, 120, 180, 300, 600, 900, 1800, 3600 };
static const char * iv_interval_names[] = { "3s", "5s", "10s", "15s", "20s", "30s", "45s", "1m", "1m30s",
                                            "2m", "3m", "5m", "10m", "15m", "30m", "1h" };
static const int iv_delays[] = { 3, 5, 10, 30, 60 };
static const char * iv_delay_names[] = { "3s", "5s", "10s", "30s", "1m" };

/* saved: only the settings; a running series is never saved and never resumed */
CONFIG_INT("r6.iv.interval", r6_iv_interval_idx, 2);    /* 10 s */
CONFIG_INT("r6.iv.count",    r6_iv_count, 10);
CONFIG_INT("r6.iv.delay",    r6_iv_delay_idx, 0);       /* 3 s */

static struct r6_iv_stats st;
static volatile int iv_cmd = 0;          /* menu -> task: 1 = start, 2 = stop (last press wins) */
static int iv_refused_cam = R6IV_CAM_OK; /* why the last Start was refused (menu) */
static volatile int iv_t_menu = -100000; /* last time the task saw the ML menu open */
static int iv_t_lv = -1;                 /* Canon LV running since (ms), -1 = not running */
static int iv_gui_seen = 0;              /* foreign Canon GUI mode seen during the series (latched), 0 = none */
static int iv_due = 0;                   /* ms of the next picture slot */
static uint32_t iv_wait_ms = 0;          /* camera not ready (ML menu excluded) since the last picture */
static uint32_t iv_wait_mask = 0;
static volatile uint32_t iv_tv_prop = 0; /* PROP_SHUTTER as delivered (lens.c drops it while ML still sees Av / P) */
static volatile int iv_tv_seen = 0;

/* read-only copy of an already registered property (lens.c has the other handler) */
PROP_HANDLER(PROP_SHUTTER)
{
    iv_tv_prop = buf[0];
    iv_tv_seen = 1;
}

static const char * const iv_stop_names[R6IV_NSTOPS] = {
    "-", "done", "stopped (menu)", "ML shutdown", "camera setting", "Canon state", "Canon GUI",
    "display off (power saving)", "camera not ready", "Release returned", "Release too slow", "no new picture", "extra picture",
    "Canon job open",
};
static const char * const iv_cam_names[R6IV_NCAMS] = {
    "ok", "movie mode", "not M", "Tv > 1s or unknown", "not single shot", "not MF", "no card / full",
};
static const char * const iv_canon_names[R6IV_NCANON] = {
    "ready", "remote switch not ready", "factory part not ready", "Release in flight", "factory log to file",
    "boot < 20 s", "LV not running", "review / GUI state", "Canon job running", "buffer full",
    "UI locked", "sensor cleaning", "ML menu",
};

const struct r6_iv_stats * r6_iv_stats(void) { return &st; }
int r6_iv_running(void) { return st.running || iv_cmd == 1; }
const char * r6_iv_stop_name(int s) { return (s >= 0 && s < R6IV_NSTOPS) ? iv_stop_names[s] : "?"; }
const char * r6_iv_cam_name(int c) { return (c >= 0 && c < R6IV_NCAMS) ? iv_cam_names[c] : "?"; }
const char * r6_iv_canon_name(int c) { return (c >= 0 && c < R6IV_NCANON) ? iv_canon_names[c] : "?"; }
int r6_iv_tv_prop(int * seen) { if (seen) *seen = iv_tv_seen; return iv_tv_prop; }

static int iv_interval_s(void) { return iv_intervals[COERCE(r6_iv_interval_idx, 0, COUNT(iv_intervals) - 1)]; }
static int iv_delay_s(void) { return iv_delays[COERCE(r6_iv_delay_idx, 0, COUNT(iv_delays) - 1)]; }
static int iv_count(void) { return COERCE(r6_iv_count, 1, R6IV_MAX_PICS); }

static int iv_file_number(void)
{
    struct card_info * c = get_shooting_card();
    return c ? c->file_number : 0;
}

/* the only Canon call of this file (QEMU tests replace it in the test worktree) */
static int iv_release(void)
{
    return call("Release");
}

int r6_iv_cam_check(void)
{
    if (is_movie_mode() || lv_movie_select == LVMS_ENABLE_MOVIE) return R6IV_CAM_MOVIE;
    if (shooting_mode != SHOOTMODE_M) return R6IV_CAM_MODE;
    int tv = iv_tv_prop;
    if (!iv_tv_seen || tv < R6IV_RAW_TV_MIN || tv > R6IV_RAW_TV_MAX) return R6IV_CAM_TV;
    if (drive_mode != DRIVE_SINGLE) return R6IV_CAM_DRIVE;
    if ((af_mode & 0xF) != AF_MODE_MANUAL_FOCUS) return R6IV_CAM_FOCUS;
    if (avail_shot <= 0) return R6IV_CAM_CARD;
    return R6IV_CAM_OK;
}

static int iv_lv_ok(int now)
{
    return iv_t_lv >= 0 && now - iv_t_lv >= R6IV_LV_MS;
}

/* task = 0: for the menu display, which is only seen with the ML menu open:
 * skip the ML menu and what it causes (Canon MENU mode: LV stopped, gui_state) */
static int iv_canon_check(int * hard, int task)
{
    int now = get_ms_clock();
    *hard = 0;
    if (now < R6IV_EARLY_MS) return R6IV_CANON_EARLY;
    *hard = 1;
    if (MEM(R6IV_RAM_CC_SW1) != R6IV_CC_SW1_FN || MEM(R6IV_RAM_CC_SW2) != R6IV_CC_SW2_FN || !MEM(R6IV_RAM_CC_OBJ))
        return R6IV_CANON_CC;
    if (!MEM(R6IV_RAM_FA_STORE) || !MEM(R6IV_RAM_FA_LOG) || !MEM(R6IV_RAM_FANEO) || !MEM(R6IV_RAM_FANEO_SEM))
        return R6IV_CANON_FAINIT;
    if (MEM(R6IV_RAM_FASY_SEM)) return R6IV_CANON_FASEM;
    if (MEM(R6IV_RAM_FA_LOGMODE) || MEM(R6IV_RAM_FA_LOGFILE)) return R6IV_CANON_FALOG;
    *hard = 0;
    if (task)
    {
        if (gui_menu_shown() || now - iv_t_menu < R6IV_MENU_HOLD_MS) return R6IV_CANON_MENU;
        if (!iv_lv_ok(now)) return R6IV_CANON_LV;
        int m = CURRENT_GUI_MODE;
        if (gui_state != GUISTATE_IDLE || m == R6IV_GUI_REVIEW || m == R6IV_GUI_SR) return R6IV_CANON_REVIEW;
    }
    if (MEM(R6IV_RAM_JOBCNT)) return R6IV_CANON_JOBS;
    if (burst_count == 0) return R6IV_CANON_BUFFER;
    if (icu_uilock & 0xFF) return R6IV_CANON_UILOCK;
    if (sensor_cleaning) return R6IV_CANON_CLEANING;
    return R6IV_CANON_OK;
}

int r6_iv_canon_check(int * hard)
{
    return iv_canon_check(hard, 0);
}

static void iv_end(int stop, int detail, int32_t val)
{
    st.stop = stop;
    st.stop_cam = stop == R6IV_STOP_CAMERA ? detail : 0;
    st.stop_canon = (stop == R6IV_STOP_CANON || stop == R6IV_STOP_WAIT) ? detail : 0;
    st.stop_val = val;
    st.t_stop = get_ms_clock();
    st.running = 0;
}

static void iv_begin(int now)
{
    int c = r6_iv_cam_check();
    if (c != R6IV_CAM_OK)
    {
        iv_refused_cam = c;
        st.refused++;
        return;
    }
    iv_refused_cam = R6IV_CAM_OK;
    st.interval_s = iv_interval_s();
    st.n_target = iv_count();
    st.n_done = 0;
    st.skipped = 0;
    st.stop = R6IV_STOP_NONE;
    st.stop_cam = st.stop_canon = 0;
    st.stop_val = 0;
    st.t_start = now;
    iv_due = now + iv_delay_s() * 1000;
    iv_wait_ms = 0;
    iv_wait_mask = 0;
    iv_gui_seen = 0;
    st.running = 1;
}

/* what the task watches: every tick, and while iv_shoot waits (otherwise an
 * ML menu, an LV restart or a Canon MENU visit inside a picture would go
 * unseen). Canon MENU, playback, Q, screen saver, ...: latched for iv_tick;
 * not while the ML menu (it sets 0x2003 itself) is open or just closed */
static void iv_sample(int now)
{
    int menu = gui_menu_shown();
    if (menu) iv_t_menu = now;
    uint32_t v = 1;
    int len = 0;
    if (lv_action_raw_get(&v, &len) && len == 4 && v == 0)
    {
        if (iv_t_lv < 0 || now < iv_t_lv) iv_t_lv = now;
    }
    else iv_t_lv = -1;
    int mode = CURRENT_GUI_MODE;
    if (st.running && !iv_gui_seen && mode != 0 && mode != R6IV_GUI_REVIEW && mode != R6IV_GUI_SR
        && !menu && now - iv_t_menu >= R6IV_GUI_GRACE_MS)
        iv_gui_seen = mode;
}

/* camera ready late: skip the slots it missed (no catch-up pictures) */
static void iv_skip_late(int now)
{
    while (now - iv_due > R6IV_LATE_MS)
    {
        iv_due += st.interval_s * 1000;
        st.skipped++;
    }
}

/* one picture; called by the task only, with the camera checked as ready */
static void iv_shoot(void)
{
    struct r6_iv_shot * e = &st.ring[st.shots % R6IV_RING];
    memset(e, 0, sizeof(*e));
    e->wait_ms = iv_wait_ms;
    e->wait_mask = iv_wait_mask;
    int cnt0 = total_shots_count;
    int fn0 = iv_file_number();
    e->js_before = lens_info.job_state;
    if (ml_shutdown_requested)
    {
        iv_end(R6IV_STOP_SHUTDOWN, 0, 0);
        return;
    }

    int t0 = get_ms_clock();
    e->t = t0;
    st.calls++;
    int r = iv_release();
    int t1 = get_ms_clock();
    iv_sample(t1);
    e->ret = r;
    e->dur = MAX(t1 - t0, 0);
    st.shots++;
    if (r != 0) { iv_end(R6IV_STOP_RET, 0, r); return; }
    if ((int) e->dur >= R6IV_SLOW_MS) { iv_end(R6IV_STOP_SLOW, 0, e->dur); return; }

    /* the new picture: PROP_RELEASE_COUNTER or the file number (whichever
     * Canon delivers first); a negative file number step (folder change) is
     * not counted */
    int d_cnt = 0, d_fn = 0, t = t1;
    for (;;)
    {
        d_cnt = total_shots_count - cnt0;
        d_fn = MAX(iv_file_number() - fn0, 0);
        if (d_cnt || d_fn) break;
        t = get_ms_clock();
        if (t - t0 >= R6IV_PIC_MS || t < t0) break;
        msleep(20);
        iv_sample(get_ms_clock());
    }
    e->d_cnt = d_cnt;
    e->d_fn = d_fn;
    if (!d_cnt && !d_fn) { iv_end(R6IV_STOP_NOPIC, 0, 0); return; }
    e->pic_ms = MAX(get_ms_clock() - t0, 0);

    /* Canon's job finished: job count 0 again */
    uint32_t jobs = 0;
    for (;;)
    {
        jobs = MEM(R6IV_RAM_JOBCNT);
        if (!jobs) break;
        t = get_ms_clock();
        if (t - t0 >= R6IV_JOB_MS || t < t0) break;
        msleep(20);
        iv_sample(get_ms_clock());
    }
    e->jobs_after = jobs;
    if (jobs) { iv_end(R6IV_STOP_JOB, 0, jobs); return; }
    e->job_ms = MAX(get_ms_clock() - t0, 0);
    msleep(R6IV_SETTLE_MS);
    iv_sample(get_ms_clock());
    e->js_after = lens_info.job_state;
    e->d_cnt = d_cnt = total_shots_count - cnt0;
    e->d_fn = d_fn = MAX(iv_file_number() - fn0, 0);
    if (d_cnt > 1 || d_fn > 1) { iv_end(R6IV_STOP_EXTRA, 0, MAX(d_cnt, d_fn)); return; }

    st.n_done++;
    iv_wait_ms = 0;
    iv_wait_mask = 0;
    if (st.n_done >= st.n_target) { iv_end(R6IV_STOP_DONE, 0, 0); return; }
    iv_due = t0 + st.interval_s * 1000;
}

static void iv_tick(int now, int dt)
{
    int c = iv_cmd;
    if (c)
    {
        iv_cmd = 0;
        if (c == 1 && !st.running) iv_begin(now);
        else if (c == 2 && st.running) iv_end(R6IV_STOP_USER, 0, 0);
    }
    if (!st.running) return;

    /* stop conditions: every tick, also between slots */
    if (ml_shutdown_requested) { iv_end(R6IV_STOP_SHUTDOWN, 0, 0); return; }
    int cam = r6_iv_cam_check();
    if (cam != R6IV_CAM_OK) { iv_end(R6IV_STOP_CAMERA, cam, 0); return; }
    /* Canon MENU, playback, Q, ...: the user took over; screen saver / eco
     * mode: Canon's power saving (latched by iv_sample, also during a picture) */
    int mode = iv_gui_seen;
    if (mode)
    {
        iv_end((mode == R6IV_GUI_SAVER || mode == R6IV_GUI_ECO) ? R6IV_STOP_PWRSAVE : R6IV_STOP_GUI, 0, mode);
        return;
    }
    int hard = 0;
    int k = iv_canon_check(&hard, 1);
    if (hard) { iv_end(R6IV_STOP_CANON, k, 0); return; }

    int due = now - iv_due >= 0;
    if (k != R6IV_CANON_OK)
    {
        /* not ready: counts at a slot, and between slots once a slot found
         * the camera not ready (no picture since) */
        if (!due && !iv_wait_ms) return;
        if (k == R6IV_CANON_MENU || (k == R6IV_CANON_LV && now - iv_t_menu < R6IV_MENU_HOLD_MS + R6IV_LV_MS + 1000))
        {
            /* the ML menu, and Canon's LV restart after it: postpone, never skip */
            if (due) iv_due = now;
            return;
        }
        iv_wait_mask |= 1 << k;
        iv_wait_ms += dt;
        if (iv_wait_ms >= R6IV_WAIT_MAX_MS) { iv_end(R6IV_STOP_WAIT, k, iv_wait_ms); return; }
        if (due) iv_skip_late(now);
        return;
    }
    if (!due) return;
    if (now - iv_due > R6IV_LATE_MS) { iv_skip_late(now); return; }
    iv_shoot();
}

/* never ends (its stack holds the frames Canon's remote switch slots may
 * still point to); every call("Release") is made here */
static void r6_iv_task(void * unused)
{
    int last = get_ms_clock();
    for (;;)
    {
        msleep(50);
        int now = get_ms_clock();
        int dt = now - last;
        last = now;
        if (dt < 0 || dt > 1000) dt = 50;     /* clock glitch (SMP) */
        iv_sample(now);
        iv_tick(now, dt);
    }
}

TASK_CREATE("r6_iv_task", r6_iv_task, 0, 0x1a, 0x4000);

/* ---- menu ---- */
static MENU_SELECT_FUNC(iv_start_stop)
{
    if (iv_cmd == 2) return;                 /* Stop pressed, not taken over by the task yet */
    if (st.running || iv_cmd == 1) { iv_cmd = 2; return; }
    int c = r6_iv_cam_check();
    if (c != R6IV_CAM_OK)
    {
        iv_refused_cam = c;
        st.refused++;
        return;
    }
    iv_refused_cam = R6IV_CAM_OK;
    iv_cmd = 1;
}

static MENU_UPDATE_FUNC(iv_start_update)
{
    if (iv_cmd == 2)
    {
        MENU_SET_NAME("Stop");
        MENU_SET_VALUE("stopping");
        return;
    }
    if (st.running || iv_cmd == 1)
    {
        MENU_SET_NAME("Stop");
        int due = (iv_due - get_ms_clock() + 999) / 1000;
        if (iv_cmd == 1) MENU_SET_VALUE("starting");
        else MENU_SET_VALUE("%d/%d, next %ds", st.n_done, st.n_target, MAX(due, 0));
        MENU_SET_ICON(MNI_ON, 0);
        return;
    }
    MENU_SET_NAME("Start");
    MENU_SET_ICON(MNI_ACTION, 0);
    if (iv_refused_cam != R6IV_CAM_OK)
    {
        MENU_SET_VALUE("refused");
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Not started: %s (see Camera).", r6_iv_cam_name(iv_refused_cam));
    }
}

static const char * iv_mode_name(int m)
{
    switch (m)
    {
        case SHOOTMODE_P: return "P";
        case SHOOTMODE_TV: return "Tv";
        case SHOOTMODE_AV: return "Av";
        case SHOOTMODE_M: return "M";
        case SHOOTMODE_BULB: return "B";
    }
    return "?";
}

static MENU_UPDATE_FUNC(iv_cam_update)
{
    int seen = 0;
    int tv = r6_iv_tv_prop(&seen);
    if (!seen) tv = 0;
    MENU_SET_VALUE("%s, %s, %s, %s", iv_mode_name(shooting_mode),
        drive_mode == DRIVE_SINGLE ? "single" : "drive?",
        (af_mode & 0xF) == AF_MODE_MANUAL_FOCUS ? "MF" : "AF",
        (tv >= 15 && tv <= R6IV_RAW_TV_MAX) ? lens_format_shutter(tv) : "Tv?");
    int c = r6_iv_cam_check();
    if (c == R6IV_CAM_OK) return;
    switch (c)
    {
        case R6IV_CAM_MODE:
            MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Mode %d: set M.", shooting_mode); break;
        case R6IV_CAM_TV:
            MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Tv raw %d (%s): set 1 s or shorter, turn the main dial.", tv, seen ? "seen" : "not seen"); break;
        case R6IV_CAM_DRIVE:
            MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Drive 0x%x: set single shot (0 expected).", drive_mode); break;
        case R6IV_CAM_FOCUS:
            MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Focus 0x%x: set MF (low nibble 3 expected).", af_mode & 0xFFFF); break;
        case R6IV_CAM_CARD:
            MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Shots left %d: card missing or full.", avail_shot); break;
        default:
            MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "%s.", r6_iv_cam_name(c)); break;
    }
}

static MENU_UPDATE_FUNC(iv_canon_update)
{
    int hard = 0;
    int k = r6_iv_canon_check(&hard);
    MENU_SET_VALUE("%s", r6_iv_canon_name(k));
    MENU_SET_RINFO("jobs %d", (int) MEM(R6IV_RAM_JOBCNT));
    MENU_SET_HELP("shots %d, file %d, job 0x%x, buffer %d, lock 0x%x",
        total_shots_count, iv_file_number(), lens_info.job_state, burst_count, icu_uilock);
    if (hard) MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "No release possible: %s.", r6_iv_canon_name(k));
}

static MENU_UPDATE_FUNC(iv_last_update)
{
    if (st.stop == R6IV_STOP_NONE && !st.running)
    {
        MENU_SET_VALUE("-");
        return;
    }
    MENU_SET_VALUE("%d/%d, %s", st.n_done, st.n_target, st.running ? "running" : r6_iv_stop_name(st.stop));
    if (st.shots)
    {
        const struct r6_iv_shot * e = &st.ring[(st.shots - 1) % R6IV_RING];
        MENU_SET_RINFO("%dms r%d", (int) e->dur, (int) e->ret);
    }
    if (st.running) return;
    switch (st.stop)
    {
        case R6IV_STOP_CAMERA:
            MENU_SET_WARNING(MENU_WARN_INFO, "Stopped: %s.", r6_iv_cam_name(st.stop_cam)); break;
        case R6IV_STOP_CANON: case R6IV_STOP_WAIT:
            MENU_SET_WARNING(MENU_WARN_INFO, "Stopped: %s (%s).", r6_iv_stop_name(st.stop), r6_iv_canon_name(st.stop_canon)); break;
        case R6IV_STOP_GUI:
            MENU_SET_WARNING(MENU_WARN_INFO, "Stopped: Canon GUI mode 0x%x.", (int) st.stop_val); break;
        case R6IV_STOP_PWRSAVE:
            MENU_SET_WARNING(MENU_WARN_INFO, "Stopped: Canon screen off / eco mode. Set Screen off to the longest time."); break;
        case R6IV_STOP_RET: case R6IV_STOP_SLOW: case R6IV_STOP_EXTRA: case R6IV_STOP_JOB:
            MENU_SET_WARNING(MENU_WARN_INFO, "Stopped: %s %d. Log snapshot, please.", r6_iv_stop_name(st.stop), (int) st.stop_val); break;
        case R6IV_STOP_NOPIC:
            MENU_SET_WARNING(MENU_WARN_INFO, "Stopped: no new picture seen. Log snapshot, please."); break;
    }
}

static MENU_UPDATE_FUNC(iv_apo_update)
{
    MENU_SET_VALUE("%d", auto_power_off_time);
}

static struct menu_entry iv_menu[] = {
    {
        .name   = "R6 Interval",
        .select = menu_open_submenu,
        .help   = "M8.0: take pictures at an interval (Canon's Release, no settings written).",
        .help2  = "Limits: M, Tv 1 s or shorter, single shot, MF, at most 50 pictures.",
        .children = (struct menu_entry[]) {
            {
                .name   = "Start",
                .select = iv_start_stop,
                .update = iv_start_update,
                .help   = "Start / stop a series. Canon MENU, playback, Q or screen off stop it.",
                .help2  = "First picture after the start delay, at least 3 s after this menu closed.",
            },
            {
                .name   = "Interval",
                .priv   = &r6_iv_interval_idx,
                .max    = COUNT(iv_intervals) - 1,
                .choices = iv_interval_names,
                .help   = "Time from one picture to the next. A late slot is skipped.",
                .help2  = "Canon's Screen off must be longer than this (screen off ends a series).",
            },
            {
                .name   = "Pictures",
                .priv   = &r6_iv_count,
                .min    = 1,
                .max    = R6IV_MAX_PICS,
                .help   = "Pictures per series (1..50).",
            },
            {
                .name   = "Start delay",
                .priv   = &r6_iv_delay_idx,
                .max    = COUNT(iv_delays) - 1,
                .choices = iv_delay_names,
                .help   = "Time from Start to the first picture.",
            },
            {
                .name   = "Camera",
                .update = iv_cam_update,
                .icon_type = IT_ALWAYS_ON,
                .help   = "How ML reads mode, drive, focus and Tv. Start only if all fit (M only).",
                .help2  = "Compare with Canon's display; a wrong value here means: do not start.",
            },
            {
                .name   = "Canon",
                .update = iv_canon_update,
                .icon_type = IT_ALWAYS_ON,
                .help   = "Canon state before a picture (read only).",
            },
            {
                .name   = "Last series",
                .update = iv_last_update,
                .icon_type = IT_ALWAYS_ON,
                .help   = "Pictures / target and why it ended; right: last release time, return.",
            },
            {
                .name   = "Auto power off",
                .update = iv_apo_update,
                .icon_type = IT_ALWAYS_ON,
                .help   = "Canon's value (raw). ML cannot keep the camera on during a series:",
                .help2  = "set Canon's auto power off to Disable for long series.",
            },
            MENU_EOL
        },
    },
};

static void iv_init()
{
    menu_add("Shoot", iv_menu, COUNT(iv_menu));
}

INIT_FUNC("r6_interval", iv_init);

#endif /* CONFIG_R6_INTERVAL */
