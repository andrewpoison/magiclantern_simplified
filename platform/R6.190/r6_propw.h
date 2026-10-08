#ifndef _r6_propw_h_
#define _r6_propw_h_

/* R6.190 M10.2 Tor 1: the Tv write path (r6_propw.c, CONFIG_R6_PROPW_TV, spec m10/m102 spec_m102.md section 3).
 * Only r6_interval.c (bracket, "Tv back to ...", the press block), r6_ettr.c (M11.1 "Apply once", spec m11/m111 3) and
 * lvprobe.c (log) use this header. Every name is
 * _r6pw_* (platform/Makefile keeps names "_[a-zA-Z]*" out of magiclantern.sym, K29): nothing here is exported to
 * modules or Lua. Every function that can lead to a write checks that it runs in r6_iv_task. */

#include <stdint.h>

/* limits and timings (spec 3.1; R6PW_ID / R6PW_LEN stay in r6_propw.c, its only user) */
#define R6PW_TV_MIN        (-3840)     /* 0xF100 = 30 s   (user answer 9b) */
#define R6PW_TV_MAX        9984        /* 0x2700 = 1/8000 */
#define R6PW_GRID          256         /* 1/3 stop in Code768 (0x100) */
#define R6PW_CONFIRM_MS    500         /* confirmation window (K10: expected a few ms) */
#define R6PW_POLL_MS       10
#define R6PW_SPACING_MS    300         /* call to call (plan 2.5) */
#define R6PW_SETTLE_MS     200         /* LV adapts before the next press (plan 2.5 step 6) */
#define R6PW_STABLE_MS     10          /* two reads at least this far apart make a "stable" Tv (3.6a, K11) */
#define R6PW_HOLD_MS       3000        /* every write: ML menu closed and LV running at least this (row 12) */
#define R6PW_PEND_MS       120000      /* a deferred restore is given up after this (3.8, then "Tv back" offer) */
#define R6PW_XSYNC_CAP     0x1700      /* 1/200: no new value faster than this unless row 43 shows no flash */
#define R6PW_RING          32          /* write ring (log) */
#define R6PW_HRING         16          /* handler deliveries (PropMgr task -> r6_iv_task) */

/* write contexts (lock row 5) */
#define R6PW_CTX_BKT        1      /* bracket series / "Test: 1 bracket" (Mode Bracket, M) */
#define R6PW_CTX_BACK       2      /* "Tv back to ..." (an offer is open) */
#define R6PW_CTX_PEND       3      /* a stopped series whose restore is pending (only RESTORE_PEND to base, 3.8) */
#define R6PW_CTX_ETTR       4      /* M11.1: R6 ETTR "Apply once" (spec m11/m111 3) */

/* M11.1 (spec m11/m111 3.1; answers E3 / E6 of 2026-10-07): one ETTR command, row 5 checks it a second time */
#define R6PW_ETTR_N         5      /* answer E6: steps per command; with the one way back at most N + 1 calls, D3 included */
#define R6PW_ETTR_STEP      3072   /* 4 EV: step up (member 1..5) and the way back (member 6), row 5 */
#define R6PW_ETTR_DOWN      2304   /* 3 EV: step down, member 1..5 (E3 / E6), row 5 (Rev. 2, S1-08) */
#define R6PW_ETTR_GSV       7680   /* G = MAX(Sv, ISO 100) - 7680: 30 s at ISO 100 (row 5, Rev. 2, S1-02) */
#define R6PW_SV_ISO100      3840   /* Sv of ISO 100 in Code768 */
#define R6PW_ETTR_WAIT_MS   1000   /* _r6pw_wait_ready for K_ETTR: the bound of the remaining wait rows (Rev. 2, S1-01) */

/* lock classes (3.5): HARD = the rows that hold also with the ML menu open (menu task, Start / Test / display);
 * ALL = every row, the task before every write */
#define R6PW_CLASS_HARD     1
#define R6PW_CLASS_ALL      2
#define R6PW_WANT_NONE      0x7FFF  /* _r6pw_locks: no new value (row 40 skipped) */

/* kinds of one write (3.12) */
enum { R6PW_K_NONE = 0, R6PW_K_STEP, R6PW_K_RESTORE, R6PW_K_RESTORE_STOP, R6PW_K_RESTORE_PEND, R6PW_K_CLAMPFIX,
       R6PW_K_BACK, R6PW_K_ETTR, R6PW_NKINDS };   /* M11.1: K_ETTR = 7, appended: pw_restore_kind (3..6) excludes it */

/* results (3.7) */
enum { R6PW_R_NONE = 0, R6PW_R_OK, R6PW_R_REFUSED, R6PW_R_DONE, R6PW_R_NOCONF, R6PW_R_SIG, R6PW_R_FOREIGN,
       R6PW_R_FOREIGN_OPEN, R6PW_R_CLAMP, R6PW_R_DEP, R6PW_R_MODE, R6PW_R_MODE_OPEN, R6PW_R_LANDED, R6PW_R_INVOLVED,
       R6PW_R_TOOK_OVER, R6PW_R_LATE_APPLIED, R6PW_R_LATE_SEEN, R6PW_NRES };

/* locks: the row numbers of spec 3.5 (evaluation order 1..37, 42, 43, 44, 38..41), row 2's reasons after row 44 */
enum r6pw_lock {
    R6PW_L_NONE = 0, R6PW_L_OFF, R6PW_L_DEAD, R6PW_L_SIG, R6PW_L_TASK, R6PW_L_ARMED, R6PW_L_SPACING, R6PW_L_MARKER,
    R6PW_L_PTR, R6PW_L_MODE, R6PW_L_MOVIE, R6PW_L_CCSTATE, R6PW_L_GUI, R6PW_L_PRESS, R6PW_L_JOB, R6PW_L_ISO,
    R6PW_L_AEB, R6PW_L_WBB, R6PW_L_HDR, R6PW_L_ME, R6PW_L_FBKT, R6PW_L_MSNR, R6PW_L_ESHUT, R6PW_L_TIMER, R6PW_L_STEP,
    R6PW_L_SUBST, R6PW_L_INIT, R6PW_L_C62C, R6PW_L_CTX, R6PW_L_B78, R6PW_L_SLOT28, R6PW_L_SLOT59, R6PW_L_SLOT0F,
    R6PW_L_SLOT2D, R6PW_L_SLOT20, R6PW_L_SLOT66, R6PW_L_VIEWS, R6PW_L_OLD, R6PW_L_BKUP_AEB, R6PW_L_BKUP_ALO,
    R6PW_L_NEW, R6PW_L_TORN, R6PW_L_MECHA, R6PW_L_FLASH, R6PW_L_LAST,
    R6PW_L_LIMITED, R6PW_L_OPEN, R6PW_L_PEND, R6PW_L_OFFER, R6PW_L_BOOT, R6PW_L_MKEPT, R6PW_NLOCKS
};

/* ring flags (r6pw_rec.fl) */
#define R6PW_FL_OPEN    0x01   /* the request is open after this entry */
#define R6PW_FL_MODE    0x02   /* mode_left seen in the window */
#define R6PW_FL_MLFIRST 0x04   /* ML's value was delivered before the foreign one */
#define R6PW_FL_OVF     0x08   /* handler ring overflow in the window */
#define R6PW_FL_USER    0x10   /* user evidence (TOOK_OVER, FOREIGN) */
#define R6PW_FL_MOVIE   0x20   /* movie_left (the movie position of the mode dial) */
#define R6PW_FL_STICKY  0x40   /* lock 44 failed sticky (fgn_m / rec_moved / ovf) without user evidence */

/* 3.8 (plus the event / menu fields M10.2 needs; read-only for the callers) */
struct r6pw_state {
    int armed;            /* 0, R6PW_CTX_BKT, R6PW_CTX_BACK, R6PW_CTX_PEND (row 5) */
    int16_t base;         /* the user's Tv for this arm (series base / marker base); 0x7FFF = none */
    int16_t last;         /* the value ML's last resolved request produced in M; = base after a restore; 0x7FFF = none */
    uint32_t t_res, t_ev; /* ms of that resolution; evidence reference (t_res, or t_h after FOREIGN; 3.6a) */
    int fgn_m, rec_moved, ovf;   /* sticky summary since t_res (3.3), read by lock 44 */
    int open;             /* = pw_inflight: a request is open (3.7) */
    int16_t open_want, close_v, close_rec, pad0;
    uint32_t open_t;      /* t_call of the open request */
    int watch, w_other, w_recv, w_want, w_land;   /* open-request watch (3.3, 3.7) */
    int pend, pend_why;   /* restore pending (deferred) and the stop reason */
    uint32_t pend_t0;     /* when it became pending (R6PW_PEND_MS bound) */
    uint32_t pend_cnt, pend_fn;  /* shot counter / file number at pend_t0 (cancel baseline) */
    int dead, dead_why;   /* writes off until reboot (= pw_dead) */
    int lim;              /* = pw_lim: only the one restore to base (LANDED_OTHER, CLAMP) */
    int offer;            /* "Tv back to" offered (boot marker, or a cancelled pending restore) */
    int16_t boot_rec;     /* rec_tv when the offer was made */
    int16_t pad1;
    int boot_done;        /* the boot check (3.10) has finished; arm refused before (row 2 BOOT) */
    int marker;           /* 0 none, 1 written by this arm, 2 kept (unclean), 3 found at boot, -1 write failed */
    int mk_boot;          /* 1: this boot wrote a marker */
    int se_done;          /* _r6pw_series_end already ran for this arm */
    uint8_t landed_ae, pad2; int16_t landed_v;   /* LANDED_OTHER: mode and value (also in the marker, 3.9); the newest record */
    int landed_ok;        /* that mode was seen with stable-mode(landed_ae) != landed_v */
    uint8_t landed_ae2, pad4; int16_t landed_v2; /* review round 2 (S2-1): an older unresolved record of another mode, kept
                                                  * when a second LANDED_OTHER comes (0xFF: a third mode, "Tv, B and Fv") */
    int landed_ok2;       /* as landed_ok for it (never for 0xFF) */
    /* M10.2 implementation */
    int ev_on, ev_no;     /* a bracket event runs (ML menu blocked, 4.7); its number */
    int busy;             /* a restore of a stopped event runs (ML menu blocked) */
    int last_res, last_lock;     /* result / lock of the newest write attempt */
    int16_t u_last; int16_t pad3; uint32_t u_t; int u_ae;   /* newest delivered Tv (len 2), its time, AE then */
    int uns;              /* unsettled Tv reads (3.6a, pw.uns) */
    uint32_t calls;       /* _prop_request_change calls since boot */
    int sig;              /* abort latches: 1 PROP_ABORT, 2 PROP_TERMINATE_SHUT_REQ, 4 PROP_SHUTDOWN_REASON */
    int back_res, back_lock;     /* last "Tv back" attempt */
    /* M11.1 (appended: the M10.2 fields keep their offsets) */
    int e_ctx;            /* ctx of the newest arm (kept through CTX_PEND); 4: the calls below count */
    int16_t e_lo, e_hi;   /* the ETTR limits of the arm (row 5) */
    int e_calls;          /* _prop_request_change calls since the newest arm while e_ctx == 4 (row 5: < R6PW_ETTR_N + 1) */
    int mk_ctx;           /* ctx in the marker line this arm wrote (pw_mk_landed, texts) */
    uint32_t notes;       /* NotifyBox notices given by pw_note */
    uint32_t commits;     /* _r6pw_commit successes */
    int commit_lock;      /* the newest _r6pw_commit refusal (0 = none) */
    int16_t e_sv;         /* Sv (low 16 bits of [p+0x510]) at the arm, checked fresh against it (row 5, Rev. 2 S1-02) */
    uint8_t e_steps, e_back;   /* posted K_ETTR calls with member 1..5 / member 6 since the arm (row 5, Rev. 2 S1-08) */
    uint32_t e_okm[2];    /* bit i: Tv i thirds from 30 s was confirmed OK by a K_ETTR write of this arm (row 5, member 6) */
};

/* one write attempt (3.12) */
struct r6pw_rec {
    uint32_t t;                   /* ms of the attempt / call */
    uint16_t d_ret, d_h, d_cc, d_mm, d_conf;   /* ms after t: return, first handler delivery of want, first [p+0x61E] ==
                                                * want, first rec/cpy == want, confirmed; 0xFFFF = never */
    int16_t  old, want, rep, cc, mm, ds4;      /* rep = last handler value; cc = [p+0x61E]; mm = rec_tv; ds4 = cpy_tv (at the end) */
    int16_t  fval, bak;           /* first foreign value (0 none); tvbak_val after */
    uint8_t  kind, res, lock, k;  /* R6PW_K_*, R6PW_R_*, R6PW_L_* (0 = none), member index (0xFF: not a member) */
    uint8_t  ev, ndel, nfor, state;   /* event number (low 8 bits), Tv deliveries in the window, foreign ones, CC state at the call */
    uint8_t  sl0, sl1, c62c, ae;  /* slot flags before / after: bit 0 0x28, 1 0x0F, 2 0x2D, 3 0x20, 4 0x66, 5 0x59; AE at the end */
    uint16_t dep, fl;             /* dependent bits (3.6); R6PW_FL_* */
};

/* Canon's Tv ranges now (bracket list, 4.2); one read-only snapshot pair (menu task: one read) */
struct r6pw_rng {
    int16_t fast, slow;           /* Canon's cached range [p+0x114] / [p+0x116] (what CC_SetTv clamps to) */
    int16_t cf_fast, cf_slow;     /* raw C.Fn range [p+0x110] / [p+0x112] (K36) */
    int16_t tv;                   /* [p+0x61E] */
    uint8_t noflash, ok;          /* D1 row 43: [cc+0xA30] == [cc+0x7A8] == 0 (both reads); ok: cc and p readable */
};

/* r6_iv_task (each checks the task name; no write from any other task) */
int _r6pw_arm(int ctx, int base);              /* 0 = armed, else enum r6pw_lock */
void _r6pw_disarm(int clean);
int _r6pw_write(int want, int kind, int k, int ev);   /* STEP, RESTORE (event end: waits, then writes); R6PW_R_* */
int _r6pw_wait_ready(int want, int kind);      /* wait rows, no call, <= R6IV_BKT_WAIT_MS; 0 = ready, else the lock */
void _r6pw_tick(int now);                      /* consumer, boot check, open-request watch, pending restore, offer, marker */
int _r6pw_back(void);                          /* command 5 "Tv back to ..."; R6PW_R_* (REFUSED: back_lock; NONE: dropped,
                                                * live view lost in its wait, the notice given; M10.2b review S1) */
void _r6pw_back_dropped(int why);              /* M10.2b: command 5 dropped before its write (1 no LV, 2 not steady): the
                                                * notice; no call */
void _r6pw_consume(int now);                   /* the ring consumer (3.3) */
int _r6pw_tv_stable(int16_t * v);              /* 1 = stable-M *v (3.6a), 0 = unsettled / not in M */
int _r6pw_user_evidence(int16_t expect, int16_t * u);   /* 1 = user evidence for *u (3.6a) */
void _r6pw_event_stop(int why);                /* acts once, only while an event runs */
void _r6pw_series_end(int stop);               /* from iv_end: once per arm */
void _r6pw_begin(int base);                    /* a bracket Start / Test was accepted: base / last = base (no write) */
void _r6pw_ev_begin(int no);                   /* an event starts: ML menu blocked */
int _r6pw_ev_on(void);                         /* an event or its restore runs (ML menu blocked, 4.7) */
/* M11.1 (spec m11/m111 3.1): r6_iv_task, r6_ettr.c only (gate 11); neither writes */
int _r6pw_arm_ettr(int base, int lo, int hi, int sv);   /* 0 = armed (marker R6TV.LCK ctx=4), else enum r6pw_lock */
int _r6pw_commit(int keep);                    /* 0 = kept (marker gone, base = keep, disarmed), else the lock that holds it */
int _r6pw_is_wait_row(int lock);               /* read only: a wait row of _r6pw_wait_ready (pw_wait_row) */
/* any task, read only */
int _r6pw_locks(int want, int cls, int * lock);   /* 0 = all rows of the class hold, else the first failing row */
int _r6pw_press_ok(void);                      /* 0 while a Tv request is open: no ML press (3.7) */
int _r6pw_sig(void);                           /* a shutdown / abort signal latched (3.11): no further press of an event */
int _r6pw_ranges(struct r6pw_rng * r);         /* 1 = r valid */
const struct r6pw_state * _r6pw_state(void);
int _r6pw_ring(int i, struct r6pw_rec * e);    /* i = 0 oldest; returns the count (<= R6PW_RING) */
uint32_t _r6pw_ring_n(void);                   /* entries written since boot */
const char * _r6pw_lock_name(int l);
const char * _r6pw_res_name(int r);
const char * _r6pw_kind_name(int k);
void _r6pw_lock_text(int lock, char * buf, int n);   /* "Tv writes: <lock>" (MARKER_KEPT with its own text) */
const char * _r6pw_msg(void);                  /* the newest notice for the user ("" none) */
char * _r6pw_tvs(char * buf, int v);           /* Code768 -> "1/125" (buf >= 12 bytes) */
int _r6pw_on(void);                            /* main switch "Tv writes" */
void _r6pw_toggle(void);                       /* menu task: flips the saved switch, nothing else */
void _r6pw_log(int * hn, int16_t * hv, int * hlen, int * ht);   /* handler: count and newest entry (log) */

#endif /* _r6_propw_h_ */
