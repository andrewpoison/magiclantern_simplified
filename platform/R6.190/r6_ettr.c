/*
 * R6.190 M11.0: Auto-ETTR advisor, ML menu Shoot > "R6 ETTR" (docs/m11_plan.md sections 1, 2.1 .. 2.10, 3.1, 5.2, 6,
 * 7.1, 8.1 and the user's answers of 2026-10-07: E2 Tor 0e reads, E7 target selectable, E8 longest Tv 30 s, E9 hints
 * only).
 *
 * What: measures Canon's LV display histogram per channel R, G, B (not Y), shows the Tv that would put the brightest
 * channel into the target band (in the ML menu and as one line in the LV), and logs the measurement. The advisor itself
 * never writes and never presses. No Canon call, no MMIO, no write to Canon RAM, no file (lvprobe.c writes ETTnnnnn.LOG
 * through ML's FIO wrappers).
 *
 * M11.1 (spec m11/m111 spec_m111.md Rev. 2; answers E3 / E4 / E6 / F1..F4 of 2026-10-07 and Q1..Q4 (all a) of
 * 2026-10-08): Mode "Apply" and the menu command "Apply once". On the command only (never at startup, never repeated
 * without a new command, never during R6 Interval / R6 Motion): ML waits as "Tv back to ..." does (3 s after the
 * command, the ML menu closed 3 s, LV 3 s), measures, writes Tv through Tor 1 (r6_propw.c: kind K_ETTR, context
 * CTX_ETTR, all 44 rows, row 5 a second time with the run's limits, member, direction, Sv and G), measures again: at most
 * R6PW_ETTR_N (5) steps and one way back, at most 6 calls per command with the restores. It keeps (like a dial turn,
 * _r6pw_commit, no call) only a value whose newest settled measurement was "not over"; every stop restores the start
 * value through Tor 1's paths (E4 a). The run steps in r6_iv_task (r6_interval.c command 6, _r6ettr_run_tick), never
 * sleeps itself; the advisor's decision comes over a seqlock (et_dec). From r6_propw.c only the _r6pw_* functions of
 * gate 11 are used; ML never presses in ETTR (no CC_SW1_CBR / CC_SW2_CBR).
 *
 * Reads (all RAM reads, every pointer checked; approvals: Canon RAM since M7.0, FaceYuv 2026-10-01, the sheets Tor 0
 * 2026-10-04, D1 (flash inputs, C.Fn range) 2026-10-06, Tor 0e = E2 2026-10-07): Canon's histogram DISP set and the
 * newest RandomData record through r6_overlays.c r6h_read_canon / r6h_canon_set (the box's reader and rules), the CC
 * settings sheet p = [[0x10AB8]+0x34FC] (AE, Tv, Av, ISO, ExpSim, HTP, ALO, anti-flicker, drive, Canon's Tv range and
 * the C.Fn range; r6_fy_ram_ok), the flash inputs [cc+0xA30] / [cc+0x7A8], [0x1427F8] through [0x1778C+0x18] ==
 * 0x1427C4, FaceYuv (2 x 2, second-newest frame, only while the Expo log is ON), the CC switch bytes (r6_iv_cc_read).
 * One more ML handler on PROP_SHUTTER, an id ML registers anyway (r6_interval.c, r6_propw.c, src/lens.c): copy only.
 *
 * Measurement (plan 2.1 / 2.2), 4 per second while Mode is not Off and Canon's LV runs (M11.1: every 50 ms tick during
 * a run), in r6_hist_task (r6_overlays.c, a task beside r6_iv_task): one pass per channel from bin 255 down gives P_c
 * (the highest level with more than q x numPix pixels at or above it, q = Highlight ignore), the counts >= 224 / 240 /
 * 248 / 252 and in bin 255, the median, 5 % and the mean; H = max(P_R, P_G, P_B), s = max share >= 248, b = max share
 * in bin 255 (ppm of numPix). A read counts only bound to its RandomData record (M11.1: the same record before and
 * after; a read bound only through the record counts 2 frames less), rd[0x5FC] = 1, ExpSim (ML and [p+0x548]) 1, no
 * magnify, a known numPix, [0x1427F8] readable without bit 4 (X5), no LV idle (X6: the DISP set fixed >= 0.5 s,
 * FrameNum fixed >= 1 s, dark with the set fixed >= 0.25 s, rd[0x608] < -9 EV).
 *
 * Freshness (M11.1 per frame-rate class: class 0 < 25 frames/s, 1 below 45, 2 above): after a change of Tv (every
 * PROP_SHUTTER delivery), ISO, Av, AE, drive, HTP, ALO, anti-flicker, a picture, numPix, the class, or a gap of 2 s
 * without a good read, a measurement counts once >= 1 s passed, FrameNum >= F_ref + 30 (class 0: 13) and rd[0x600] >=
 * its value at F_ref + 2. Calm window: the newest >= 3 fresh measurements over >= 400 ms (class 0: 3 s) with max -
 * min(H) <= 4, trend <= 2 (class 0: the quarter means <= 1), delta s <= max(0.25 %, 20 %), delta Y mean <= 2; the
 * decision uses the window maxima of H, s, b.
 *
 * Decision (E7, M11.1 1.2 with the measured LV curve et_cal_d of picture style Standard): black (H < 8, Y mean < 4) /
 * foot (H < 32): +3 EV; "wide" (more than 1 EV to Z_o) aims at the band's lower end, +1 .. +4 EV; "near" aims at Z_o,
 * at most +1 EV, rounded down; target band lo .. Z_o: ok; close (Z_o < H <= 247): -1/3; stau (H >= 248, bin 255 <=
 * q): -1/3, H >= 252: -2/3; hard (bin 255 > q): -max(1/3, min(3, log2(1 + p^2))), p = s in %, at least stau's step.
 * Targets: JPEG safe 228 .. 244, Tight 228 .. 247, Reserve 210 .. 232. Limits: the user's longest / shortest Tv, Tor
 * 1's window 1/8000 .. 30 s, Canon's cached range [p+0x114] and the C.Fn range [p+0x110], 1/200 while a flash input
 * is not 0, and the simulation limit G = Tv - Sv (30 s at ISO 100), Sv = max(Sv, ISO 100); 1/3 grid, never B; a
 * clamp shows the rest, a Tv already beyond a limit in the step's direction "beyond limit" (no counter step). Hints
 * only (E9 b, Q3 a): HTP, ALO, ISO L, anti-flicker below 1/100, ISO >= 12800, Tv shorter than 1/500, a picture style
 * other than Standard (M11.1b: ISO >= 12800 only as a log bit and a neutral menu info, not in the LV line). The menu and
 * the LV line show the decision with a hysteresis (a new proposal twice in a row).
 *
 * Read-only traces (plan 6.2): ring raw = 32 ticks of 50 ms after every Tv delivery (dial or ML), also during an R6
 * Interval bracket; ring bk = r6_ettr_trace() from r6_interval.c iv_event; ring m = every second advisor measurement;
 * ring fy = FaceYuv 2 x 2 counts once per second while the Expo log is ON; M11.1 ring w = the ETTR writes.
 *
 * Settings saved in ML's config (Mode default Off). Integer arithmetic only, no division by a constant that would need
 * a reciprocal word (tables, shifts, et_div with a variable divisor), no negative literal (the Tv limits in "thirds from
 * 30 s").
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
#include "r6_overlays.h"
#include "r6_faceyuv.h"
#include "r6_interval.h"
#include "r6_propw.h"
#include "r6_ettr.h"

#ifdef CONFIG_R6_ETTR

#if !defined(FEATURE_R6_CANON_HISTOGRAM) || !defined(CONFIG_R6_INTERVAL) || !defined(CONFIG_R6_PROPW_TV) \
 || !defined(CONFIG_R6_GRID)
#error "R6 ETTR needs the Canon histogram reader (r6_overlays.c), R6 Interval, the Tv write path (Tor 1) and the grid's display path (LV line)"
#endif
#ifdef CONFIG_PROP_REQUEST_CHANGE
#error "R6 M11.1: CONFIG_PROP_REQUEST_CHANGE stays off (Tor 1 writes only through r6_propw.c)"
#endif

extern int lv_action_raw_get(uint32_t * value, int * len);   /* propvalues.c */
#ifdef FEATURE_LV_BUFFER_PROBE
extern int lvprobe_expo_on(void);                            /* lvprobe.c: Debug > LV probe > Expo log */
extern int lvprobe_snapshot_busy(void);                      /* lvprobe.c: M11.1, a log snapshot runs (2.2 point 6) */
#endif

#define RD(a) (*(volatile uint32_t *)(a))

/* ---- limits and timings (plan 2.3 / 2.4 / 2.7 / 2.8; M11.1 spec 1.5 / 1.9 from the M11.0 camera log) ---- */
#define ET_T_ML          1000      /* freshness: >= this long after the change */
#define ET_L_FAST          30      /* ... and >= this many frames (FrameNum), class 1 / 2 (>= 25 frames/s, Eval 4.1) */
#define ET_L_SLOW          13      /* ... class 0 (< 25 frames/s): the largest settling measured at 7..15 frames/s (S2-L08) */
#define ET_AE_ML            2      /* ... and rd[0x600] (Canon's AE counter, ~8 Hz) advanced by this */
#define ET_UNB_FR           2      /* a read bound only through its record counts FrameNum this much less (1.4) */
#define ET_GAP_MS        2000      /* no good read this long: the window and the freshness start again */
#define ET_KEEP_MS       1500      /* a read failure keeps the last decision this long */
#define ET_WIN_MIN          3      /* calm window: >= 3 measurements ... */
#define ET_WIN_SPAN_MS    400      /* ... over >= 400 ms (2.8; class 1 / 2) */
#define ET_WIN_SLOW_MS   3000      /* ... over >= 3 s (class 0, answer F2 b) */
#define ET_WIN_DH           4      /* max - min(H) */
#define ET_WIN_TREND        2      /* |H newest - H oldest| (class 1 / 2) */
#define ET_WIN_TREND_SLOW   1      /* class 0: |mean of the newest quarter - mean of the oldest| (>= 8 entries) */
#define ET_WIN_DS_PPM    2500      /* delta s <= max(0.25 %, 20 % of the larger) (Eval 4.1, ETT00109) */
#define ET_WIN_DY10        20      /* delta Y mean (x 10) */
#define ET_FPS_MS         500      /* frame rate over >= 0.5 s */
#define ET_FPS_SLOW10     250      /* class 0 below 25 frames/s (M11.1: no longer idle) */
#define ET_FPS_30_10      450      /* class 1 below, 2 above */
#define ET_IDLE_SET_MS    500      /* X6: the DISP set fixed this long */
#define ET_IDLE_FRAME_MS 1000      /* X6: FrameNum fixed this long */
#define ET_IDLE_DARK_MS   250      /* X6: Y mean < 4 with the set fixed this long */
#define ET_M608_IDLE     6912      /* rd[0x608] < -9 EV (9 x 768): LV idle (X6) */
#define ET_RETRY            2      /* an unbound / torn advisor read: again on the next tick, at most twice */
#define ET_FY_MS         1000      /* FaceYuv 2 x 2 once per second (Expo log ON) */
#define ET_FY_READ_US   40000      /* FaceYuv read longer than this: discarded (K_code 1.4) */
#define ET_G_ISO100     (-3840)    /* simulation limit Tv - Sv at ISO 100: 30 s (M11.1, Eval 4.3; M11.0: 2 s) */
#define ET_SV_ISO100     3840      /* Sv of ISO 100 in Code768 (5 x 768) */
#define ET_SV_ISO12800 0x2400      /* menu info (log bit) from ISO 12800 (F3; M11.1b: not in the LV line) */
#define ET_TV_1_100      5120      /* 1/100 s (0x1400) */
#define ET_TV_500      0x1B00      /* hint [Tv] for Tv shorter than 1/500 (Q3 a, S2-L01) */
#define ET_TV_4096     0x2400      /* X12 off above 1/4096: Canon's LV clamp (0xE09759C6) */
#define ET_TV_MAX_ANY   10752      /* 1/16000 s: the shortest Tv R6 Interval decodes (r6_interval.c R6IV_TV_EV768_MAX) */
#define ET_GRID           256      /* 1/3 stop */
#define ET_BLACK_H          8
#define ET_BLACK_Y10       40
#define ET_FOOT_H          32
#define ET_STAU_H         248
#define ET_STAU2_H        252
#define ET_WIDE_EV        768      /* "wide" if more than 1 EV to Z_o */
#define ET_UP_WIDE         12      /* step limits in thirds: +4 EV wide, +3 EV foot / black, +1 EV near */
#define ET_UP_WIDE_MIN      3      /* M11.1: wide at least +1 EV (Eval 4.5) */
#define ET_UP_FOOT          9
#define ET_UP_NEAR          3

/* ---- Canon RAM (read only) ---- */
#define ET_RANGE_PTR    (0x1778C + 0x18)   /* E2: must hold ET_RANGE_OBJ */
#define ET_RANGE_OBJ    0x1427C4           /* 0x80050012 copy object (R_rom.md 1: literal 0xE08A4552) */
#define ET_RANGE_WORD   0x1427F8           /* = ET_RANGE_OBJ + 0x34: bit 0 sim, bit 1 no sim, bit 4 out of range */
#define ET_P_AE         0x008
#define ET_P_CFN_W      0x110              /* M11.1 (D1): s16 C.Fn fastest | s16 slowest << 16 (K36, rows 37 / 40) */
#define ET_P_RANGE_W    0x114              /* s16 fastest (Canon's cached dial limit) | s16 slowest << 16 (K36) */
#define ET_P_ALO        0x0A8              /* byte 0: ALO effective (3 = off, K17) */
#define ET_P_DRIVE      0x4C0
#define ET_P_FLICK      0x4E4
#define ET_P_HTP        0x4FC
#define ET_P_ISO        0x510              /* low 16 bits: Sv x 768, 0 = Auto */
#define ET_P_EXPSIM     0x548
#define ET_P_TVAV_W     0x61C              /* s16 Av | s16 Tv << 16 */
#define ET_CC_FLASH1    0xA30              /* M11.1 (D1): the flash inputs of row 43 (K37) */
#define ET_CC_FLASH2    0x7A8
#define ET_STYLE_STD     0x81              /* lens_info.raw_picstyle of Standard (src/picstyle.c) */

/* ---- settings (saved; Mode default Off) ---- */
static CONFIG_INT("r6.ettr.mode",     et_mode, 0);       /* 0 Off, 1 Display only, 2 Apply (M11.1) */
static CONFIG_INT("r6.ettr.target",   et_target, 0);     /* E7: 0 JPEG safe, 1 Tight, 2 Reserve */
static CONFIG_INT("r6.ettr.q",        et_q, 4);          /* Highlight ignore, index into et_q_ppm (4 = 0.1 %) */
static CONFIG_INT("r6.ettr.longest",  et_long, 0);       /* E8: index into et_tv_names (0 = 30 s) */
static CONFIG_INT("r6.ettr.shortest", et_short, 54);     /* 54 = 1/8000 */
static CONFIG_INT("r6.ettr.lv",       et_lv, 1);         /* Show in LV */

static const uint16_t et_q_ppm[] = { 0, 100, 200, 500, 1000, 2000, 5000, 10000, 20000 };
static const char * et_q_names[] = { "0", "0.01%", "0.02%", "0.05%", "0.1%", "0.2%", "0.5%", "1%", "2%" };
typedef char et_q_complete[COUNT(et_q_ppm) == COUNT(et_q_names) ? 1 : -1];
/* E7: band lower end and Z_o per target (JPEG safe: Z_o = 248 - margin 4, F3 a) */
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
/* M11.1 (spec 1.1, Anhang A; Eval 4.3, C_curve 7, V2 2): the measured LV chain of picture style Standard (R channel),
 * D(L) = round(-256 * pos(L)) in Code768, pos = thirds from 1 s of the supporting points 46 .. 253.5, linear between
 * them, below 46 with the slope 46 -> 57, above 253.5 stretched to 255.5; D(0) := D(1). The headroom from H to an aim A
 * is D(H) - D(A). Data words, no arithmetic constant (gen/cal_table.py -> gen/cal_table.txt) */
static const uint16_t et_cal_d[256] = {      /* M11.1: the measured LV chain (picture style Standard), R channel */
    5655, 5655, 5632, 5609, 5585, 5562, 5539, 5516, 5492, 5469, 5446, 5423, 5399, 5376, 5353, 5329,
    5306, 5283, 5260, 5236, 5213, 5190, 5167, 5143, 5120, 5097, 5073, 5050, 5027, 5004, 4980, 4957,
    4934, 4911, 4887, 4864, 4841, 4817, 4794, 4771, 4748, 4724, 4701, 4678, 4655, 4631, 4608, 4585,
    4561, 4538, 4515, 4492, 4468, 4445, 4422, 4399, 4375, 4352, 4332, 4313, 4293, 4273, 4254, 4234,
    4214, 4194, 4175, 4155, 4135, 4116, 4096, 4078, 4061, 4043, 4025, 4008, 3990, 3972, 3955, 3937,
    3919, 3902, 3884, 3866, 3849, 3832, 3817, 3802, 3787, 3772, 3757, 3742, 3727, 3712, 3697, 3682,
    3667, 3652, 3637, 3622, 3607, 3592, 3577, 3562, 3547, 3533, 3518, 3504, 3489, 3474, 3460, 3445,
    3430, 3416, 3401, 3387, 3372, 3357, 3343, 3328, 3315, 3301, 3288, 3274, 3261, 3247, 3234, 3220,
    3207, 3193, 3180, 3166, 3153, 3139, 3126, 3112, 3099, 3085, 3072, 3059, 3046, 3034, 3021, 3008,
    2995, 2982, 2970, 2957, 2944, 2931, 2918, 2906, 2893, 2880, 2867, 2854, 2842, 2829, 2816, 2802,
    2788, 2774, 2761, 2747, 2733, 2719, 2705, 2691, 2678, 2664, 2650, 2636, 2622, 2608, 2595, 2581,
    2567, 2553, 2538, 2523, 2509, 2494, 2480, 2465, 2450, 2436, 2421, 2406, 2392, 2377, 2363, 2348,
    2333, 2319, 2304, 2289, 2275, 2260, 2245, 2231, 2216, 2202, 2187, 2172, 2158, 2143, 2128, 2114,
    2099, 2085, 2070, 2055, 2039, 2020, 2001, 1982, 1963, 1944, 1925, 1906, 1887, 1868, 1849, 1830,
    1811, 1792, 1772, 1751, 1731, 1710, 1690, 1669, 1649, 1628, 1608, 1587, 1567, 1546, 1523, 1498,
    1472, 1446, 1421, 1395, 1370, 1344, 1318, 1293, 1259, 1216, 1173, 1131, 1088, 1045, 960, 832,
};
typedef char et_cal_d_complete[COUNT(et_cal_d) == 256 ? 1 : -1];
/* "hard" (2.5): thirds k = ceil(3 log2(1 + p^2)) >= 1, capped at 9; p = s in %: the k-th entry is the largest s (ppm)
 * that still gives k, i.e. 10000 sqrt(2^(k/3) - 1) (computed offline) */
static const uint16_t et_hard_ppm[8] = { 5098, 7664, 10000, 12328, 14747, 17321, 20099, 23129 };
/* the known histogram sizes (X10; L_logs.md 1): 900x600 display, 1280x854 EVF, 848x566 HDR mode, 900x506 16:9 */
static const uint32_t et_numpix[4] = { 540000, 1093120, 479968, 455400 };

static const char * const et_why_names[] = {
    "ok", "off", "Interval runs", "Tv write busy", "no live view", "not M", "Auto ISO", "settings unreadable",
    "ExpSim off", "magnified", "display size?", "Tv not on 1/3", "limits crossed", "no histogram", "no simulation",
    "frame not bound", "LV idle", "settling", "scene moving", "LV sim out of range",
};
static const char * const et_st_names[R6ETT_NS] = { "-", "black", "foot", "under (wide)", "under (near)", "ok", "close",
                                                    "stau", "hard" };
static const char * const et_valid_names[] = { "ok", "no LV", "read", "torn", "unbound", "no sim", "ExpSim",
                                                       "magnify", "numpix", "idle", "range" };
/* M11.1: R6ETT_A_* (<= 30 characters, never a comma: iv.ref and the menu) */
static const char * const et_a_names[] = {
    "-", "Mode is not Apply", "Tv writes OFF (Interval)", "busy", "Interval / Motion trigger runs", "log snapshot runs",
    "not M", "Tv writes", "Tv not on 1/3", "Tv beyond your limits / G", "advisor", "not from the ML menu",
    "no live view", "live view not steady", "clock went back", "Tv changed", "a setting changed", "picture taken",
    "display changed (EVF/LCD)", "half press", "Canon menu / playback", "Trash pressed", "LV not steady",
    "shutdown / abort", "LV does not follow Tv", "Canon AE did not take Tv", "Tv write", "Tv changed by you",
    "every value was over", "nothing better", "over again after the way back", "not kept (card or Tv busy)",
    "Canon job running", "over before kept", "ML menu opened again", "command too old", "no write needed",
};
typedef char et_why_complete[COUNT(et_why_names) == R6ETT_NW ? 1 : -1];
typedef char et_valid_complete[COUNT(et_valid_names) == R6ETT_NM ? 1 : -1];
typedef char et_a_complete[COUNT(et_a_names) == R6ETT_NA ? 1 : -1];
const char * r6_ettr_why_name(int w) { return (w >= 0 && w < R6ETT_NW) ? et_why_names[w] : "?"; }
const char * r6_ettr_st_name(int s) { return (s >= 0 && s < R6ETT_NS) ? et_st_names[s] : "?"; }
const char * r6_ettr_valid_name(int v) { return (v >= 0 && v < R6ETT_NM) ? et_valid_names[v] : "?"; }
const char * r6_ettr_a_name(int a) { return (a >= 0 && a < R6ETT_NA) ? et_a_names[a] : "?"; }

/* ---- state ---- */
static struct r6_ettr_stats st = { .range = -1, .hyst_want = 0x7FFF };
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
typedef char et_win_size[sizeof(struct et_win) == 16 ? 1 : -1];
static struct et_win et_w[R6ETTR_WIN_N];        /* M11.1: a ring (spec 1.5) */
static int et_wn = 0;                           /* entries */
static int et_wh = 0;                           /* index of the oldest entry */
static struct et_basis et_b0;
static int et_b0_have = 0;
/* M11.1 (X6 new, 1.4): the DISP set and FrameNum, watched every tick */
static int et_set_last = -2, et_set_t = 0;      /* the set last seen and when it changed (or LV started) */
static int et_fr_have = 0, et_fr_t = 0;
static uint32_t et_fr_last = 0;
/* M11.1 (1.7): the 424 watch, the newest good read's numPix and rd+0x424, the decision for r6_iv_task (seqlock) */
static int16_t et_424_for = 0x7FFF, et_424_last = 0x7FFF;
static uint32_t et_424_t = 0;
static uint32_t et_np_last = 0;
static struct r6_ettr_dec et_dec;
static volatile uint32_t et_dec_seq = 0;        /* odd while r6_hist_task writes */
/* M11.1 "Apply once" (2): only r6_iv_task writes the run; the menu task writes ap_req / ap_cmd_b0 / et_trash */
static volatile uint32_t ap_req = 0;            /* get_ms_clock() | 1 of a command r6_iv_task has not taken yet (2.2) */
static volatile int et_trash = 0;               /* a Trash press during the run (menu_open's task, 2.8, Q1 a) */
static volatile int et_ap_fast = 0;             /* phase >= MEASURE: the advisor steps every tick (1.7) */
static struct r6_ettr_run ap = { .phase = R6ETTR_AP_IDLE };
static struct et_basis ap_cmd_b0;               /* the run basis at the command (2.2 point 11) */
static uint32_t ap_cmd_hn;                      /* M11.1b (B2): et_hn of the command's newest delivery (with ap_cmd_b0) */
static int et_run_no = 0;
static struct r6_ettr_w et_ring_w[R6ETTR_W_RING];
static uint32_t et_w_n = 0;
static struct r6_ettr_w * ap_wcur = 0;          /* ring w entry of the newest write of the run */
static struct r6_ettr_dec ap_dc;                /* the decision that started the commit (its texts) */
static int ap_commit_end = 0;                   /* R6ETT_E_KEPT / R6ETT_E_BACKKEPT */
static uint32_t ap_notes0 = 0;                  /* r6_propw.c notices at the start of this tick (2.8) */
static char et_msg[100] = "";                   /* the newest notice (NotifyBox keeps 98) */
enum { AP_UI_NONE = 0, AP_UI_REFUSED, AP_UI_DROPPED, AP_UI_STOPPED, AP_UI_KEPT, AP_UI_OK };
static int ap_ui = AP_UI_NONE;                  /* the "Apply once" menu line */
static int ap_ref_a = 0, ap_ref_l = 0;          /* the menu's last refusal */

#define AP_IDLE    R6ETTR_AP_IDLE
#define AP_WAIT    R6ETTR_AP_WAIT
#define AP_MEASURE R6ETTR_AP_MEASURE
#define AP_SETTLE  R6ETTR_AP_SETTLE
#define AP_BACK    R6ETTR_AP_BACK
#define AP_COMMIT  R6ETTR_AP_COMMIT

const struct r6_ettr_stats * r6_ettr_stats(void) { return &st; }
int r6_ettr_mode(void) { return et_mode; }
const struct r6_ettr_run * r6_ettr_run(void) { return &ap; }
const char * r6_ettr_msg(void) { return et_msg; }
int r6_ettr_run_active(void) { return ap.phase != AP_IDLE; }
int r6_ettr_menu_lock(void) { return ap.phase >= AP_MEASURE; }
int r6_ettr_busy(void)
{
    uint32_t r = ap_req;
    return ap.phase != AP_IDLE || (r && (int) (get_ms_clock() - (r & ~1)) < R6ETTR_REQ_MS);
}

void r6_ettr_cfg_text(char * buf, int n)
{
    /* M11.1 (5.2): T_ml,L_fast,L_slow,AE_ml,win_min,win_ms,win_slow_ms,win_dh,trend,trend_slow,ds_ppm,dy10,fps_slow10,
     * idle_set_ms,idle_frame_ms,gap_ms,keep_ms,g_iso100,set_ms,set_slow_ms,ms_424,commit_ms,N,step,meas_ms,curve (G printed
     * with its sign in the format: no negative literal) */
    snprintf(buf, n, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,-%d,%d,%d,%d,%d,%d,%d,%d,cal1", ET_T_ML, ET_L_FAST,
        ET_L_SLOW, ET_AE_ML, ET_WIN_MIN, ET_WIN_SPAN_MS, ET_WIN_SLOW_MS, ET_WIN_DH, ET_WIN_TREND, ET_WIN_TREND_SLOW,
        ET_WIN_DS_PPM, ET_WIN_DY10, ET_FPS_SLOW10, ET_IDLE_SET_MS, ET_IDLE_FRAME_MS, ET_GAP_MS, ET_KEEP_MS, -ET_G_ISO100,
        R6ETTR_SET_MS, R6ETTR_SET_SLOW_MS, R6ETTR_424_MS, R6ETTR_COMMIT_MS, R6PW_ETTR_N, R6PW_ETTR_STEP, R6ETTR_MEAS_MS);
}

/* a division the compiler cannot turn into a reciprocal word (canon_gate: no new high constants) */
static uint32_t __attribute__((noinline, noipa)) et_div(uint32_t a, uint32_t b) { return b ? a / b : 0; }
static uint32_t et_ppm(uint32_t x, uint32_t n) { return n ? (uint32_t) (((uint64_t) x * 1000000u) / n) : 0; }
static int et_file_number(void) { struct card_info * c = get_shooting_card(); return c ? c->file_number : 0; }
static int et_iv_task(void) { return streq(get_current_task_name(), "r6_iv_task"); }
static int et_menu_task(void) { return streq(get_current_task_name(), R6IV_MENU_TASK); }
static int et_open_task(void) { return streq(get_current_task_name(), R6IV_MENU_OPEN_TASK); }

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

/* M11.1: the newest delivery (any task); M11.1b: returns the count it belongs to (0 = none yet) */
static uint32_t et_h_newest(int16_t * v, int * len)
{
    uint32_t n = et_hn;
    __sync_synchronize();
    if (!n) return 0;
    const volatile struct et_h * s = &et_h[(n - 1) & (ET_HRING - 1)];
    *v = s->v;
    *len = s->len;
    return n;
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
    /* M11.1 (1.4): bound through the same RandomData record before and after counts (bit 2), even without the pointer
     * equality; such a read counts ET_UNB_FR frames less (et_fresh) */
    if (m->valid == R6ETT_M_OK && !(m->bind & 4)) { m->valid = R6ETT_M_UNBOUND; st.unbound++; }
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

/* M11.1 (X6 new, 1.4): the DISP set and FrameNum seen by the task, every tick (a measurement or r6h_canon_set) */
static void et_obs_set(int set, int now)
{
    if (set != et_set_last) { et_set_last = set; et_set_t = now; }
}

static void et_obs_meas(const struct r6_ettr_meas * m, int now)
{
    et_obs_set(m->set == 0xFF ? -1 : m->set, now);
    if (m->rr != R6H_SHOWN) return;
    if (!et_fr_have || m->frame != et_fr_last) { et_fr_have = 1; et_fr_last = m->frame; et_fr_t = now; }
    et_np_last = m->numpix;                         /* X10: the newest good read's numPix (1.7) */
    if (m->bind & 4)                                /* the 424 watch: a valid read (1.7, Eval 4.1) */
    {
        et_424_last = m->tv424;
        if (!et_424_t && m->tv424 == et_424_for) et_424_t = m->t ? m->t : 1;
    }
}

/* X6: the set fixed >= 0.5 s, FrameNum fixed >= 1 s, or dark with the set fixed >= 0.25 s (rd[0x608]: et_valid_light) */
static int et_idle(const struct r6_ettr_meas * m, int now)
{
    if (now - et_set_t >= ET_IDLE_SET_MS) return 1;
    if (et_fr_have && now - et_fr_t >= ET_IDLE_FRAME_MS) return 1;
    return m->ch[0].mean10 < ET_BLACK_Y10 && now - et_set_t >= ET_IDLE_DARK_MS;
}

/* ---- Canon's settings sheet p (Tor 0 a, read only; M11.1 + the C.Fn range and the flash inputs, D1) ---- */
struct et_sheet { int ok; uint32_t ae, sv, expsim, htp, alo, flick, drive; int16_t tv, av, fast, slow, cf_fast, cf_slow; int flash; };

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
    uint32_t w = RD(p + ET_P_TVAV_W), r = RD(p + ET_P_RANGE_W), cf = RD(p + ET_P_CFN_W);
    s->av = (int16_t) w; s->tv = (int16_t) (w >> 16);
    s->fast = (int16_t) r; s->slow = (int16_t) (r >> 16);
    s->cf_fast = (int16_t) cf; s->cf_slow = (int16_t) (cf >> 16);
    s->flash = RD(cc + ET_CC_FLASH1) != 0 || RD(cc + ET_CC_FLASH2) != 0;
    s->ok = 1;
    return 1;
}

/* E2: [0x1427F8] through [0x1778C+0x18] (must be 0x1427C4); -1 = not readable. M11.1: X5 strict (et_advise) */
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

/* ---- freshness and the window (plan 2.7 / 2.8; M11.1 1.5 per class) ---- */
static void et_reset(int why, int now)
{
    et_wn = 0;
    et_wh = 0;
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
    int cls = st.fps10 < ET_FPS_SLOW10 ? 0 : st.fps10 < ET_FPS_30_10 ? 1 : 2;   /* M11.1: 0 = slow, no longer idle */
    if (st.fps_cls != cls)                  /* plan 2.7 item 5: L counts from the last class change */
    {
        if (st.fps_cls >= 0) et_reset(3, now);
        st.fps_cls = cls;
    }
}

/* M11.1 (1.8): L per class; a read bound only through its record counts ET_UNB_FR frames less. The reference keeps the
 * record's FrameNum (task A: the safe side; the spec's pseudo-code took the reduced one) */
static int et_fresh(const struct r6_ettr_meas * m)
{
    uint32_t f = (m->bind & 3) == 3 ? m->frame : m->frame - ET_UNB_FR;
    if (!et_ref_have) { st.f_ref = m->frame; st.ae_ref = m->ae600; et_ref_have = 1; return 0; }
    int L = st.fps_cls == 0 ? ET_L_SLOW : ET_L_FAST;
    return (int) (m->t - st.t_ref) >= ET_T_ML && (int) (f - st.f_ref) >= L && m->ae600 - st.ae_ref >= ET_AE_ML;
}

/* the window ring: W(i) = entry i from the oldest (no division: canon_gate) */
static struct et_win * W(int i)
{
    int x = et_wh + i;
    return &et_w[x >= R6ETTR_WIN_N ? x - R6ETTR_WIN_N : x];
}

static void et_win_add(const struct r6_ettr_meas * m)
{
    if (et_wn == R6ETTR_WIN_N)                      /* full: the oldest goes */
    {
        if (++et_wh == R6ETTR_WIN_N) et_wh = 0;
        et_wn--;
    }
    struct et_win * w = W(et_wn++);
    w->t = m->t; w->H = m->H; w->ym10 = m->ch[0].mean10; w->s = m->s; w->b = m->b;
}

/* the newest >= 3 entries spanning >= 400 ms (class 0: 3 s): 0 = not enough, -1 = not calm, else the number used;
 * maxima in a (M11.1 1.8) */
static int et_win_calm(struct r6_ettr_adv * a)
{
    int slow = st.fps_cls == 0, span = slow ? ET_WIN_SLOW_MS : ET_WIN_SPAN_MS;
    int n = 0;
    for (int k = ET_WIN_MIN; k <= et_wn; k++)
        if ((int) (W(et_wn - 1)->t - W(et_wn - k)->t) >= span) { n = k; break; }
    if (!n) return 0;
    const struct et_win * o = W(et_wn - n), * z = W(et_wn - 1);
    uint32_t hx = 0, hn = 255, sx = 0, sn = 0xFFFFFFFF, bx = 0, yx = 0, yn = 0xFFFF;
    for (int i = et_wn - n; i < et_wn; i++)
    {
        const struct et_win * w = W(i);
        hx = MAX(hx, w->H); hn = MIN(hn, w->H);
        sx = MAX(sx, w->s); sn = MIN(sn, w->s);
        bx = MAX(bx, w->b);
        yx = MAX(yx, w->ym10); yn = MIN(yn, w->ym10);
    }
    a->H = hx; a->s = sx; a->b = bx; a->ym10 = yx;
    a->n = n; a->span = z->t - o->t;
    a->hspread = hx - hn;
    a->t_win0 = o->t;
    int tr;
    if (slow && n >= 8)                             /* class 0: the quarter means against single-frame noise (F2 b) */
    {
        int q = n >> 2, s0 = 0, s1 = 0;
        for (int i = 0; i < q; i++) { s0 += W(et_wn - n + i)->H; s1 += W(et_wn - 1 - i)->H; }
        tr = s1 - s0;
        if (ABS(tr) > ET_WIN_TREND_SLOW * q) return -1;
    }
    else
    {
        tr = (int) z->H - (int) o->H;
        if (ABS(tr) > (slow ? ET_WIN_TREND_SLOW : ET_WIN_TREND)) return -1;
    }
    if (hx - hn > ET_WIN_DH || yx - yn > ET_WIN_DY10 || (sx - sn > ET_WIN_DS_PPM && (sx - sn) * 5 > sx))
        return -1;
    return n;
}

/* ---- the decision (plan 2.3 .. 2.5, E7 .. E9; M11.1 1.2 / 1.3) ---- */
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
    if (sh->cf_fast > sh->cf_slow)                                 /* M11.1: the C.Fn range too (rows 37 / 40 check both) */
    {
        if (sh->cf_slow + ET_U0 > lo) { lo = sh->cf_slow + ET_U0; *lo_why = R6ETT_L_CANON; }
        if (sh->cf_fast + ET_U0 < hi) { hi = sh->cf_fast + ET_U0; *hi_why = R6ETT_L_CANON; }
    }
    if (sh->flash && R6PW_XSYNC_CAP + ET_U0 < hi)                  /* M11.1: the flash cap 1/200 (row 43 would refuse) */
    {
        hi = R6PW_XSYNC_CAP + ET_U0;
        *hi_why = R6ETT_L_FLASH;
    }
    int sv = MAX((int) sh->sv, ET_SV_ISO100);                     /* E9: ISO L counts as ISO 100 (the safe side) */
    int g = sv - ET_SV_ISO100 + ET_G_ISO100 + ET_U0;              /* G = Tv - Sv: 30 s at ISO 100 */
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
    int dh = et_cal_d[MAX(H, 1)];                                 /* H 0 as 1: a bound */
    a->dz = dh - et_cal_d[zo];                                    /* log only: the uncapped headroom */
    a->dw = dh - et_cal_d[lo];
    if (a->b > q) { a->st = R6ETT_S_HARD; k = -MAX(et_hard_k(a->s), H >= ET_STAU2_H ? 2 : 1); }   /* M11.1: >= stau (S2-L07) */
    else if (H >= ET_STAU_H) { a->st = R6ETT_S_STAU; k = H >= ET_STAU2_H ? -2 : -1; }
    else if (H > zo) { a->st = R6ETT_S_CLOSE; k = -1; }
    else if (H >= lo) { a->st = R6ETT_S_TARGET; k = 0; }
    else if (H < ET_BLACK_H && a->ym10 < ET_BLACK_Y10) { a->st = R6ETT_S_BLACK; k = ET_UP_FOOT; }
    else if (H < ET_FOOT_H) { a->st = R6ETT_S_FOOT; k = ET_UP_FOOT; }
    else if (dh - et_cal_d[zo] > ET_WIDE_EV)
    {
        /* wide = more than 1 EV to Z_o: aims at the band's lower end, at least +1 EV (M11.1, Eval 4.5) */
        a->st = R6ETT_S_WIDE;
        k = COERCE((dh - et_cal_d[lo]) >> 8, ET_UP_WIDE_MIN, ET_UP_WIDE);
    }
    else
    {
        a->st = R6ETT_S_NEAR;
        k = COERCE((dh - et_cal_d[zo]) >> 8, 0, ET_UP_NEAR);      /* rounded down */
    }
    a->k_raw = k;
    int lo_why = 0, hi_why = 0;
    et_limits(a, sh, &lo_why, &hi_why);
    if (a->lo > a->hi) { a->why = R6ETT_W_LIMITS; return; }
    int want = a->tv - ET_GRID * k;                               /* + thirds = longer = smaller Code768 */
    a->lim = R6ETT_L_NONE;
    a->rest = 0;
    a->beyond = 0;
    if ((k > 0 && a->tv < a->lo) || (k < 0 && a->tv > a->hi))     /* M11.1 (1.3): beyond the limit in the step's direction */
    {
        a->beyond = 1;
        a->lim = k > 0 ? lo_why : hi_why;
        want = a->tv;                                             /* k 0: no counter step */
    }
    else if (want < a->lo) { a->rest = (a->lo - want) >> 8; a->lim = lo_why; want = a->lo; }
    else if (want > a->hi) { a->rest = -((want - a->hi) >> 8); a->lim = hi_why; want = a->hi; }
    a->want = want;
    a->k = (a->tv - want) >> 8;
    if (sh->htp) a->hint |= R6ETT_H_HTP;
    if (sh->alo != 3) a->hint |= R6ETT_H_ALO;
    if ((int) sh->sv < ET_SV_ISO100) a->hint |= R6ETT_H_ISOL;
    if (sh->flick && (a->tv > ET_TV_1_100 || want > ET_TV_1_100)) a->hint |= R6ETT_H_FLICK;
    if (!lens_info.raw_picstyle) a->hint |= R6ETT_H_STYLE_NA;     /* M11.1: not delivered (all M11.0 logs): menu info only */
    else if (lens_info.raw_picstyle != ET_STYLE_STD) a->hint |= R6ETT_H_STYLE;
    if ((int) sh->sv >= ET_SV_ISO12800) a->hint |= R6ETT_H_ISO;   /* M11.1 (F3) */
    if (a->tv > ET_TV_500 || want > ET_TV_500) a->hint |= R6ETT_H_TV;   /* M11.1 (Q3 a) */
    a->why = R6ETT_W_OK;
}

/* why there can be no proposal now, before the measurement counts (0 = none); M11.1: the run's own arm does not block */
static int et_pre(const struct et_sheet * sh, int tv, int tvlen)
{
    const struct r6pw_state * p = _r6pw_state();
    if (r6_iv_running()) return R6ETT_W_SERIES;
    if ((p->armed && !(p->armed == R6PW_CTX_ETTR && et_ap_fast)) || p->busy || p->open || p->pend) return R6ETT_W_TVW;
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
                                         R6ETT_W_NOSIM, R6ETT_W_EXPSIM, R6ETT_W_MAGNIFY, R6ETT_W_NUMPIX, R6ETT_W_IDLE,
                                         R6ETT_W_RANGE };
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

/* M12.0 (spec m12/m120 5): the proposal the LV line showed when the ML menu paused live view, for the menu line only.
 * Written by r6_hist_task (et_shown_update), read by the menu task (seqlock); Apply, et_dec and Tor 1 never read it. */
#define ET_PM_MS 30000                 /* a copy older than this is not shown (QEMU: variable) */
struct et_pm { struct r6_ettr_adv a; struct et_basis b; uint32_t hn; int target, q, lng, sht, ok; };
static struct et_pm et_pm;
static volatile uint32_t et_pm_seq = 0;            /* odd while r6_hist_task writes */
static uint32_t et_pm_saves = 0, et_pm_drops = 0;  /* log only (ett.pm) */
static volatile uint32_t et_pm_shows = 0, et_pm_old = 0;

static void et_pm_track(const struct r6_ettr_adv * a)
{
    int save = a->why == R6ETT_W_NOLV && st.shown.why == R6ETT_W_OK && et_b0_have && gui_menu_shown();
    int drop = !save && et_pm.ok && (a->why != R6ETT_W_NOLV || !gui_menu_shown());
    if (!save && !drop) return;
    et_pm_seq++;
    __sync_synchronize();
    if (save)
    {
        et_pm.a = st.shown; et_pm.b = et_b0; et_pm.hn = et_hn;
        et_pm.target = et_target; et_pm.q = et_q; et_pm.lng = et_long; et_pm.sht = et_short; et_pm.ok = 1;
        et_pm_saves++;
    }
    else { et_pm.ok = 0; et_pm_drops++; }
    __sync_synchronize();
    et_pm_seq++;
}

/* 1 = the pre-menu copy may be shown now (menu task) */
static int et_pm_get(struct r6_ettr_adv * a)
{
    struct et_pm p = { .ok = 0 };
    int ok = 0;
    for (int i = 0; i < 3 && !ok; i++)
    {
        uint32_t s1 = et_pm_seq;
        __sync_synchronize();
        if (s1 & 1) continue;
        p = et_pm;
        __sync_synchronize();
        ok = et_pm_seq == s1;
    }
    if (!ok || !p.ok || !gui_menu_shown()) return 0;
    if (get_ms_clock() - (int) p.a.t_meas > ET_PM_MS) { et_pm_old++; return 0; }
    struct et_sheet sh;
    struct et_basis b;
    if (et_hn != p.hn || et_target != p.target || et_q != p.q || et_long != p.lng || et_short != p.sht || !et_sheet_read(&sh)) return 0;
    et_basis(&b, &sh, 0);                 /* the sheet now: av, sv, ae, drive, htp, alo, flick, picture count, file number */
    b.tv = sh.tv;                         /* the sheet's Tv (X-Sync in standby, M11.1b K11: no copy) */
    b.len = p.b.len; b.numpix = p.b.numpix;   /* not compared */
    if (memcmp(&b, &p.b, sizeof(b))) return 0;
    *a = p.a;
    et_pm_shows++;
    return 1;
}

/* M12.0 (spec 5.2): the ETT line ett.pm = ok,t_meas,want,saves,shows,drops (drops + old) */
int r6_ettr_pm_text(char * s, int n)
{
    return snprintf(s, n, "%d,%d,%d,%d,%d,%d", et_pm.ok, (int) et_pm.a.t_meas, et_pm.a.want, (int) et_pm_saves, (int) et_pm_shows,
        (int) (et_pm_drops + et_pm_old));
}

/* M11.1 (1.6): the display's decision. A reason at once; the same proposal as shown, or none shown before, at once;
 * another proposal only when it comes twice in a row */
static void et_shown_update(const struct r6_ettr_adv * a)
{
    et_pm_track(a);
    if (a->why != R6ETT_W_OK || st.shown.why != R6ETT_W_OK || a->want == st.shown.want || a->want == st.hyst_want)
    {
        st.shown = *a;
        st.hyst_want = 0x7FFF;
        return;
    }
    st.hyst_want = a->want;                                       /* seen once: shown is kept */
}

/* M11.1 (1.7): one advisor step for r6_iv_task (seqlock; r6_hist_task is the only writer) */
static void et_dec_publish(const struct r6_ettr_adv * a, const struct r6_ettr_meas * m)
{
    struct r6_ettr_dec * d = &et_dec;
    et_dec_seq++;
    __sync_synchronize();
    d->t = a->t; d->t_ref = st.t_ref; d->t_win0 = a->t_win0; d->t_meas = a->t_meas;
    d->tv = a->tv; d->want = a->why == R6ETT_W_OK ? a->want : 0x7FFF; d->lo = a->lo; d->hi = a->hi;
    d->why = a->why; d->st = a->st; d->lim = a->lim; d->beyond = a->beyond;
    d->k = a->k; d->rest = a->rest; d->cls = st.fps_cls; d->n = a->n;
    d->H = a->H; d->ym10 = a->ym10; d->span = MIN(a->span, 0xFFFF); d->hspread = a->hspread;
    d->s = a->s; d->b = a->b;
    d->numpix = et_np_last; d->frame = m ? m->frame : 0; d->fps10 = st.fps10;
    d->tv424 = et_424_last; d->tv424_for = et_424_for; d->t_424ok = et_424_t;
    d->valid = m ? m->valid : R6ETT_M_NOLV; d->set = m ? m->set : 0xFF;
    d->hint = a->hint; d->sv = a->sv; d->pad = 0;
    __sync_synchronize();
    et_dec_seq++;
}

/* Rev. 2 (S1-04): leading underscore, not in magiclantern.sym; read only, any task. 1 = a stable copy */
int _r6ettr_dec_get(struct r6_ettr_dec * d, uint32_t * seq)
{
    for (int i = 0; i < 3; i++)
    {
        uint32_t s1 = et_dec_seq;
        __sync_synchronize();
        if (s1 & 1) continue;
        *d = et_dec;
        __sync_synchronize();
        if (et_dec_seq == s1) { *seq = s1; return 1; }
    }
    return 0;
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
        if (m->valid == R6ETT_M_OK && (st.range < 0 || (st.range & 0x10))) m->valid = R6ETT_M_RANGE;   /* M11.1: X5 strict */
        et_fps(m, now);
        if (m->valid == R6ETT_M_OK && et_idle(m, now)) m->valid = R6ETT_M_IDLE;                         /* M11.1: X6 new */
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
        if (et_wn && now - (int) W(et_wn - 1)->t <= ET_KEEP_MS && st.adv.why == R6ETT_W_OK) keep = 1;
        else
        {
            why = et_v2why(m->valid);
            if (et_wn && now - (int) W(et_wn - 1)->t > ET_GAP_MS) et_restart(4, now);
        }
    }
    else if (m->valid != R6ETT_M_OK) { why = et_v2why(m->valid); et_restart(6, now); }
    else
    {
        if (et_wn && (int) (m->t - W(et_wn - 1)->t) > ET_GAP_MS) et_reset(4, now);
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
                a.t_meas = W(et_wn - 1)->t;
                et_propose(&a, &sh);
                why = a.why;
            }
        }
    }
    if (keep) st.adv.t = now;                                     /* the same window content (Apply needs a newer t_meas) */
    else
    {
        if (why != R6ETT_W_OK) { a.st = R6ETT_S_NONE; a.k = a.k_raw = 0; a.lim = R6ETT_L_NONE; a.rest = 0; a.want = 0x7FFF; a.beyond = 0; }
        a.why = why;
        if (st.range >= 0 && (st.range & 0x10)) a.hint |= R6ETT_H_RANGE;
        st.adv = a;
        st.why_n[why]++;
        et_shown_update(&a);
    }
    et_dec_publish(&st.adv, m);                                   /* M11.1: every step, also without LV */
    if (!m) return;
    if (!(et_m_odd ^= 1)) return;                                 /* ring m: every second advisor measurement (2 / s) */
    struct r6_ettr_m * e = &et_ring_m[st.m_n % R6ETTR_M_RING];
    memset(e, 0, sizeof(*e));
    e->t = m->t; e->frame = m->frame;
    e->tv = st.tv; e->want = st.adv.why == R6ETT_W_OK ? st.adv.want : 0x7FFF;
    e->H = MIN(m->H, 255u); e->valid = m->valid; e->rr = m->rr;
    e->flags = (m->bind & 7) | fresh << 3 | inwin << 4 | (st.range >= 0 && (st.range & 0x10) ? 1 : 0) << 5
             | (ap.phase != AP_IDLE) << 6 | ((m->bind & 4) && (m->bind & 3) != 3) << 7;   /* M11.1: bits 6 / 7 */
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
    if (h.v != et_424_for)                  /* M11.1: the 424 watch refers to this value (1.7); review L1: Canon's unchanged */
    {                                       /* re-delivery (M10.2 K15: Standby entry at X-sync 1/200) keeps it (every ML write */
        et_424_for = h.v;                   /* is a changed delivery: row 40) */
        et_424_t = 0;
    }
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
            et_424_for = s->v;
            et_424_t = 0;
        }
        et_raw = 0;
        et_b0_have = 0;
        et_fps_have = 0;
        st.fps_cls = -1;
        et_retry = 0;
        et_t_adv = now - R6ETTR_MEAS_MS;
        et_set_t = now;
        et_fr_have = 0;
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
    /* M11.1 (1.7): during an "Apply once" run the advisor steps every tick */
    int adv = et_retry || et_ap_fast || now - et_t_adv >= R6ETTR_MEAS_MS || now < et_t_adv;
    if (!inlv) { et_set_t = now; et_fr_have = 0; }  /* X6 starts again with LV */
    else if (et_raw || adv)
    {
        et_measure(&et_tm, 0);
        measured = 1;
        et_obs_meas(&et_tm, now);
    }
    else et_obs_set(r6h_canon_set(), now);          /* M11.1 (X6): the set in every tick without a measurement */
    if (et_raw)
    {
        if (measured) et_valid_light(&et_tm);
        et_raw_tick(now, measured ? &et_tm : 0);
    }
    if (adv)
    {
        et_t_adv = now;
        if (!inlv) { et_retry = 0; et_advise(now, 0); }
        else
        {
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
ET_RING_GET(r6_ettr_ring_w, et_ring_w, et_w_n, R6ETTR_W_RING)

/* =====================================================================================================================
 * M11.1 "Apply once" (spec m11/m111 2): r6_iv_task, one step per iv_tick, no sleep of its own (canon_gate: no new edge
 * r6_ettr_* -> msleep); the waits are those of r6_propw.c (its wait rows, its 500 ms window)
 * ===================================================================================================================== */

/* the reason as text (<= 70 characters): Tor 1's row, the advisor's reason, the write's result, else the name (only the
 * _r6pw_* names of gate 11: _r6pw_lock_name, _r6pw_res_name) */
static void ap_reason(char * s, int n, int a, int lock)
{
    if (a == R6ETT_A_TVW) snprintf(s, n, "Tv writes: %s", _r6pw_lock_name(lock));
    else if (a == R6ETT_A_ADV) snprintf(s, n, "%s", r6_ettr_why_name(lock));
    else if (a == R6ETT_A_WRITE) snprintf(s, n, "Tv write: %s", _r6pw_res_name(lock));
    else snprintf(s, n, "%s", r6_ettr_a_name(a));
}

static void ap_box(void) { NotifyBox(5000, "%s", et_msg); }

/* every end of a run (2.1, Rev. 2 S1-07) */
static void ap_end(int end, int a)
{
    ap.end = end;
    ap.why = a;
    et_ap_fast = 0;
    ap.armed = 0;
    ap.phase = AP_IDLE;
    et_trash = 0;
}

/* before the arm (2.8): no write, no marker; the notice, iv.ref src 8 */
static void ap_drop(int a, int lock)
{
    char r[72];
    ap.lock = lock;
    r6_iv_ref_add(R6IV_REF_ETTR_TASK, a, lock);
    ap_reason(r, sizeof(r), a, lock);
    snprintf(et_msg, sizeof(et_msg), "ETTR dropped: %s.\nNothing written.", r);
    ap_box();
    st.apn.dropped++;
    if (a == R6ETT_A_TRASH) st.apn.trash++;
    ap_ui = AP_UI_DROPPED;
    ap_end(R6ETT_E_DROP, a);
}

/* the texts of an end without a write (2.9): ok, ~ok, limit reached, still over */
static void ap_nowrite_text(const struct r6_ettr_dec * d)
{
    char t1[16], t2[24];
    r6_ettr_tvs(t1, sizeof(t1), d->tv);
    if (d->st == R6ETT_S_TARGET) snprintf(et_msg, sizeof(et_msg), "ETTR: Tv %s is ok. Nothing written.", t1);
    else if (d->lim && d->rest > 0)
    {
        const char * ge = (d->st == R6ETT_S_BLACK || d->st == R6ETT_S_FOOT || d->st == R6ETT_S_WIDE) ? ">=" : "";
        et_ev3(t2, sizeof(t2), d->rest);
        snprintf(et_msg, sizeof(et_msg), "ETTR: Limit %s reached: %s%s missing.\nNothing written.", t1, ge, t2);
    }
    else if (d->st >= R6ETT_S_CLOSE) snprintf(et_msg, sizeof(et_msg), "ETTR: still over at Limit %s.\nNothing written.", t1);
    else if (d->st == R6ETT_S_NEAR) snprintf(et_msg, sizeof(et_msg), "ETTR: %s is ~ok (<1/3 EV under).\nNothing written.", t1);
    else snprintf(et_msg, sizeof(et_msg), "ETTR: Tv %s stays. Nothing written.", t1);
}

/* after the arm (2.8): _r6pw_series_end (restore now / pending / offer / none), then the notice only if r6_propw.c gave
 * none in this tick */
static void ap_stop(int a)
{
    uint32_t wn0 = _r6pw_ring_n();
    _r6pw_series_end(a);
    const struct r6pw_state * p = _r6pw_state();
    struct r6pw_rec e;
    memset(&e, 0, sizeof(e));
    int got = 0, end;
    if (_r6pw_ring_n() != wn0) { int n = _r6pw_ring(0, &e); if (n) { _r6pw_ring(n - 1, &e); got = 1; } }
    if (p->pend) end = R6ETT_E_PEND;
    else if (p->offer) end = R6ETT_E_OFFER;
    else if (got && e.res == R6PW_R_TOOK_OVER) end = R6ETT_E_USER;
    else if (got && (e.res == R6PW_R_OK || e.res == R6PW_R_DONE)) end = R6ETT_E_RESTORED;
    else if (p->last_res == R6PW_R_FOREIGN || p->last_res == R6PW_R_TOOK_OVER) end = R6ETT_E_USER;
    else end = R6ETT_E_TOR1;
    if (ap_wcur) { ap_wcur->end = end; if (!ap_wcur->a) ap_wcur->a = a; }
    if (end == R6ETT_E_RESTORED) st.apn.restored++;
    else if (end == R6ETT_E_PEND) st.apn.pend++;
    else if (end == R6ETT_E_OFFER) st.apn.offer++;
    else if (end == R6ETT_E_USER) st.apn.user++;
    else st.apn.tor1++;
    if (a == R6ETT_A_TRASH) st.apn.trash++;
    if (p->notes == ap_notes0)                                   /* r6_propw.c gave no notice in this tick */
    {
        char r[72], b1[16];
        ap_reason(r, sizeof(r), a, ap.lock);
        r6_ettr_tvs(b1, sizeof(b1), ap.base);
        int ok = end == R6ETT_E_RESTORED && got && e.res == R6PW_R_OK;
        if (a == R6ETT_A_NONEED && end == R6ETT_E_RESTORED) { /* et_msg holds the no-write text (ap_nowrite_text) */ }
        else if (ok && a == R6ETT_A_OVER) snprintf(et_msg, sizeof(et_msg), "ETTR: every value was over.\nTv back to %s.", b1);
        else if (ok && a == R6ETT_A_NOBETTER)
            snprintf(et_msg, sizeof(et_msg), "ETTR: nothing better than your %s.\nTv back to %s.", b1, b1);
        else if (ok) snprintf(et_msg, sizeof(et_msg), "ETTR stopped: %s.\nTv back to %s.", r, b1);
        else if (end == R6ETT_E_RESTORED) snprintf(et_msg, sizeof(et_msg), "ETTR stopped: %s.\nTv stays %s.", r, b1);
        else if (end == R6ETT_E_PEND) snprintf(et_msg, sizeof(et_msg), "ETTR stopped: %s.\nTv returns to %s in LV (2 min).", r, b1);
        else if (end == R6ETT_E_OFFER) snprintf(et_msg, sizeof(et_msg), "ETTR stopped: %s.\nInterval > Tv back to %s.", r, b1);
        else if (end == R6ETT_E_USER) snprintf(et_msg, sizeof(et_msg), "ETTR stopped: %s.\nYour Tv stays.", r);
        else snprintf(et_msg, sizeof(et_msg), "ETTR stopped: %s.\nTv not restored (see Interval).", r);
        ap_box();
    }
    else                                                         /* Tor 1's notice stays on screen; the menu line points to it */
    {
        char r[72];
        ap_reason(r, sizeof(r), a, ap.lock);
        snprintf(et_msg, sizeof(et_msg), "ETTR stopped: %s. Tor 1 notice: Interval > Tv writes.", r);
    }
    ap_ui = a == R6ETT_A_NONEED ? AP_UI_OK : AP_UI_STOPPED;
    ap_end(end, a);
}

/* drop before the arm, stop after it */
static void ap_halt(int a, int lock)
{
    if (ap.armed) { ap.lock = lock; ap_stop(a); }
    else ap_drop(a, lock);
}

/* M11.1b (B2): every Tv delivery since the command, from the handler's ring (not only the newest): one with another value or
 * length than the base means the dial moved, also if a later one brings the base back; Canon's unchanged re-deliveries (K15:
 * mode change, Standby at X-sync) pass. ET_HRING or more deliveries since the last look, or the oldest entry overwritten
 * while read, count as moved. Reads only (r6_iv_task, before the arm); 1 = moved (ap.mv) */
static int ap_tv_moved(void)
{
    if (ap.mv) return 1;
    uint32_t n = et_hn, k = ap.hn;
    __sync_synchronize();                                        /* the entries before the count (the handler's order) */
    if (n - k > ET_HRING) { ap.mv = 2; ap.hn = n; return 1; }
    for (; k != n; k++)
    {
        const volatile struct et_h * e = &et_h[k & (ET_HRING - 1)];
        int16_t v = e->v;
        int len = e->len;
        if (v != ap.b0.tv || len != ap.b0.len) { ap.mv = 1; ap.mv_v = v; ap.mv_t = e->t; break; }
    }
    __sync_synchronize();
    if (!ap.mv && et_hn - ap.hn >= ET_HRING) ap.mv = 2;          /* delivery ap.hn + 8 may have overwritten a slot we read */
    ap.hn = n;
    return ap.mv != 0;
}

/* M11.1b (B2): the base items of the checks (2.8 points 1-7): every tick from the command's AP_WAIT on, and first in
 * ap_checks. Nothing of half press, Canon GUI / LV or series (points 8-10: AP_MEASURE and after). wait: AP_WAIT, where a
 * difference of the sheet's Tv alone must hold two ticks in a row (K11: Canon's Standby entry at X-sync shows another
 * value there for microseconds; a real change always has a delivery). 0 or R6ETT_A_* */
static int ap_basis_checks(struct et_sheet * sh, int armed, int wait)
{
    struct et_basis b;
    if (_r6pw_sig() || ml_shutdown_requested) return R6ETT_A_SHUTDOWN;
    if (et_trash) return R6ETT_A_TRASH;
    if (et_mode != 2) return R6ETT_A_MODE;
    if (is_movie_mode() || shooting_mode != SHOOTMODE_M || shooting_mode_custom != SHOOTMODE_M) return R6ETT_A_NOTM;
    if (!et_sheet_read(sh)) return R6ETT_A_SETCHG;
    if (sh->ae != 3) return R6ETT_A_NOTM;
    et_basis(&b, sh, 0);                                         /* Rev. 2 (S1-10): the full basis */
    if (b.av != ap.b0.av || b.sv != ap.b0.sv || b.ae != ap.b0.ae || b.drive != ap.b0.drive || b.htp != ap.b0.htp
        || b.alo != ap.b0.alo || b.flick != ap.b0.flick) return R6ETT_A_SETCHG;
    if (b.shots != ap.b0.shots || b.fn != ap.b0.fn) return R6ETT_A_PIC;
    if (!armed)                                                  /* X16: Tv until the arm (every delivery, newest, sheet) */
    {
        int16_t v = 0;
        int len = 0;
        /* the ring first, so ap.mv records every foreign delivery (also a plain turn) */
        if (ap_tv_moved() || !et_h_newest(&v, &len) || v != ap.b0.tv || len != ap.b0.len) return R6ETT_A_TVCHG;
        if (sh->tv != ap.b0.tv) { if (!wait || ++ap.sh_n >= 2) return R6ETT_A_TVCHG; }   /* AP_WAIT: K11 */
        else ap.sh_n = 0;
    }
    return 0;
}

/* the checks of every tick and right before every call (2.8; 0 = none, else R6ETT_A_*); *sw: SW1 / SW2 byte set */
static int ap_checks(int now, int armed, int * sw)
{
    struct et_sheet sh;
    struct r6_iv_cc c;
    *sw = 0;
    int a = ap_basis_checks(&sh, armed, 0);                      /* M11.1b: points 1-7 (single sheet read, as M11.1) */
    if (a) return a;
    r6_iv_cc_read(&c);
    *sw = c.df || c.e0;
    if (*sw && armed) return R6ETT_A_HALF;
    if (CURRENT_GUI_MODE != 0 || gui_state != GUISTATE_IDLE || _r6iv_holds(now) == R6IV_HOLD_NOLV) return R6ETT_A_GUI;
    if (r6_iv_running()) return R6ETT_A_SERIES;
    return 0;
}

/* the advisor's hard reasons (2.4 point 3; after a write without TVW / LIMITS / NOLV, 2.6 point 7) */
static int ap_hard(int why, int written)
{
    uint32_t m = 1u << R6ETT_W_NOTM | 1u << R6ETT_W_AUTOISO | 1u << R6ETT_W_P | 1u << R6ETT_W_EXPSIM
               | 1u << R6ETT_W_MAGNIFY | 1u << R6ETT_W_NUMPIX | 1u << R6ETT_W_TV | 1u << R6ETT_W_NOSIM
               | 1u << R6ETT_W_RANGE | 1u << R6ETT_W_SERIES;
    if (!written) m |= 1u << R6ETT_W_LIMITS | 1u << R6ETT_W_TVW | 1u << R6ETT_W_NOLV;
    return why >= 0 && why < 32 && ((m >> why) & 1);
}

static int ap_not_over(int st0) { return st0 < R6ETT_S_CLOSE; }

/* ring w: a new entry for an attempt after the arm (5.1) */
static struct r6_ettr_w * ap_w_new(int t, int j, int want, const struct r6_ettr_dec * d)
{
    struct r6_ettr_w * w = &et_ring_w[et_w_n % R6ETTR_W_RING];
    memset(w, 0, sizeof(*w));
    w->t = t; w->j = j; w->no = ap.no;
    w->old = ap.last; w->want = want; w->lo = ap.lo; w->hi = ap.hi;
    w->st = d->st; w->why = d->why;
    w->k = (ap.last - want) >> 8;                                /* + thirds = brighter (longer) */
    w->cls = -1;
    w->H0 = MIN(d->H, 255);
    w->x12 = 0xFF;
    w->dpred = ET_GRID * w->k;
    w->d_settle = w->n_settle = 0xFFFF;
    et_w_n++;
    ap_wcur = w;
    return w;
}

/* S1-06: only an entry of this attempt counts */
static int ap_took_over(uint32_t n0)
{
    struct r6pw_rec e;
    if (_r6pw_ring_n() == n0) return 0;
    int n = _r6pw_ring(0, &e);
    if (n) _r6pw_ring(n - 1, &e);
    return n && e.res == R6PW_R_TOOK_OVER;
}

/* the stop reason of a refused attempt */
static int ap_lock_a(int l)
{
    return l == R6PW_L_GUI ? R6ETT_A_GUI : l == R6PW_L_PRESS ? R6ETT_A_HALF : l == R6PW_L_JOB ? R6ETT_A_JOB : R6ETT_A_TVW;
}

/* one attempt (2.5); afterwards the phase is SETTLE / BACK (written), unchanged (deferred: a wait row before the arm,
 * or a stale decision), or the run has ended (ap_drop / ap_stop) */
static void ap_write(int now, int want, const struct r6_ettr_dec * d, int back)
{
    int l = 0, sw = 0;
    if (!ap.armed)                                              /* the arm: only right before write 1 (plan 4.1 step 4) */
    {
        /* Rev. 2 (S1-01): every row on fresh reads before anything is armed. A wait row keeps measuring; a hard row drops */
        _r6pw_locks(COERCE(want, d->lo, d->hi), R6PW_CLASS_ALL, &l);
        if (l && _r6pw_is_wait_row(l)) { if (l == R6PW_L_PRESS) ap.t_sw1 = now; ap.nwait++; return; }
        if (l) { ap_drop(R6ETT_A_TVW, l); return; }
        int16_t v;
        if (!_r6pw_tv_stable(&v) || v != d->tv || v != ap.b0.tv) { ap_drop(R6ETT_A_TVCHG, 0); return; }   /* row 36's rule */
        if (ap_tv_moved()) { ap_drop(R6ETT_A_TVCHG, 0); return; }   /* M11.1b (B2): the ring once more, right before the arm */
        l = _r6pw_arm_ettr(d->tv, d->lo, d->hi, d->sv);         /* marker R6TV.LCK ctx=4; lo / hi / sv / counts (3.2) */
        if (l) { ap_drop(R6ETT_A_TVW, l); return; }
        ap.armed = 1;
        ap.base = ap.last = d->tv; ap.lo = d->lo; ap.hi = d->hi; ap.sv = d->sv;
        ap.v_ok = ap_not_over(d->st) ? d->tv : 0x7FFF;
        ap.over_lim = ap_not_over(d->st) ? 0x7FFF : d->tv;
        _r6pw_ev_begin(ap.no);                                  /* ev_on: event_stop acts; last_res NONE */
    }
    want = COERCE(want, ap.lo, ap.hi);                          /* the run's limits (row 5 checks them again) */
    int kk = back ? R6ETTR_K_BACK : ap.j + 1;                   /* member index: 1..5, 6 = way back */
    uint32_t n0 = _r6pw_ring_n();                               /* S1-06: ring entries before this attempt */
    l = _r6pw_wait_ready(want, R6PW_K_ETTR);                    /* K_ETTR: GUI / PRESS / JOB at once, other wait rows <= 1 s */
    int t1 = get_ms_clock();
    if (l)                                                      /* refused (its ring entry is written) or your value */
    {
        int a = ap_took_over(n0) ? R6ETT_A_USER : ap_lock_a(l);
        struct r6_ettr_w * w = ap_w_new(t1, kk, want, d);
        w->lock = l;
        w->res = a == R6ETT_A_USER ? R6PW_R_TOOK_OVER : R6PW_R_REFUSED;
        w->a = a;
        ap.lock = l;
        ap_stop(a);
        return;
    }
    int a = ap_checks(t1, 1, &sw);                              /* S1-01: again, right before the call */
    if (a) { struct r6_ettr_w * w = ap_w_new(t1, kk, want, d); w->a = a; ap_stop(a); return; }
    /* S1-01: the decision at most R6ETTR_DEC_AGE_MS old at the call; review L3: its window content too (a keep republish,
     * 1.7, has a new t but the old t_meas); review TOR1-1: checked here with the reserve R6ETTR_PRECALL_MS for r6pw_write's
     * work before the call. Else no call, no ring entry; a newer decision decides (the deadline still applies) */
    int age = MAX(t1 - (int) d->t, t1 - (int) d->t_meas);
    if (age > R6ETTR_DEC_AGE_MS - R6ETTR_PRECALL_MS) { ap.stale++; return; }
    struct r6_ettr_w * w = ap_w_new(t1, kk, want, d);           /* ring w (5.1) */
    if (back) ap.back = 1; else ap.j = kk;
    ap.want = want; ap.d0 = *d; ap.H0 = d->H; ap.st0 = d->st; ap.t_call = t1;
    int r = _r6pw_write(want, R6PW_K_ETTR, kk, ap.no);          /* all 44 rows (row 5 with member kk and Sv), the call, 500 ms */
    w->res = r;
    w->lock = _r6pw_state()->last_lock;
    if (r == R6PW_R_TOOK_OVER) { ap_stop(R6ETT_A_USER); return; }
    if (r != R6PW_R_OK) { ap.lock = r; ap_stop(R6ETT_A_WRITE); return; }   /* r6_propw.c decides: restore / pending / open / dead */
    struct r6pw_rec e;
    memset(&e, 0, sizeof(e));
    int n = _r6pw_ring(0, &e);
    if (n) _r6pw_ring(n - 1, &e);
    ap.last = want;
    ap.t_conf = e.t + e.d_conf;                                 /* call + confirmation (18..50 ms, M10.2) */
    w->t_conf = ap.t_conf;
    ap.deadline = ap.t_conf + (ap.slow ? R6ETTR_SET_SLOW_MS : R6ETTR_SET_MS);
    ap.f_conf = 0;                                              /* from the first decision after t_conf (log) */
    ap.cls_last = d->cls;
    ap.phase = back ? AP_BACK : AP_SETTLE;
}

/* X12 "LV follows Tv" on H0 (window before the write) and H1 (after), k in thirds (+ brighter): 1 follows, 0 does not, 2
 * black stays black (no evidence, counted) */
static int ap_x12(int H0, int H1, int k, int tv_old, int tv_new, uint32_t s0, uint32_t s1)
{
    H0 = COERCE(H0, 0, 255);                                            /* table index (H is a level 0..255 anyway) */
    H1 = COERCE(H1, 0, 255);
    if (tv_old > ET_TV_4096 || tv_new > ET_TV_4096) return 1;          /* Canon's LV clamp 1/4096 (0xE09759C6) */
    if (H0 < ET_BLACK_H && H1 < ET_BLACK_H) return 2;                  /* black stays black: no evidence (counted) */
    int pred = ET_GRID * ABS(k), meas = (int) et_cal_d[MAX(H0, 1)] - (int) et_cal_d[MAX(H1, 1)];
    if (k < 0) meas = -meas;
    if (H0 >= 46 && H0 <= 247 && H1 >= 46 && H1 <= 247)                /* the measured chain: ratio */
        return 10 * meas >= (MAX(H0, H1) >= 200 ? 5 : 6) * pred && ABS(H1 - H0) > ET_WIN_DH;
    if (k > 0) return H1 >= H0 + 3 || H1 >= 250;                        /* foot / clipped: direction beyond the spread */
    return H1 <= H0 - 3 || (H0 >= 250 && (H1 < H0 || s1 < s0));
}

static void ap_commit_start(int now, const struct r6_ettr_dec * d, int end)
{
    ap.phase = AP_COMMIT;
    ap.t_commit0 = now;
    ap_dc = *d;
    ap_commit_end = end;
}

/* plan 4.1 step 7, S2 F14: the way back onto the newest value measured "not over", else the start value back. Review L2
 * (stricter than spec 2.6): v_ok counts only while it is shorter than every value measured over (Code768: larger =
 * shorter) and is not the current value (row 40); a scene that brightened during the run leaves A_OVER, the restore */
static void ap_way_back(int now, const struct r6_ettr_dec * d)
{
    int ok = ap.v_ok != 0x7FFF && ap.v_ok != ap.last && (ap.over_lim == 0x7FFF || ap.v_ok > ap.over_lim);
    if (ok && ap.v_ok != ap.base && !ap.back && ABS(ap.v_ok - ap.last) <= R6PW_ETTR_STEP)
    {
        if (ap_wcur) ap_wcur->end = R6ETT_E_BACKW;
        ap_write(now, ap.v_ok, d, 1);                           /* K_ETTR, member 6 */
    }
    else ap_stop(ap.v_ok == ap.base ? R6ETT_A_NOBETTER : R6ETT_A_OVER);   /* RESTORE_STOP to base */
}

/* a usable decision after a write (2.6 point 8): X12 once per write, then keep, the next step or the way back */
static void ap_decide(int now, const struct r6_ettr_dec * d)
{
    struct r6_ettr_w * w = ap_wcur;
    int over = d->st >= R6ETT_S_CLOSE;                          /* close, stau, hard */
    if (w)
    {
        w->H1 = MIN(d->H, 255);
        w->cls = d->cls;
        w->dmeas = (int) et_cal_d[MAX(w->H0, 1)] - (int) et_cal_d[MAX(w->H1, 1)];
        w->d_settle = MIN(MAX((int) (d->t_meas - ap.t_conf), 0), 0xFFFE);
        w->n_settle = ap.f_conf ? MIN(d->frame - ap.f_conf, 0xFFFEu) : 0xFFFF;
    }
    if (w && w->x12 == 0xFF)
    {
        int x = ap_x12(ap.H0, d->H, w->k, w->old, ap.last, ap.d0.s, d->s);
        w->x12 = x;
        if (x == 0) { st.apn.x12++; ap_stop(R6ETT_A_X12); return; }
        if (x == 2) { if (++ap.black_n >= 2) { st.apn.x12++; ap_stop(R6ETT_A_X12); return; } }
        else ap.black_n = 0;
    }
    if (!over) ap.v_ok = ap.last;
    else ap.over_lim = ap.over_lim == 0x7FFF ? ap.last : MAX(ap.over_lim, ap.last);
    if (ap.phase == AP_BACK)                                    /* Q2 a: after the way back only "not over" is kept */
    {
        if (!over) ap_commit_start(now, d, R6ETT_E_BACKKEPT);
        else ap_stop(R6ETT_A_BACKOVER);
        return;
    }
    if (d->st == R6ETT_S_TARGET || (!over && d->k == 0)) { ap_commit_start(now, d, R6ETT_E_KEPT); return; }
    int want = COERCE(d->want, ap.lo, ap.hi);
    if (!over && ap.over_lim != 0x7FFF && want <= ap.over_lim) want = ap.over_lim + ET_GRID;   /* never back onto "over" */
    if (want > ap.hi) want = ap.last;                           /* task A: nothing between "not over" and "over": keep */
    if (want == ap.last || ap.j >= R6PW_ETTR_N)
    {
        if (!over) ap_commit_start(now, d, R6ETT_E_KEPT);      /* "+x EV missing" (limit) or "Rest" (N reached / between) */
        else ap_way_back(now, d);
        return;
    }
    if (w) w->end = R6ETT_E_NEXT;
    ap_write(now, want, d, 0);
}

/* the kept value's notice (2.9) */
static void ap_kept(void)
{
    char t1[16], t2[16], t3[24], t4[24];
    int end = ap_commit_end;
    if (ap_wcur) ap_wcur->end = end;
    r6_ettr_tvs(t1, sizeof(t1), ap.last);
    r6_ettr_tvs(t2, sizeof(t2), ap.base);
    et_ev3(t3, sizeof(t3), (ap.base - ap.last) >> 8);
    int o = snprintf(et_msg, sizeof(et_msg), "ETTR: %s kept (was %s, %s).", t1, t2, t3);
    o = COERCE(o, 0, (int) sizeof(et_msg) - 1);
    const struct r6_ettr_dec * d = &ap_dc;
    if (end == R6ETT_E_BACKKEPT) snprintf(et_msg + o, sizeof(et_msg) - o, "\nBrighter values were over.");
    else if (d->lim && d->rest > 0 && d->want == ap.last)
    {
        const char * ge = (d->st == R6ETT_S_BLACK || d->st == R6ETT_S_FOOT || d->st == R6ETT_S_WIDE) ? ">=" : "";
        et_ev3(t4, sizeof(t4), d->rest);
        snprintf(et_msg + o, sizeof(et_msg) - o, "\nLimit %s: %s%s missing.", t1, ge, t4);
    }
    else if (ap.j >= R6PW_ETTR_N && d->st != R6ETT_S_TARGET && d->k > 0)
    {
        et_ev3(t4, sizeof(t4), d->k);
        snprintf(et_msg + o, sizeof(et_msg) - o, "\nRest %s after %d steps.", t4, R6PW_ETTR_N);
    }
    ap_box();
    if (end == R6ETT_E_BACKKEPT) st.apn.backkept++;
    else st.apn.kept++;
    ap_ui = AP_UI_KEPT;
    ap_end(end, 0);
}

/* AP_WAIT (2.3): as "Tv back to ..." (_r6iv_cmd_wait), with the Rev. 2 bounds: the ML menu opened again, 60 s; M11.1b (B2):
 * the base checks (2.8 points 1-7) every tick, after these two bounds */
static void ap_wait_tick(int now)
{
    if (!gui_menu_shown()) ap.closed = 1;
    else if (ap.closed) { ap_drop(R6ETT_A_REOPEN, 0); return; }
    if (now - (int) (ap.t_cmd & ~1) > R6ETTR_WAIT_MAX_MS) { ap_drop(R6ETT_A_LATE, 0); return; }
    struct et_sheet sh;
    int a = ap_basis_checks(&sh, 0, 1);                          /* M11.1b (B2): spec m11/m111b 2.9 (2.3 aligned with X16) */
    if (a) { ap_drop(a, 0); return; }
    int lw = ap.t_lvw;
    int w = _r6iv_cmd_wait(now, ap.t_cmd, &lw);
    ap.t_lvw = lw;
    if (w == R6IV_CW_CLOCK) { ap_drop(R6ETT_A_CLOCK, 0); return; }
    if (w == R6IV_CW_NOLV) { ap_drop(R6ETT_A_NOLV, 0); return; }
    if (w == R6IV_CW_STEADY) { ap_drop(R6ETT_A_LVSTEADY, 0); return; }
    if (w != R6IV_CW_GO) return;
    ap.phase = AP_MEASURE;
    ap.t_meas0 = now;
    ap.deadline = now + R6ETTR_SET_MS;
    et_ap_fast = 1;
}

/* AP_MEASURE (2.4): a settled decision made wholly after the wait, then write 1 (or no write) */
static void ap_measure_tick(int now)
{
    int sw = 0;
    int a = ap_checks(now, ap.armed, &sw);
    if (a) { ap_halt(a, 0); return; }
    if (!ap.armed)                                              /* X14: before the arm a half press only re-measures */
    {
        if (!sw) ap.t_hold = 0;
        else
        {
            ap.t_sw1 = now;
            if (!ap.t_hold) ap.t_hold = now | 1;
            else if (now - (int) (ap.t_hold & ~1) >= R6ETTR_HALF_MS) { ap_drop(R6ETT_A_HALF, 0); return; }
        }
    }
    struct r6_ettr_dec d;
    uint32_t seq;
    if (_r6ettr_dec_get(&d, &seq))
    {
        if (ap_hard(d.why, 0)) { ap_halt(R6ETT_A_ADV, d.why); return; }
        if (ap.numpix && d.numpix != ap.numpix) { ap_halt(R6ETT_A_DISP, 0); return; }   /* X10 (task A: also before write 1) */
        if (d.cls == 0 && !ap.slow) { ap.slow = 1; ap.deadline = ap.t_meas0 + R6ETTR_SET_SLOW_MS; }
        if (d.why == R6ETT_W_OK && d.tv == ap.b0.tv && (int) (d.t_win0 - ap.t_meas0) >= 0
            && (int) (d.t_win0 - (ap.t_sw1 + ET_T_ML)) > 0 && (int) (d.t_meas - ap.t_meas_used) > 0)
        {
            if (!ap.numpix) ap.numpix = d.numpix;
            if (d.beyond || d.tv < d.lo || d.tv > d.hi) { ap_halt(R6ETT_A_LIMITS, 0); return; }   /* X11 */
            ap.t_meas_used = d.t_meas;
            ap.dec_seq = seq;
            if (d.k == 0)                                       /* target, ~ok, limit reached, still over: no write */
            {
                ap_nowrite_text(&d);
                if (ap.armed) { ap_stop(R6ETT_A_NONEED); return; }   /* an earlier attempt was stale: DONE, no call */
                ap_box();
                st.apn.nowrite++;
                ap_ui = AP_UI_OK;
                ap_end(R6ETT_E_NOWRITE, 0);
                return;
            }
            ap_write(now, d.want, &d, 0);
            if (ap.phase != AP_MEASURE) return;                 /* written, or the run ended */
        }
    }
    if ((int) (now - ap.deadline) >= 0) ap_halt(R6ETT_A_NOSETTLE, 0);
}

/* AP_SETTLE / AP_BACK (2.6) */
static void ap_settle_tick(int now)
{
    int sw = 0;
    int a = ap_checks(now, 1, &sw);
    if (a) { ap_stop(a); return; }
    int16_t u;
    if (_r6pw_user_evidence(ap.last, &u)) { ap_stop(R6ETT_A_USER); return; }   /* the restore finds your value */
    struct r6_ettr_dec d;
    uint32_t seq;
    if (_r6ettr_dec_get(&d, &seq))
    {
        if (!ap.f_conf && (int) (d.t - ap.t_conf) >= 0) { ap.f_conf = d.frame ? d.frame : 1; if (ap_wcur) ap_wcur->f_conf = d.frame; }
        if (d.numpix != ap.numpix) { ap_stop(R6ETT_A_DISP); return; }             /* X10 */
        if ((int) (now - (ap.t_conf + R6ETTR_424_MS)) >= 0                        /* X18: Canon's AE took the Tv */
            && !(d.tv424_for == ap.last && d.t_424ok && (int) (d.t_424ok - (ap.t_conf + R6ETTR_424_MS)) <= 0))
        {
            st.apn.a424++;
            ap_stop(R6ETT_A_424);
            return;
        }
        if (d.cls != ap.cls_last)                               /* a class change: the bound moves, never beyond the cap */
        {
            ap.cls_last = d.cls;
            ap.t_cls = now;
            if (d.cls == 0) ap.slow = 1;
            uint32_t cap = ap.t_conf + (ap.slow ? R6ETTR_SET_SLOW_MS : R6ETTR_SET_CAP_MS);
            uint32_t ext = now + (d.cls ? R6ETTR_CLS_EXT_MS : R6ETTR_CLS_EXT_SLOW_MS);
            if ((int) (ext - ap.deadline) > 0) ap.deadline = ext;
            if (d.cls == 0 && (int) (ap.t_conf + R6ETTR_SET_SLOW_MS - ap.deadline) > 0) ap.deadline = ap.t_conf + R6ETTR_SET_SLOW_MS;
            if ((int) (ap.deadline - cap) > 0) ap.deadline = cap;
        }
        if (ap_hard(d.why, 1)) { ap.lock = d.why; ap_stop(R6ETT_A_ADV); return; }
        if (d.why == R6ETT_W_OK && d.tv == ap.last && (int) (d.t_ref - ap.t_call) >= 0
            && (int) (d.t_win0 - (ap.t_conf + ET_T_ML)) >= 0 && d.tv424 == ap.last && (int) (d.t_meas - ap.t_meas_used) > 0)
        {
            ap.t_meas_used = d.t_meas;
            ap.dec_seq = seq;
            ap_decide(now, &d);
            if (ap.phase != AP_SETTLE && ap.phase != AP_BACK) return;   /* kept / written / ended; a stale attempt stays */
        }
    }
    if ((int) (now - ap.deadline) >= 0) ap_stop(R6ETT_A_NOSETTLE);
}

/* AP_COMMIT (2.7): keep the value (no call) once nothing speaks against it; card or M not settled: <= 5 s */
static void ap_commit_tick(int now)
{
    int sw = 0;
    int a = ap_checks(now, 1, &sw);
    if (a) { ap_stop(a); return; }
    struct r6_ettr_dec d;
    uint32_t seq;
    if (_r6ettr_dec_get(&d, &seq))
    {
        /* Rev. 2 (S1-09): a newer settled decision "over" before the commit: not kept */
        if (d.why == R6ETT_W_OK && d.tv == ap.last && (int) (d.t_win0 - (ap.t_conf + ET_T_ML)) >= 0
            && (int) (d.t_meas - ap.t_meas_used) > 0 && d.st >= R6ETT_S_CLOSE) { ap_stop(R6ETT_A_LATEOVER); return; }
        if (ap_hard(d.why, 1)) { ap.lock = d.why; ap_stop(R6ETT_A_ADV); return; }
    }
    int l = _r6pw_commit(ap.last);
    if (!l) { ap_kept(); return; }
    if ((l == R6PW_L_JOB || l == R6PW_L_VIEWS) && now - (int) ap.t_commit0 < R6ETTR_COMMIT_MS) return;
    int16_t u;
    ap.lock = l;
    if (l == R6PW_L_LAST) a = _r6pw_user_evidence(ap.last, &u) ? R6ETT_A_USER : R6ETT_A_TVCHG;
    else a = l == R6PW_L_SIG ? R6ETT_A_SHUTDOWN : R6ETT_A_COMMIT;
    ap_stop(a);
}

/* a command or a run refused in r6_iv_task (iv.ref src 8) */
static void ap_refused_task(int a)
{
    char r[72];
    ap_req = 0;
    r6_iv_ref_add(R6IV_REF_ETTR_TASK, a, 0);
    ap_reason(r, sizeof(r), a, 0);
    snprintf(et_msg, sizeof(et_msg), "ETTR dropped: %s.\nNothing written.", r);
    ap_box();
    st.apn.dropped++;
    ap_ui = AP_UI_DROPPED;
}

void _r6ettr_run_begin(int now)
{
    if (!et_iv_task()) return;                                  /* Rev. 2 (S1-04) */
    uint32_t r = ap_req;
    ap_req = 0;
    int t0 = (int) (r & ~1);
    if (!r || now - t0 >= R6ETTR_REQ_MS || ap.phase != AP_IDLE) { ap_refused_task(R6ETT_A_LATE); return; }
    if (t0 - now > R6ETTR_SKEW_MS) { ap_refused_task(R6ETT_A_CLOCK); return; }   /* review TOR1-2: a clock jump */
    __sync_synchronize();                                       /* ap_cmd_b0 was written before ap_req (menu task) */
    memset(&ap, 0, sizeof(ap));
    ap.tv0 = ap.base = ap.last = ap.want = ap.v_ok = ap.over_lim = ap.lo = ap.hi = 0x7FFF;
    ap.cls_last = -2;
    et_trash = 0;                                               /* a press of an earlier run does not stop this one */
    ap_wcur = 0;
    ap.no = ++et_run_no;
    /* review TOR1-2: the menu task (GuiMainTask 0x17 preempts r6_iv_task 0x1a, or the other core) may store the command
     * after this tick read its clock: then the wait counts from r6_iv_task's time (never later than the command: the 60-s
     * bound is never longer; the 3-s wait at most R6ETTR_SKEW_MS shorter while the ML menu holds anyway) */
    ap.t_cmd = t0 - now > 0 ? (uint32_t) (now | 1) : r;
    ap.b0 = ap_cmd_b0;
    ap.hn0 = ap.hn = ap_cmd_hn;                                  /* M11.1b (B2): every delivery after the command's newest */
    ap.mv_v = 0x7FFF;
    ap.tv0 = ap.b0.tv;
    ap_ui = AP_UI_NONE;
    et_msg[0] = 0;
    ap.phase = AP_WAIT;
}

void _r6ettr_run_refused(int now, int a)
{
    if (!et_iv_task()) return;
    ap_refused_task(a);
    (void) now;
}

void _r6ettr_run_stop(int now, int a)
{
    if (!et_iv_task() || ap.phase == AP_IDLE) return;
    ap_notes0 = _r6pw_state()->notes;
    ap_halt(a, 0);
    (void) now;
}

void _r6ettr_run_tick(int now)
{
    if (!et_iv_task() || ap.phase == AP_IDLE) return;
    ap_notes0 = _r6pw_state()->notes;
    if (ap.phase == AP_WAIT) ap_wait_tick(now);
    else if (ap.phase == AP_MEASURE) ap_measure_tick(now);
    else if (ap.phase == AP_SETTLE || ap.phase == AP_BACK) ap_settle_tick(now);
    else if (ap.phase == AP_COMMIT) ap_commit_tick(now);
}

/* the ML menu's open task (src/menu.c menu_task -> menu_open -> r6_iv_menu_block; task A: the spec named GuiMainTask, the
 * key handler, which only gives the semaphore): one store, only during a run. Both ML menu tasks are accepted: the flag
 * can only stop a run (the restore of E4), and Q1 a must hold whichever of the two runs menu_open */
void _r6ettr_trash(void)
{
    if (!(et_open_task() || et_menu_task()) || !r6_ettr_menu_lock()) return;
    et_trash = 1;
}

/* ---- the proposal as text (menu and LV line) ---- */
#define ET_LV_FONT FONT(FONT_MED, COLOR_WHITE, COLOR_BLACK)   /* = r6_grid.c GR_ET_FONT */
static void et_text(char * s, int n, const struct r6_ettr_adv * a, int lv)
{
    char t1[16], t2[24], t3[16];
    const char * px = lv ? "ETTR " : "";
    if (a->why != R6ETT_W_OK) { snprintf(s, n, "%s%s", lv ? "ETTR: " : "", r6_ettr_why_name(a->why)); return; }
    r6_ettr_tvs(t1, sizeof(t1), a->want);
    if (a->beyond)                                                /* M11.1 (1.3): no counter step */
    {
        r6_ettr_tvs(t1, sizeof(t1), a->k_raw > 0 ? a->lo : a->hi);
        snprintf(s, n, "%sbeyond limit %s", px, t1);
    }
    else if (a->lim && a->rest)
    {
        /* black / foot (fixed +3 EV) and wide (capped): the step is only a lower bound of the deficit */
        const char * ge = (a->st == R6ETT_S_BLACK || a->st == R6ETT_S_FOOT || a->st == R6ETT_S_WIDE) ? ">=" : "";
        const char * fl = a->lim == R6ETT_L_FLASH ? " (flash?)" : "";
        et_ev3(t2, sizeof(t2), a->rest);
        snprintf(s, n, "%sLimit %s%s: %s%s missing", px, t1, fl, ge, t2);
        /* too wide for the LV box, or too long for the menu value (MENU_MAX_VALUE_LEN; QEMU m111 W53: "... EV miss"):
         * without "(flash?)"; ett.dec keeps the reason (lim R6ETT_L_FLASH) */
        if (fl[0] && (lv ? bmp_string_width(ET_LV_FONT, s) + 4 > R6ETTR_LV_WMAX : (int) strlen(s) >= MENU_MAX_VALUE_LEN))
            snprintf(s, n, "%sLimit %s: %s%s missing", px, t1, ge, t2);
    }
    else if (a->k == 0 && a->st == R6ETT_S_TARGET) snprintf(s, n, "%sok %s", px, t1);
    else if (a->k == 0) snprintf(s, n, "%s~ok %s (<1/3 under)", px, t1);   /* near below the band, rounded down */
    else if (a->st == R6ETT_S_BLACK) snprintf(s, n, "%sdark: >=+3 EV, %s", px, t1);
    else
    {
        et_ev3(t2, sizeof(t2), a->k);
        if (a->st == R6ETT_S_HARD) { et_pct(t3, sizeof(t3), a->s); snprintf(s, n, "%sOVER %s: %s (%s)", px, t3, t1, t2); }
        else snprintf(s, n, "%s%s (%s)", px, t1, t2);
    }
    /* the LV line's hints, order Tv HTP ALO L FL (1.6; M11.1b B3: no [ISO], the bit stays in the log and the menu's info) */
    if (lv && (a->hint & (R6ETT_H_TV | R6ETT_H_HTP | R6ETT_H_ALO | R6ETT_H_ISOL | R6ETT_H_FLICK)))
    {
        int l = strlen(s);
        snprintf(s + l, n - l, " [%s%s%s%s%s", a->hint & R6ETT_H_TV ? "Tv " : "", a->hint & R6ETT_H_HTP ? "HTP " : "",
            a->hint & R6ETT_H_ALO ? "ALO " : "", a->hint & R6ETT_H_ISOL ? "L " : "", a->hint & R6ETT_H_FLICK ? "FL " : "");
        l = strlen(s);
        if (l && s[l - 1] == ' ') s[l - 1] = ']';                /* "[HTP ALO " -> "[HTP ALO]" */
    }
}

int r6_ettr_lv_wanted(void) { return et_mode && et_lv; }

int r6_ettr_lv_text(char * buf, int n)
{
    if (!et_mode || !et_lv || r6_iv_in_pic()) return 0;           /* nothing while a picture runs (as R6 Motion) */
    if (ap.phase >= AP_MEASURE)                                   /* M11.1: the run (1.6) */
    {
        char t[16];
        if (ap.j || ap.back) { r6_ettr_tvs(t, sizeof(t), ap.last); snprintf(buf, n, "ETTR apply %d/%d: %s", ap.j, R6PW_ETTR_N, t); }
        else snprintf(buf, n, "ETTR apply: measuring");
        return 1;
    }
    struct r6_ettr_adv a = st.shown;
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
    else if (et_mode == 2 && !_r6pw_on()) MENU_SET_WARNING(MENU_WARN_ADVICE, "Apply once needs Interval > Tv writes ON.");
}

static MENU_UPDATE_FUNC(et_lim_update)
{
    if (COERCE(et_long, 0, ET_TV_N - 1) > COERCE(et_short, 0, ET_TV_N - 1))
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Longest Tv is shorter than Shortest Tv: no proposal.");
    else
        MENU_SET_WARNING(MENU_WARN_INFO, "The simulation limit G applies too: G = 30 s at ISO 100, 2 s at ISO 1600.");
}

/* M11.1 (2.2): the command, GuiMainTask. Each refusal: the menu text and iv.ref src 7, no run */
static void et_apply_refuse(int a, int l)
{
    ap_ref_a = a;
    ap_ref_l = l;
    ap_ui = AP_UI_REFUSED;
    st.apn.refused++;
    r6_iv_ref_add(R6IV_REF_ETTR, a, l);
}

static MENU_SELECT_FUNC(et_apply_select)
{
    struct et_sheet sh;
    struct r6_ettr_adv lim;
    int l = 0, lw = 0, hw = 0, len = 0;
    int16_t tv = 0;
    if (!et_menu_task()) { et_apply_refuse(R6ETT_A_CALLER, 0); return; }
    if (et_mode != 2) { et_apply_refuse(R6ETT_A_MODE, 0); return; }
    if (!_r6pw_on()) { et_apply_refuse(R6ETT_A_TVW_OFF, 0); return; }
    if (r6_ettr_busy()) { et_apply_refuse(R6ETT_A_BUSY, 0); return; }
    if (r6_iv_running() || r6_iv_md_running()) { et_apply_refuse(R6ETT_A_SERIES, 0); return; }
#ifdef FEATURE_LV_BUFFER_PROBE
    if (lvprobe_snapshot_busy()) { et_apply_refuse(R6ETT_A_SNAP, 0); return; }
#endif
    if (is_movie_mode() || lv_movie_select == LVMS_ENABLE_MOVIE || shooting_mode != SHOOTMODE_M
        || shooting_mode_custom != SHOOTMODE_M || !et_sheet_read(&sh) || sh.ae != 3) { et_apply_refuse(R6ETT_A_NOTM, 0); return; }
    if (_r6pw_locks(R6PW_WANT_NONE, R6PW_CLASS_HARD, &l)) { et_apply_refuse(R6ETT_A_TVW, l); return; }
    uint32_t hn = et_h_newest(&tv, &len);
    if (!hn || len != 2 || (tv & (ET_GRID - 1)) || tv != sh.tv) { et_apply_refuse(R6ETT_A_TV, 0); return; }
    memset(&lim, 0, sizeof(lim));
    et_limits(&lim, &sh, &lw, &hw);                               /* X11, fresh (beyond implies Tv outside lo .. hi) */
    if (lim.lo > lim.hi || tv < lim.lo || tv > lim.hi) { et_apply_refuse(R6ETT_A_LIMITS, 0); return; }
    struct et_basis b;
    et_basis(&b, &sh, 0);                                         /* the run basis (Rev. 2 S1-10), Tv as delivered */
    b.tv = tv;
    b.len = len;
    b.numpix = 0;
    ap_cmd_b0 = b;
    ap_cmd_hn = hn;                                               /* M11.1b (B2): with ap_cmd_b0, before ap_req */
    __sync_synchronize();
    ap_req = get_ms_clock() | 1;                                  /* Rev. 2 (S1-07): the request before the command */
    __sync_synchronize();
    int c = _r6iv_ettr_cmd();
    if (c != R6IV_CAM_OK) { ap_req = 0; et_apply_refuse(c == R6IV_CAM_CALLER ? R6ETT_A_CALLER : R6ETT_A_BUSY, 0); return; }
    ap_ui = AP_UI_NONE;
    et_msg[0] = 0;
    st.apn.cmds++;
}

static MENU_UPDATE_FUNC(et_apply_update)
{
    char r[72], t[16];
    int ph = ap.phase;
    if (ph == AP_WAIT || (ph == AP_IDLE && r6_ettr_busy())) MENU_SET_VALUE("pending: close menu");
    else if (ph == AP_MEASURE) MENU_SET_VALUE("measuring");
    else if (ph != AP_IDLE) MENU_SET_VALUE("step %d/%d", ap.j, R6PW_ETTR_N);
    else if (ap_ui == AP_UI_KEPT) { r6_ettr_tvs(t, sizeof(t), ap.last); MENU_SET_VALUE("kept %s", t); }
    else if (ap_ui == AP_UI_OK) MENU_SET_VALUE("ok");
    else if (ap_ui == AP_UI_DROPPED) MENU_SET_VALUE("dropped");
    else if (ap_ui == AP_UI_STOPPED) MENU_SET_VALUE("stopped");
    else if (ap_ui == AP_UI_REFUSED) MENU_SET_VALUE("refused");
    else MENU_SET_VALUE("-");
    if (ph == AP_IDLE && ap_ui == AP_UI_REFUSED)
    {
        ap_reason(r, sizeof(r), ap_ref_a, ap_ref_l);
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Apply refused: %s.", r);
    }
    else if (ph == AP_IDLE && et_msg[0]) MENU_SET_WARNING(MENU_WARN_INFO, "%s", et_msg);
    else if (et_mode != 2) MENU_SET_WARNING(MENU_WARN_INFO, "Set Mode to Apply first.");
}

static MENU_UPDATE_FUNC(et_prop_update)
{
    char s[64];
    if (!et_mode) { MENU_SET_VALUE("Mode Off"); return; }
    struct r6_ettr_adv a = st.shown;                              /* M11.1: with the hysteresis (1.6) */
    int pre = a.why == R6ETT_W_NOLV && et_pm_get(&a);         /* M12.0 (spec m12/m120 5.2) */
    if (a.why == R6ETT_W_NOLV && gui_menu_shown())             /* LV paused by this menu, no valid copy */
    {
        MENU_SET_VALUE("paused in menu");
        MENU_SET_WARNING(MENU_WARN_INFO, "Live view pauses while this menu is open; the proposal returns a few seconds after closing.");
        return;
    }
    et_text(s, sizeof(s), &a, 0);
    MENU_SET_VALUE("%s", s);
    if (a.why == R6ETT_W_OK)
    {
        int age = MAX(get_ms_clock() - (int) a.t_meas, 0);
        uint32_t t10 = et_div(age + 50, 100);
        if (pre) MENU_SET_RINFO("pre-menu %ds", (int) et_div(age, 1000));   /* M12.0: the copy from before the ML menu */
        else MENU_SET_RINFO("%d.%ds", (int) et_div(t10, 10), (int) (t10 - 10 * et_div(t10, 10)));
        char tv[16];
        r6_ettr_tvs(tv, sizeof(tv), a.tv);
        int h = a.hint & ~R6ETT_H_RANGE;
        if (h & (R6ETT_H_TV | R6ETT_H_HTP | R6ETT_H_ALO | R6ETT_H_ISOL | R6ETT_H_FLICK))   /* M11.1b: ISO is no warning */
            MENU_SET_WARNING(MENU_WARN_ADVICE, "Inexact:%s%s%s%s%s. Measured at %s, H %d.", h & R6ETT_H_TV ? " Tv<1/500" : "",
                h & R6ETT_H_HTP ? " HTP (more RAW)" : "", h & R6ETT_H_ALO ? " ALO on" : "", h & R6ETT_H_ISOL ? " ISO L" : "",
                h & R6ETT_H_FLICK ? " flicker" : "", tv, (int) a.H);
        else if (h & R6ETT_H_STYLE)
            MENU_SET_WARNING(MENU_WARN_INFO, "Measured at %s: H %d (%s); the curve is for picture style Standard.", tv,
                (int) a.H, r6_ettr_st_name(a.st));
        else if (h & R6ETT_H_ISO)                                 /* M11.1b (B3): a neutral info, measured in the M11.1 test */
            MENU_SET_WARNING(MENU_WARN_INFO, "Measured at %s: H %d (%s). ISO>=12800: measured, no extra error.", tv, (int) a.H,
                r6_ettr_st_name(a.st));
        else if (h & R6ETT_H_STYLE_NA)
            MENU_SET_WARNING(MENU_WARN_INFO, "Measured at %s: H %d (%s); picture style not readable: curve for Standard.", tv,
                (int) a.H, r6_ettr_st_name(a.st));
        else
            MENU_SET_WARNING(MENU_WARN_INFO, "Measured at %s: H %d (%s).", tv, (int) a.H, r6_ettr_st_name(a.st));
    }
    else if (a.why == R6ETT_W_SETTLE || a.why == R6ETT_W_MOVING)
        MENU_SET_WARNING(MENU_WARN_INFO, "Waits for 3 calm measurements over 0.4 s (3 s below 25 fps), 1 s after a change.");
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
        .name   = "ETTR",
        .select = menu_open_submenu,
        .help   = "M11.1: proposes a Tv from Canon's LV histogram (R, G, B); Apply once writes it.",
        .help2  = "M, ExpSim on, ISO fixed. Apply: Tv only, through Interval's Tv writes; ML never presses.",
        .children = (struct menu_entry[]) {
            {
                .name    = "Mode",
                .priv    = &et_mode,
                .max     = 2,
                .choices = CHOICES("Off", "Display only", "Apply"),
                .update  = et_mode_update,
                .help    = "Apply: also 'Apply once' writes Tv (Interval > Tv writes ON). Never at startup.",
                .help2   = "Log: Debug > LV probe > Log snapshot writes ETTnnnnn.LOG next to LVPnnnnn.LOG.",
            },
            {
                .name      = "Apply once",
                .select    = et_apply_select,
                .update    = et_apply_update,
                .icon_type = IT_ACTION,
                .help      = "Measures, writes Tv up to 5 steps, keeps a value that is not over.",
                .help2     = "Close menu: starts 3 s after live view runs 3 s. Reopening drops it. Trash stops it.",
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
                .help    = "No proposal longer than this (default 30 s; tripod). G = 30 s at ISO 100, 2 s at ISO 1600.",
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
