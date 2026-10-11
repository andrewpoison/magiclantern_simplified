/*
 * R6.190 M11.0: Auto-ETTR advisor, ML menu Shoot > "R6 ETTR" (r6_ettr.c, CONFIG_R6_ETTR; docs/m11_plan.md 1, 2.1 .. 2.10,
 * 3.1, 6, answers of 2026-10-07). Measures Canon's LV histogram per channel, shows a proposed Tv, logs the measurement.
 * Used by r6_overlays.c (the sampling task), r6_interval.c (the read-only bracket trace), r6_grid.c (the LV line) and
 * lvprobe.c (ETTnnnnn.LOG).
 * M11.1 (spec m11/m111 spec_m111.md, answers E3 / E4 / E6 / F2 of 2026-10-07, Q1..Q4 of 2026-10-08): "Apply once", on the
 * user's menu command only: measure, write Tv through Tor 1 (r6_propw.c, kind K_ETTR, context CTX_ETTR), measure again;
 * at most 5 steps and 1 way back (at most 6 calls per command with the restores), keeps only a value measured settled
 * and "not over", else the start value back. The run steps in r6_iv_task (r6_interval.c command 6). Never presses.
 */
#ifndef _r6_ettr_h_
#define _r6_ettr_h_

#include <stdint.h>

#define R6ETTR_TICK_MS      50     /* sampling tick in r6_hist_task while Mode is not Off (else R6H_TICK_MS) */
#define R6ETTR_MEAS_MS     250     /* advisor measurement: 4 per second (plan 2.9); M11.1: every tick during a run */
#define R6ETTR_RAW_N        32     /* raw trace ticks per Tv delivery (32 x 50 ms = 1.6 s, plan 6.2 ring raw) */
#define R6ETTR_RAW_RING     12     /* Tv deliveries kept (a test bracket has 10 writes) */
#define R6ETTR_M_RING      128     /* ring m: every second advisor measurement (2 per s, about 64 s) */
#define R6ETTR_BK_RING      32     /* ring bk: bracket trace entries (after a member's confirmation / before its SW1): 2 per
                                    * member, plan 6.1 "16 pairs"; a 9-member bracket (18 entries) fits whole (QEMU V17) */
#define R6ETTR_FY_RING      32     /* ring fy: FaceYuv 2x2 counts, 1 per s while the Expo log is ON */
#define R6ETTR_WIN_N        80     /* M11.1: the measurement window, a ring (3 s at 50 ms + reserve, spec m11/m111 1.5) */

/* M11.1 "Apply once" (spec m11/m111 1.9, 2) */
#define R6ETTR_SET_MS        4000  /* >= 25 frames/s: no usable settled decision this long after the confirmation (or the
                                    * measuring start): stop */
#define R6ETTR_SET_SLOW_MS  10000  /* < 25 frames/s (answer F2 b): the bound, never longer (Rev. 2, S1-12) */
#define R6ETTR_SET_CAP_MS    6000  /* >= 25 frames/s: the bound after a frame-rate class change, never longer */
#define R6ETTR_CLS_EXT_MS    2500  /* a class change: the bound at least this far from now (class 1 / 2) ... */
#define R6ETTR_CLS_EXT_SLOW_MS 7000   /* ... (class 0) */
#define R6ETTR_424_MS        1000  /* X18: rd+0x424 holds the written Tv at the latest this long after the confirmation */
#define R6ETTR_COMMIT_MS     5000  /* the commit waits this long for the card / a stable M (2.7) */
#define R6ETTR_HALF_MS       5000  /* X14: a half press held this long before the arm drops the command */
#define R6ETTR_W_RING          16  /* ring w: the ETTR writes (also refused ones), log */
#define R6ETTR_DEC_AGE_MS     500  /* the decision a call rests on, also its window content (review L3: a keep republish has a
                                    * new time, old measurements), is at most this old at the call (Rev. 2, S1-01) */
#define R6ETTR_PRECALL_MS     100  /* review TOR1-1: ap_write checks that age with this reserve for r6pw_write's work before
                                    * the call (up to 3 x R6PW_STABLE_MS sleeps, the marker size read at the run's first write,
                                    * two read pairs), so the bound above holds at the call itself */
#define R6ETTR_REQ_MS        2000  /* a command request r6_iv_task did not take expires (Rev. 2, S1-07) */
#define R6ETTR_SKEW_MS        100  /* review TOR1-2: the menu task can run between r6_iv_task's clock read and its read of the
                                    * command; a request up to this much "after" that clock is taken at r6_iv_task's time, a
                                    * larger one is a clock jump (dropped, A_CLOCK) */
#define R6ETTR_WAIT_MAX_MS  60000  /* AP_WAIT: the command is dropped after this (Rev. 2, S1-11) */
#define R6ETTR_K_BACK           6  /* member index of the way back in the write ring */

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
    R6ETT_M_IDLE,        /* LV idle (X6, M11.1): the DISP set fixed >= 0.5 s, FrameNum fixed >= 1 s, Y mean < 4 with the set
                          * fixed >= 0.25 s, or rd[0x608] < -9 EV */
    R6ETT_M_RANGE,       /* M11.1 (X5 strict): [0x1427F8] not readable or bit 4 (simulation out of range) set */
    R6ETT_NM
};

/* the advisor's state (plan 2.3, decided on the window maximum of H, s, b) */
enum {
    R6ETT_S_NONE = 0,
    R6ETT_S_BLACK,       /* H < 8 and Y mean < 4: a bound, +3 EV */
    R6ETT_S_FOOT,        /* H < 32: +3 EV */
    R6ETT_S_WIDE,        /* under, more than 1 EV to Z_o (M11.1: et_cal_d): aim at the band's lower end, +1 .. +4 EV */
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
    R6ETT_W_RANGE,       /* M11.1 (X5 strict): LV simulation out of range ([0x1427F8] bit 4 or not readable) */
    R6ETT_NW
};

/* hints (E9: logged and shown, never a refusal) */
#define R6ETT_H_HTP     0x01   /* [p+0x4FC] != 0: highlight tone priority */
#define R6ETT_H_ALO     0x02   /* byte [p+0xA8] != 3: ALO effective */
#define R6ETT_H_ISOL    0x04   /* ISO below 100 (L): G uses ISO 100 */
#define R6ETT_H_FLICK   0x08   /* anti-flicker on and Tv (now or proposed) shorter than 1/100 */
#define R6ETT_H_STYLE   0x10   /* M11.1: a picture style other than Standard (raw 0x81), known: the curve is Standard's */
#define R6ETT_H_RANGE   0x20   /* bit 4 of [0x1427F8] (simulation out of range); M11.1: also R6ETT_M_RANGE, log bit */
#define R6ETT_H_ISO     0x40   /* Sv >= ISO 12800: log bit and menu info (M11.1b: not in the LV line) */
#define R6ETT_H_STYLE_NA 0x80  /* M11.1: picture style not delivered (raw 0): menu info only */
#define R6ETT_H_TV      0x100  /* M11.1 (Q3 a): Tv now or proposed shorter than 1/500, LV vs picture not measured: "[Tv]" */

/* the limit that clamped a proposal; M11.1: + the flash cap (hi <= 1/200 while a flash input is not 0) */
enum { R6ETT_L_NONE = 0, R6ETT_L_LONG, R6ETT_L_SHORT, R6ETT_L_WIN, R6ETT_L_CANON, R6ETT_L_G, R6ETT_L_FLASH };

/* M11.1: the phases of an "Apply once" run (spec m11/m111 2.1) */
enum { R6ETTR_AP_IDLE = 0, R6ETTR_AP_WAIT, R6ETTR_AP_MEASURE, R6ETTR_AP_SETTLE, R6ETTR_AP_BACK, R6ETTR_AP_COMMIT };

/* M11.1: why a command was refused, a run dropped (before the arm) or stopped (after it); names <= 30 characters */
enum {
    R6ETT_A_NONE = 0,
    R6ETT_A_MODE, R6ETT_A_TVW_OFF, R6ETT_A_BUSY, R6ETT_A_SERIES, R6ETT_A_SNAP, R6ETT_A_NOTM, R6ETT_A_TVW, R6ETT_A_TV,
    R6ETT_A_LIMITS, R6ETT_A_ADV, R6ETT_A_CALLER, R6ETT_A_NOLV, R6ETT_A_LVSTEADY, R6ETT_A_CLOCK, R6ETT_A_TVCHG,
    R6ETT_A_SETCHG, R6ETT_A_PIC, R6ETT_A_DISP, R6ETT_A_HALF, R6ETT_A_GUI, R6ETT_A_TRASH, R6ETT_A_NOSETTLE,
    R6ETT_A_SHUTDOWN, R6ETT_A_X12, R6ETT_A_424, R6ETT_A_WRITE, R6ETT_A_USER, R6ETT_A_OVER, R6ETT_A_NOBETTER,
    R6ETT_A_BACKOVER, R6ETT_A_COMMIT, R6ETT_A_JOB, R6ETT_A_LATEOVER, R6ETT_A_REOPEN, R6ETT_A_LATE, R6ETT_A_NONEED,
    R6ETT_NA
};

/* M11.1: the end of one write (ring w) and of a run (r6_ettr_run.end); 11 only for a run dropped before the arm */
enum {
    R6ETT_E_OPEN = 0,    /* not decided yet */
    R6ETT_E_NEXT,        /* the next step was written */
    R6ETT_E_KEPT,        /* kept (_r6pw_commit) */
    R6ETT_E_BACKKEPT,    /* the way back kept */
    R6ETT_E_BACKW,       /* the way back was written after it */
    R6ETT_E_RESTORED,    /* stopped, the start value back (OK) or M had it already (DONE) */
    R6ETT_E_PEND,        /* stopped, the restore pending (D3) */
    R6ETT_E_OFFER,       /* stopped, the "Tv back" offer */
    R6ETT_E_USER,        /* stopped, the user's value stays */
    R6ETT_E_TOR1,        /* stopped, Tor 1's state (open / dead / signal) */
    R6ETT_E_NOWRITE,     /* no write needed (target, ~ok, limit reached, still over) */
    R6ETT_E_DROP,        /* the run was dropped before the arm (no write) */
};

/* M11.1: the run basis at the command (X16) and the advisor's restart basis (M11.0) */
struct et_basis { int16_t tv, av; uint32_t sv, ae, drive, htp, alo, flick, numpix; int shots, fn, len; };

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
    uint8_t H, valid, rr, flags;    /* flags: bit 0..1 bind, 2 same record, 3 fresh, 4 in the window, 5 range bit 4; M11.1:
                                     * 6 an "Apply once" run is active, 7 bound only through the record (bit 2, not 0 + 1) */
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
    int dz, dw;              /* log only: D(H) - D(Z_o), D(H) - D(band lower end) (M11.1: et_cal_d; Code768, uncapped) */
    /* M11.1 */
    int beyond;              /* 1 = Tv already beyond lo / hi in the step's direction: k 0, no counter step (1.3) */
    uint32_t t_win0;         /* ms of the oldest window entry used */
};

/* M11.1 (spec m11/m111 1.7): one advisor step as r6_iv_task takes it, published by r6_hist_task (seqlock) */
struct r6_ettr_dec {
    uint32_t t, t_ref, t_win0, t_meas;   /* decision, freshness reference, oldest / newest window entry */
    int16_t tv, want;                /* delivered Tv the window measured; the one step (0x7FFF none) */
    int16_t lo, hi;                  /* limits (Code768, lo = longest) */
    uint8_t why, st, lim, beyond;    /* R6ETT_W_*, R6ETT_S_*, R6ETT_L_*, 1 = Tv beyond lo / hi in step direction */
    int8_t k, rest, cls, n;          /* thirds, the part a limit cut, frame-rate class, window entries */
    uint16_t H, ym10, span, hspread; /* window maxima, span (ms), max - min H */
    uint32_t s, b;                   /* window maxima (ppm) */
    uint32_t numpix, frame, fps10;   /* numPix of the newest good read (X10), FrameNum of the newest read, frame rate */
    int16_t tv424, tv424_for;        /* newest valid read's rd+0x424; the Tv the 424 watch refers to */
    uint32_t t_424ok;                /* first valid read since that Tv's delivery with tv424 == tv424_for (0 none) */
    uint8_t valid, set;              /* newest measurement R6ETT_M_*, DISP set */
    uint16_t hint;                   /* R6ETT_H_* */
    uint16_t sv;                     /* Sv of the sheet the decision used (low 16 bits of [p+0x510]; the arm, 2.5) */
    uint16_t pad;
};

/* M11.1 ring w (spec m11/m111 5.1): one ETTR write, also a refused one */
struct r6_ettr_w {
    uint32_t t, t_conf, f_conf;     /* attempt; confirmation (0 none); FrameNum then (first decision after t_conf) */
    uint16_t d_settle, n_settle;    /* ms / frames from t_conf to the deciding window (0xFFFF none) */
    int16_t old, want, lo, hi;      /* Code768 */
    uint8_t j, no, st, why;         /* 1..5, 6 = way back; run; state / advisor reason of the decision before */
    int8_t k, cls;                  /* thirds; class at the decision after */
    uint8_t res, lock;              /* R6PW_R_* / R6PW_L_* */
    uint8_t H0, H1, x12, end;       /* window max before / after; X12 result 0/1/2 (0xFF: not yet, once per write); R6ETT_E_* */
    int16_t dpred, dmeas;           /* 256 k, D(H0) - D(H1) (et_cal_d) */
    uint8_t a, pad[3];              /* R6ETT_A_* of the end (0 = none) */
};                                  /* 44 B (the field list of spec 5.1; its "48 B" was miscounted) */

/* M11.1: an "Apply once" run (spec m11/m111 2.1); only r6_iv_task writes it, menu and log read it */
struct r6_ettr_run {
    int phase, no, end, why;           /* R6ETTR_AP_*, run number (= ev of the ring entries), R6ETT_E_*, R6ETT_A_* */
    uint32_t t_cmd, t_lvw, t_meas0, t_call, t_conf, deadline, t_cls, t_sw1, t_commit0, f_conf;
    int16_t tv0, base, last, want, v_ok, over_lim, lo, hi;   /* Code768; 0x7FFF none */
    int j, back, slow, black_n;        /* steps written, way back written, class 0 seen since t_conf, black->black steps */
    uint32_t numpix, dec_seq;          /* display path of the run; seq of the decision used last */
    uint32_t t_meas_used;              /* d.t_meas of the decision used last ("new" = newer window, 1.7) */
    int armed, closed;                 /* _r6pw_arm_ettr done (the arm is not repeated); ML menu seen closed in AP_WAIT */
    int nwait, stale;                  /* write 1 deferred by a wait row before the arm; attempts dropped as stale */
    uint16_t sv; int16_t pad1;         /* Sv at the arm (= S.e_sv) */
    struct r6_ettr_dec d0;             /* the decision the newest write rested on (log, QEMU invariants) */
    struct et_basis b0;                /* basis at the command (X16): tv, av, sv, ae, drive, htp, alo, flick, shots, fn */
    int16_t H0; uint8_t st0, pad;      /* window max / state before the newest write */
    /* implementation (task A) */
    uint32_t t_hold;                   /* start (| 1) of a half press held before the arm (X14), 0 = none */
    int cls_last;                      /* frame-rate class of the newest decision since t_conf (2.6 point 6) */
    int lock;                          /* the end's Tor 1 row (A_TVW) / result (A_WRITE) / advisor reason (A_ADV) */
    /* M11.1b (B2): the Tv deliveries since the command (et_h ring), r6_iv_task only */
    uint32_t hn0, hn;                  /* et_hn at the command; the next delivery not yet looked at */
    uint32_t mv_t;                     /* time of the first foreign delivery (0 none) */
    int16_t mv_v;                      /* its value (0x7FFF none) */
    uint8_t mv;                        /* 0 none, 1 a delivery with another value / length, 2 ET_HRING or more since the last look */
    uint8_t sh_n;                      /* AP_WAIT ticks in a row whose only Tv difference is the sheet (K11) */
};

/* M11.1: "Apply once" counters since boot (log ett.apn) */
struct r6_ettr_apn {
    uint32_t cmds, refused, dropped, nowrite, kept, backkept, restored, pend, offer, user, tor1, trash, x12, a424;
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
    int range;                                /* [0x1427F8] (-1 not readable); M11.1: X5 strict (R6ETT_M_RANGE) */
    struct r6_ettr_adv adv;
    struct r6_ettr_meas cur;                  /* the newest measurement of the task */
    /* M11.1 */
    struct r6_ettr_adv shown;                 /* what the menu and the LV line show (hysteresis, spec m11/m111 1.6) */
    int hyst_want;                            /* a new proposal seen once, waiting for its second (0x7FFF none) */
    struct r6_ettr_apn apn;                   /* "Apply once" counters */
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
int r6_ettr_mode(void);                                   /* the Mode setting (0 Off, 1 Display only, M11.1: 2 Apply) */
void r6_ettr_cfg_text(char * buf, int n);                 /* the compiled-in limits, comma separated (log ett.consts) */
int r6_ettr_ring_m(int i, struct r6_ettr_m * e);          /* i = 0 oldest; the count (<= R6ETTR_M_RING) */
int r6_ettr_ring_raw(int i, struct r6_ettr_raw * e);      /* the count (<= R6ETTR_RAW_RING) */
int r6_ettr_ring_bk(int i, struct r6_ettr_bk * e);        /* the count (<= R6ETTR_BK_RING) */
int r6_ettr_ring_fy(int i, struct r6_ettr_fy * e);        /* the count (<= R6ETTR_FY_RING) */
const char * r6_ettr_why_name(int w);
int r6_ettr_pm_text(char * s, int n);    /* M12.0: ett.pm (the pre-menu proposal copy: ok,t_meas,want,saves,shows,drops) */
const char * r6_ettr_st_name(int s);
const char * r6_ettr_valid_name(int v);
void r6_ettr_tvs(char * buf, int n, int tv);              /* Code768 -> "1/125" ("Tv+x.y" off the 1/3 grid) */
uint32_t r6_ettr_lv_count(int draw);                      /* r6_grid.c: count a draw (1) / erase (0) of the LV line */

/* M11.1 "Apply once" (spec m11/m111 2.11). Read only, exported as before (no state change) */
int r6_ettr_busy(void);            /* any task: a fresh command request (ap_req < R6ETTR_REQ_MS) or a run is active */
int r6_ettr_menu_lock(void);       /* any task: phase >= AP_MEASURE (ML menu closed, a Trash press stops the run) */
int r6_ettr_run_active(void);      /* any task: phase != AP_IDLE */
const struct r6_ettr_run * r6_ettr_run(void);
int r6_ettr_ring_w(int i, struct r6_ettr_w * e);          /* i = 0 oldest; the count (<= R6ETTR_W_RING) */
const char * r6_ettr_a_name(int a);
const char * r6_ettr_msg(void);    /* the newest "Apply once" notice ("" none) */
/* Rev. 2 (S1-04): everything that changes run state starts with an underscore (platform/Makefile keeps such names out of
 * magiclantern.sym, so no module can call them) and checks the task name first */
void _r6ettr_run_begin(int now);   /* r6_iv_task only, command 6; starts only with a fresh ap_req (2.3) */
void _r6ettr_run_refused(int now, int a);   /* r6_iv_task only, command 6 not taken (iv.ref src 8); clears ap_req */
void _r6ettr_run_stop(int now, int a);      /* r6_iv_task only, a series started (never expected) */
void _r6ettr_run_tick(int now);    /* r6_iv_task only, every iv_tick while active */
void _r6ettr_trash(void);          /* ML's menu tasks only (menu_task: menu_open -> r6_iv_menu_block; GuiMainTask): one
                                    * volatile store, only while menu_lock */
int _r6ettr_dec_get(struct r6_ettr_dec * d, uint32_t * seq);   /* read only, any task: 1 = a stable copy */

#endif /* _r6_ettr_h_ */
