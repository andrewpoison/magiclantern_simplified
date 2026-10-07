/*
 * R6.190 M11.0: Auto-ETTR advisor, ML menu Shoot > "R6 ETTR" (r6_ettr.c, CONFIG_R6_ETTR; docs/m11_plan.md 1, 2.1 .. 2.10,
 * 3.1, 6, answers of 2026-10-07). Measures Canon's LV histogram per channel, shows a proposed Tv, logs the measurement.
 * Never writes a property, never presses. Used by r6_overlays.c (the sampling task), r6_interval.c (the read-only
 * bracket trace), r6_grid.c (the LV line) and lvprobe.c (ETTnnnnn.LOG).
 */
#ifndef _r6_ettr_h_
#define _r6_ettr_h_

#include <stdint.h>

#define R6ETTR_TICK_MS      50     /* sampling tick in r6_hist_task while Mode is not Off (else R6H_TICK_MS) */
#define R6ETTR_MEAS_MS     250     /* advisor measurement: 4 per second (plan 2.9) */
#define R6ETTR_RAW_N        32     /* raw trace ticks per Tv delivery (32 x 50 ms = 1.6 s, plan 6.2 ring raw) */
#define R6ETTR_RAW_RING     12     /* Tv deliveries kept (a test bracket has 10 writes) */
#define R6ETTR_M_RING      128     /* ring m: every second advisor measurement (2 per s, about 64 s) */
#define R6ETTR_BK_RING      32     /* ring bk: bracket trace entries (after a member's confirmation / before its SW1): 2 per
                                    * member, plan 6.1 "16 pairs"; a 9-member bracket (18 entries) fits whole (QEMU V17) */
#define R6ETTR_FY_RING      32     /* ring fy: FaceYuv 2x2 counts, 1 per s while the Expo log is ON */
#define R6ETTR_WIN_N         8     /* the measurement window (valid, fresh measurements since the last reset) */

/* the LV line (r6_grid.c): ML coordinates, FONT_MED, right of the small waveform box, above Canon's bottom bar */
#define R6ETTR_LV_X        290
#define R6ETTR_LV_Y        358
#define R6ETTR_LV_WMAX     426     /* box width at most (x + w <= 716) */

/* bracket trace kinds (r6_ettr_trace) */
#define R6ETTR_BK_CONF       1     /* right after a member's Tv write was confirmed (R6PW_R_OK) */
#define R6ETTR_BK_SW1        2     /* right before the member's picture (iv_shoot: SW1 ON) */

/* one measurement's validity (ring m, raw, bk): why it does not count */
enum {
    R6ETT_M_OK = 0,
    R6ETT_M_NOLV,        /* Canon's LV not running (or ML's bulb picture) */
    R6ETT_M_READ,        /* the histogram read failed (rr = r6_overlays.h R6H_*: no set, bad pointer, sum, no FrameNum) */
    R6ETT_M_TORN,        /* the published set changed during every read (3 tries) */
    R6ETT_M_UNBOUND,     /* the newest RandomData record's histogram pointer rd[0x5EC] was not the copied set before and
                          * after the read (plan 2.1: FrameNum / flags belong to another frame) */
    R6ETT_M_NOSIM,       /* rd[0x5FC] (fValidExposureSim) != 1 (X4) */
    R6ETT_M_EXPSIM,      /* ML's _expsim or Canon's [p+0x548] != 1 (X3) */
    R6ETT_M_MAGNIFY,     /* lv_dispsize != 1 (X10) */
    R6ETT_M_NUMPIX,      /* numPix not one of the known displays (X10) */
    R6ETT_M_IDLE,        /* LV idle: < 12 frames / s over >= 0.5 s, or rd[0x608] < -9 EV (X6) */
    R6ETT_NM
};

/* the advisor's state (plan 2.3, decided on the window maximum of H, s, b) */
enum {
    R6ETT_S_NONE = 0,
    R6ETT_S_BLACK,       /* H < 8 and Y mean < 4: a bound, +3 EV */
    R6ETT_S_FOOT,        /* H < 32: +3 EV */
    R6ETT_S_WIDE,        /* under, more than 1 EV to Z_o by the sRGB start curve: aim at 200, +1/3 .. +4 EV */
    R6ETT_S_NEAR,        /* under, at most 1 EV to Z_o: aim at Z_o, at most +1 EV, rounded down (0: "under <1/3 EV") */
    R6ETT_S_TARGET,      /* in the target band: ok */
    R6ETT_S_CLOSE,       /* Z_o < H <= 247: -1/3 EV */
    R6ETT_S_STAU,        /* H >= 248, bin 255 <= q in every channel: -1/3 EV, H >= 252: -2/3 EV */
    R6ETT_S_HARD,        /* bin 255 > q: -max(1/3, min(3, log2(1 + p^2))) EV, p = s in % */
    R6ETT_NS
};

/* why there is no proposal (0 = there is one) */
enum {
    R6ETT_W_OK = 0,
    R6ETT_W_OFF,         /* Mode Off */
    R6ETT_W_SERIES,      /* R6 Interval / R6 Motion runs: only the read-only trace */
    R6ETT_W_TVW,         /* the Tv write path is armed, busy, open or a restore is pending */
    R6ETT_W_NOLV,        /* no LV */
    R6ETT_W_NOTM,        /* not M */
    R6ETT_W_AUTOISO,     /* ISO Auto: a Tv proposal is meaningless */
    R6ETT_W_P,           /* Canon's settings sheet p not readable */
    R6ETT_W_EXPSIM,      /* ExpSim off (X3) */
    R6ETT_W_MAGNIFY,     /* magnified (X10) */
    R6ETT_W_NUMPIX,      /* unknown display size (X10) */
    R6ETT_W_TV,          /* Tv unknown or not on the 1/3 grid */
    R6ETT_W_LIMITS,      /* longest Tv shorter than the shortest (after the window / Canon / G) */
    R6ETT_W_NOREAD,      /* no valid read for a while (no histogram set, torn, bad pointer) */
    R6ETT_W_NOSIM,       /* rd[0x5FC] = 0 (X4) */
    R6ETT_W_UNBOUND,     /* the reads were not bound to their frame (X4) */
    R6ETT_W_IDLE,        /* LV idle (X6) */
    R6ETT_W_SETTLE,      /* after a change of Tv / settings / frame rate: not fresh yet (X7, plan 2.7) */
    R6ETT_W_MOVING,      /* the window is not calm (X8, plan 2.8) */
    R6ETT_NW
};

/* hints (E9: logged and shown, never a refusal) */
#define R6ETT_H_HTP     0x01   /* [p+0x4FC] != 0: highlight tone priority */
#define R6ETT_H_ALO     0x02   /* byte [p+0xA8] != 3: ALO effective */
#define R6ETT_H_ISOL    0x04   /* ISO below 100 (L): G uses ISO 100 */
#define R6ETT_H_FLICK   0x08   /* anti-flicker on and Tv (now or proposed) shorter than 1/100 */
#define R6ETT_H_STYLE   0x10   /* picture style without a calibration (in M11.0: every style) */
#define R6ETT_H_RANGE   0x20   /* bit 4 of [0x1427F8] (simulation out of range): log only until M11.0 confirms it (X5) */

/* the limit that clamped a proposal */
enum { R6ETT_L_NONE = 0, R6ETT_L_LONG, R6ETT_L_SHORT, R6ETT_L_WIN, R6ETT_L_CANON, R6ETT_L_G };

/* one channel of one measurement, one pass from bin 255 down (plan 2.2): the levels L with count(>= L) > threshold */
struct r6_ettr_ch {
    uint16_t pq, p999, p99, p50, p5;   /* thresholds q * numPix, 0.1 %, 1 %, 50 %, 95 % */
    uint16_t mean10;                   /* mean level x 10 */
    uint32_t n224, n240, n248, n252, n255;   /* pixels >= 224 / 240 / 248 / 252, in bin 255 */
};

/* one full measurement (the advisor's newest, the log's "cur") */
struct r6_ettr_meas {
    uint32_t t, us;          /* ms; read + statistics in us */
    uint8_t valid, rr, set, bind;   /* R6ETT_M_*; R6H_* of the read (-1 torn: 0xFF); DISP set; bit 0 / 1: rd[0x5EC] == the
                                     * set's Y pointer before / after the read, bit 2: same record before and after */
    uint8_t tries, sim, pad0, pad1; /* reads (torn retries); rd[0x5FC] */
    uint32_t numpix, frame;  /* RandomData FrameNum (record after the read) */
    uint32_t rd_b, rd_a, hp_b, hp_a, p0;   /* records before / after, their histogram pointers, the set's Y pointer */
    uint32_t ae600, fr52c;   /* rd[0x600] (AE counter ~8 Hz), rd[0x52C] (GmtAeRecFrameRate) */
    int16_t m608, tv424;     /* rd[0x608] (Canon's meter, s16 Code768), rd+0x424 (GmtAeParam Tv, s16) */
    uint32_t H, s, b;        /* max of P_R / P_G / P_B; max share >= 248 (ppm of numPix); max share in bin 255 (ppm) */
    struct r6_ettr_ch ch[4]; /* Y, R, G, B */
};

/* ring m (2 per s): one advisor measurement and the decision at that moment */
struct r6_ettr_m {
    uint32_t t, frame;
    int16_t tv, want;        /* delivered Tv, the proposal (0x7FFF none) */
    uint8_t H, valid, rr, flags;    /* flags: bit 0..1 bind, 2 same record, 3 fresh, 4 in the window, 5 range bit 4 */
    uint8_t pr, pg, pb, sim;
    uint16_t s, b;           /* 0.01 % */
    uint16_t ym10, fps10;
    uint8_t st, why;         /* R6ETT_S_*, R6ETT_W_* */
    int8_t k, set;           /* proposal in thirds; DISP set */
    uint16_t ae600;          /* low 16 bits */
    int16_t tv424, m608;
    uint8_t rng, pad;        /* [0x1427F8] low byte at this measurement (bit 0 sim, 1 no sim, 4 out of range; 0xFF not
                              * readable), the tail padding: 40 B as before (plan 6.1) */
};

/* ring raw: the trace of one Tv delivery (plan 6.2, S2 F1): head + R6ETTR_RAW_N ticks */
struct r6_ettr_rt {          /* one tick, 16 B */
    uint16_t dt;             /* ms since the delivery */
    uint16_t dfr;            /* FrameNum - FrameNum at the start */
    int16_t tv424;
    uint16_t ym10;
    uint16_t s;              /* 0.01 % */
    uint8_t H, flags;        /* flags: valid (R6ETT_M_*) | set << 4 | bound << 5 | same record << 6 */
    uint8_t ae;              /* rd[0x600] - rd[0x600] at the start (low 8 bits) */
    uint8_t pr, pg, pb;
};
struct r6_ettr_raw {
    uint32_t t;              /* ms of the delivery (handler) */
    int16_t old, tv;         /* the Tv before / delivered */
    uint8_t src, n, len, f0ok; /* 1 = ML's write (newest r6_propw.c ring entry wants this value), 0 = dial / Canon; ticks;
                                * length; f0 / ae0 taken (the first good read) */
    uint32_t f0, wn;         /* FrameNum at the first tick; r6_propw.c ring entries written (_r6pw_ring_n) */
    uint16_t ae0, t0;        /* rd[0x600] at the first tick; ms from the delivery to the first tick */
    struct r6_ettr_rt tk[R6ETTR_RAW_N];
};

/* ring bk: the bracket trace (plan 2.10 questions 1 and 2) */
struct r6_ettr_bk {
    uint32_t t, frame;
    uint8_t what, k, ev, valid;     /* R6ETTR_BK_*, member, event (low 8 bits), R6ETT_M_* */
    int16_t want, tv;        /* the member's Tv, delivered Tv */
    int32_t fn;              /* file number */
    uint8_t H, pr, pg, pb;
    uint16_t s, b;           /* 0.01 % */
    uint16_t ym10, us;
    uint8_t set, bind, sim, pad;
    int16_t tv424, m608;
    uint16_t ae600, pad2;
};

/* ring fy: FaceYuv 2 x 2 (second-newest frame, the same pixels for channels and union; plan 2.1, S2 F13) */
struct r6_ettr_fy {
    uint32_t t, f2, us;
    uint8_t ok, mr, mg, mb;  /* 1 = read good (probes, hand-outs, time); maxima R / G / B */
    uint8_t my, pad0; uint16_t pad1;
    uint32_t npx;            /* pixels counted */
    uint32_t r250, g250, b250, u250;   /* R / G / B >= 250, max(R, G, B) >= 250 */
    uint32_t y235, y245, y250, y255;
};

/* the advisor (newest decision; menu, LV line, log) */
struct r6_ettr_adv {
    uint32_t t;              /* ms of the decision */
    uint32_t t_meas;         /* ms of the newest measurement in the window */
    int why, st, k, k_raw, lim, rest, hint;   /* R6ETT_W_*, R6ETT_S_*, thirds (+ = longer), before the limits, R6ETT_L_*,
                                               * the part the limit cut (thirds, + = brightness missing) */
    int16_t tv, want;        /* the Tv the window measured, the proposal */
    int16_t lo, hi;          /* the allowed range (Code768, lo = longest) */
    int16_t g;               /* the simulation limit (Code768) for this ISO */
    int16_t sv;              /* Canon's Sv (Code768, low 16 bits of [p+0x510]) */
    uint32_t H, s, b, ym10;  /* window maxima */
    int n, span;             /* window entries used, their span (ms) */
    int hspread;             /* max - min H in the window (plan 2.3 margin, measured) */
    int dz, dw;              /* log only: sRGB start curve D(H) - D(Z_o), D(H) - D(200) (Code768, uncapped, 256 = 1/3 EV) */
};

struct r6_ettr_stats {
    int mode, target, q_ppm, zo, lo;          /* settings now; Z_o and the band's lower end */
    int longest, shortest;                    /* Code768 */
    uint32_t ticks, meas, meas_ok, tries, torn, unbound, retries;
    uint32_t nv[R6ETT_NM];                    /* measurements per validity */
    uint32_t why_n[R6ETT_NW];                 /* decisions per reason */
    uint32_t us_last, us_max, us_avg;         /* one measurement (read + statistics) */
    uint32_t bk_us_max, bk_n;                 /* the bracket trace (r6_iv_task) */
    uint32_t resets, fresh_n;                 /* window resets (Tv / settings / frame rate / gap), fresh measurements */
    int fps10, fps_cls;                       /* frame rate x 10 over >= 0.5 s; class 0 idle, 1 ~30, 2 ~60 */
    uint32_t t_ref, f_ref, ae_ref;            /* freshness reference (plan 2.7); f_ref 0 = not yet */
    int ref_why;                              /* 1 Tv, 2 settings, 3 frame rate, 4 gap, 5 start */
    uint32_t h_n, h_ovf;                      /* PROP_SHUTTER deliveries seen by the task, missed (ring overflow) */
    int16_t tv, tv_len;                       /* the newest delivered Tv and its length */
    uint32_t raw_n, m_n, bk_ring_n, fy_n;     /* ring entries written since boot */
    uint32_t fy_torn, fy_us_max;
    uint32_t lv_draws, lv_erases;             /* the LV line (r6_grid.c) */
    int range;                                /* [0x1427F8] (-1 not readable), log only */
    struct r6_ettr_adv adv;
    struct r6_ettr_meas cur;                  /* the newest measurement of the task */
};

/* r6_hist_task (r6_overlays.c), never r6_iv_task */
int r6_ettr_tick_ms(void);                    /* R6ETTR_TICK_MS while Mode is not Off, else 0 */
void r6_ettr_tick(void);
/* r6_iv_task, bracket events only (r6_interval.c iv_event): read only, no sleep, no drawing, no card I/O; nothing while
 * Mode is Off */
void r6_ettr_trace(int what, int ev, int k, int want);
/* overlay task (r6_grid.c): 1 = the LV line is wanted (Mode not Off, Show in LV ON, no picture running), its text in
 * buf; r6_ettr_lv_wanted: the settings only (r6_grid_wanted) */
int r6_ettr_lv_text(char * buf, int n);
int r6_ettr_lv_wanted(void);
/* lvprobe.c (snapshot task): one full measurement with the bins copied into bins (no ring touched) */
int r6_ettr_read_full(uint32_t (*bins)[256], struct r6_ettr_meas * m);
const struct r6_ettr_stats * r6_ettr_stats(void);
int r6_ettr_mode(void);                                   /* the Mode setting (0 Off, 1 Display only) */
void r6_ettr_cfg_text(char * buf, int n);                 /* the compiled-in limits, comma separated (log ett.consts) */
int r6_ettr_ring_m(int i, struct r6_ettr_m * e);          /* i = 0 oldest; the count (<= R6ETTR_M_RING) */
int r6_ettr_ring_raw(int i, struct r6_ettr_raw * e);      /* the count (<= R6ETTR_RAW_RING) */
int r6_ettr_ring_bk(int i, struct r6_ettr_bk * e);        /* the count (<= R6ETTR_BK_RING) */
int r6_ettr_ring_fy(int i, struct r6_ettr_fy * e);        /* the count (<= R6ETTR_FY_RING) */
const char * r6_ettr_why_name(int w);
const char * r6_ettr_st_name(int s);
const char * r6_ettr_valid_name(int v);
void r6_ettr_tvs(char * buf, int n, int tv);              /* Code768 -> "1/125" ("Tv+x.y" off the 1/3 grid) */
uint32_t r6_ettr_lv_count(int draw);                      /* r6_grid.c: count a draw (1) / erase (0) of the LV line */

#endif /* _r6_ettr_h_ */
