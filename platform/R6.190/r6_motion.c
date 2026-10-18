/*
 * R6.190 M9.3: motion trigger, ML menu Shoot > "R6 Motion" (docs/m9_plan.md 1.2; spec m9/m93 spec_m93.md with the
 * corrections of spec_m93_skeptic.md).
 *
 * What: a picture when something moves in a part of the LV image, or (Trigger "Brightness") when that part gets
 * brighter or darker. Mode "Display only" runs the detector and counts, it never presses. Mode "Release" takes up to
 * 50 pictures, each through R6 Interval's iv_shoot (r6_interval.c): the two CameraConductor shutter callbacks
 * CC_SW1_CBR 0xE0296518 / CC_SW2_CBR 0xE029652E with all M8.x checks (approved 2026-10-04, also for releases driven
 * by the LV image at any time and after long waits). M with Tv up to 30 s, single shot, MF; no B (no bulb for
 * motion). Motion and R6 Interval never run at the same time (one series, one presser, one task).
 *
 * This file computes and decides only. It calls nothing of Canon's, writes nothing of Canon's and presses nothing:
 * no property, no MMIO, no file, no Canon call (malloc / free / get_us_clock are ML's). Upstream's
 * FEATURE_MOTION_DETECT stays off (force_liveview, property writes, page-0 reads).
 *
 * Image: Canon's FaceYuv (640x426 UYVY, signed chroma, 4 buffers, ~60 fps; r6_faceyuv.c), only the second-newest
 * ring frame F2 through r6_fy_locate_done (per R2Y job 0xE0365006: ring step 0xE0809212, getter 0xE0809390, record
 * 0xE0809550, then the hardware writes; F1 may still be written; [ctx+0xB048] & 6 == 0; exactly one hand-out since
 * F2), read through the verified uncached alias. A frame is analysed only on a tick whose own locate succeeded
 * (skeptic A5), and it counts only if the read took <= 20 ms from before the locate, at most 2 hand-outs happened
 * meanwhile (r6_fy_recheck: F2's buffer comes back with the 3rd) and 24 probe words (one per block row) did not
 * change during the read (skeptic A1: Canon can run two R2Y jobs at once, so "F2 complete" is inferred from timing;
 * a writer seen in F2's buffer discards the frame, md.torn_w). A discarded frame neither counts nor resets the
 * confirmation.
 *
 * Detector (integers, no division by a constant that is not a power of 2): 32 x 24 blocks over rows 9..416, 16
 * luma samples per block (rows 13 + 17 by and +8, words 10 bx + 1 / 3 / 6 / 8). Background R (Q8 luma) from 16 quiet
 * frames, noise N (Q4 luma, 1..40). Light: per block-row gain = median ratio Y / R over the row's bright blocks (>= 8
 * of them), else the median of the row medians (skeptic C3), clamped 0.5 .. 2; the image gain outside 0.67 .. 1.5 is
 * a "light" event. A block is hit if |Y - P| > max(Level, 3 N + 2 luma) with P = R * gain capped at the clip (16 x
 * 255). Review F1 / round 2: a block may be clipped, wholly or in part, if its background is near clip (>= 231 luma,
 * also video-range white 235) or one of its 16 samples reached 250 in this frame or in a frame its background took in
 * (clip bits per block row). Its true value is unknown, but a light change alone keeps it between R and P: only a drop
 * below both or a rise above both counts, and it stays out of the gains and the noise (flicker on a clipped area is
 * no motion; md.ymax shows where FaceYuv tops out). More than 40 % of all blocks hit is
 * a "move" event (shake, pan, zoom). Events re-measure and never trigger. Over = hits in the area >= 2 / 3 / 4 / 6
 * (Small .. Full); Confirm frames in a row trigger; a gap longer than max(150 ms, 2 x the frame period + 40 ms)
 * without a good frame restarts the count (skeptic C2). The background follows only without a trigger or an event
 * (non-hit blocks tau ~32 frames, hit blocks ~256). Brightness: the area mean moved by >= Level.
 *
 * Gates, every tick in SETTLE / BASE / ARMED (a failing gate re-measures, it is not a stop): Canon LV running 2 s,
 * ML menu closed + 3 s (and R6 Interval's hold after a blocked Trash press), Canon GUI mode 0 and gui_state idle, no
 * magnify, display mask exactly 0x01 or 0x10 ([[0xEA40]+0x18], r6_gdraw.c), settings (motion settings, Tv, Av, ISO
 * setting, ExpSim) unchanged since the background, a locate and a new F2 within max(300 ms, 4 x the frame period).
 * Stops (r6_interval.c iv_tick_md): ML shutdown, Canon GUI / power saving, the maximum duration, and in Release mode
 * a picture ML did not take (every tick), a mode change, the camera check, Canon's hard reasons, every iv_shoot stop.
 * A trigger while Canon is not ready is dropped, never caught up; every outcome waits, settles 500 ms and measures a
 * new background. Review F2: Mode and Pictures define a run (taken at the Start) and are fixed while it is active;
 * r6_interval.c presses only for a run started as Release (its mode and the detector's copy).
 *
 * Display (r6_grid.c, overlay task, <= 4 Hz): area frame, value bar with the threshold tick, state square, in ML's
 * buffer with the grid's pixel rules (mirror marks), under Canon's display; nothing while a picture runs.
 * Memory: one heap block of 4896 B per run (freed at the end), < 1 KB statics.
 */

#include "dryos.h"
#include "bmp.h"
#include "menu.h"
#include "config.h"
#include "property.h"
#include "propvalues.h"
#include "lens.h"
#include "r6_faceyuv.h"
#include "r6_gdraw.h"
#include "r6_interval.h"
#include "r6_motion.h"

#ifdef CONFIG_R6_MOTION

#if !defined(CONFIG_R6_INTERVAL) || !defined(CONFIG_R6_FACEYUV_LV) || !defined(CONFIG_R6_GLOBAL_DRAW) || !defined(CONFIG_R6_GRID)
#error "R6 Motion needs R6 Interval, the FaceYuv LV image, global draw and the grid display path"
#endif
#ifdef FEATURE_MOTION_DETECT
#error "R6 M9.3: upstream motion detect stays off (force_liveview, property writes, page-0 reads)"
#endif
#ifdef CONFIG_PROP_REQUEST_CHANGE
#error "R6 M9.3: property writes stay off"
#endif

#define R6MD_ANA_MIN_MS     30     /* at most one analysis per 30 ms */
#define R6MD_SETTLE_MS    1000     /* gates OK this long before a background (start, gate failure) */
#define R6MD_SETTLE2_MS    500     /* after WAIT / a global event */
#define R6MD_BASE_N         16     /* quiet frames for a background */
#define R6MD_BASE_MAX_MS  5000     /* then "unstable scene" (keeps trying) */
#define R6MD_STALE_MIN_MS  300     /* a locate and a new frame within max(this, 4 x the frame period) */
#define R6MD_GAP_MS        150     /* no good frame for max(this, 2 x the frame period + 40): the confirmation restarts */
#define R6MD_READ_MAX_US 20000     /* locate + pixel read + recheck longer: discarded (torn) */
#define R6MD_ROW0            9     /* FaceYuv rows 9..416 = 24 block rows x 17 */
#define R6MD_DARK_Q4       384     /* R >= 24 luma: the block gives a gain ratio */
#define R6MD_GAIN_LO       171     /* image gain below 0.67 or above 1.5 (Q8): light changed (global event) */
#define R6MD_GAIN_HI       384
#define R6MD_NOISE_MIN      16     /* Q4: 1 .. 40 luma */
#define R6MD_NOISE_MAX     640
#define R6MD_CLIP16       4080     /* review F1: 16 x 255, the largest block value (Y16 = sum of 16 samples) */
#define R6MD_NEAR16       3696     /* review F1: 16 x 231, "near clip": also video-range white (235) less 4 luma of noise */
#define R6MD_CLIP_Y        250     /* round 2: a sample >= 250 luma: its block may be partly clipped (clip bit) */

/* display colours: indices no grid colour / mark uses (spotmeter 0x80, grid r6_grid.c gr_colors / masks / shadow) */
#define MD_C_FRAME  COLOR_ORANGE
#define MD_C_ARMED  COLOR_GREEN1
#define MD_C_CONF   COLOR_MAGENTA
#define MD_C_IDLE   COLOR_LIGHT_BLUE

/* settings (saved; a run never starts by itself) */
CONFIG_INT("r6.md.mode",    r6_md_cfg_mode, 0);       /* 0 Display only, 1 Release */
CONFIG_INT("r6.md.trig",    r6_md_cfg_trig, 0);       /* 0 Motion, 1 Brightness */
CONFIG_INT("r6.md.area",    r6_md_cfg_area, 1);       /* Small / Medium / Large / Full */
CONFIG_INT("r6.md.pos",     r6_md_cfg_pos, 0);        /* Center / Left / Right / Top / Bottom */
CONFIG_INT("r6.md.level",   r6_md_cfg_level, 8);      /* 3..30 luma */
CONFIG_INT("r6.md.confirm", r6_md_cfg_confirm, 2);    /* 1..3 frames */
CONFIG_INT("r6.md.count",   r6_md_cfg_count, 5);      /* 1..50 */
CONFIG_INT("r6.md.wait",    r6_md_cfg_wait, 1);       /* 2 s */
CONFIG_INT("r6.md.delay",   r6_md_cfg_delay, 1);      /* 5 s */
CONFIG_INT("r6.md.maxdur",  r6_md_cfg_maxdur, 1);     /* 30 min */
CONFIG_INT("r6.md.disp",    r6_md_cfg_disp, 1);

/* tables instead of divisions */
static const uint8_t md_wait_s[] = { 1, 2, 5, 10, 30, 60 };
static const char * md_wait_names[] = { "1s", "2s", "5s", "10s", "30s", "1m" };
static const uint8_t md_delay_s[] = { 3, 5, 10, 30, 60 };
static const char * md_delay_names[] = { "3s", "5s", "10s", "30s", "1m" };
static const uint16_t md_maxdur_s[] = { 600, 1800, 3600, 7200, 0 };
static const char * md_maxdur_names[] = { "10m", "30m", "1h", "2h", "no limit" };
static const uint8_t md_area_wh[4][2] = { { 4, 4 }, { 8, 6 }, { 16, 12 }, { 32, 24 } };
static const uint8_t md_minblk[4] = { 2, 3, 4, 6 };
/* position: x rule << 4 | y rule (0 = 1 block from the edge, 1 = centred, 2 = 1 block from the far edge) */
static const uint8_t md_posf[5] = { 0x11, 0x01, 0x21, 0x10, 0x12 };
/* ML rows of the block-row tops: round((9 + 17 by) * 480 / 426), computed offline (no / 426 at run time) */
static const uint16_t md_ytab[R6MD_H + 1] = { 10, 29, 48, 68, 87, 106, 125, 144, 163, 183, 202, 221, 240, 259, 278, 297,
                                              317, 336, 355, 374, 393, 412, 432, 451, 470 };

static int md_cfg(int v, int lo, int hi) { return COERCE(v, lo, hi); }
int r6_md_shoot_setting(void) { return r6_md_cfg_mode == 1; }
int r6_md_count(void) { return md_cfg(r6_md_cfg_count, 1, 50); }
static int md_wait_sec(void) { return md_wait_s[md_cfg(r6_md_cfg_wait, 0, COUNT(md_wait_s) - 1)]; }
static int md_delay_sec(void) { return md_delay_s[md_cfg(r6_md_cfg_delay, 0, COUNT(md_delay_s) - 1)]; }
int r6_md_maxdur_ms(void) { return md_maxdur_s[md_cfg(r6_md_cfg_maxdur, 0, COUNT(md_maxdur_s) - 1)] * 1000; }

/* ---- state (r6_iv_task writes; the overlay task and the menu read plain ints) ---- */
static struct r6_md_stats st = { .nr_last = "" };
static uint16_t * md_buf = 0;                    /* heap: R, N, Y[768] (Q8, Q4, 16 x mean); hit, clp, clpr[24] (u32) */
#define MD_R   (md_buf)
#define MD_N   (md_buf + R6MD_N)
#define MD_Y   (md_buf + 2 * R6MD_N)
#define MD_HIT ((uint32_t *) (md_buf + 3 * R6MD_N))
#define MD_CLP  (MD_HIT + R6MD_H)                /* round 2: bit bx of row by: a sample >= 250 in the last read */
#define MD_CLPR (MD_HIT + 2 * R6MD_H)            /* round 2: the same, OR of every frame the background R took in */
#define MD_BUF_SIZE (3 * R6MD_N * 2 + 3 * R6MD_H * 4)

static int md_until, md_ok_since, md_settle_ms, md_rb;
static int md_t_fy, md_t_f2, md_t_ana, md_t_good, md_t_base, md_t_over, md_t_trig;
static uint32_t md_last_f2, md_sig0, md_shots0, md_amask, md_trig0;
static int md_f2_seen, md_have, md_ivl_q4, md_aivl_q4, md_q, md_base_init, md_unst, md_stale_on;
static int md_trig_k, md_level, md_confirm;      /* settings of the current background */

const struct r6_md_stats * r6_md_stats(void) { return &st; }
int r6_md_active(void) { return st.state != R6MD_OFF; }
int r6_md_disp_wanted(void) { return r6_md_cfg_disp && st.state != R6MD_OFF; }

/* the int32 fields of struct r6_md_stats before nr_last, in order (log keys md.*) */
static const char md_knames[] =
    "state\0release\0gate_last\0t_state\0t_start\0cfg\0count\0wait_s\0delay_s\0maxdur_s\0ax0\0ay0\0ax1\0ay1\0minblk\0"
    "frames\0analysed\0fps_x10\0ana_x10\0fstep\0retry\0us_last\0us_avg\0us_max\0torn\0torn_w\0stale\0"
    "global_light\0global_move\0unstable\0base_ms_last\0triggers\0shots\0dropped\0dropped_mask\0"
    "score_last\0score_max\0hits_all_last\0hits_all_max\0val\0thr\0run\0"
    "noise_avg_q4\0gain_all_q8\0gain_min_q8\0gain_max_q8\0lat_conf_ms_last\0lat_sw1_ms_last\0frame_age_ms_last\0"
    "stop\0n_done\0n_target\0ymax\0ymax_last";
#define MD_NKEYS 54
typedef char md_knames_match[__builtin_offsetof(struct r6_md_stats, nr_last) == MD_NKEYS * 4 ? 1 : -1];
const char * r6_md_knames(int * n) { *n = MD_NKEYS; return md_knames; }

static const char * const md_state_names[R6MD_NSTATES] = { "off", "delay", "settle", "background", "armed", "picture", "wait" };
static const char * const md_gate_names[R6MD_NGATES] = { "-", "LV", "menu", "GUI", "magnify", "display", "settings",
                                                         "FaceYuv", "stale" };
static const char * const md_nr_names[R6MD_NR_OTHER] = { "b048", "ring_f", "no_rec_new", "no_rec_done", "same_buf",
                                                         "handout", "size" };
const char * r6_md_state_name(int s) { return (s >= 0 && s < R6MD_NSTATES) ? md_state_names[s] : "?"; }
const char * r6_md_gate_name(int g) { return (g >= 0 && g < R6MD_NGATES) ? md_gate_names[g] : "?"; }

static void md_enter(int s, int now) { st.state = s; st.t_state = now; }

/* the settings of a background: area (blocks, half-open), level, confirm, trigger kind; packed for the log */
static void md_cfg_snap(void)
{
    int a = md_cfg(r6_md_cfg_area, 0, 3), p = md_cfg(r6_md_cfg_pos, 0, 4);
    int w = md_area_wh[a][0], h = md_area_wh[a][1], f = a == 3 ? 0x11 : md_posf[p];
    st.ax0 = 1 + (((R6MD_W - 2 - w) * (f >> 4)) >> 1);       /* 1, centred (w even) or R6MD_W - 1 - w; Full: 0 */
    st.ay0 = 1 + (((R6MD_H - 2 - h) * (f & 15)) >> 1);
    st.ax1 = st.ax0 + w; st.ay1 = st.ay0 + h;
    st.minblk = md_minblk[a];
    md_amask = w == R6MD_W ? 0xFFFFFFFFu : ((1u << w) - 1) << st.ax0;
    md_trig_k = md_cfg(r6_md_cfg_trig, 0, 1);
    md_level = md_cfg(r6_md_cfg_level, 3, 30);
    md_confirm = md_cfg(r6_md_cfg_confirm, 1, 3);
    st.cfg = md_trig_k | a << 4 | p << 8 | md_level << 12 | md_confirm << 20 | (r6_md_cfg_disp ? 1 : 0) << 24;
    st.count = r6_md_count(); st.wait_s = md_wait_sec(); st.delay_s = md_delay_sec(); st.maxdur_s = r6_md_maxdur_ms() / 1000;
}

/* settings a background depends on: motion settings, Tv as delivered (R6 Interval's handler), Av, the ISO setting
 * (0 = auto: Auto ISO steps are a gain), ExpSim. Count, wait, delay, maximum duration and Display do not re-measure. */
static uint32_t md_sig(void)
{
    int seen = 0, len = 0, tv = r6_iv_tv_prop(&seen, &len);
    int32_t a[10] = { r6_md_cfg_trig, r6_md_cfg_area, r6_md_cfg_pos, r6_md_cfg_level, r6_md_cfg_confirm, tv, seen,
                      lens_info.raw_aperture, lens_info.raw_iso, _expsim };
    return r6_gd_bv_hash_mem(a, sizeof(a));
}

static void md_last_add(int now, int result)
{
    struct r6_md_last * e = &st.last[st.nlast % R6MD_RING];
    e->t = now; e->f2 = md_last_f2; e->state = st.state; e->score = st.score_last; e->hits = st.hits_all_last;
    e->run = st.run; e->result = result;
    st.nlast++;
}
static void md_last_set(int result) { if (st.nlast) st.last[(st.nlast - 1) % R6MD_RING].result = result; }

/* into SETTLE: gates OK for ms before the next background (why = R6MD_RB_*); fresh = coming from DELAY / WAIT, where
 * the tracker did not run */
static void md_settle(int now, int ms, int why, int fresh)
{
    md_enter(R6MD_SETTLE, now);
    md_settle_ms = ms;
    md_ok_since = -1;
    md_rb = why;
    st.run = 0;
    if (fresh) md_t_fy = md_t_f2 = now;
}

int r6_md_begin(int now, int shoot)
{
    if (!md_buf) md_buf = malloc(MD_BUF_SIZE);
    if (!md_buf) return -1;
    st.release = shoot; st.t_start = now;
    st.stop = 0; st.n_done = 0; st.n_target = shoot ? r6_md_count() : 0;
    st.gate_last = 0; st.run = 0; st.val = st.thr = 0;
    md_cfg_snap();
    md_f2_seen = md_have = md_ivl_q4 = md_aivl_q4 = md_stale_on = 0;
    md_trig0 = st.triggers;
    md_until = now + st.delay_s * 1000;
    md_enter(R6MD_DELAY, now);
    return 0;
}

/* idempotent (skeptic B3: R6 Interval's iv_end can run twice) */
void r6_md_end(void)
{
    if (st.state == R6MD_OFF && !md_buf) return;
    const struct r6_iv_stats * s = r6_iv_stats();
    if (st.state == R6MD_PIC)                         /* iv_shoot ended the run (DONE or a stop): its outcome */
    {
        int shot = s->shots != md_shots0;
        md_last_set(shot ? R6MD_R_SHOT : R6MD_R_DROP);
        if (shot) st.shots++;
        else { st.dropped++; st.dropped_mask |= 1; }
    }
    st.stop = s->stop; st.n_done = s->n_done;
    if (md_buf) { free(md_buf); md_buf = 0; }
    md_enter(R6MD_OFF, get_ms_clock());
}

void r6_md_after(int now, int result)
{
    if (st.state != R6MD_PIC) return;
    md_last_set(result);
    if (result == R6MD_R_SHOT) { st.shots++; st.lat_sw1_ms_last = now - md_t_trig; }
    if (result >= R6MD_R_DROP) { st.dropped++; st.dropped_mask |= 1u << ((result - R6MD_R_DROP) & 31); }
    md_until = now + md_wait_sec() * 1000;
    md_enter(R6MD_WAIT, now);
}

/* ---- frame tracker and gates ---- */
static void md_track(const struct r6_fy_loc * l, int ok, int now)
{
    if (!ok)
    {
        int c = 0;
        while (c < R6MD_NR_OTHER && strcmp(l->why, md_nr_names[c])) c++;
        st.nr[c]++;
        st.nr_last = l->why;
        return;
    }
    md_t_fy = now;
    if (md_f2_seen && l->f2 == md_last_f2) return;
    if (md_f2_seen)
    {
        int dt = now - md_t_f2;
        st.fstep = l->f2 - md_last_f2;
        if (dt > 0 && dt < 1000) md_ivl_q4 = md_ivl_q4 ? md_ivl_q4 + (((dt << 4) - md_ivl_q4) >> 3) : dt << 4;
        st.fps_x10 = md_ivl_q4 ? 160000 / md_ivl_q4 : 0;
    }
    md_last_f2 = l->f2;
    md_f2_seen = 1;
    md_t_f2 = now;
    md_have = 1;
    md_stale_on = 0;
    st.frames++;
}

static int md_gates(const struct r6_md_in * in, int now)
{
    if (!in->lv_ok) return R6MD_G_LV;
    if (!in->menu_ok) return R6MD_G_MENU;
    if (CURRENT_GUI_MODE != 0 || gui_state != GUISTATE_IDLE) return R6MD_G_GUI;
    if (lv_dispsize != 1) return R6MD_G_MAGNIFY;
    uint32_t m = r6_gd_disp_mask();
    if (m != 0x01 && m != 0x10) return R6MD_G_DISPLAY;
    if (st.state != R6MD_SETTLE && md_sig() != md_sig0) return R6MD_G_SETTINGS;
    int lim = MAX(R6MD_STALE_MIN_MS, md_ivl_q4 >> 2);
    if (now - md_t_fy > lim) return R6MD_G_FY;
    if (now - md_t_f2 > lim)
    {
        if (!md_stale_on) { md_stale_on = 1; st.stale++; }
        return R6MD_G_STALE;
    }
    return R6MD_G_OK;
}

/* ---- the pixels ---- */
/* Y0 + Y1 of a UYVY word; the larger of the two into *mx (the block's maximum: clip bit, md.ymax) */
static inline uint32_t md_y2(uint32_t w, uint32_t * mx)
{
    uint32_t a = (w >> 8) & 0xFF, b = w >> 24;
    *mx = MAX(*mx, MAX(a, b));
    return a + b;
}
#define MD_PROBE(cpu, by) (*(const volatile uint32_t *) ((cpu) + (R6MD_ROW0 + 8 + 17 * (by)) * R6_FY_PITCH + (10 * (by) + 5) * 4))

/* 16 samples per block into Y and the clip bits into MD_CLP (like Y: the last read; only a good frame is analysed);
 * 1 = a good frame (no writer seen, <= 2 hand-outs, <= 20 ms since t0) */
static int md_read(const struct r6_fy_loc * l, uint64_t t0)
{
    const uint8_t * cpu = (const uint8_t *) l->cpu;
    uint32_t pr[R6MD_H];
    for (int by = 0; by < R6MD_H; by++) pr[by] = MD_PROBE(cpu, by);
    uint16_t * y = MD_Y;
    uint32_t mx = 0;
    for (int by = 0; by < R6MD_H; by++)
    {
        const volatile uint32_t * a = (const volatile uint32_t *) (cpu + (R6MD_ROW0 + 4 + 17 * by) * R6_FY_PITCH);
        const volatile uint32_t * b = a + 8 * (R6_FY_PITCH / 4);
        uint32_t c = 0;
        for (int bx = 0; bx < R6MD_W; bx++, a += 10, b += 10)
        {
            uint32_t bm = 0;                                      /* round 2: the block's largest sample */
            *y++ = md_y2(a[1], &bm) + md_y2(a[3], &bm) + md_y2(a[6], &bm) + md_y2(a[8], &bm)
                 + md_y2(b[1], &bm) + md_y2(b[3], &bm) + md_y2(b[6], &bm) + md_y2(b[8], &bm);
            c |= (uint32_t) (bm >= R6MD_CLIP_Y) << bx;
            mx = MAX(mx, bm);
        }
        MD_CLP[by] = c;
    }
    /* the read is done: was F2's buffer written meanwhile (probes), handed out again (recheck), too slow? */
    int w = 0;
    for (int by = 0; by < R6MD_H; by++) w |= MD_PROBE(cpu, by) != pr[by];
    int n = r6_fy_recheck(l);
    uint32_t us = (uint32_t) (get_us_clock() - t0);
    st.us_last = us;
    if ((int) us > st.us_max) st.us_max = us;
    st.us_avg += ((int) us - st.us_avg) >> 3;
    if (w) st.torn_w++;
    int ok = !w && n >= 0 && n <= 2 && us <= R6MD_READ_MAX_US;
    if (ok) { st.ymax_last = mx; st.ymax = MAX(st.ymax, (int) mx); }    /* review F1: 255 (full range) or ~235? */
    return ok;
}

static int md_median(uint16_t * v, int n)       /* insertion sort, n <= 32 */
{
    for (int i = 1; i < n; i++)
    {
        uint16_t x = v[i];
        int j = i;
        for (; j > 0 && v[j - 1] > x; j--) v[j] = v[j - 1];
        v[j] = x;
    }
    return v[n >> 1];
}

/* review F1 / round 2: block i may be clipped (its true value is unknown): its background is near clip, or one of its
 * samples reached R6MD_CLIP_Y in this frame (MD_CLP) or in a frame its background took in (MD_CLPR: partly clipped
 * blocks at the edge of a highlight) */
static int md_near(int i)
{
    return (MD_R[i] >> 4) >= R6MD_NEAR16 || (((MD_CLP[i >> 5] | MD_CLPR[i >> 5]) >> (i & 31)) & 1);
}

/* per block-row gains (Q8, clamped 128..512) into gr; returns the image gain (Q8, 256 if not enough bright rows).
 * Review F1 / round 2: a block that may be clipped gives no ratio (a clipped block does not follow the light) */
static int md_gains(int16_t * gr)
{
    uint16_t q[R6MD_W], rm[R6MD_H];
    int nr = 0;
    for (int by = 0; by < R6MD_H; by++)
    {
        int n = 0, n2 = R6MD_W;
        for (int bx = 0; bx < R6MD_W; bx++)
        {
            int i = by * R6MD_W + bx, r16 = MD_R[i] >> 4;
            if (r16 < R6MD_DARK_Q4 || r16 >= R6MD_NEAR16) continue;
            uint16_t v = ((uint32_t) MD_Y[i] << 8) / (uint32_t) r16;                     /* variable divisor */
            if (md_near(i)) q[--n2] = v; else q[n++] = v;   /* round 3: near only through a clip bit: kept apart */
        }
        if (n < 8) while (n2 < R6MD_W) q[n++] = q[n2++];    /* ... and used when the row has too few clean blocks */
        gr[by] = n >= 8 ? md_median(q, n) : -1;
        if (gr[by] >= 0) rm[nr++] = gr[by];
    }
    int ga = nr >= 4 ? md_median(rm, nr) : 256;
    int gmin = 0x7FFF, gmax = 0;
    for (int by = 0; by < R6MD_H; by++)
    {
        int g = COERCE(gr[by] < 0 ? ga : gr[by], 128, 512);
        gr[by] = g;
        gmin = MIN(gmin, g); gmax = MAX(gmax, g);
    }
    st.gain_all_q8 = ga; st.gain_min_q8 = gmin; st.gain_max_q8 = gmax;
    return ga;
}

/* the deviation of block i from its prediction P = R * gain, capped at the clip (16 x luma). Review F1 / round 2: a
 * block that may be clipped (md_near) stays between R and P under a light change alone (per sample clip(g x) lies
 * between clip(x) and g clip(x): a clip at 235, a whole or a partly clipped block, brighter or darker), so only a drop
 * below min(P, R) (the one-sided rule) or a rise above max(P, R) counts: a flicker gives 0, an object leaving a clipped
 * area or rising into clip still hits. Else |Y - P| */
static int md_err(int i, int g)
{
    int r = MD_R[i] >> 4, p = MIN((r * g) >> 8, R6MD_CLIP16), y = MD_Y[i];
    if (md_near(i)) return MAX(MAX(MIN(p, r) - y, y - MAX(p, r)), 0);
    int e = y - p;
    return e < 0 ? -e : e;
}

/* round 2: the background R took this frame in: its clip bits stay with the background until the next one */
static void md_clp_fold(void) { for (int by = 0; by < R6MD_H; by++) MD_CLPR[by] |= MD_CLP[by]; }

static void md_base_start(int now)
{
    md_cfg_snap();
    md_sig0 = md_sig();
    for (int by = 0; by < R6MD_H; by++) MD_CLPR[by] = 0;           /* round 2: the clip bits of the new background */
    st.rebase[md_rb & 3]++;
    md_base_init = 0; md_q = 0; md_t_base = now; md_unst = 0; st.run = 0;
    md_enter(R6MD_BASE, now);
}

/* background: the first good frame, then 16 quiet frames in a row (<= 16 blocks off by more than max(Level, 6 luma)) */
static void md_base(int now)
{
    uint16_t * R = MD_R, * N = MD_N, * Y = MD_Y;
    if (!md_base_init)
    {
        for (int i = 0; i < R6MD_N; i++) { R[i] = Y[i] << 4; N[i] = 32; }
        md_clp_fold();
        md_base_init = 1;
        return;
    }
    int16_t gr[R6MD_H];
    md_gains(gr);
    int thr = MAX(md_level * 16, 96), noisy = 0;
    for (int i = 0; i < R6MD_N; i++) noisy += md_err(i, gr[i >> 5]) > thr;          /* review F1: the clip-aware rule */
    int quiet = noisy <= 16;
    for (int i = 0; i < R6MD_N; i++)
    {
        int e = md_err(i, gr[i >> 5]), nc = (R[i] >> 4) >= R6MD_NEAR16;   /* round 3: only a near-clip background */
        R[i] += ((Y[i] << 4) - R[i]) >> (quiet ? 2 : 1);
        if (quiet && !nc) N[i] = COERCE(N[i] + ((e - N[i]) >> 2), R6MD_NOISE_MIN, R6MD_NOISE_MAX);   /* F1: not near clip */
    }
    md_clp_fold();
    md_q = quiet ? md_q + 1 : 0;
    if (!md_unst && now - md_t_base > R6MD_BASE_MAX_MS) { md_unst = 1; st.unstable++; }
    if (md_q >= R6MD_BASE_N)
    {
        st.base_ms_last = now - md_t_base;
        st.run = 0;
        md_t_good = now;
        md_enter(R6MD_ARMED, now);
    }
}

/* one armed frame: R6MD_TRIGGER, or 0 */
static int md_armed(int now)
{
    uint16_t * R = MD_R, * N = MD_N, * Y = MD_Y;
    uint32_t * hit = MD_HIT;
    int16_t gr[R6MD_H] = { 0 };
    int gap = MAX(R6MD_GAP_MS, (md_ivl_q4 >> 3) + 40);       /* skeptic C2: 2 x the frame period + 40 ms */
    if (now - md_t_good > gap) st.run = 0;
    md_t_good = now;
    int over, ev = 0, score = 0, hits = 0;
    if (!md_trig_k)
    {
        int ga = md_gains(gr), th0 = md_level * 16;
        uint32_t nsum = 0;
        for (int by = 0; by < R6MD_H; by++)
        {
            uint32_t m = 0;
            for (int bx = 0; bx < R6MD_W; bx++)
            {
                int i = by * R6MD_W + bx;
                if (md_err(i, gr[by]) > MAX(th0, 3 * N[i] + 32)) m |= 1u << bx;
                nsum += N[i];
            }
            hit[by] = m;
            hits += __builtin_popcount(m);
            if (by >= st.ay0 && by < st.ay1) score += __builtin_popcount(m & md_amask);
        }
        st.noise_avg_q4 = ((nsum >> 2) * 21845u) >> 22;        /* nsum / 768 without a reciprocal constant */
        if (ga < R6MD_GAIN_LO || ga > R6MD_GAIN_HI) { ev = 1; st.global_light++; }
        else if (hits * 5 > R6MD_N * 2) { ev = 1; st.global_move++; }
        over = score >= st.minblk;
        st.val = score; st.thr = st.minblk;
    }
    else
    {
        int sy = 0, sr = 0, na = (st.ax1 - st.ax0) * (st.ay1 - st.ay0);
        for (int by = st.ay0; by < st.ay1; by++)
            for (int bx = st.ax0; bx < st.ax1; bx++) { sy += Y[by * R6MD_W + bx]; sr += R[by * R6MD_W + bx] >> 4; }
        int d = sy > sr ? sy - sr : sr - sy;
        over = d >= md_level * 16 * na;
        score = d / (16 * na);                                    /* luma (variable divisor) */
        st.val = score; st.thr = md_level;
    }
    st.score_last = score; st.score_max = MAX(st.score_max, score);
    st.hits_all_last = hits; st.hits_all_max = MAX(st.hits_all_max, hits);
    if (ev) { md_settle(now, R6MD_SETTLE2_MS, R6MD_RB_GLOBAL, 0); return 0; }
    if (over) { if (!st.run) md_t_over = now; st.run++; }
    else st.run = 0;
    if (st.run >= md_confirm)
    {
        st.triggers++;
        st.lat_conf_ms_last = now - md_t_over;
        md_t_trig = now;
        md_shots0 = r6_iv_stats()->shots;
        md_last_add(now, -1);
        md_enter(R6MD_PIC, now);
        return R6MD_TRIGGER;
    }
    /* the background follows (no trigger, no event; Brightness: only while not over); hits never raise the noise */
    if (!md_trig_k || !over)
    {
        for (int i = 0; i < R6MD_N; i++)
        {
            int h = !md_trig_k && ((hit[i >> 5] >> (i & 31)) & 1);
            if (!h && !md_trig_k && (R[i] >> 4) < R6MD_NEAR16)     /* F1 / round 3: no noise from a near-clip background
                                                                     * (a clip bit alone still learns: glitter, glints) */
                N[i] = COERCE(N[i] + ((md_err(i, gr[i >> 5]) - N[i]) >> 4), R6MD_NOISE_MIN, R6MD_NOISE_MAX);
            R[i] += ((Y[i] << 4) - R[i]) >> (h ? 8 : 5);
        }
        md_clp_fold();
    }
    return 0;
}

int r6_md_step(const struct r6_md_in * in)
{
    int now = in->now, s = st.state;
    uint32_t sb = 1u << s;
    if (!(sb & ((1 << R6MD_DELAY) | (1 << R6MD_WAIT) | (1 << R6MD_SETTLE) | (1 << R6MD_BASE) | (1 << R6MD_ARMED))))
        return 0;                                                 /* OFF, PIC (r6_interval.c runs iv_shoot) */
    if (sb & ((1 << R6MD_DELAY) | (1 << R6MD_WAIT)))
    {
        int dl = s == R6MD_DELAY;
        if (now - md_until >= 0) md_settle(now, dl ? R6MD_SETTLE_MS : R6MD_SETTLE2_MS, dl ? R6MD_RB_START : R6MD_RB_PIC, 1);
        return 0;
    }
    /* SETTLE / BASE / ARMED: one locate per tick (frame tracker), then the gates */
    uint64_t t0 = get_us_clock();                                 /* skeptic A5: the 20 ms count from before the locate */
    struct r6_fy_loc loc;
    int ok = r6_fy_locate_done(&loc);
    md_track(&loc, ok, now);
    int g = md_gates(in, now);
    if (g)
    {
        if (s != R6MD_SETTLE || md_ok_since >= 0) { st.gate[g]++; st.gate_last = g; md_settle(now, R6MD_SETTLE_MS, R6MD_RB_GATE, 0); }
        md_ok_since = -1;
        return 0;
    }
    if (s == R6MD_SETTLE)
    {
        if (md_ok_since < 0) md_ok_since = now;
        if (now - md_ok_since >= md_settle_ms) md_base_start(now);
        return 0;
    }
    if (!md_have || now - md_t_ana < R6MD_ANA_MIN_MS) return 0;
    if (!ok) { st.retry++; return 0; }                           /* skeptic A5: this tick's locate failed: next tick */
    md_have = 0;
    int da = now - md_t_ana;
    if (da > 0 && da < 1000) { md_aivl_q4 = md_aivl_q4 ? md_aivl_q4 + (((da << 4) - md_aivl_q4) >> 3) : da << 4; st.ana_x10 = 160000 / md_aivl_q4; }
    md_t_ana = now;
    st.frame_age_ms_last = now - md_t_f2;
    if (!md_read(&loc, t0)) { st.torn++; return 0; }
    st.analysed++;
    if (s == R6MD_BASE) { md_base(now); return 0; }
    return md_armed(now);
}

/* ---- display (r6_grid.c, overlay task) ---- */
int r6_md_shape(struct r6_md_rect * r, int max, int evf)
{
    uint32_t sb = 1u << st.state;
    if (!r6_md_cfg_disp || max < 7 || r6_iv_in_pic()
        || !(sb & ((1 << R6MD_SETTLE) | (1 << R6MD_BASE) | (1 << R6MD_ARMED) | (1 << R6MD_WAIT))))
        return 0;
    int yo = evf ? R6_GD_EVF_IMG_Y : 0;
    int x0 = (180 * st.ax0) >> 3, x1 = (180 * st.ax1) >> 3, y0 = md_ytab[st.ay0] + yo, y1 = md_ytab[st.ay1] + yo;
    int armed = (sb & (1 << R6MD_ARMED)) != 0, n = 0;
    int th = MAX(st.thr, 1), f = armed ? MIN(64, 32 * st.val / th) : 0;
#define MD_RECT(a, b, c, d, k) do { r[n].x0 = (a); r[n].y0 = (b); r[n].x1 = (c); r[n].y1 = (d); r[n].col = (k); n++; } while (0)
    MD_RECT(x0 - 2, y0 - 2, x1 + 2, y0, MD_C_FRAME);
    MD_RECT(x0 - 2, y1, x1 + 2, y1 + 2, MD_C_FRAME);
    MD_RECT(x0 - 2, y0, x0, y1, MD_C_FRAME);
    MD_RECT(x1, y0, x1 + 2, y1, MD_C_FRAME);
    MD_RECT(x0 + 36, y0 + 2, x0 + 38, y0 + 10, MD_C_CONF);                       /* threshold tick (first: the bar skips it) */
    if (f > 0) MD_RECT(x0 + 4, y0 + 4, x0 + 4 + f, y0 + 8, MD_C_ARMED);           /* value bar */
    MD_RECT(x1 - 12, y0 + 4, x1 - 4, y0 + 12, !armed ? MD_C_IDLE : st.run ? MD_C_CONF : MD_C_ARMED);
#undef MD_RECT
    return n;
}

/* ---- menu (Shoot > R6 Motion) ---- */
void r6_md_status(char * buf, int n)
{
    int now = get_ms_clock(), s = st.state;
    uint32_t sb = 1u << s;
    if (sb & ((1 << R6MD_DELAY) | (1 << R6MD_WAIT))) snprintf(buf, n, "%s %ds", s == R6MD_DELAY ? "start in" : "wait", MAX(md_until - now + 999, 0) / 1000);
    else if ((sb & (1 << R6MD_SETTLE)) && st.gate_last) snprintf(buf, n, "off: %s", md_gate_names[st.gate_last]);
    else if ((sb & (1 << R6MD_BASE)) && md_unst) snprintf(buf, n, "unstable scene");
    else if (sb & (1 << R6MD_BASE)) snprintf(buf, n, "background %d/%d", md_q, R6MD_BASE_N);
    else if ((sb & (1 << R6MD_ARMED)) && st.run) snprintf(buf, n, "confirm %d/%d", st.run, md_confirm);
    else if (sb & (1 << R6MD_ARMED)) snprintf(buf, n, "armed %d/%d", st.val, st.thr);
    else snprintf(buf, n, "%s", r6_md_state_name(s));
}

static MENU_SELECT_FUNC(md_start_stop)
{
    if (r6_iv_md_running()) r6_iv_md_stop();
    else r6_iv_md_start();
}

static MENU_UPDATE_FUNC(md_main_update)
{
    char s[32];
    if (!r6_iv_md_running()) return;
    r6_md_status(s, sizeof(s));
    MENU_SET_VALUE("%s", s);
}

static MENU_UPDATE_FUNC(md_start_update)
{
    char s[32];
    if (r6_iv_md_running())
    {
        const struct r6_iv_stats * i = r6_iv_stats();
        MENU_SET_NAME("Stop");
        MENU_SET_ICON(MNI_ON, 0);
        r6_md_status(s, sizeof(s));
        if (st.state == R6MD_OFF) MENU_SET_VALUE("starting");
        else if (st.release) MENU_SET_VALUE("%d/%d, %s", i->n_done, i->n_target, s);
        else MENU_SET_VALUE("%s, %d triggers", s, (int) (st.triggers - md_trig0));
        return;
    }
    MENU_SET_NAME("Start");
    MENU_SET_ICON(MNI_ACTION, 0);
    int c = r6_iv_md_refused();
    if (c)
    {
        MENU_SET_VALUE("refused");
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Not started: %s.", r6_iv_cam_name(c));
    }
}

/* review F2: Mode and Pictures define a run (Release or not, the picture target; both taken at the Start): while a run
 * is active or starting a change is ignored, the line shows the run's value and why */
static MENU_SELECT_FUNC(md_fixed_select)
{
    if (r6_iv_md_running()) return;
    int k = priv == &r6_md_cfg_mode;
    menu_numeric_toggle((int *) priv, delta, k ? 0 : 1, k ? 1 : 50);
}

static MENU_UPDATE_FUNC(md_fixed_update)
{
    if (!r6_iv_md_running()) return;
    int on = st.state != R6MD_OFF;                    /* else starting: the settings are the run's (fixed since Start) */
    if (entry->priv == &r6_md_cfg_mode) MENU_SET_VALUE("%s", (on ? st.release : r6_md_cfg_mode == 1) ? "Release" : "Display only");
    else MENU_SET_VALUE("%d", on && st.release ? st.n_target : r6_md_count());
    MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Fixed while a run is active: Stop first.");
}

static MENU_UPDATE_FUNC(md_det_update)
{
    MENU_SET_VALUE("%d fps, %d us, noise %d/16", md_ivl_q4 ? 16000 / md_ivl_q4 : 0, st.us_avg, st.noise_avg_q4);
    MENU_SET_RINFO("trig %d drop %d glob %d", st.triggers, st.dropped, st.global_light + st.global_move);
    MENU_SET_HELP("torn %d stale %d, last gate %s, locate %s", st.torn, st.stale, r6_md_gate_name(st.gate_last),
        st.nr_last[0] ? st.nr_last : "ok");
}

static MENU_UPDATE_FUNC(md_cam_update)
{
    if (shooting_mode == SHOOTMODE_BULB) { MENU_SET_VALUE("B: not for Motion trigger"); return; }
    MENU_SET_VALUE("%s", r6_iv_cam_name(r6_iv_cam_check()));
}

static MENU_UPDATE_FUNC(md_last_update)
{
    const struct r6_iv_stats * i = r6_iv_stats();
    if (r6_iv_md_running() || !st.stop) { MENU_SET_VALUE("-"); return; }
    if (st.release) MENU_SET_VALUE("%d/%d, %s", st.n_done, st.n_target, r6_iv_stop_name(st.stop));
    else MENU_SET_VALUE("%d triggers, %s", (int) (st.triggers - md_trig0), r6_iv_stop_name(st.stop));
    if (i->mode != R6IV_MODE_INTERVAL && i->stop == st.stop) r6_iv_stop_warn(entry, info);
}

static struct menu_entry md_menu[] = {
    {
        .name   = "Motion trigger",
        .select = menu_open_submenu,
        .update = md_main_update,
        .help   = "M9.3: picture when something moves (Canon's FaceYuv image, shutter callbacks).",
        .help2  = "M, Tv up to 30 s, single, MF, max 50. Display only: counts, never presses.",
        .children = (struct menu_entry[]) {
            {
                .name   = "Start",
                .select = md_start_stop,
                .update = md_start_update,
                .help   = "Start / stop. First background after the start delay and 3 s after this menu closed.",
            },
            {
                .name    = "Mode",
                .priv    = &r6_md_cfg_mode,
                .select  = md_fixed_select,
                .update  = md_fixed_update,
                .max     = 1,
                .choices = (const char *[]) { "Display only", "Release" },
                .help    = "Display only: detector runs and counts. Release: takes pictures (Interval checks).",
            },
            {
                .name    = "Trigger",
                .priv    = &r6_md_cfg_trig,
                .max     = 1,
                .choices = (const char *[]) { "Motion", "Brightness" },
                .help    = "Motion: blocks that changed. Brightness: the area got brighter or darker.",
            },
            {
                .name    = "Area",
                .priv    = &r6_md_cfg_area,
                .max     = 3,
                .choices = (const char *[]) { "Small", "Medium", "Large", "Full" },
                .help    = "Where the detector looks: 1/8, 1/4, 1/2 of the width, or the whole image.",
            },
            {
                .name    = "Position",
                .priv    = &r6_md_cfg_pos,
                .max     = 4,
                .choices = (const char *[]) { "Center", "Left", "Right", "Top", "Bottom" },
                .help    = "Where the area sits (Full: ignored).",
            },
            {
                .name    = "Level",
                .priv    = &r6_md_cfg_level,
                .min     = 3,
                .max     = 30,
                .help    = "Luma change per block (lower = more sensitive). Noise raises it automatically.",
            },
            {
                .name    = "Confirm",
                .priv    = &r6_md_cfg_confirm,
                .min     = 1,
                .max     = 3,
                .help    = "Changed on this many analysed frames in a row (2 = fewer false triggers).",
            },
            {
                .name    = "Pictures",
                .priv    = &r6_md_cfg_count,
                .select  = md_fixed_select,
                .update  = md_fixed_update,
                .min     = 1,
                .max     = 50,
                .help    = "Pictures per run (Release).",
            },
            {
                .name    = "Wait after picture",
                .priv    = &r6_md_cfg_wait,
                .max     = COUNT(md_wait_s) - 1,
                .choices = md_wait_names,
                .help    = "Pause after each picture; then a new background (no catch-up).",
            },
            {
                .name    = "Start delay",
                .priv    = &r6_md_cfg_delay,
                .max     = COUNT(md_delay_s) - 1,
                .choices = md_delay_names,
                .help    = "Time from Start to the first background.",
            },
            {
                .name    = "Max duration",
                .priv    = &r6_md_cfg_maxdur,
                .max     = COUNT(md_maxdur_s) - 1,
                .choices = md_maxdur_names,
                .help    = "The run stops after this time. Canon's Screen off must be longer.",
            },
            {
                .name    = "Display",
                .priv    = &r6_md_cfg_disp,
                .max     = 1,
                .help    = "Area frame, level bar and state square under Canon's display (Global Draw).",
            },
            {
                .name   = "Detector",
                .update = md_det_update,
                .icon_type = IT_ALWAYS_ON,
                .help   = "FaceYuv frames per second, read time, noise.",
            },
            {
                .name   = "Camera",
                .update = md_cam_update,
                .icon_type = IT_ALWAYS_ON,
                .help   = "Release: Interval's camera check (M, Tv up to 30 s, single, MF).",
            },
            {
                .name   = "Last run",
                .update = md_last_update,
                .icon_type = IT_ALWAYS_ON,
                .help   = "Pictures (or triggers) and why the last run ended.",
            },
            MENU_EOL
        },
    },
};

static void md_init()
{
    menu_add("Shoot", md_menu, COUNT(md_menu));
}

INIT_FUNC("r6_motion", md_init);

#endif /* CONFIG_R6_MOTION */
