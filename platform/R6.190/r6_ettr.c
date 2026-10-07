/*
 * R6.190 M11.0: Auto-ETTR advisor, ML menu Shoot > "R6 ETTR" (docs/m11_plan.md sections 1, 2.1 .. 2.10, 3.1, 5.2, 6,
 * 7.1, 8.1 and the user's answers of 2026-10-07: E2 Tor 0e reads, E7 target selectable, E8 longest Tv 30 s, E9 hints
 * only).
 *
 * What: measures Canon's LV display histogram per channel R, G, B (not Y), shows the Tv that would put the brightest
 * channel into the target band ("Display only", in the ML menu and as one line in the LV), and logs the measurement for
 * the M11.0 camera test. It NEVER writes a property and never presses: no Tor 1 state is touched. From r6_propw.c only
 * the read-only _r6pw_state(), _r6pw_ring() and _r6pw_ring_n() are used (gate_m110_ro.sh). No Canon call, no MMIO, no
 * write to Canon RAM, no file (lvprobe.c writes ETTnnnnn.LOG through ML's FIO wrappers).
 *
 * Reads (all RAM reads, every pointer checked; approvals: Canon RAM since M7.0, FaceYuv 2026-10-01, the sheets Tor 0
 * 2026-10-04, Tor 0e = E2 2026-10-07): Canon's histogram DISP set and the newest RandomData record through
 * r6_overlays.c r6h_read_canon (the box's reader and rules), the CC settings sheet p = [[0x10AB8]+0x34FC] (AE, Tv, Av,
 * ISO, ExpSim, HTP, ALO, anti-flicker, drive, Canon's Tv range; r6_fy_ram_ok), [0x1427F8] through [0x1778C+0x18] ==
 * 0x1427C4 (log only), FaceYuv (2 x 2, second-newest frame, only while the Expo log is ON). One more ML handler on
 * PROP_SHUTTER, an id ML registers anyway (r6_interval.c, r6_propw.c, src/lens.c): copy only.
 *
 * Measurement (plan 2.1 / 2.2), 4 per second while Mode is not Off and Canon's LV runs, in r6_hist_task (r6_overlays.c,
 * a task beside r6_iv_task): one pass per channel from bin 255 down gives P_c (the highest level with more than
 * q x numPix pixels at or above it, q = Highlight ignore), the counts >= 224 / 240 / 248 / 252 and in bin 255, the
 * median, 5 % and the mean; H = max(P_R, P_G, P_B), s = max share >= 248, b = max share in bin 255 (ppm of numPix).
 * A read counts only with the RandomData binding of plan 2.1 (rd[0x5EC] before and after == the copied set; torn reads
 * and a record that changed during the read are read again at once, at most 3 reads; an unbound read is retried on the
 * next tick, at most twice), rd[0x5FC] = 1, ExpSim (ML and [p+0x548]) 1, no magnify, a known numPix, no LV idle.
 *
 * Freshness (plan 2.7): after a change of Tv (every PROP_SHUTTER delivery), ISO, Av, AE, drive, HTP, ALO,
 * anti-flicker, a picture (counter / file number), numPix, the frame rate class, or a gap of 2 s without a good read, a
 * measurement counts only once FrameNum >= F_ref + 60, >= 1 s passed and rd[0x600] >= its value at F_ref + 2 (F_ref:
 * the first read after the change). rd+0x424 is only logged (not confirmed yet). Calm window (2.7 / 2.8): the newest
 * >= 3 fresh measurements over >= 400 ms with max - min(H) <= 4, trend <= 2, delta s <= max(0.05 %, 20 %), delta Y mean
 * <= 2; the decision uses the window maxima of H, s, b.
 *
 * Decision (2.3 .. 2.5, E7): black (H < 8, Y mean < 4) / foot (H < 32): +3 EV; under: "wide" (more than 1 EV to Z_o by
 * the sRGB start curve) aims at 200 with at most +4 EV, "near" aims at Z_o with at most +1 EV, thirds rounded down;
 * target band lo .. Z_o: ok; close (Z_o < H <= 247): -1/3; stau (H >= 248, bin 255 <= q): -1/3, H >= 252: -2/3; hard
 * (bin 255 > q): -max(1/3, min(3, log2(1 + p^2))), p = s in %, thirds rounded up. Targets: JPEG safe 228 .. 244 (Z_o =
 * 248 - 4), Tight 228 .. 247, Reserve 210 .. 232. Limits (2.4, E8, E9): the user's longest / shortest Tv, Tor 1's window
 * 1/8000 .. 30 s, Canon's cached range [p+0x114] / [p+0x116], and the simulation limit G as Tv - Sv: 2 s at ISO 100
 * (M10.2), Sv = max(Sv, ISO 100); 1/3 grid, never B; a clamp shows the rest ("Limit 1/15: +1 1/3 EV missing").
 * E9: HTP, ALO, ISO L, an uncalibrated picture style (in M11.0 every one) and anti-flicker with Tv shorter than 1/100
 * are hints, never a refusal. No proposal while R6 Interval / R6 Motion runs, while the Tv write path is armed / busy /
 * open / has a restore pending, outside M, with Auto ISO, or with a reason above.
 *
 * Read-only traces (plan 6.2): ring raw = 32 ticks of 50 ms after every Tv delivery (dial or ML, the source from
 * r6_propw.c's write ring), also during an R6 Interval bracket; ring bk = r6_ettr_trace() from r6_interval.c iv_event
 * (right after each member's confirmed write and right before its SW1; cheap: one histogram read, no sleep, no
 * drawing, no card I/O); ring m = every second advisor measurement; ring fy = FaceYuv 2 x 2 counts once per second while
 * the Expo log is ON. ETTnnnnn.LOG (lvprobe.c) prints them with the bins and the Tor 0e fields.
 *
 * Settings saved in ML's config (Mode default Off). Integer arithmetic only, no division by a constant that would need
 * a reciprocal word (tables, shifts, et_div with a variable divisor).
 */

#include "dryos.h"
#include "bmp.h"
#include "menu.h"
#include "config.h"
#include "property.h"
#include "propvalues.h"
#include "lens.h"
#include "fio-ml.h"
#include "r6_overlays.h"
#include "r6_faceyuv.h"
#include "r6_interval.h"
#include "r6_propw.h"
#include "r6_ettr.h"

#ifdef CONFIG_R6_ETTR

#if !defined(FEATURE_R6_CANON_HISTOGRAM) || !defined(CONFIG_R6_INTERVAL) || !defined(CONFIG_R6_PROPW_TV) \
 || !defined(CONFIG_R6_GRID)
#error "R6 ETTR needs the Canon histogram reader (r6_overlays.c), R6 Interval, the Tv write path's read-only state and the grid's display path (LV line)"
#endif
#ifdef CONFIG_PROP_REQUEST_CHANGE
#error "R6 M11.0: property writes stay off"
#endif

extern int lv_action_raw_get(uint32_t * value, int * len);   /* propvalues.c */
#ifdef FEATURE_LV_BUFFER_PROBE
extern int lvprobe_expo_on(void);                            /* lvprobe.c: Debug > LV probe > Expo log */
#endif

#define RD(a) (*(volatile uint32_t *)(a))

/* ---- limits and timings (plan 2.3 / 2.4 / 2.7 / 2.8; starting values, M11.0 measures them) ---- */
#define ET_T_ML          1000      /* freshness: >= this long after the change */
#define ET_L_ML            60      /* ... and >= this many frames (FrameNum) */
#define ET_AE_ML            2      /* ... and rd[0x600] (Canon's AE counter, ~8 Hz) advanced by this */
#define ET_GAP_MS        2000      /* no good read this long: the window and the freshness start again */
#define ET_KEEP_MS       1500      /* a read failure keeps the last decision this long */
#define ET_WIN_MIN          3      /* calm window: >= 3 measurements ... */
#define ET_WIN_SPAN_MS    400      /* ... over >= 400 ms (2.8) */
#define ET_WIN_DH           4      /* max - min(H) */
#define ET_WIN_TREND        2      /* |H newest - H oldest| */
#define ET_WIN_DS_PPM     500      /* delta s <= max(0.05 %, 20 % of the larger) */
#define ET_WIN_DY10        20      /* delta Y mean (x 10) */
#define ET_FPS_MS         500      /* frame rate over >= 0.5 s */
#define ET_FPS_IDLE10     120      /* < 12 frames / s: LV idle (X6) */
#define ET_FPS_30_10      450      /* class 1 below, 2 above */
#define ET_M608_IDLE     6912      /* rd[0x608] < -9 EV (9 x 768): LV idle (X6) */
#define ET_RETRY            2      /* an unbound / torn advisor read: again on the next tick, at most twice */
#define ET_FY_MS         1000      /* FaceYuv 2 x 2 once per second (Expo log ON) */
#define ET_FY_READ_US   40000      /* FaceYuv read longer than this: discarded (K_code 1.4) */
#define ET_G_ISO100     (-768)     /* simulation limit Tv - Sv at ISO 100: 2 s (M10.2, plan 2.4) */
#define ET_SV_ISO100     3840      /* Sv of ISO 100 in Code768 (5 x 768) */
#define ET_TV_1_100      5120      /* 1/100 s (0x1400) */
#define ET_TV_MAX_ANY   10752      /* 1/16000 s: the shortest Tv R6 Interval decodes (r6_interval.c R6IV_TV_EV768_MAX) */
#define ET_GRID           256      /* 1/3 stop */
#define ET_BLACK_H          8
#define ET_BLACK_Y10       40
#define ET_FOOT_H          32
#define ET_STAU_H         248
#define ET_STAU2_H        252
#define ET_WIDE_AIM       200
#define ET_WIDE_EV        768      /* "wide" if more than 1 EV to Z_o */
#define ET_UP_WIDE         12      /* step limits in thirds: +4 EV wide, +3 EV foot / black, +1 EV near */
#define ET_UP_FOOT          9
#define ET_UP_NEAR          3

/* ---- Canon RAM (read only) ---- */
#define ET_RANGE_PTR    (0x1778C + 0x18)   /* E2: must hold ET_RANGE_OBJ */
#define ET_RANGE_OBJ    0x1427C4           /* 0x80050012 copy object (R_rom.md 1: literal 0xE08A4552) */
#define ET_RANGE_WORD   0x1427F8           /* = ET_RANGE_OBJ + 0x34: bit 0 sim, bit 1 no sim, bit 4 out of range */
#define ET_P_AE         0x008
#define ET_P_RANGE_W    0x114              /* s16 fastest (Canon's cached dial limit) | s16 slowest << 16 (K36) */
#define ET_P_ALO        0x0A8              /* byte 0: ALO effective (3 = off, K17) */
#define ET_P_DRIVE      0x4C0
#define ET_P_FLICK      0x4E4
#define ET_P_HTP        0x4FC
#define ET_P_ISO        0x510              /* low 16 bits: Sv x 768, 0 = Auto */
#define ET_P_EXPSIM     0x548
#define ET_P_TVAV_W     0x61C              /* s16 Av | s16 Tv << 16 */

/* ---- settings (saved; Mode default Off) ---- */
static CONFIG_INT("r6.ettr.mode",     et_mode, 0);       /* 0 Off, 1 Display only (Apply comes with M11.1) */
static CONFIG_INT("r6.ettr.target",   et_target, 0);     /* E7: 0 JPEG safe, 1 Tight, 2 Reserve */
static CONFIG_INT("r6.ettr.q",        et_q, 4);          /* Highlight ignore, index into et_q_ppm (4 = 0.1 %) */
static CONFIG_INT("r6.ettr.longest",  et_long, 0);       /* E8: index into et_tv_names (0 = 30 s) */
static CONFIG_INT("r6.ettr.shortest", et_short, 54);     /* 54 = 1/8000 */
static CONFIG_INT("r6.ettr.lv",       et_lv, 1);         /* Show in LV */

static const uint16_t et_q_ppm[] = { 0, 100, 200, 500, 1000, 2000, 5000, 10000, 20000 };
static const char * et_q_names[] = { "0", "0.01%", "0.02%", "0.05%", "0.1%", "0.2%", "0.5%", "1%", "2%" };
typedef char et_q_complete[COUNT(et_q_ppm) == COUNT(et_q_names) ? 1 : -1];
/* E7: band lower end and Z_o per target (JPEG safe: Z_o = 248 - margin 4, the margin M11.0 measures) */
static const uint8_t et_band_lo[3] = { 228, 228, 210 };
static const uint8_t et_band_zo[3] = { 244, 247, 232 };
/* Tv names on the 1/3 grid, 30 s (Code768 -3840) .. 1/8000 (9984): index i = tv / 256 + 15 */
#define ET_TV_N 55
static const char * et_tv_names[ET_TV_N] = {
    "30s", "25s", "20s", "15s", "13s", "10s", "8s", "6s", "5s", "4s", "3.2s", "2.5s", "2s", "1.6s", "1.3s", "1s",
    "0.8s", "0.6s", "0.5s", "0.4s", "0.3s", "1/4", "1/5", "1/6", "1/8", "1/10", "1/13", "1/15", "1/20", "1/25",
    "1/30", "1/40", "1/50", "1/60", "1/80", "1/100", "1/125", "1/160", "1/200", "1/250", "1/320", "1/400", "1/500",
    "1/640", "1/800", "1/1000", "1/1250", "1/1600", "1/2000", "1/2500", "1/3200", "1/4000", "1/5000", "1/6400", "1/8000",
};
#define ET_TV_I0 15                /* index of 1 s */
/* sRGB start curve (plan 2.4): D(L) = round(-768 log2(lin(L))), lin = the sRGB transfer of L / 255 (D(255) = 0); the
 * headroom from H to an aim A in Code768 is D(H) - D(A). D(0) unused (9999). Generated offline (m11/m110 impl_notes.md) */
static const uint16_t et_srgb_d[256] = {
    9999, 8975, 8207, 7758, 7439, 7192, 6990, 6819, 6671, 6540, 6424, 6315, 6211, 6111, 6014, 5921,
    5831, 5744, 5659, 5578, 5498, 5421, 5347, 5274, 5203, 5134, 5067, 5001, 4937, 4874, 4813, 4754,
    4695, 4638, 4582, 4527, 4473, 4421, 4369, 4319, 4269, 4220, 4172, 4125, 4079, 4034, 3989, 3945,
    3902, 3859, 3817, 3776, 3736, 3696, 3656, 3617, 3579, 3541, 3504, 3468, 3431, 3396, 3361, 3326,
    3291, 3258, 3224, 3191, 3159, 3126, 3094, 3063, 3032, 3001, 2971, 2941, 2911, 2882, 2853, 2824,
    2795, 2767, 2739, 2712, 2685, 2658, 2631, 2604, 2578, 2552, 2527, 2501, 2476, 2451, 2426, 2402,
    2378, 2354, 2330, 2306, 2283, 2259, 2236, 2214, 2191, 2168, 2146, 2124, 2102, 2081, 2059, 2038,
    2017, 1995, 1975, 1954, 1933, 1913, 1893, 1873, 1853, 1833, 1813, 1794, 1775, 1755, 1736, 1717,
    1699, 1680, 1661, 1643, 1625, 1607, 1589, 1571, 1553, 1535, 1518, 1500, 1483, 1466, 1449, 1432,
    1415, 1398, 1381, 1365, 1348, 1332, 1316, 1300, 1283, 1268, 1252, 1236, 1220, 1205, 1189, 1174,
    1158, 1143, 1128, 1113, 1098, 1083, 1068, 1053, 1039, 1024, 1010, 995, 981, 967, 953, 938,
    924, 911, 897, 883, 869, 855, 842, 828, 815, 801, 788, 775, 762, 748, 735, 722,
    709, 697, 684, 671, 658, 646, 633, 621, 608, 596, 583, 571, 559, 547, 535, 523,
    511, 499, 487, 475, 463, 451, 440, 428, 416, 405, 393, 382, 371, 359, 348, 337,
    326, 314, 303, 292, 281, 270, 259, 248, 238, 227, 216, 205, 195, 184, 174, 163,
    153, 142, 132, 121, 111, 101, 90, 80, 70, 60, 50, 40, 30, 20, 10, 0,
};
/* "hard" (2.5): thirds k = ceil(3 log2(1 + p^2)) >= 1, capped at 9; p = s in %: the k-th entry is the largest s (ppm)
 * that still gives k, i.e. 10000 sqrt(2^(k/3) - 1) (computed offline) */
static const uint16_t et_hard_ppm[8] = { 5098, 7664, 10000, 12328, 14747, 17321, 20099, 23129 };
/* the known histogram sizes (X10; L_logs.md 1): 900x600 display, 1280x854 EVF, 848x566 HDR mode, 900x506 16:9 */
static const uint32_t et_numpix[4] = { 540000, 1093120, 479968, 455400 };

static const char * const et_why_names[R6ETT_NW] = {
    "ok", "off", "R6 Interval runs", "Tv write busy", "no live view", "not M", "Auto ISO", "settings unreadable",
    "ExpSim off", "magnified", "display size?", "Tv not on 1/3", "limits crossed", "no histogram", "no simulation",
    "frame not bound", "LV idle", "settling", "scene moving",
};
static const char * const et_st_names[R6ETT_NS] = { "-", "black", "foot", "under (wide)", "under (near)", "ok", "close",
                                                    "stau", "hard" };
static const char * const et_valid_names[R6ETT_NM] = { "ok", "no LV", "read", "torn", "unbound", "no sim", "ExpSim",
                                                       "magnify", "numpix", "idle" };
const char * r6_ettr_why_name(int w) { return (w >= 0 && w < R6ETT_NW) ? et_why_names[w] : "?"; }
const char * r6_ettr_st_name(int s) { return (s >= 0 && s < R6ETT_NS) ? et_st_names[s] : "?"; }
const char * r6_ettr_valid_name(int v) { return (v >= 0 && v < R6ETT_NM) ? et_valid_names[v] : "?"; }

/* ---- state ---- */
static struct r6_ettr_stats st = { .range = -1 };
static struct r6_ettr_meas et_tm;               /* the task's measurement (r6_hist_task) */
static struct r6_ettr_meas et_bm;               /* the bracket trace's (r6_iv_task) */
static struct r6_ettr_m et_ring_m[R6ETTR_M_RING];
typedef char et_m_size[sizeof(struct r6_ettr_m) == 40 ? 1 : -1];   /* plan 6.1: 128 x 40 B */
static struct r6_ettr_raw et_ring_raw[R6ETTR_RAW_RING];
static struct r6_ettr_bk et_ring_bk[R6ETTR_BK_RING];
static struct r6_ettr_fy et_ring_fy[R6ETTR_FY_RING];
static uint32_t et_bk_n = 0;                    /* written by r6_iv_task only */
static struct r6_ettr_raw * et_raw = 0;         /* the running raw trace, 0 = none */
static int et_was_on = 0, et_t_adv = 0, et_retry = 0, et_m_odd = 0, et_t_fy = 0;
static int et_fps_have = 0, et_fps_t = 0;
static uint32_t et_fps_f = 0;
static int et_ref_have = 0;                     /* F_ref / ae_ref taken (first read after the change) */
static uint32_t et_h_seen = 0;                  /* handler deliveries taken by the task */
static int et_tv_ev = 0;                        /* a Tv delivery since the last advisor step: the freshness restarts */
struct et_win { uint32_t t; uint16_t H, ym10; uint32_t s, b; };
static struct et_win et_w[R6ETTR_WIN_N];
static int et_wn = 0;                           /* entries (newest last) */
struct et_basis { int16_t tv, av; uint32_t sv, ae, drive, htp, alo, flick, numpix; int shots, fn, len; };
static struct et_basis et_b0;
static int et_b0_have = 0;

const struct r6_ettr_stats * r6_ettr_stats(void) { return &st; }
int r6_ettr_mode(void) { return et_mode; }

void r6_ettr_cfg_text(char * buf, int n)
{
    snprintf(buf, n, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,srgb1", ET_T_ML, ET_L_ML, ET_AE_ML, ET_WIN_MIN, ET_WIN_SPAN_MS,
        ET_WIN_DH, ET_WIN_TREND, ET_WIN_DS_PPM, ET_WIN_DY10, ET_FPS_IDLE10, ET_GAP_MS, ET_KEEP_MS, ET_G_ISO100, ET_WIDE_AIM,
        R6ETTR_MEAS_MS);
}

/* a division the compiler cannot turn into a reciprocal word (canon_gate: no new high constants) */
static uint32_t __attribute__((noinline, noipa)) et_div(uint32_t a, uint32_t b) { return b ? a / b : 0; }
static uint32_t et_ppm(uint32_t x, uint32_t n) { return n ? (uint32_t) (((uint64_t) x * 1000000u) / n) : 0; }
static int et_file_number(void) { struct card_info * c = get_shooting_card(); return c ? c->file_number : 0; }

/* ---- PROP_SHUTTER: one more handler on an id ML registers anyway (copy only, Canon's property task) ---- */
struct et_h { uint32_t t; int16_t v; uint8_t len, pad; };
#define ET_HRING 8
static volatile struct et_h et_h[ET_HRING];
static volatile uint32_t et_hn = 0;

PROP_HANDLER(PROP_SHUTTER)
{
    int16_t v = 0;
    if (len >= 2) memcpy(&v, buf, 2);       /* only the delivered bytes (r6_interval.c: exactly 2, Code768) */
    uint32_t n = et_hn;
    volatile struct et_h * e = &et_h[n & (ET_HRING - 1)];
    e->t = get_ms_clock();
    e->v = v;
    e->len = MIN(len, 255u);
    __sync_synchronize();                   /* the entry before the count (PropMgr and r6_hist_task may run on other cores) */
    et_hn = n + 1;
}

/* ---- formatting ---- */
void r6_ettr_tvs(char * buf, int n, int tv)
{
    int i = (tv >> 8) + ET_TV_I0;
    if (!(tv & (ET_GRID - 1)) && i >= 0 && i < ET_TV_N) { snprintf(buf, n, "%s", et_tv_names[i]); return; }
    uint32_t a = tv < 0 ? -tv : tv, t10 = et_div(a * 10 + 384, 768);
    snprintf(buf, n, "Tv%s%d.%d", tv < 0 ? "-" : "+", (int) et_div(t10, 10), (int) (t10 - 10 * et_div(t10, 10)));
}

/* thirds -> "+1 2/3 EV", "-1/3 EV", "+2 EV", "0 EV" */
static void et_ev3(char * s, int n, int k)
{
    uint32_t a = k < 0 ? -k : k, w = et_div(a, 3), r = a - 3 * w;
    const char * sg = k < 0 ? "-" : k > 0 ? "+" : "";
    if (!r) snprintf(s, n, "%s%d EV", sg, (int) w);
    else if (!w) snprintf(s, n, "%s%d/3 EV", sg, (int) r);
    else snprintf(s, n, "%s%d %d/3 EV", sg, (int) w, (int) r);
}

/* ppm -> "2.1%" */
static void et_pct(char * s, int n, uint32_t ppm)
{
    uint32_t t = et_div(ppm + 500, 1000), w = et_div(t, 10);
    snprintf(s, n, "%d.%d%%", (int) w, (int) (t - 10 * w));
}

/* ---- the measurement (plan 2.2) ---- */
struct et_vis { uint32_t n, tq, t999, t99, t50, t5, q_ppm; struct r6_ettr_ch * ch; uint32_t (*bins)[256]; };

/* r6h_read_canon's visitor: one pass over Canon's array c from bin 255 down (bins != 0: copied as well) */
static uint32_t et_visit(void * arg, int c, const volatile uint32_t * a, uint32_t n)
{
    struct et_vis * v = arg;
    if (v->n != n)
    {
        /* count(>= L) > x * n  <=>  count > floor(x * n) for an integer count */
        v->n = n;
        v->tq = (uint32_t) (((uint64_t) v->q_ppm * n) / 1000000u);
        v->t999 = n / 1000;
        v->t99 = n / 100;
        v->t50 = n >> 1;
        v->t5 = (95 * n) / 100;
    }
    struct r6_ettr_ch * h = &v->ch[c];
    memset(h, 0, sizeof(*h));
    uint32_t cum = 0;
    uint64_t ws = 0;
    int fq = 0, f999 = 0, f99 = 0, f50 = 0, f5 = 0;
    for (int i = 255; i >= 0; i--)
    {
        uint32_t x = a[i];                  /* 32-bit loads, Canon's uncached heap */
        if (v->bins) v->bins[c][i] = x;
        cum += x;
        ws += (uint64_t) x * i;
        if (!fq && cum > v->tq) { h->pq = i; fq = 1; }
        if (!f999 && cum > v->t999) { h->p999 = i; f999 = 1; }
        if (!f99 && cum > v->t99) { h->p99 = i; f99 = 1; }
        if (!f50 && cum > v->t50) { h->p50 = i; f50 = 1; }
        if (!f5 && cum > v->t5) { h->p5 = i; f5 = 1; }
        if (i == 255) h->n255 = cum;
        else if (i == 252) h->n252 = cum;
        else if (i == 248) h->n248 = cum;
        else if (i == 240) h->n240 = cum;
        else if (i == 224) h->n224 = cum;
    }
    h->mean10 = cum ? (uint16_t) ((ws * 10) / cum) : 0;
    return cum;
}

/* one measurement: reads until bound, torn reads and a record that changed meanwhile again at once (3 reads at most) */
static void et_measure(struct r6_ettr_meas * m, uint32_t (*bins)[256])
{
    uint64_t t0 = get_us_clock();
    memset(m, 0, sizeof(*m));
    struct et_vis v;
    memset(&v, 0, sizeof(v));
    v.q_ppm = et_q_ppm[COERCE(et_q, 0, (int) COUNT(et_q_ppm) - 1)];
    v.ch = m->ch;
    v.bins = bins;
    struct r6h_canon o;
    int r = -1;
    for (int i = 0; i < 3; i++)
    {
        m->tries = i + 1;
        r = r6h_read_canon(et_visit, &v, &o);
        if (r == R6H_SHOWN && ((o.hp_b == o.p0 && o.hp_a == o.p0) || o.rd_b == o.rd_a)) break;
        if (r > 0) break;                   /* no set, bad pointer, sum, no FrameNum: not again now */
        st.retries++;
        if (r < 0) st.torn++;
    }
    st.tries += m->tries;
    m->t = get_ms_clock();
    m->rr = r < 0 ? 0xFF : r;
    m->set = o.set < 0 ? 0xFF : o.set;
    m->numpix = o.numpix;
    m->frame = o.frame;
    m->rd_b = o.rd_b; m->rd_a = o.rd_a; m->hp_b = o.hp_b; m->hp_a = o.hp_a; m->p0 = o.p0;
    m->bind = (o.hp_b && o.hp_b == o.p0) | (o.hp_a && o.hp_a == o.p0) << 1 | (o.rd_b && o.rd_b == o.rd_a) << 2;
    m->sim = o.sim & 0xFF;
    m->ae600 = o.ae600;
    m->m608 = (int16_t) o.m608;
    m->fr52c = o.fr52c;
    m->tv424 = (int16_t) o.tv424;
    if (r == R6H_SHOWN)
    {
        for (int c = 1; c < 4; c++)
        {
            m->H = MAX(m->H, m->ch[c].pq);
            m->s = MAX(m->s, et_ppm(m->ch[c].n248, o.numpix));
            m->b = MAX(m->b, et_ppm(m->ch[c].n255, o.numpix));
        }
        m->valid = R6ETT_M_OK;
    }
    else m->valid = r < 0 ? R6ETT_M_TORN : R6ETT_M_READ;
    if (m->valid == R6ETT_M_OK && (m->bind & 3) != 3) { m->valid = R6ETT_M_UNBOUND; st.unbound++; }
    uint32_t us = (uint32_t) (get_us_clock() - t0);
    m->us = us;
}

/* the validity checks that need no sheet (X4 / X6 / X10); the bracket trace uses only these */
static int et_numpix_ok(uint32_t n)
{
    for (int i = 0; i < (int) COUNT(et_numpix); i++) if (et_numpix[i] == n) return 1;
    return 0;
}

static void et_valid_light(struct r6_ettr_meas * m)
{
    if (m->valid != R6ETT_M_OK) return;
    if (lv_dispsize != 1) m->valid = R6ETT_M_MAGNIFY;
    else if (!et_numpix_ok(m->numpix)) m->valid = R6ETT_M_NUMPIX;
    else if (m->sim != 1) m->valid = R6ETT_M_NOSIM;
    else if (-(int) m->m608 > ET_M608_IDLE) m->valid = R6ETT_M_IDLE;
}

/* ---- Canon's settings sheet p (Tor 0 a, read only) ---- */
struct et_sheet { int ok; uint32_t ae, sv, expsim, htp, alo, flick, drive; int16_t tv, av, fast, slow; };

static int et_sheet_read(struct et_sheet * s)
{
    memset(s, 0, sizeof(*s));
    uint32_t cc = RD(R6IV_RAM_CC);
    if (!r6_fy_ram_ok(cc, R6IV_CC_SIZE)) return 0;
    uint32_t p = RD(cc + R6IV_CC_P);
    if (!r6_fy_ram_ok(p, R6IV_P_SIZE)) return 0;           /* the uncached alias on the camera (M8.2) */
    s->ae = RD(p + ET_P_AE);
    s->sv = RD(p + ET_P_ISO) & 0xFFFF;
    s->expsim = RD(p + ET_P_EXPSIM);
    s->htp = RD(p + ET_P_HTP);
    s->alo = RD(p + ET_P_ALO) & 0xFF;
    s->flick = RD(p + ET_P_FLICK);
    s->drive = RD(p + ET_P_DRIVE);
    uint32_t w = RD(p + ET_P_TVAV_W), r = RD(p + ET_P_RANGE_W);
    s->av = (int16_t) w; s->tv = (int16_t) (w >> 16);
    s->fast = (int16_t) r; s->slow = (int16_t) (r >> 16);
    s->ok = 1;
    return 1;
}

/* E2: [0x1427F8] through [0x1778C+0x18] (must be 0x1427C4); -1 = not readable. Log only (X5 until M11.0 confirms it) */
static int et_range_word(void)
{
    if (RD(ET_RANGE_PTR) != ET_RANGE_OBJ) return -1;
    return (int) (RD(ET_RANGE_WORD) & 0x7FFFFFFF);
}

static int et_in_lv(void)
{
    uint32_t v = 1;
    int len = 0;
    if (r6_iv_bulb_running()) return 0;     /* ML's bulb picture: no live image */
    if (lv) return 1;
    return lv_action_raw_get(&v, &len) && len == 4 && v == 0;
}

/* ---- freshness and the window (plan 2.7 / 2.8) ---- */
static void et_reset(int why, int now)
{
    et_wn = 0;
    et_ref_have = 0;
    st.t_ref = now;
    st.f_ref = st.ae_ref = 0;
    st.ref_why = why;
    st.resets++;
}

/* frame rate over >= 0.5 s from the reads (any read with a FrameNum); a class change restarts the freshness */
static void et_fps(const struct r6_ettr_meas * m, int now)
{
    if (m->rr != R6H_SHOWN) return;
    if (!et_fps_have) { et_fps_have = 1; et_fps_t = m->t; et_fps_f = m->frame; return; }
    int dt = (int) m->t - et_fps_t;
    if (dt < 0 || dt > 10000) { et_fps_t = m->t; et_fps_f = m->frame; return; }
    if (dt < ET_FPS_MS) return;
    st.fps10 = (int) et_div((m->frame - et_fps_f) * 10000u, dt);
    et_fps_t = m->t;
    et_fps_f = m->frame;
    int cls = st.fps10 < ET_FPS_IDLE10 ? 0 : st.fps10 < ET_FPS_30_10 ? 1 : 2;
    if (st.fps_cls != cls)                  /* plan 2.7 item 5: L counts from the last class change */
    {
        if (st.fps_cls >= 0) et_reset(3, now);
        st.fps_cls = cls;
    }
}

static int et_fresh(const struct r6_ettr_meas * m)
{
    if (!et_ref_have) { st.f_ref = m->frame; st.ae_ref = m->ae600; et_ref_have = 1; return 0; }
    return (int) (m->t - st.t_ref) >= ET_T_ML && m->frame - st.f_ref >= ET_L_ML && m->ae600 - st.ae_ref >= ET_AE_ML;
}

static void et_win_add(const struct r6_ettr_meas * m)
{
    if (et_wn == R6ETTR_WIN_N)
    {
        for (int i = 1; i < R6ETTR_WIN_N; i++) et_w[i - 1] = et_w[i];
        et_wn--;
    }
    struct et_win * w = &et_w[et_wn++];
    w->t = m->t; w->H = m->H; w->ym10 = m->ch[0].mean10; w->s = m->s; w->b = m->b;
}

/* the newest >= 3 entries over >= 400 ms: 0 = not enough, -1 = not calm, else the number used; maxima in a */
static int et_win_calm(struct r6_ettr_adv * a)
{
    int n = 0;
    for (int k = ET_WIN_MIN; k <= et_wn; k++)
        if ((int) (et_w[et_wn - 1].t - et_w[et_wn - k].t) >= ET_WIN_SPAN_MS) { n = k; break; }
    if (!n) return 0;
    const struct et_win * o = &et_w[et_wn - n], * z = &et_w[et_wn - 1];
    uint32_t hx = 0, hn = 255, sx = 0, sn = 0xFFFFFFFF, bx = 0, yx = 0, yn = 0xFFFF;
    for (int i = et_wn - n; i < et_wn; i++)
    {
        const struct et_win * w = &et_w[i];
        hx = MAX(hx, w->H); hn = MIN(hn, w->H);
        sx = MAX(sx, w->s); sn = MIN(sn, w->s);
        bx = MAX(bx, w->b);
        yx = MAX(yx, w->ym10); yn = MIN(yn, w->ym10);
    }
    a->H = hx; a->s = sx; a->b = bx; a->ym10 = yx;
    a->n = n; a->span = z->t - o->t;
    a->hspread = hx - hn;
    int dtr = (int) z->H - (int) o->H;
    if (hx - hn > ET_WIN_DH || ABS(dtr) > ET_WIN_TREND || yx - yn > ET_WIN_DY10
        || (sx - sn > ET_WIN_DS_PPM && (sx - sn) * 5 > sx))
        return -1;
    return n;
}

/* ---- the decision (plan 2.3 .. 2.5, E7 .. E9) ---- */
/* the limits in "thirds from 30 s" x 256 (u = Code768 - R6PW_TV_MIN >= 0): 0 = 30 s (no negative constant in the code,
 * canon_gate) */
#define ET_U0   (-R6PW_TV_MIN)              /* 3840 */
static void et_limits(struct r6_ettr_adv * a, const struct et_sheet * sh, int * lo_why, int * hi_why)
{
    int lo = 0, hi = R6PW_TV_MAX + ET_U0;
    *lo_why = *hi_why = R6ETT_L_WIN;
    int ul = COERCE(et_long, 0, ET_TV_N - 1) * ET_GRID, us = COERCE(et_short, 0, ET_TV_N - 1) * ET_GRID;
    if (ul > lo) { lo = ul; *lo_why = R6ETT_L_LONG; }
    if (us < hi) { hi = us; *hi_why = R6ETT_L_SHORT; }
    if (sh->fast > sh->slow)                                       /* Canon's cached range (a sane pair only, not 0 / 0) */
    {
        if (sh->slow + ET_U0 > lo) { lo = sh->slow + ET_U0; *lo_why = R6ETT_L_CANON; }
        if (sh->fast + ET_U0 < hi) { hi = sh->fast + ET_U0; *hi_why = R6ETT_L_CANON; }
    }
    int sv = MAX((int) sh->sv, ET_SV_ISO100);                     /* E9: ISO L counts as ISO 100 (the safe side) */
    int g = sv - ET_SV_ISO100 + ET_G_ISO100 + ET_U0;              /* G = Tv - Sv: 2 s at ISO 100 */
    if (g > lo) { lo = g; *lo_why = R6ETT_L_G; }
    a->g = g - ET_U0;
    a->lo = (((lo + ET_GRID - 1) >> 8) << 8) - ET_U0;              /* on the 1/3 grid, the shorter side */
    a->hi = ((hi >> 8) << 8) - ET_U0;
}

static int et_hard_k(uint32_t s)
{
    int k = 1;
    while (k < 9 && s > et_hard_ppm[k - 1]) k++;
    return k;
}

static void et_propose(struct r6_ettr_adv * a, const struct et_sheet * sh)
{
    int t = COERCE(et_target, 0, 2), lo = et_band_lo[t], zo = et_band_zo[t];
    uint32_t q = et_q_ppm[COERCE(et_q, 0, (int) COUNT(et_q_ppm) - 1)];
    int H = MIN((int) a->H, 255), k;
    a->dz = et_srgb_d[MAX(H, 1)] - et_srgb_d[zo];                 /* log only: the uncapped sRGB headroom (H 0 as 1: a bound) */
    a->dw = et_srgb_d[MAX(H, 1)] - et_srgb_d[ET_WIDE_AIM];
    if (a->b > q) { a->st = R6ETT_S_HARD; k = -et_hard_k(a->s); }
    else if (H >= ET_STAU_H) { a->st = R6ETT_S_STAU; k = H >= ET_STAU2_H ? -2 : -1; }
    else if (H > zo) { a->st = R6ETT_S_CLOSE; k = -1; }
    else if (H >= lo) { a->st = R6ETT_S_TARGET; k = 0; }
    else if (H < ET_BLACK_H && a->ym10 < ET_BLACK_Y10) { a->st = R6ETT_S_BLACK; k = ET_UP_FOOT; }
    else if (H < ET_FOOT_H) { a->st = R6ETT_S_FOOT; k = ET_UP_FOOT; }
    else if (et_srgb_d[H] - et_srgb_d[zo] > ET_WIDE_EV)
    {
        /* wide = more than 1 EV to Z_o: at least +1/3 (Tight, H 181 is only 0.32 EV below the aim 200) */
        a->st = R6ETT_S_WIDE;
        k = COERCE((et_srgb_d[H] - et_srgb_d[ET_WIDE_AIM]) >> 8, 1, ET_UP_WIDE);
    }
    else
    {
        a->st = R6ETT_S_NEAR;
        k = COERCE((et_srgb_d[H] - et_srgb_d[zo]) >> 8, 0, ET_UP_NEAR);
    }
    a->k_raw = k;
    int lo_why = 0, hi_why = 0;
    et_limits(a, sh, &lo_why, &hi_why);
    if (a->lo > a->hi) { a->why = R6ETT_W_LIMITS; return; }
    int want = a->tv - ET_GRID * k;                               /* + thirds = longer = smaller Code768 */
    a->lim = R6ETT_L_NONE;
    a->rest = 0;
    if (want < a->lo) { a->rest = (a->lo - want) >> 8; a->lim = lo_why; want = a->lo; }
    else if (want > a->hi) { a->rest = -((want - a->hi) >> 8); a->lim = hi_why; want = a->hi; }
    a->want = want;
    a->k = (a->tv - want) >> 8;
    if (sh->htp) a->hint |= R6ETT_H_HTP;
    if (sh->alo != 3) a->hint |= R6ETT_H_ALO;
    if ((int) sh->sv < ET_SV_ISO100) a->hint |= R6ETT_H_ISOL;
    if (sh->flick && (a->tv > ET_TV_1_100 || want > ET_TV_1_100)) a->hint |= R6ETT_H_FLICK;
    a->hint |= R6ETT_H_STYLE;                                     /* M11.0: no picture style is calibrated yet */
    a->why = R6ETT_W_OK;
}

/* why there can be no proposal now, before the measurement counts (0 = none) */
static int et_pre(const struct et_sheet * sh, int tv, int tvlen)
{
    const struct r6pw_state * p = _r6pw_state();
    if (r6_iv_running()) return R6ETT_W_SERIES;
    if (p->armed || p->busy || p->open || p->pend) return R6ETT_W_TVW;
    if (shooting_mode != SHOOTMODE_M) return R6ETT_W_NOTM;
    if (!sh->ok) return R6ETT_W_P;
    if (sh->ae != 3) return R6ETT_W_NOTM;
    if (!sh->sv) return R6ETT_W_AUTOISO;
    if (tvlen != 2 || (tv & (ET_GRID - 1)) || tv < R6PW_TV_MIN || tv > ET_TV_MAX_ANY) return R6ETT_W_TV;
    return 0;
}

static int et_v2why(int v)
{
    static const uint8_t m[R6ETT_NM] = { R6ETT_W_OK, R6ETT_W_NOLV, R6ETT_W_NOREAD, R6ETT_W_NOREAD, R6ETT_W_UNBOUND,
                                         R6ETT_W_NOSIM, R6ETT_W_EXPSIM, R6ETT_W_MAGNIFY, R6ETT_W_NUMPIX, R6ETT_W_IDLE };
    return (v >= 0 && v < R6ETT_NM) ? m[v] : R6ETT_W_NOREAD;
}

/* the settings a measurement depends on (X16 as reasons): a change restarts the freshness */
static void et_basis(struct et_basis * b, const struct et_sheet * sh, const struct r6_ettr_meas * m)
{
    memset(b, 0, sizeof(*b));
    b->tv = st.tv; b->len = st.tv_len;
    b->av = sh->av; b->sv = sh->sv; b->ae = sh->ae; b->drive = sh->drive;
    b->htp = sh->htp; b->alo = sh->alo; b->flick = sh->flick;
    b->shots = total_shots_count;
    b->fn = et_file_number();
    b->numpix = (m && m->rr == R6H_SHOWN) ? m->numpix : et_b0.numpix;
}

/* restart the freshness (plan 2.7) unless it is restarted already: no window and no reference read yet */
static void et_restart(int why, int now)
{
    if (et_wn || et_ref_have) et_reset(why, now);
}

/* one advisor step on the task's measurement m (or none: no LV) */
static void et_advise(int now, struct r6_ettr_meas * m)
{
    struct et_sheet sh;
    et_sheet_read(&sh);
    st.range = et_range_word();
    struct r6_ettr_adv a;
    memset(&a, 0, sizeof(a));
    a.t = now;
    a.tv = st.tv;
    a.sv = sh.sv;
    a.want = 0x7FFF;
    int why = R6ETT_W_OK, fresh = 0, inwin = 0, keep = 0;
    if (et_tv_ev) { et_tv_ev = 0; et_reset(1, now); }            /* every Tv delivery (dial or ML), even back and forth */
    if (m)
    {
        if (sh.ok && (_expsim != 1 || sh.expsim != 1) && m->valid == R6ETT_M_OK) m->valid = R6ETT_M_EXPSIM;   /* X3 */
        et_valid_light(m);                                       /* X4 sim, X6 meter, X10 */
        et_fps(m, now);
        if (m->valid == R6ETT_M_OK && st.fps_cls == 0) m->valid = R6ETT_M_IDLE;                      /* X6 rate */
        st.meas++;
        st.nv[m->valid]++;
        if (m->valid == R6ETT_M_OK) st.meas_ok++;
        st.us_last = m->us;
        st.us_max = MAX(st.us_max, m->us);
        st.us_avg += ((int) m->us - (int) st.us_avg) >> 3;
        if (sh.ok)
        {
            /* X16 as reasons: a changed setting, picture or display restarts the freshness */
            struct et_basis b;
            et_basis(&b, &sh, m);
            if (!et_b0_have || memcmp(&b, &et_b0, sizeof(b)))
            {
                int w = !et_b0_have ? 5 : (b.tv != et_b0.tv || b.len != et_b0.len) ? 1 : 2;
                et_b0 = b;
                et_b0_have = 1;
                et_reset(w, now);
            }
        }
    }
    int pre = et_pre(&sh, st.tv, st.tv_len);
    if (!m) { why = R6ETT_W_NOLV; et_restart(4, now); et_fps_have = 0; }   /* the frame rate starts again with LV */
    else if (pre) { why = pre; et_restart(6, now); }
    else if (m->valid == R6ETT_M_TORN || m->valid == R6ETT_M_READ || m->valid == R6ETT_M_UNBOUND)
    {
        /* a read failure: the last decision stays a short while (the next tick reads again) */
        if (et_wn && now - (int) et_w[et_wn - 1].t <= ET_KEEP_MS && st.adv.why == R6ETT_W_OK) keep = 1;
        else
        {
            why = et_v2why(m->valid);
            if (et_wn && now - (int) et_w[et_wn - 1].t > ET_GAP_MS) et_restart(4, now);
        }
    }
    else if (m->valid != R6ETT_M_OK) { why = et_v2why(m->valid); et_restart(6, now); }
    else
    {
        if (et_wn && (int) (m->t - et_w[et_wn - 1].t) > ET_GAP_MS) et_reset(4, now);
        fresh = et_fresh(m);
        if (!fresh) why = R6ETT_W_SETTLE;
        else
        {
            st.fresh_n++;
            et_win_add(m);
            inwin = 1;
            int c = et_win_calm(&a);
            if (!c) why = R6ETT_W_SETTLE;
            else if (c < 0) why = R6ETT_W_MOVING;
            else
            {
                a.t_meas = et_w[et_wn - 1].t;
                et_propose(&a, &sh);
                why = a.why;
            }
        }
    }
    if (keep) st.adv.t = now;
    else
    {
        if (why != R6ETT_W_OK) { a.st = R6ETT_S_NONE; a.k = a.k_raw = 0; a.lim = R6ETT_L_NONE; a.rest = 0; a.want = 0x7FFF; }
        a.why = why;
        if (st.range >= 0 && (st.range & 0x10)) a.hint |= R6ETT_H_RANGE;
        st.adv = a;
        st.why_n[why]++;
    }
    if (!m) return;
    if (!(et_m_odd ^= 1)) return;                                 /* ring m: every second advisor measurement (2 / s) */
    struct r6_ettr_m * e = &et_ring_m[st.m_n % R6ETTR_M_RING];
    memset(e, 0, sizeof(*e));
    e->t = m->t; e->frame = m->frame;
    e->tv = st.tv; e->want = st.adv.why == R6ETT_W_OK ? st.adv.want : 0x7FFF;
    e->H = MIN(m->H, 255u); e->valid = m->valid; e->rr = m->rr;
    e->flags = (m->bind & 7) | fresh << 3 | inwin << 4 | (st.range >= 0 && (st.range & 0x10) ? 1 : 0) << 5;
    e->pr = m->ch[1].pq; e->pg = m->ch[2].pq; e->pb = m->ch[3].pq; e->sim = m->sim;
    e->s = MIN(et_div(m->s, 100), 0xFFFFu); e->b = MIN(et_div(m->b, 100), 0xFFFFu);
    e->ym10 = m->ch[0].mean10; e->fps10 = MIN(st.fps10, 0xFFFF);
    e->st = st.adv.st; e->why = st.adv.why; e->k = st.adv.k; e->set = m->set;
    e->ae600 = m->ae600; e->tv424 = m->tv424; e->m608 = m->m608;
    e->rng = st.range < 0 ? 0xFF : (st.range & 0xFF);            /* plan 2.10 question 4: bits 0 / 1 / 4 next to rd[0x5FC] */
    st.m_n++;
}

/* ---- the raw trace of a Tv delivery (plan 6.2 ring raw) ---- */
static void et_deliveries(int now)
{
    uint32_t n = et_hn;
    if (n == et_h_seen) return;
    __sync_synchronize();
    if (n - et_h_seen > ET_HRING) st.h_ovf += n - et_h_seen - ET_HRING;
    st.h_n += n - et_h_seen;
    et_h_seen = n;
    struct et_h h;
    const volatile struct et_h * s = &et_h[(n - 1) & (ET_HRING - 1)];
    h.t = s->t; h.v = s->v; h.len = s->len;
    int16_t old = st.tv;
    st.tv = h.v;
    st.tv_len = h.len;
    et_tv_ev = 1;
    /* ML's write: the newest entry of r6_propw.c's write ring (written before its call) wants this value */
    int src = 0;
    uint32_t wn = _r6pw_ring_n();
    const struct r6pw_state * ps = _r6pw_state();
    if (ps->open && ps->open_want == h.v) src = 1;
    else
    {
        struct r6pw_rec e;
        memset(&e, 0, sizeof(e));
        int c = _r6pw_ring(0, &e);
        if (c) _r6pw_ring(c - 1, &e);
        int d = (int) h.t - (int) e.t;
        if (c && e.want == h.v && d >= 0 && d <= R6PW_CONFIRM_MS + 100) src = 1;
    }
    struct r6_ettr_raw * r = &et_ring_raw[st.raw_n % R6ETTR_RAW_RING];
    memset(r, 0, sizeof(*r));
    r->t = h.t; r->old = old; r->tv = h.v; r->src = src; r->len = h.len; r->wn = wn;
    r->t0 = MIN(MAX(now - (int) h.t, 0), 0xFFFF);
    et_raw = r;
    st.raw_n++;
}

static void et_raw_tick(int now, const struct r6_ettr_meas * m)
{
    struct r6_ettr_raw * r = et_raw;
    if (!r) return;
    struct r6_ettr_rt * k = &r->tk[r->n];
    if (!r->f0ok && m && m->rr == R6H_SHOWN) { r->f0 = m->frame; r->ae0 = m->ae600; r->f0ok = 1; }   /* the first read */
    k->dt = MIN(MAX(now - (int) r->t, 0), 0xFFFF);
    if (m)
    {
        k->dfr = r->f0ok ? m->frame - r->f0 : 0;
        k->tv424 = m->tv424;
        k->ym10 = m->ch[0].mean10;
        k->s = MIN(et_div(m->s, 100), 0xFFFFu);
        k->H = MIN(m->H, 255u);
        k->flags = (m->valid & 0xF) | (m->set & 1) << 4 | ((m->bind & 3) == 3) << 5 | ((m->bind >> 2) & 1) << 6;
        k->ae = (m->ae600 - r->ae0) & 0xFF;
        k->pr = m->ch[1].pq; k->pg = m->ch[2].pq; k->pb = m->ch[3].pq;
    }
    else k->flags = R6ETT_M_NOLV;
    if (++r->n >= R6ETTR_RAW_N) et_raw = 0;
}

/* ---- FaceYuv 2 x 2 (plan 2.1 last item, S2 F13): channels and union from the same pixels, Expo log only ---- */
#define ET_FY_NPROBE 16
#define ET_FY_PROBE(cpu, i) (*(const volatile uint32_t *) ((cpu) + (13 + 26 * (i)) * R6_FY_PITCH + (19 * (i) + 7) * 4))

static void et_fy_sample(int now)
{
    struct r6_ettr_fy * e = &et_ring_fy[st.fy_n % R6ETTR_FY_RING];
    memset(e, 0, sizeof(*e));
    e->t = now;
    uint64_t t0 = get_us_clock();
    struct r6_fy_loc l;
    if (!r6_fy_locate_done(&l)) { st.fy_n++; st.fy_torn++; return; }      /* ok 0, f2 0: not located */
    const uint8_t * cpu = (const uint8_t *) l.cpu;
    uint32_t pr[ET_FY_NPROBE];
    for (int i = 0; i < ET_FY_NPROBE; i++) pr[i] = ET_FY_PROBE(cpu, i);
    uint32_t mr = 0, mg = 0, mb = 0, my = 0;
    for (int y = 0; y < R6_FY_H; y += 2)
    {
        const volatile uint32_t * row = (const volatile uint32_t *) (cpu + y * R6_FY_PITCH);
        for (int x = 0; x < R6_FY_W / 2; x += 2)                   /* every second UYVY word: 2 pixels */
        {
            uint32_t w = row[x];
            int u = (int8_t) (w & 0xFF), v = (int8_t) ((w >> 16) & 0xFF);   /* signed chroma (M7.3) */
            int dr = (359 * v) >> 8, dg = (88 * u + 183 * v) >> 8, db = (454 * u) >> 8;   /* BT.601, an approximation */
            for (int j = 0; j < 2; j++)
            {
                int yy = j ? (int) (w >> 24) : (int) ((w >> 8) & 0xFF);
                int r = yy + dr, g = yy - dg, b = yy + db;
                e->npx++;
                if (r >= 250) e->r250++;
                if (g >= 250) e->g250++;
                if (b >= 250) e->b250++;
                if (r >= 250 || g >= 250 || b >= 250) e->u250++;
                if (yy >= 235) { e->y235++; if (yy >= 245) { e->y245++; if (yy >= 250) { e->y250++; if (yy >= 255) e->y255++; } } }
                if (r > (int) mr) mr = MIN(r, 255);
                if (g > (int) mg) mg = MIN(g, 255);
                if (b > (int) mb) mb = MIN(b, 255);
                if (yy > (int) my) my = yy;
            }
        }
    }
    int wr = 0;
    for (int i = 0; i < ET_FY_NPROBE; i++) wr |= ET_FY_PROBE(cpu, i) != pr[i];
    int nh = r6_fy_recheck(&l);
    uint32_t us = (uint32_t) (get_us_clock() - t0);
    e->f2 = l.f2;
    e->us = us;
    e->mr = mr; e->mg = mg; e->mb = mb; e->my = my;
    e->ok = !wr && nh >= 0 && nh <= 2 && us <= ET_FY_READ_US;
    if (!e->ok) st.fy_torn++;
    st.fy_us_max = MAX(st.fy_us_max, us);
    st.fy_n++;
}

/* ---- the task (r6_hist_task, r6_overlays.c) ---- */
int r6_ettr_tick_ms(void)
{
    if (et_mode) return R6ETTR_TICK_MS;
    et_was_on = 0;                          /* the next switch-on starts again */
    return 0;
}

void r6_ettr_tick(void)
{
    int now = get_ms_clock();
    if (!et_was_on)
    {
        /* Mode switched on: everything starts again (the handler kept counting) */
        et_was_on = 1;
        et_h_seen = et_hn;
        if (et_h_seen)                      /* the Tv delivered before (no raw trace for it) */
        {
            __sync_synchronize();
            const volatile struct et_h * s = &et_h[(et_h_seen - 1) & (ET_HRING - 1)];
            st.tv = s->v;
            st.tv_len = s->len;
        }
        et_raw = 0;
        et_b0_have = 0;
        et_fps_have = 0;
        st.fps_cls = -1;
        et_retry = 0;
        et_t_adv = now - R6ETTR_MEAS_MS;
        et_reset(5, now);
    }
    st.ticks++;
    int t = COERCE(et_target, 0, 2);
    st.mode = et_mode; st.target = t; st.q_ppm = et_q_ppm[COERCE(et_q, 0, (int) COUNT(et_q_ppm) - 1)];
    st.lo = et_band_lo[t]; st.zo = et_band_zo[t];
    st.longest = (COERCE(et_long, 0, ET_TV_N - 1) - ET_TV_I0) * ET_GRID;
    st.shortest = (COERCE(et_short, 0, ET_TV_N - 1) - ET_TV_I0) * ET_GRID;
    et_deliveries(now);
    int inlv = et_in_lv(), measured = 0;
    if (et_raw)
    {
        if (inlv) { et_measure(&et_tm, 0); et_valid_light(&et_tm); measured = 1; }
        et_raw_tick(now, measured ? &et_tm : 0);
    }
    if (et_retry || now - et_t_adv >= R6ETTR_MEAS_MS || now < et_t_adv)
    {
        et_t_adv = now;
        if (!inlv) { et_retry = 0; et_advise(now, 0); }
        else
        {
            if (!measured) { et_measure(&et_tm, 0); measured = 1; }
            et_advise(now, &et_tm);
            int v = et_tm.valid;
            et_retry = (v == R6ETT_M_TORN || v == R6ETT_M_READ || v == R6ETT_M_UNBOUND) && et_retry < ET_RETRY ? et_retry + 1 : 0;
        }
    }
    if (measured) st.cur = et_tm;
#ifdef FEATURE_LV_BUFFER_PROBE
    if (lvprobe_expo_on() && inlv && (now - et_t_fy >= ET_FY_MS || now < et_t_fy))
    {
        et_t_fy = now;
        et_fy_sample(now);
    }
#endif
}

/* ---- the bracket trace (r6_iv_task, r6_interval.c iv_event; plan 3.1 / 6.2 ring bk) ---- */
void r6_ettr_trace(int what, int ev, int k, int want)
{
    if (!et_mode) return;
    uint64_t t0 = get_us_clock();
    et_measure(&et_bm, 0);
    et_valid_light(&et_bm);
    struct r6_ettr_bk * e = &et_ring_bk[et_bk_n % R6ETTR_BK_RING];
    memset(e, 0, sizeof(*e));
    e->t = et_bm.t; e->frame = et_bm.frame;
    e->what = what; e->k = k; e->ev = ev; e->valid = et_bm.valid;
    uint32_t hn = et_hn;                    /* the newest delivery (the task may not have taken it yet) */
    __sync_synchronize();
    e->want = want; e->tv = hn ? et_h[(hn - 1) & (ET_HRING - 1)].v : 0x7FFF; e->fn = et_file_number();
    e->H = MIN(et_bm.H, 255u); e->pr = et_bm.ch[1].pq; e->pg = et_bm.ch[2].pq; e->pb = et_bm.ch[3].pq;
    e->s = MIN(et_div(et_bm.s, 100), 0xFFFFu); e->b = MIN(et_div(et_bm.b, 100), 0xFFFFu);
    e->ym10 = et_bm.ch[0].mean10;
    e->set = et_bm.set; e->bind = et_bm.bind; e->sim = et_bm.sim;
    e->tv424 = et_bm.tv424; e->m608 = et_bm.m608; e->ae600 = et_bm.ae600;
    uint32_t us = (uint32_t) (get_us_clock() - t0);
    e->us = MIN(us, 0xFFFFu);
    st.bk_us_max = MAX(st.bk_us_max, us);
    st.bk_n++;
    st.bk_ring_n = ++et_bk_n;
}

/* ---- the log (lvprobe.c, snapshot task) ---- */
int r6_ettr_read_full(uint32_t (*bins)[256], struct r6_ettr_meas * m)
{
    et_measure(m, bins);
    et_valid_light(m);
    return m->rr == R6H_SHOWN;
}

#define ET_RING_GET(name, ring, cnt, N) \
int name(int i, typeof(ring[0]) * e) \
{ \
    uint32_t n = (cnt); \
    int c = n < (N) ? (int) n : (N); \
    if (i >= 0 && i < c) *e = ring[(n - c + i) % (N)]; \
    return c; \
}
ET_RING_GET(r6_ettr_ring_m, et_ring_m, st.m_n, R6ETTR_M_RING)
ET_RING_GET(r6_ettr_ring_raw, et_ring_raw, st.raw_n, R6ETTR_RAW_RING)
ET_RING_GET(r6_ettr_ring_bk, et_ring_bk, et_bk_n, R6ETTR_BK_RING)
ET_RING_GET(r6_ettr_ring_fy, et_ring_fy, st.fy_n, R6ETTR_FY_RING)

/* ---- the proposal as text (menu and LV line) ---- */
static void et_text(char * s, int n, const struct r6_ettr_adv * a, int lv)
{
    char t1[16], t2[24], t3[16];
    const char * px = lv ? "ETTR " : "";
    if (a->why != R6ETT_W_OK) { snprintf(s, n, "%s%s", lv ? "ETTR: " : "", r6_ettr_why_name(a->why)); return; }
    r6_ettr_tvs(t1, sizeof(t1), a->want);
    if (a->lim && a->rest)
    {
        /* black / foot (fixed +3 EV) and wide (aim 200, capped): the step is only a lower bound of the deficit */
        const char * ge = (a->st == R6ETT_S_BLACK || a->st == R6ETT_S_FOOT || a->st == R6ETT_S_WIDE) ? ">=" : "";
        et_ev3(t2, sizeof(t2), a->rest);
        snprintf(s, n, "%sLimit %s: %s%s missing", px, t1, ge, t2);
    }
    else if (a->k == 0 && a->st == R6ETT_S_TARGET) snprintf(s, n, "%sok %s", px, t1);
    else if (a->k == 0) snprintf(s, n, "%s%s (under <1/3 EV)", px, t1);   /* near below the band, rounded down (plan 2.4) */
    else if (a->st == R6ETT_S_BLACK) snprintf(s, n, "%sdark: >=+3 EV, %s", px, t1);
    else
    {
        et_ev3(t2, sizeof(t2), a->k);
        if (a->st == R6ETT_S_HARD) { et_pct(t3, sizeof(t3), a->s); snprintf(s, n, "%sOVER %s: %s (%s)", px, t3, t1, t2); }
        else snprintf(s, n, "%s%s (%s)", px, t1, t2);
    }
    if (lv && (a->hint & (R6ETT_H_HTP | R6ETT_H_ALO | R6ETT_H_ISOL | R6ETT_H_FLICK)))
    {
        int l = strlen(s);
        snprintf(s + l, n - l, " [%s%s%s%s", a->hint & R6ETT_H_HTP ? "HTP " : "", a->hint & R6ETT_H_ALO ? "ALO " : "",
            a->hint & R6ETT_H_ISOL ? "L " : "", a->hint & R6ETT_H_FLICK ? "FL " : "");
        l = strlen(s);
        if (l && s[l - 1] == ' ') s[l - 1] = ']';                /* "[HTP ALO " -> "[HTP ALO]" */
    }
}

int r6_ettr_lv_wanted(void) { return et_mode && et_lv; }

int r6_ettr_lv_text(char * buf, int n)
{
    if (!et_mode || !et_lv || r6_iv_in_pic()) return 0;           /* nothing while a picture runs (as R6 Motion) */
    struct r6_ettr_adv a = st.adv;
    et_text(buf, n, &a, 1);
    return 1;
}

uint32_t r6_ettr_lv_count(int draw)
{
    if (draw) return ++st.lv_draws;
    return ++st.lv_erases;
}

/* ---- menu (Shoot > R6 ETTR) ---- */
static MENU_UPDATE_FUNC(et_mode_update)
{
    if (et_mode && !et_lv) MENU_SET_WARNING(MENU_WARN_INFO, "Show in LV is OFF: the proposal is only in this menu.");
}

static MENU_UPDATE_FUNC(et_lim_update)
{
    if (COERCE(et_long, 0, ET_TV_N - 1) > COERCE(et_short, 0, ET_TV_N - 1))
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Longest Tv is shorter than Shortest Tv: no proposal.");
    else
        MENU_SET_WARNING(MENU_WARN_INFO, "The simulation limit G applies too: 2 s at ISO 100, 1/8 at ISO 1600 (plan 2.4).");
}

static MENU_UPDATE_FUNC(et_prop_update)
{
    char s[64];
    if (!et_mode) { MENU_SET_VALUE("Mode Off"); return; }
    struct r6_ettr_adv a = st.adv;
    et_text(s, sizeof(s), &a, 0);
    MENU_SET_VALUE("%s", s);
    if (a.why == R6ETT_W_OK)
    {
        int age = MAX(get_ms_clock() - (int) a.t_meas, 0);
        uint32_t t10 = et_div(age + 50, 100);
        MENU_SET_RINFO("%d.%ds", (int) et_div(t10, 10), (int) (t10 - 10 * et_div(t10, 10)));
        char tv[16];
        r6_ettr_tvs(tv, sizeof(tv), a.tv);
        int h = a.hint & ~R6ETT_H_RANGE;
        if (h & ~R6ETT_H_STYLE)
            MENU_SET_WARNING(MENU_WARN_ADVICE, "Inexact:%s%s%s%s. Measured at %s, H %d.", h & R6ETT_H_HTP ? " HTP on" : "",
                h & R6ETT_H_ALO ? " ALO on" : "", h & R6ETT_H_ISOL ? " ISO L" : "", h & R6ETT_H_FLICK ? " anti-flicker, Tv < 1/100" : "",
                tv, (int) a.H);
        else
            MENU_SET_WARNING(MENU_WARN_INFO, "Measured at %s: H %d (%s), sRGB start curve (style not calibrated).", tv,
                (int) a.H, r6_ettr_st_name(a.st));
    }
    else if (a.why == R6ETT_W_SETTLE || a.why == R6ETT_W_MOVING)
        MENU_SET_WARNING(MENU_WARN_INFO, "Waits for 3 calm measurements over 0.4 s, 1 s and 60 frames after a change.");
    else
        MENU_SET_WARNING(MENU_WARN_INFO, "No proposal: %s.", r6_ettr_why_name(a.why));
}

static MENU_UPDATE_FUNC(et_meas_update)
{
    const struct r6_ettr_meas * m = &st.cur;
    if (!et_mode || !st.meas) { MENU_SET_VALUE("-"); return; }
    char sp[12], bp[12];
    et_pct(sp, sizeof(sp), m->s);
    et_pct(bp, sizeof(bp), m->b);
    MENU_SET_VALUE("H %d s %s b %s", (int) m->H, sp, bp);
    MENU_SET_RINFO("%d.%d fps", (int) et_div(st.fps10, 10), (int) (st.fps10 - 10 * et_div(st.fps10, 10)));
    MENU_SET_WARNING(MENU_WARN_INFO, "R/G/B %d/%d/%d, %s; read %d us (max %d); %d of %d ok.", m->ch[1].pq, m->ch[2].pq,
        m->ch[3].pq, r6_ettr_valid_name(m->valid), (int) st.us_last, (int) st.us_max, (int) st.meas_ok, (int) st.meas);
}

static struct menu_entry et_menu[] = {
    {
        .name   = "R6 ETTR",
        .select = menu_open_submenu,
        .help   = "M11.0: proposes a Tv from Canon's LV histogram (R, G, B). Display only: no write.",
        .help2  = "M, ExpSim on, ISO fixed. You set the Tv; ML never writes or presses.",
        .children = (struct menu_entry[]) {
            {
                .name    = "Mode",
                .priv    = &et_mode,
                .max     = 1,
                .choices = CHOICES("Off", "Display only"),
                .update  = et_mode_update,
                .help    = "Display only: 4 measurements per second in LV, a proposal here and in LV.",
                .help2   = "Log: Debug > LV probe > Log snapshot writes ETTnnnnn.LOG next to LVPnnnnn.LOG.",
            },
            {
                .name    = "Target",
                .priv    = &et_target,
                .max     = 2,
                .choices = CHOICES("JPEG safe", "Tight", "Reserve"),
                .help    = "Brightest channel: JPEG safe 228..244, Tight 228..247, Reserve 210..232.",
                .help2   = "Measured on Canon's LV histogram per channel R, G, B (8 bit, after the picture style).",
            },
            {
                .name    = "Highlight ignore",
                .priv    = &et_q,
                .max     = COUNT(et_q_ppm) - 1,
                .choices = et_q_names,
                .help    = "This share of the image may lie above the target (small lights). 0..2 %.",
            },
            {
                .name    = "Longest Tv",
                .priv    = &et_long,
                .max     = ET_TV_N - 1,
                .choices = et_tv_names,
                .update  = et_lim_update,
                .help    = "No proposal longer than this (default 30 s; tripod).",
            },
            {
                .name    = "Shortest Tv",
                .priv    = &et_short,
                .max     = ET_TV_N - 1,
                .choices = et_tv_names,
                .update  = et_lim_update,
                .help    = "No proposal shorter than this (default 1/8000).",
            },
            {
                .name    = "Show in LV",
                .priv    = &et_lv,
                .max     = 1,
                .help    = "One line under Canon's display, LCD shooting screen only (Global Draw).",
            },
            {
                .name      = "Proposal",
                .update    = et_prop_update,
                .icon_type = IT_ALWAYS_ON,
                .help      = "The Tv to dial (or why there is none). Right: age of the measurement.",
                .help2     = "Steps: +3 EV dark, up to +4 EV, -1/3 .. -3 EV over; limits shown, never clamped silently.",
            },
            {
                .name      = "Measurement",
                .update    = et_meas_update,
                .icon_type = IT_ALWAYS_ON,
                .help      = "H = brightest channel level (R/G/B), s = share >= 248, b = share at 255.",
            },
            MENU_EOL
        },
    },
};

static void et_init()
{
    menu_add("Shoot", et_menu, COUNT(et_menu));
}

INIT_FUNC("r6_ettr", et_init);

#endif /* CONFIG_R6_ETTR */
