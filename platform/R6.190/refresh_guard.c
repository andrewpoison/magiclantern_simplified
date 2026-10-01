/*
 * R6.190 M7.2a: guard for ML's OSD refresh (src/bmp.c refresh_yuv_from_rgb).
 *
 * Why: ML runs XimrExe on Canon's XCM context 0 (XIMR_CONTEXT, consts.h) from
 * its own tasks (redraw_task, menu_redraw_flood, lvprobe "ON, refresh").
 * Canon's OSD flush, VMIX_TransferRectangleToVram (TRV, 0xE0260A16),
 * rebuilds that context in EVERY transfer without a lock: XCM_MakeContext
 * (bl at 0xE0260C16; it zeroes the context count [xcm+0xC] at 0xE017510E and
 * rewrites context 0 field by field, setSourceToContext 0xE0174A12) runs
 * before take_semaphore(winsys_sem, 0x1388) at 0xE0260C22, whose result is
 * ignored (movs r4,#0 at 0xE0260C26). winsys_sem only means "last frame
 * flipped": TRV sets [0x18424] = [0x18420] and [0x18428] = 0 (strd at
 * 0xE0260C66), the display callback decrements [0x18424] and gives the
 * semaphore when [0x18424] | [0x18428] == 0, then sets [0x18428] = 1
 * (0xE0260934..0xE026094A). The suspected EVF -> LCD hang (M7.1) is an ML
 * XimrExe overlapping such a rebuild (new geometry 1280x960 -> 900x600).
 *
 * What (ML side only, fail-safe: the guard can only suppress or delay ML's
 * own XimrExe; Canon's state is never written):
 *  1. finite take_semaphore(winsys_sem, 100) in bmp.c; on timeout no give, no
 *     XimrExe, ml_refresh_display_needed back to 1
 *  2. ml_refresh_display_needed is cleared BEFORE the conversion (both paths;
 *     a draw during the conversion sets it again, nothing is lost) and set
 *     back to 1 on every exit that showed nothing (take timeout, gate failure,
 *     piggyback not confirmed)
 *  3. Canon VMIX state gates, before the conversion and again right after the
 *     take (= right before XimrExe):
 *       MEM(0x1841C) == 0  pending: TRV's only entry gate ([0x18404+0x18] at
 *                          0xE0260A26); set by VMIX_SetPending 0xE0260E74
 *                          (called by GUI_SetDisplayType at 0xE0799D72),
 *                          cleared by TRV at 0xE0260CFA
 *       MEM(0x18418) == 0  output size change: VMIX_SetOutputSize sets it
 *                          (0xE026089A), TRV rebuilds the surfaces and clears
 *                          it (0xE0260CF8)
 *       MEM(MEM(0x14B2D8)+0xC) == 1  XCM context count; XCM_MakeContext sets
 *                          it to 0 first (0xE017510E), XCM_GetContext checks
 *                          it (0xE017527A); 1 in all M7.1 snapshots
 *       !(MEM(0x18424) != 0 && MEM(0x18428) == 0)  no Canon frame in flight
 *                          (see above); also catches a winsys_sem count > 1
 *                          after Canon's 5 s timeout
 *  4. piggyback: while Canon is active ML converts its indexed pixels into
 *     Canon's layer 0 but runs no XimrExe; Canon's TRV composes layer 0
 *     (XimrExe for every context, 0xE0260C2A..0xE0260C50; XimrExe 0xE01A2688
 *     itself does only dsb + the PL310 sync store, no layer clean, so Canon's
 *     call sees ML's CPU writes exactly like ML's own call).
 *     "Canon active" = within the activity window one of: the VMIX buffer
 *     index MEM(0x18414) changed (rsb #1 at 0xE0260CB4, after TRV's XimrExe
 *     loop; values 0/1 only, so ML's sampling can miss an even number of
 *     flips), pending was seen, or a frame in flight was seen (pending stays
 *     set for the whole TRV body). Window: 200 ms, but 1000 ms (about 15
 *     Canon flushes, LVP00002) while the display mask has the EVF bit 0x10 or
 *     is unknown, so that missed toggles cannot open ML's own XimrExe while
 *     Canon flushes in the EVF. The activity is sampled at every poll: the
 *     50 ms redraw_task loop, the start of every guarded refresh, right after
 *     the conversion (before the take), right after the take and at the end
 *     of every piggyback.
 *     Confirmation: two observed index changes within 500 ms after the
 *     conversion (= at least one complete Canon flush started after ML's
 *     writes); otherwise ml_refresh_display_needed = 1 again (re-arm) and the
 *     next refresh decides again (own XimrExe only once Canon is quiet for the
 *     whole window).
 *  5. settle: no ML XimrExe for 1000 ms after any change of the display mask
 *     MEM(MEM(0xEA40)+0x18), vrammgr_mode MEM(0xEA94), the GUI display type
 *     MEM(0x8D0C) (GUI_SetDisplayType 0xE0799D48: cmp [0x8D00+0xC] at
 *     0xE0799D4E, stored at 0xE0799DA6) or the XOC layer enable
 *     MEM(MEM(0x14B2E4)+4) (XOC_GetLayerEnable 0xE019E724), M7.3: or the
 *     XOC handle MEM(0x14B2E4) itself. M7.3: only 300 ms when a poll sees
 *     exactly the XOC enable change and nothing else, with the same handle,
 *     readable in this and the previous poll (approved by the user
 *     2026-10-01 for "only the XOC layer enable changed"; a handle swap or a
 *     handle turning invalid/valid is a VMIX re-init: 1000 ms; M7.2a camera
 *     test: 86 episodes > 500 ms, mostly this settle window, e.g. opening the
 *     ML menu). The
 *     window has its own start/length (t_settle, settle_len_ms; t_change is
 *     statistics only); a later change restarts it only if its own window
 *     ends later, so an XOC-only change never shortens a running 1 s window.
 *     During the window
 *     a piggyback conversion is still done while Canon is active (RAM writes
 *     into ML's layer only, no XimrExe), so Canon's flushes keep showing ML's
 *     pixels. A late detection only starts the window later (more skipping).
 *     Note: Canon MENU changes the XOC enable (0xFFFFFFFF -> 0xFFFFFFFD,
 *     M7.1), and ML's menu runs over Canon MENU (GUIMODE_ML_MENU 0x2003), so
 *     opening / closing the ML menu from LV starts a settle window too.
 *  6. at most 5 ML XimrExe per second while LV runs (ML lv, or Canon's
 *     PROP_LV_ACTION == 0 as seen by propvalues.c)
 *  7. log only ("would skip" counters after a successful take, and duty
 *     counters per poll): MEM(0x18438) WINSYS transfer request (set
 *     0xE07B8C88, cleared by the flush 0xE07B8C96), MEM(0x1843C) > 0
 *     draw-disable counter (++ at 0xE07B8D1E), MEM(0x18440) m_fDisableRefresh
 *     (checked by the flush 0xE07B8C8E), MEM(0x21A58) display type change
 *     sequence (BeginDisplayTypeChangeSequence 0xE078A77A, cleared 0xE078A7F0)
 *  8. counters, timestamps, a starvation tracker (episode = refresh requested
 *     and nothing shown; longest episode and a ring of the last 16 episodes
 *     > 500 ms with the OR of their skip reasons) and an overlap detector
 *     (diagnostic only: pending / size change / context count / buffer index
 *     re-read right after ML's XimrExe), all in RAM (r6_refresh_guard_stats()),
 *     shown by the LV probe. No files.
 * A try-lock (LDREX/STREX via __sync_lock_test_and_set, CPU only, never
 * waited on) serializes the guarded refreshes and the poll: a second ML
 * refresh while one runs is skipped ("busy") and retried by redraw_task.
 * M7.3: rgba_buffer_clear takes the same lock (r6g_clear_lock: try-lock,
 * msleep(10) steps, at most 20 ms from bmp.c), so the clear and the
 * conversion are single writers of ML's pixels in layer 0.
 *
 * Remaining gap (not closable from an ML task): Canon's TRV can start its
 * unlocked MakeContext between the last check (r6g_gate, after the take) and
 * the end of ML's XimrExe. That gap is XimrExe's duration (~1.5 ms, up to
 * 13 ms measured, LVP00003) plus any preemption of redraw_task (priority
 * 0x1E) in between (conversions took up to 167 ms instead of 17 ms, so
 * preemption of ~150 ms happens). The overlap detector and gate_exe_us_max
 * show how often that happens on the camera.
 *
 * Reads: RAM only (0x8D0C, 0xEA40 -> +0x18, 0xEA94, 0x18404..0x18444,
 * 0x21A58, 0x14B2D8 -> +0xC, 0x14B2E4 -> +4, 0x1355A4 memif copy). Canon
 * pointers are dereferenced only inside 0x1000..0x3FFFFFFF, or the uncached
 * alias 0x40001000..0x7FFFFFFF when Canon's memif RAM copy shows windows 4..7
 * equal to 0..3 (same rule as lvprobe.c). Timer: get_ms_clock/get_us_clock
 * (DIGIC timer 0xC0242014, existing M6 access; read more often).
 */

#include "dryos.h"
#include "bmp.h"
#include "propvalues.h"
#include "refresh_guard.h"

#ifdef CONFIG_R6_REFRESH_GUARD

extern struct semaphore * winsys_sem;                   /* stubs.S DATA_PTR 0x18408 */
extern int lv_action_raw_get(uint32_t * value, int * len);   /* propvalues.c */

#define RG_VMIX          0x18404             /* VMIX state, TRV 0xE0260A16 (ldr r6,=0x18404 at 0xE0260A24) */
#define RG_BUF_INDEX     (RG_VMIX + 0x10)    /* 0x18414 */
#define RG_SIZE_CHANGE   (RG_VMIX + 0x14)    /* 0x18418 */
#define RG_PENDING       (RG_VMIX + 0x18)    /* 0x1841C */
#define RG_CB_LEFT       (RG_VMIX + 0x20)    /* 0x18424 */
#define RG_FLIPPED       (RG_VMIX + 0x24)    /* 0x18428 */
#define RG_WINSYS        0x18430             /* WINSYS flush state (ldr =0x18430 at 0xE07B8C84/0xE07B8C8C) */
#define RG_WS_LOCK       (RG_WINSYS + 0x08)  /* 0x18438 */
#define RG_WS_DISDRAW    (RG_WINSYS + 0x0C)  /* 0x1843C */
#define RG_WS_DISREF     (RG_WINSYS + 0x10)  /* 0x18440 */
#define RG_DTC_SEQ       0x21A58             /* literal at 0xE078A7EC */
#define RG_GUI_DISPTYPE  0x8D0C              /* [0x8D00+0xC], literal at 0xE0799D4C */
#define RG_DISPDEV_PTR   0xEA40              /* [[0xEA40]+0x18] = display mask (0xE01694C8) */
#define RG_VRAMMGR_MODE  0xEA94
#define RG_XCM_TABLE     0x14B2D8            /* _pXCM (stubs.S) */
#define RG_XOC_TABLE     0x14B2E4            /* XOC handles, [xoc+4] = layer enable */
#define RG_MEMIF_WIN     0x1355A4            /* RAM copy of the static memif windows */
#define RG_MASK_EVF      0x10                /* display mask bit of the EVF output (M7.1: LCD 0x01, EVF 0x10) */

#define RG_TAKE_MS       100
#define RG_SETTLE_MS     1000
#define RG_SETTLE_XOC_MS 300                 /* XOC layer enable change only: approved 2026-10-01 */
#define RG_CH_XOC        (1 << 3)            /* change mask bit of the XOC layer enable (w[3]) */
#define RG_CH_XOC_H      (1 << 4)            /* change mask bit of the XOC handle MEM(0x14B2E4) (w[4]) */
#define RG_ACT_MS        200                 /* Canon activity window, panel */
#define RG_ACT_EVF_MS    1000                /* Canon activity window, EVF bit set or mask unknown */
#define RG_PIG_CONFIRM_MS 500
#define RG_RATE_MS       200                 /* 5 per second */
#define RG_IDLE_MS       1000                /* no refresh attempt this long: the episode ends */

#define RD(a) (*(volatile uint32_t *)(a))

static struct r6_refresh_guard_stats rg;

static volatile int rg_lock = 0;
static int rg_w_init = 0, rg_idx_init = 0;
static int rg_change_seen = 0, rg_act_seen = 0, rg_exe_seen = 0;
static uint32_t rg_last_idx = 0;
static int rg_pig_wait = 0, rg_pig_seen = 0, rg_pig_t = 0;
static uint32_t rg_pig_ref = 0;
static int rg_ep_last_call = 0;
static int rg_took = 0;                     /* winsys_sem taken in this refresh */
static uint64_t rg_take_t0_us = 0, rg_take_us = 0, rg_gate_us = 0, rg_exe_us = 0;
static uint32_t rg_gate_idx = 0;
static volatile int rg_clear_held = 0;      /* the lock is held by rgba_buffer_clear */

/* ---- guarded reads (same rule as lvprobe.c) ---- */
static int rg_uc_ok(void)
{
    if (!RD(RG_MEMIF_WIN + 0x2C)) return 0;
    for (int i = 0; i < 4; i++)
        if (RD(RG_MEMIF_WIN + 4 * (i + 4)) != RD(RG_MEMIF_WIN + 4 * i)) return 0;
    return 1;
}

static int rg_region(uint32_t a, int uc)
{
    if (a >= 0x1000 && a < 0x40000000) return 1;
    if (uc && a >= 0x40001000 && a < 0x80000000) return 2;
    return 0;
}

/* *v = MEM(base + off) if base is a plausible Canon pointer */
static int rg_field(uint32_t base, uint32_t off, uint32_t * v)
{
    uint32_t a = base + off;
    if (!base || (base & 3) || (off & 3) || a < base) return 0;
    int uc = rg_uc_ok();
    int r = rg_region(base, uc);
    if (!r || rg_region(a, uc) != r || rg_region(a + 3, uc) != r) return 0;
    *v = RD(a);
    return 1;
}

static uint32_t rg_field_or(uint32_t base, uint32_t off)
{
    uint32_t v;
    return rg_field(base, off, &v) ? v : 0xFFFFFFFF;
}

/* ms since *t; a timestamp in the future (clock glitch) is clamped to now,
 * so a window restarts once instead of lasting until the clock catches up */
static int rg_since(int * t, int now)
{
    int d = now - *t;
    if (d < 0) { *t = now; rg.clock_back++; d = 0; }
    return d;
}

static int rg_recent(int * t, int ms, int now)
{
    return rg_since(t, now) < ms;
}

static int rg_settling(int now)
{
    return rg_change_seen && rg_recent(&rg.t_settle, rg.settle_len_ms, now);
}

/* a watched value changed (mask = changed values): start or extend the
 * settle window; xoc_ok = the XOC handle was readable in this and the
 * previous poll. XOC enable only (same handle, readable) -> 300 ms,
 * anything else -> 1000 ms; a running window is never shortened */
static void rg_settle_start(uint32_t mask, int xoc_ok, int now)
{
    int xoc_only = (mask == RG_CH_XOC) && xoc_ok;
    int len = xoc_only ? RG_SETTLE_XOC_MS : RG_SETTLE_MS;
    if (xoc_only) rg.settle_xoc++;
    else if (mask == RG_CH_XOC) rg.settle_xoc_full++;
    if (rg_settling(now))
    {
        int left = (int) rg.settle_len_ms - rg_since(&rg.t_settle, now);
        if (left >= len) { rg.settle_keep++; return; }
    }
    rg.t_settle = now;
    rg.settle_len_ms = len;
}

/* DryOS semaphore handles are id << 1 (create_named_semaphore 0xE07D9EF0);
 * take_semaphore asserts (KerSem.c line 0x13A) on 0 or an odd handle
 * (0xE07D9FDE..0xE07D9FE4). Before VMIX_InitializeScreen ran, 0x18408 is
 * not a handle (QEMU: 7). */
static int rg_sem_ok(void)
{
    uint32_t h = (uint32_t) winsys_sem;
    return h && !(h & 1);
}

static int rg_in_lv(void)
{
    uint32_t v = 1;
    int len = 0;
    if (lv) return 1;
    return lv_action_raw_get(&v, &len) && len == 4 && v == 0;
}

/* EVF output active, or display mask unknown (conservative) */
static int rg_evf(void)
{
    return rg.w_mask == 0xFFFFFFFF || (rg.w_mask & RG_MASK_EVF);
}

/* ---- starvation episodes (guard lock held) ---- */
static void rg_ep_close(int t_end, uint32_t end)
{
    int d = t_end - rg.ep_start;
    uint32_t dur = d > 0 ? d : 0;
    if (dur > rg.starve_ms_max)
    {
        rg.starve_ms_max = dur;
        rg.starve_max_mask = rg.ep_mask;
        rg.starve_max_t = rg.ep_start;
    }
    if (dur > R6G_STARVE_LOG_MS)
    {
        struct r6g_episode * e = &rg.ring[rg.starve_n % R6G_RING];
        e->t_start = rg.ep_start; e->dur_ms = dur; e->mask = rg.ep_mask; e->end = end;
        rg.starve_n++;
    }
    rg.ep_active = 0;
}

/* a refresh attempt: starts an episode unless one runs */
static void rg_ep_touch(int now)
{
    if (rg.ep_active && rg_since(&rg_ep_last_call, now) > RG_IDLE_MS)
        rg_ep_close(rg_ep_last_call, 3);
    if (!rg.ep_active)
    {
        rg.ep_active = 1;
        rg.ep_start = now;
        rg.ep_mask = 0;
    }
    rg_ep_last_call = now;
}

static void rg_reason(uint32_t bits)
{
    rg.ep_mask |= bits;
}

/* something was shown: end 1 = ML XimrExe, 2 = piggyback confirmed */
static void rg_shown(int now, uint32_t end)
{
    rg.t_last_shown = now;
    if (rg.ep_active) rg_ep_close(now, end);
}

static void rg_act(int now)
{
    rg.t_act = now;
    rg_act_seen = 1;
}

static int rg_canon_active(int now)
{
    rg.act_win_ms = rg_evf() ? RG_ACT_EVF_MS : RG_ACT_MS;
    return rg_act_seen && rg_recent(&rg.t_act, rg.act_win_ms, now);
}

/* ---- change detection, Canon activity, piggyback confirmation, duty (guard lock held) ---- */
static void rg_poll_locked(int now)
{
    rg.polls++;

    uint32_t w[5], xe = 0;
    w[0] = rg_field_or(RD(RG_DISPDEV_PTR), 0x18);
    w[1] = RD(RG_VRAMMGR_MODE);
    w[2] = RD(RG_GUI_DISPTYPE);
    w[4] = RD(RG_XOC_TABLE);
    /* validity kept apart from the value: 0xFFFFFFFF is also a real enable value */
    int xoc_ok = rg_field(w[4], 4, &xe);
    w[3] = xoc_ok ? xe : 0xFFFFFFFF;
    uint32_t old[5] = { rg.w_mask, rg.w_vm, rg.w_disptype, rg.w_xoc, rg.w_xoc_h };
    uint32_t mask = 0;
    for (int i = 0; i < 5; i++)
        if (w[i] != old[i]) mask |= 1 << i;
    if (rg_w_init && mask)
    {
        rg.changes++;
        rg.change_mask = mask;
        rg.change_mask_all |= mask;
        rg.t_change = now;
        rg_settle_start(mask, xoc_ok && rg.w_xoc_ok, now);
        rg_change_seen = 1;
    }
    rg_w_init = 1;
    rg.w_mask = w[0]; rg.w_vm = w[1]; rg.w_disptype = w[2]; rg.w_xoc = w[3]; rg.w_xoc_h = w[4];
    rg.w_xoc_ok = xoc_ok;

    uint32_t idx = RD(RG_BUF_INDEX);
    if (rg_idx_init && idx != rg_last_idx)
    {
        rg.idx_changes++;
        rg.t_idx_change = now;
        rg.act_idx++;
        rg_act(now);
    }
    rg_idx_init = 1;
    rg_last_idx = idx;

    /* Canon activity and duty counters (RAM reads) */
    uint32_t pend = RD(RG_PENDING), img = RD(RG_CB_LEFT) && !RD(RG_FLIPPED);
    if (pend) { rg.d_pend++; rg.act_pend++; rg_act(now); }
    if (img)  { rg.d_img++;  rg.act_img++;  rg_act(now); }
    if (RD(RG_SIZE_CHANGE))            rg.d_size++;
    if (RD(RG_WS_LOCK))                rg.d_lock++;
    if ((int) RD(RG_WS_DISDRAW) > 0)   rg.d_disdraw++;
    if (RD(RG_WS_DISREF))              rg.d_disref++;
    if (RD(RG_DTC_SEQ))                rg.d_seq++;
    if (w[0] != 0xFFFFFFFF && (w[0] & RG_MASK_EVF)) rg.d_evf++;

    if (rg_pig_wait)
    {
        if (idx != rg_pig_ref)
        {
            rg_pig_ref = idx;
            if (++rg_pig_seen >= 2)
            {
                rg_pig_wait = 0;
                rg.pig_ok++;
                rg_shown(now, 2);
            }
        }
        else if (!rg_recent(&rg_pig_t, RG_PIG_CONFIRM_MS, now))
        {
            /* Canon's flushes not confirmed: decide again (own XimrExe only
             * once Canon is quiet for the whole activity window) */
            rg_pig_wait = 0;
            rg.pig_rearm++;
            rg_reason(R6G_R_REARM);
            ml_refresh_display_needed = 1;
        }
    }

    /* ML stopped asking (display off, shutdown): close the episode */
    if (rg.ep_active && rg_since(&rg_ep_last_call, now) > RG_IDLE_MS && !rg_pig_wait)
        rg_ep_close(rg_ep_last_call, 3);
}

/* Canon VMIX state gates (3.); counts each failing reason, returns R6G_R_* bits */
static uint32_t rg_canon_busy(int now)
{
    uint32_t r = 0;
    if (RD(RG_PENDING))     { rg.pend++; r |= R6G_R_PEND; rg.act_pend++; rg_act(now); }
    if (RD(RG_SIZE_CHANGE)) { rg.size++; r |= R6G_R_SIZE; }
    uint32_t n = 0;
    if (!rg_field(RD(RG_XCM_TABLE), 0xC, &n) || n != 1) { rg.ctx++; r |= R6G_R_CTX; }
    if (RD(RG_CB_LEFT) && !RD(RG_FLIPPED)) { rg.img++; r |= R6G_R_IMG; rg.act_img++; rg_act(now); }
    return r;
}

static int rg_rate_limited(int now)
{
    return rg_exe_seen && rg_in_lv() && rg_recent(&rg.t_exe_start, RG_RATE_MS, now);
}

/* ---- API for src/bmp.c ---- */
int r6g_begin(void)
{
    rg.calls++;
    if (__sync_lock_test_and_set(&rg_lock, 1))
    {
        rg.busy++;
        if (rg_clear_held) rg.busy_clear++;
        return R6G_SKIP;
    }

    int now = get_ms_clock();
    rg_poll_locked(now);
    rg_ep_touch(now);
    rg_took = 0;

    int mode = R6G_EXE;
    uint32_t r;
    int act = rg_canon_active(now);
    if (rg_settling(now))
    {
        if (act) { rg.settle_pig++; rg.pig++; rg_reason(R6G_R_SETTLE_PIG | R6G_R_PIG); mode = R6G_PIGGYBACK; }
        else     { rg.settle++; rg_reason(R6G_R_SETTLE); mode = R6G_SKIP; }
    }
    else if (act)                    { rg.pig++; rg_reason(R6G_R_PIG); mode = R6G_PIGGYBACK; }
    else if (rg_rate_limited(now))   { rg.rate++; rg_reason(R6G_R_RATE); mode = R6G_SKIP; }
    else if ((r = rg_canon_busy(now))) { rg_reason(r); mode = R6G_SKIP; }
    else if (!rg_sem_ok())           { rg.no_sem++; rg_reason(R6G_R_NOSEM); mode = R6G_SKIP; }

    if (mode == R6G_SKIP)
        __sync_lock_release(&rg_lock);
    return mode;
}

/* after the conversion, before the take (EXE path only) */
int r6g_pretake(void)
{
    int now = get_ms_clock();
    rg_poll_locked(now);
    if (rg_settling(now))
    {
        rg.settle++; rg_reason(R6G_R_SETTLE);
        return R6G_GATE_SKIP;
    }
    if (rg_canon_active(now))
    {
        rg.pig_late++; rg.pig++; rg_reason(R6G_R_PIG_LATE | R6G_R_PIG);
        return R6G_GATE_PIG;
    }
    rg_take_t0_us = get_us_clock();
    return R6G_GATE_OK;
}

/* after a successful take, right before XimrExe */
int r6g_gate(void)
{
    rg_take_us = get_us_clock();
    rg_took = 1;
    uint32_t wait = (uint32_t) (rg_take_us - rg_take_t0_us);
    rg.wait_us_last = wait;
    if (wait > rg.wait_us_max) rg.wait_us_max = wait;

    int now = get_ms_clock();
    rg_poll_locked(now);

    uint32_t r = rg_canon_busy(now);     /* counts, and marks pending / frame in flight as activity */
    int res = R6G_GATE_OK;
    if (rg_settling(now))           { rg.settle++; rg_reason(R6G_R_SETTLE); res = R6G_GATE_SKIP; }
    else if (rg_canon_active(now))  { rg.pig_late++; rg.pig++; rg_reason(R6G_R_PIG_LATE | R6G_R_PIG); res = R6G_GATE_PIG; }
    else if (rg_rate_limited(now))  { rg.rate++; rg_reason(R6G_R_RATE); res = R6G_GATE_SKIP; }
    else if (r)                     { rg_reason(r); res = R6G_GATE_SKIP; }

    /* log only (7.) */
    if (RD(RG_WS_LOCK))                rg.would_lock++;
    if ((int) RD(RG_WS_DISDRAW) > 0)   rg.would_disdraw++;
    if (RD(RG_WS_DISREF))              rg.would_disref++;
    if (RD(RG_DTC_SEQ))                rg.would_seq++;

    if (res != R6G_GATE_OK) rg.post_skip++;
    rg_gate_idx = rg_last_idx;
    rg_gate_us = get_us_clock();
    return res;
}

void r6g_exe_begin(void)
{
    rg_exe_us = get_us_clock();
    uint32_t d = (uint32_t) (rg_exe_us - rg_gate_us);
    if (d > rg.gate_exe_us_max) rg.gate_exe_us_max = d;
    rg.t_exe_start = get_ms_clock();
    rg_exe_seen = 1;
    rg.exe_active = 1;
    if (rg.w_mask != 0xFFFFFFFF && (rg.w_mask & RG_MASK_EVF)) rg.exe_evf++;
}

void r6g_exe_end(void)
{
    uint32_t d = (uint32_t) (get_us_clock() - rg_exe_us);
    rg.ximr_us_last = d;
    if (d > rg.ximr_us_max) rg.ximr_us_max = d;
    rg.t_exe_end = get_ms_clock();
    rg.exe_active = 0;

    /* overlap detector (diagnostic only): did Canon start a transfer while
     * ML's XimrExe ran? */
    uint32_t m = 0, n = 0;
    if (RD(RG_PENDING)) m |= 1;
    if (RD(RG_SIZE_CHANGE)) m |= 2;
    if (!rg_field(RD(RG_XCM_TABLE), 0xC, &n) || n != 1) m |= 4;
    if (RD(RG_BUF_INDEX) != rg_gate_idx) m |= 8;
    if (m)
    {
        rg.overlap++;
        rg.overlap_mask = m;
        rg.t_overlap = rg.t_exe_end;
    }
}

void r6g_end(int outcome)
{
    int now = get_ms_clock();
    if (rg_took && outcome != R6G_END_TIMEOUT)
    {
        uint32_t hold = (uint32_t) (get_us_clock() - rg_take_us);
        rg.hold_us_last = hold;
        if (hold > rg.hold_us_max) rg.hold_us_max = hold;
    }
    rg_took = 0;

    if (outcome == R6G_END_PIGGYBACK)
    {
        rg_poll_locked(now);                /* one more sample of Canon's state */
        rg_pig_ref = RD(RG_BUF_INDEX);
        rg_pig_seen = 0;
        rg_pig_t = now;
        rg_pig_wait = 1;
    }
    else if (outcome == R6G_END_TIMEOUT)
    {
        uint32_t wait = (uint32_t) (get_us_clock() - rg_take_t0_us);
        rg.take_timeout++;
        rg.wait_to_us_last = wait;
        rg.wait_us_last = wait;
        if (wait > rg.wait_us_max) rg.wait_us_max = wait;
        rg_reason(R6G_R_TIMEOUT);
        ml_refresh_display_needed = 1;      /* nothing shown: retry */
    }
    else if (outcome == R6G_END_GATE)
    {
        ml_refresh_display_needed = 1;      /* nothing shown: retry */
    }
    else /* R6G_END_OK */
    {
        rg.ok++;
        rg_pig_wait = 0;                    /* ML's own XimrExe has shown everything */
        rg_shown(now, 1);
    }
    __sync_lock_release(&rg_lock);
}

void r6g_poll(void)
{
    if (__sync_lock_test_and_set(&rg_lock, 1))
        return;             /* a guarded refresh is running and polls itself */
    rg_poll_locked(get_ms_clock());
    __sync_lock_release(&rg_lock);
}

/* M7.3: single writer for rgba_buffer_clear (see refresh_guard.h) */
int r6g_clear_lock(int timeout_ms)
{
    int waited = 0;
    while (__sync_lock_test_and_set(&rg_lock, 1))
    {
        if (waited >= timeout_ms)
        {
            rg.clear_timeouts++;
            return 0;
        }
        msleep(10);
        waited += 10;
    }
    rg_clear_held = 1;
    rg.clear_locks++;
    if (waited)
    {
        rg.clear_waits++;
        if ((uint32_t) waited > rg.clear_wait_ms_max) rg.clear_wait_ms_max = waited;
    }
    return 1;
}

void r6g_clear_unlock(void)
{
    rg_clear_held = 0;
    __sync_lock_release(&rg_lock);
}

const struct r6_refresh_guard_stats * r6_refresh_guard_stats(void)
{
    return &rg;
}

uint32_t r6_refresh_guard_starve_ms(void)
{
    if (!rg.ep_active) return 0;
    int d = get_ms_clock() - rg.ep_start;
    return d > 0 ? d : 0;
}

#endif /* CONFIG_R6_REFRESH_GUARD */
