/*
 * R6.190 M7.2a: guard for ML's OSD refresh, see refresh_guard.c.
 * Used by src/bmp.c and lvprobe.c (counters), both only under
 * CONFIG_R6_REFRESH_GUARD (platform/R6.190/internals.h).
 */
#ifndef _r6_refresh_guard_h_
#define _r6_refresh_guard_h_

#include <stdint.h>

/* r6g_begin(): what refresh_yuv_from_rgb may do now */
#define R6G_SKIP       0   /* nothing (no conversion, no XimrExe); ml_refresh_display_needed stays 1 */
#define R6G_PIGGYBACK  1   /* conversion only: Canon is flushing layer 0 itself */
#define R6G_EXE        2   /* conversion, r6g_pretake(), take, r6g_gate(), XimrExe */

/* r6g_gate() result (after a successful take) */
#define R6G_GATE_SKIP  0   /* Canon state gate failed: give, r6g_end(R6G_END_GATE) */
#define R6G_GATE_PIG   1   /* Canon started flushing during the conversion: give, r6g_end(R6G_END_PIGGYBACK) */
#define R6G_GATE_OK    2   /* XimrExe allowed */

/* r6g_end() outcome; ml_refresh_display_needed was cleared before the
 * conversion: TIMEOUT and GATE set it back to 1, PIGGYBACK sets it back to 1
 * if Canon's flushes are not confirmed within 500 ms, OK leaves it alone */
#define R6G_END_PIGGYBACK 0
#define R6G_END_TIMEOUT   1   /* take_semaphore(winsys_sem, 100) failed: no give */
#define R6G_END_GATE      2   /* r6g_gate() failed: caller gave winsys_sem back */
#define R6G_END_OK        3   /* XimrExe done, winsys_sem given back */

/* every call of r6g_begin() that does not return R6G_SKIP must be followed
 * by exactly one r6g_end() (the guard holds its own try-lock in between) */
int  r6g_begin(void);
int  r6g_pretake(void);       /* after the conversion, right before the take: 1 = take, 0 = piggyback */
int  r6g_gate(void);          /* after a successful take: R6G_GATE_* */
void r6g_exe_begin(void);     /* right before XimrExe */
void r6g_exe_end(void);       /* right after XimrExe, before give_semaphore */
void r6g_end(int outcome);

/* redraw_task loop (every ~50 ms): change detection, Canon activity,
 * piggyback confirmation, duty counters */
void r6g_poll(void);

/* skip / delay reasons, OR-ed per starvation episode */
#define R6G_R_SETTLE     0x0001
#define R6G_R_RATE       0x0002
#define R6G_R_PEND       0x0004   /* enforced gate */
#define R6G_R_SIZE       0x0008
#define R6G_R_CTX        0x0010
#define R6G_R_IMG        0x0020
#define R6G_R_NOSEM      0x0040
#define R6G_R_TIMEOUT    0x0080
#define R6G_R_PIG        0x0100   /* piggyback conversion, waiting for Canon's flushes */
#define R6G_R_SETTLE_PIG 0x0200   /* piggyback during the settle window */
#define R6G_R_PIG_LATE   0x0400   /* Canon became active during ML's conversion */
#define R6G_R_REARM      0x0800   /* piggyback not confirmed */

/* starvation episode: from the first refresh attempt after something was
 * last shown to the next ML XimrExe (end 1) or confirmed piggyback (end 2);
 * end 3 = no refresh attempt for 1 s (ML stopped asking, e.g. display off) */
struct r6g_episode { int t_start; uint32_t dur_ms; uint32_t mask; uint32_t end; };
#define R6G_RING 16
#define R6G_STARVE_LOG_MS 500     /* episodes longer than this go to the ring */

struct r6_refresh_guard_stats
{
    /* refresh_yuv_from_rgb */
    uint32_t calls;          /* r6g_begin() calls */
    uint32_t ok;             /* ML XimrExe executed */
    uint32_t take_timeout;   /* take_semaphore(winsys_sem, 100) != 0 */
    uint32_t busy;           /* another ML refresh holds the guard lock */
    uint32_t no_sem;         /* winsys_sem not a valid handle */
    /* skip reasons (enforced) */
    uint32_t pend;           /* MEM(0x1841C) != 0 at a gate: Canon transfer pending */
    uint32_t size;           /* MEM(0x18418) != 0: VMIX output size change */
    uint32_t ctx;            /* XCM pointer invalid or context count != 1 */
    uint32_t img;            /* MEM(0x18424) != 0 && MEM(0x18428) == 0 at a gate: Canon frame in flight */
    uint32_t settle;         /* < 1000 ms after a display change, Canon quiet: nothing done */
    uint32_t rate;           /* < 200 ms after the last ML XimrExe (LV) */
    uint32_t pig_late;       /* Canon became active between r6g_begin and the XimrExe decision */
    uint32_t post_skip;      /* gate skips decided after the take (subset of the above) */
    /* piggyback */
    uint32_t pig;            /* conversions without ML XimrExe (Canon active) */
    uint32_t settle_pig;     /* of these: during the settle window */
    uint32_t pig_ok;         /* confirmed: Canon flushed twice after the conversion */
    uint32_t pig_rearm;      /* not confirmed within 500 ms: ml_refresh_display_needed = 1 again */
    /* Canon activity (index change, pending or frame in flight seen) */
    int      t_act;          /* ms, last observation */
    uint32_t act_idx, act_pend, act_img;        /* observations per kind */
    uint32_t act_win_ms;     /* window used last: 200 (panel) or 1000 (EVF bit 0x10 / mask unknown) */
    /* log-only flags, counted after a successful take ("would skip") */
    uint32_t would_lock;     /* MEM(0x18438) != 0 */
    uint32_t would_disdraw;  /* (int) MEM(0x1843C) > 0 */
    uint32_t would_disref;   /* MEM(0x18440) != 0 */
    uint32_t would_seq;      /* MEM(0x21A58) != 0 */
    /* duty: polls (every ~50 ms and at each refresh step) with the flag set */
    uint32_t polls;
    uint32_t d_pend, d_size, d_img, d_lock, d_disdraw, d_disref, d_seq, d_evf;
    /* change detection */
    uint32_t changes;        /* watched value changes (settle triggers) */
    uint32_t change_mask;    /* last change: bit0 disp mask, bit1 vrammgr_mode, bit2 GUI disp type, bit3 XOC enable */
    uint32_t change_mask_all;/* OR of all changes */
    int      t_change;       /* ms */
    uint32_t idx_changes;    /* VMIX buffer index changes seen */
    int      t_idx_change;   /* ms */
    uint32_t w_mask, w_vm, w_disptype, w_xoc;   /* last watched values (0xFFFFFFFF = pointer invalid) */
    /* ML XimrExe */
    int      t_exe_start, t_exe_end;            /* ms, last ML XimrExe */
    uint32_t exe_active;     /* 1 while ML is inside XimrExe */
    uint32_t exe_evf;        /* ML XimrExe executed while the display mask had the EVF bit (0x10) */
    uint32_t hold_us_last, hold_us_max;         /* winsys_sem held by ML (take -> give) */
    uint32_t wait_us_last, wait_us_max;         /* take_semaphore wait, successful or timed out */
    uint32_t wait_to_us_last;                   /* wait of the last timed-out take */
    uint32_t ximr_us_last, ximr_us_max;         /* ML XimrExe duration */
    uint32_t gate_exe_us_max;                   /* last gate check -> XimrExe start (preemption) */
    /* overlap detector (diagnostic only): right after ML's XimrExe, before the
     * give, pending / size change / context count / buffer index differ from
     * what the last gate saw */
    uint32_t overlap;
    uint32_t overlap_mask;   /* last: bit0 pending, bit1 size, bit2 ctx count, bit3 buffer index */
    int      t_overlap;
    /* clock: a stored timestamp in the future was clamped to now */
    uint32_t clock_back;
    /* starvation */
    int      t_last_shown;   /* ms, last ML XimrExe or confirmed piggyback */
    uint32_t ep_active;      /* 1 = refresh requested, nothing shown since ep_start */
    int      ep_start;
    uint32_t ep_mask;        /* R6G_R_* seen in the current episode */
    uint32_t starve_ms_max;
    uint32_t starve_max_mask;
    int      starve_max_t;   /* start of the longest episode */
    uint32_t starve_n;       /* episodes > R6G_STARVE_LOG_MS ms; ring[(starve_n - 1) % R6G_RING] is the last */
    struct r6g_episode ring[R6G_RING];
};

const struct r6_refresh_guard_stats * r6_refresh_guard_stats(void);
/* current episode length in ms (0 if none) */
uint32_t r6_refresh_guard_starve_ms(void);

#endif
