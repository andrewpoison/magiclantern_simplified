#ifndef _r6_interval_h_
#define _r6_interval_h_

/* R6.190 M8.0 / M8.1 / M8.2 / M9.2: interval release ("R6 Interval", r6_interval.c); M9.2: B with Canon's bulb timer */

#include <stdint.h>

/* why a series stopped (r6_iv_stats.stop); R6IV_STOP_NONE = never ran.
 * The menu and the log print the name; nothing stores the number. */
enum r6_iv_stop {
    R6IV_STOP_NONE = 0,
    R6IV_STOP_DONE,          /* all pictures taken */
    R6IV_STOP_USER,          /* Stop in the ML menu */
    R6IV_STOP_SHUTDOWN,      /* ML shutdown requested (switch, auto power off, battery / card door) */
    R6IV_STOP_CAMERA,        /* camera setting not allowed (stop_cam = r6_iv_cam reason) */
    R6IV_STOP_CANON,         /* Canon state not allowed (stop_canon = r6_iv_canon reason) */
    R6IV_STOP_GUI,           /* Canon GUI mode != 0 (MENU, playback, Q, ...) */
    R6IV_STOP_PWRSAVE,       /* Canon's screen saver / eco mode (GUI mode 0xE08E / 0xE08F) */
    R6IV_STOP_WAIT,          /* waited too long for a ready camera (stop_canon = last wait reason) */
    R6IV_STOP_RET,           /* a switch call returned != 0 or iv_press refused it (stop_val = return) */
    R6IV_STOP_NOPIC,         /* no new picture seen (counter / file number) */
    R6IV_STOP_EXTRA,         /* more than one new picture / a second release (stop_val = delta) */
    R6IV_STOP_JOB,           /* Canon's job count did not return to 0 (stop_val = count) */
    R6IV_STOP_SW1,           /* M8.1: half press not taken within R6IV_SW1_MS (stop_val = CC state) */
    R6IV_STOP_ABORT,         /* M8.1: aborted before the full press (stop_canon / stop_cam = reason, stop_val = state) */
    R6IV_STOP_SW2,           /* M8.1: full press not taken within R6IV_SW2_MS (stop_val = CC state) */
    R6IV_STOP_STUCK,         /* M8.1: release accepted but did not start (state 4 at the timeout) */
    R6IV_STOP_LATE,          /* M8.1: a release started after ML's timeout OFFs */
    R6IV_STOP_BUTTON,        /* M8.1: the real shutter button changed ML's switch bytes (stop_val = DF | E0 << 8) */
    R6IV_STOP_SEQ,           /* M8.1: Canon's release sequence not idle again (stop_val = failed idle bits) */
    R6IV_STOP_CCSTATE,       /* M8.1: Canon state after the picture not as expected (stop_val = final bits) */
    R6IV_STOP_FOREIGN,       /* M8.1: a picture that ML did not take, between two slots (stop_val = delta) */
    R6IV_STOP_BULB_NOTIMER,  /* M9.2: bulb without Canon's timer (no BA0 / timer A seen within R6IV_BULB_CLASS_MS;
                              * ML's SW1 OFF ended it at once: a very short frame; stop_val = d30_ms) */
    R6IV_STOP_BULB_EARLY,    /* M9.2: bulb ended before T - R6IV_BULB_EARLY_MS (user's full press or Canon; stop_val = d30_ms) */
    R6IV_STOP_BULB_OVERRUN,  /* M9.2: bulb still running at T + R6IV_BULB_OVER_MS, or at the 3 s check nothing that could
                              * end it (timer A / B 0 and BA0 0 or seconds 0, review F1); stopped at once, no call
                              * (stop_val = d30_ms) */
    R6IV_STOP_BULB_NOT12,    /* M9.2: B mode, but Canon did not start a bulb (state 5 / 6 / sequence end; stop_val = off_state) */
    R6IV_NSTOPS
};

/* camera settings checked before the start and before every picture */
enum r6_iv_cam {
    R6IV_CAM_OK = 0,
    R6IV_CAM_MOVIE,          /* movie mode (is_movie_mode / PROP_LV_MOVIE_SELECT) */
    R6IV_CAM_MODE,           /* shooting mode not M or B (Tv mode: Canon's safety shift can lengthen the exposure);
                              * M9.2 review F2: also a series whose mode changed since its start (stop_val = new mode) */
    R6IV_CAM_TV,             /* M: Tv (PROP_SHUTTER as delivered) unknown, longer than 30 s or Bulb (M8.2; M8.1: 1 s) */
    R6IV_CAM_DRIVE,          /* drive mode not single shot */
    R6IV_CAM_FOCUS,          /* not MF ((af_mode & 0xF) != 3) */
    R6IV_CAM_CARD,           /* no card / card full (avail_shot == 0) */
    R6IV_CAM_AEB,            /* exposure bracketing on (PROP_AEB step != 0): lengthens Tv in M */
    /* CameraConductor's own copies (ids [V]: CC master table 0xE0FE3344), p = [cc+0x34FC]; gated since M8.2
     * (R6IV_GATE_CC_SETTINGS 1), only while cc is valid (an invalid cc is the Canon check's CCPTR) */
    R6IV_CAM_CC_MODE,        /* p not readable, or [p+8] (AE 0x80000001) != 3 (M) */
    R6IV_CAM_CC_DRIVE,       /* [p+0x4C0] (drive 0x80000003) != 0 (single) */
    R6IV_CAM_CC_BULB,        /* (s16) [p+0x61E] (PROP_SET_TV 0x80000005) not a Tv of 30 s .. 1/16000 (Bulb 0xDF00 / 0xDC00) */
    R6IV_CAM_CC_AF,          /* [cc+0x2C0] == 1 (AF active) */
    /* M8.2 */
    R6IV_CAM_CC_MECHA,       /* [p+0x92C] (PROP_MECHALESS 0x80020061) != 0: SW2 OFF in state 5 would be RELEASE_STOP */
    R6IV_CAM_CC_ITIMER,      /* byte [p+0x50A] (0x8004005A, [I] Canon's interval timer) != 0 */
    R6IV_CAM_CC_FBRK,        /* byte [p+0x634] (0x80040064, a Canon timer setting: h:m:s at +1..+3) != 0 */
    R6IV_CAM_INTERVAL,       /* Start only: interval < Tv + 2 s (r6_interval.c iv_interval_fit; B: bulb time + 2 s) */
    /* M9.2 (B with Canon's bulb timer; BULB_BUSY also in M, skeptic C1) */
    R6IV_CAM_TVB,            /* B: PROP_SHUTTER not delivered as 2 bytes 0xDF00 */
    R6IV_CAM_CC_NOTB,        /* B: p not readable or [p+8] (AE) != 4 */
    R6IV_CAM_CC_TVB,         /* B: (s16) [p+0x61E] != 0xDF00 */
    R6IV_CAM_BULB_OFF,       /* B: byte [p+0x648] (Canon bulb timer enable) != 1 */
    R6IV_CAM_BULB_TIME,      /* B: T = h*3600 + m*60 + s outside R6IV_BULB_MIN_S .. R6IV_BULB_MAX_S, or m / s > 59 */
    R6IV_CAM_BULB_BUSY,      /* M and B: [cc+0xD30] / [cc+0xBA0] / [0x1E5BC] / [0x1E5C4] / [cc+0x99C] != 0 (a bulb runs or
                              * did not end cleanly; spec R2, skeptic C1 / C5) */
    R6IV_CAM_CC_R5E8,        /* B: [cc+0x5E8] != 0 (skeptic D2: the re-post builder 0xE0640BCE branches on it and
                              * 0xE0718122 keeps Canon's bulb timer on for 1 / 2; camera value 0) */
    R6IV_NCAMS
};

/* Canon state, read-only. Hard reasons (CCPTR .. REMOTE) stop a series; the others only delay the picture */
enum r6_iv_canon {
    R6IV_CANON_OK = 0,
    R6IV_CANON_CCPTR,        /* [0x10AB8] not a valid CameraConductor, or a [V] copy differs */
    R6IV_CANON_SWREG,        /* MPU switch table [[0x1E444]] does not hold the two callbacks ML calls */
    R6IV_CANON_SWPATH,       /* [cc+0x2E0] != 0 or [0x1E43C] != 0: the skipped wrapper would drop / divert the button */
    R6IV_CANON_EVFLAG,       /* CC event flag [cc+8] 0 or odd (SetEventFlag asserts) */
    R6IV_CANON_FACTORY,      /* ShutterMode 10 (factory Release) or FA-Neo list not empty: reboot */
    R6IV_CANON_REMOTE,       /* [cc+0x580] != 0 (remote sequence: ActRelSeqComp re-posts held switches) */
    R6IV_CANON_EARLY,        /* less than R6IV_EARLY_MS since boot */
    R6IV_CANON_LV,           /* Canon LV not running (PROP_LV_ACTION != 0), or for less than R6IV_LV_MS */
    R6IV_CANON_REVIEW,       /* gui_state != idle or Canon's quick review / SR mode (GUI mode 0x4033 / 0x4034) */
    R6IV_CANON_JOBS,         /* Canon job count [0x9DC0] != 0 */
    R6IV_CANON_BUFFER,       /* burst_count == 0 (buffer full) */
    R6IV_CANON_UILOCK,       /* icu_uilock & 0xFF */
    R6IV_CANON_CLEANING,     /* sensor cleaning */
    R6IV_CANON_MENU,         /* ML menu open or closed less than R6IV_MENU_HOLD_MS ago */
    R6IV_CANON_SWHELD,       /* a shutter byte set, SW events pending / working, or the self-release macro in flight */
    R6IV_CANON_RELSEQ,       /* Canon's release sequence running ([0x108AC] / [cc+0xB54]) */
    R6IV_CANON_CCSTATE,      /* CC not idle in LV still (table, LV kind, shoot type, dispatch, state 1 / 3, torn read) */
    R6IV_CANON_CCFLAG,       /* M8.2: [cc+0xB98] or [cc+0x830] != 0 (set by the [p+0x634] timer setting / the interval timer) */
    R6IV_NCANON
};

#define R6IV_RING 64

struct r6_iv_shot {
    uint32_t t;              /* ms (get_ms_clock) of ML's SW1 ON */
    int32_t  ret;            /* OR of the four switch-call returns (-1: iv_press refused one) */
    uint32_t dur;            /* ms from SW1 ON to the OFFs */
    int32_t  d_cnt;          /* PROP_RELEASE_COUNTER delta */
    int32_t  d_fn;           /* file number delta */
    uint32_t pic_ms;         /* ms from SW1 ON until the new picture was seen */
    uint32_t job_ms;         /* ms from SW1 ON until Canon's job count was 0 again */
    uint32_t jobs_after;     /* [0x9DC0] after the wait */
    uint32_t js_before;      /* lens_info.job_state before / after */
    uint32_t js_after;
    uint32_t wait_ms;        /* ms the picture waited for a ready camera */
    uint32_t wait_mask;      /* bit r = waited for r6_iv_canon reason r */
    /* M8.1 */
    uint16_t sw1_ms;         /* SW1 ON -> taken (state 2 or [cc+0xA68] bit 1); 0xFFFF = not taken */
    uint16_t acc_ms;         /* SW2 ON -> first 'accepted' signal; 0xFFFF = none */
    uint16_t off_ms;         /* SW2 hold (SW2 ON -> OFFs) */
    uint8_t  off_why;        /* why the OFFs went out (r6_interval.c IV_WHY_*) */
    uint8_t  off_state;      /* CC state when the OFFs went out */
    uint32_t seq_ms;         /* OFFs -> Canon idle again; 0xFFFFFFFF = not idle (M8.2: 32 bit, log -1) */
    uint16_t states;         /* bit s = CC state s seen during the picture (0..15) */
    int16_t  d_rel;          /* RAM release counter delta; -32768 = not used */
    int16_t  d_jid;          /* [0x9DB8] delta */
    uint8_t  sm_before;      /* ShutterMode [cc+0xA40] before / after */
    uint8_t  sm_after;
    uint8_t  late;           /* late-watch mask (after a timeout OFF) */
    uint8_t  second;         /* 1 = a second release seen after Canon was idle */
    uint16_t final;          /* failed final checks (bits, r6_interval.c IV_FIN_*) */
    /* M8.2 (long Tv) */
    int16_t  tv;             /* PROP Tv (EV * 768) at the pre-SW2 check; -32768 = not read */
    int16_t  tv_cc;          /* CC copy (s16) [p+0x61E] at the pre-SW2 check; -32768 = not read */
    uint8_t  r92c;           /* [p+0x92C] (PROP_MECHALESS) at the pre-SW2 check */
    uint8_t  nr;             /* low byte of [p+0x54C] (long exposure NR) */
    uint8_t  cflag;          /* [cc+0xB98] | [cc+0x830] << 1 */
    uint8_t  pad2;
    uint32_t busy;           /* ms added to the SEQ / PIC / JOB budgets (2 x Tv bound for Tv >= 1 s, else 0) */
    uint32_t s5_ms;          /* SW1 ON -> first state 5 seen; 0 = none */
    uint32_t exp_ms;         /* first state 5 -> first state 6 seen; 0 = none */
    int32_t  slot_late;      /* SW1 ON - slot (ms); 0 for "Test: 1 picture" */
    /* M9.2 (bulb; 0 for M pictures except drive / r5e8) */
    uint8_t  bulb;           /* IV_BCLASS_*: 0 not a bulb, 1 timer bulb, 2 no timer seen in the classification window */
    uint8_t  bend;           /* IV_BEND_*: why phase W ended */
    uint8_t  bbtn;           /* real button seen in phase W: bit 0 DF, bit 1 E0 */
    uint8_t  bwarn;          /* IV_BW_* */
    uint8_t  drive;          /* low byte of [p+0x4C0] at the pre-SW2 check (M and B) */
    uint8_t  r5e8;           /* low byte of [cc+0x5E8] at the pre-SW2 check (M and B); 1 / 2 keep Canon's timer on in 0xE06423AA */
    uint16_t bt;             /* bulb time T (s) from [p+0x648] at the pre-SW2 check */
    uint16_t sec_max;        /* highest [cc+0x99C] seen in phase W */
    uint16_t clk_back;       /* phase W: clock steps back (or jumps > R6IV_BULB_GLITCH_MS) seen and skipped (skeptic C3) */
    uint32_t bulbw;          /* [p+0x648] at the pre-SW2 check */
    uint32_t bulbw_after;    /* [p+0x648] at the end of the picture (Canon may switch its timer off itself, K10) */
    uint32_t ta_ms;          /* [0x1E5C0] the first time [0x1E5BC] was seen 1 */
    uint32_t bulb_ms;        /* [0x1E568] when phase W ended (Canon's measured bulb time) */
    uint32_t d30_ms;         /* OFFs (t3) -> [cc+0xD30] 0 or state != 12 seen, elapsed (OVERRUN / SHUTDOWN: t3 -> the stop) */
};

struct r6_iv_stats {
    int running;
    int n_done;              /* pictures of the current / last series */
    int n_target;
    int interval_s;          /* of the current / last series (0 for "Test: 1 picture") */
    uint32_t t_start;        /* ms when Start was pressed */
    int stop;                /* enum r6_iv_stop of the last series */
    int stop_cam;            /* enum r6_iv_cam, for R6IV_STOP_CAMERA (and ABORT) */
    int stop_canon;          /* enum r6_iv_canon, for R6IV_STOP_CANON / WAIT / ABORT */
    int32_t stop_val;
    uint32_t t_stop;
    int skipped;             /* interval slots skipped (camera not ready within R6IV_LATE_MS) */
    uint32_t calls;          /* pictures started (SW1 ON) since boot */
    uint32_t shots;          /* ring entries written since boot */
    uint32_t refused;        /* Start refused */
    struct r6_iv_shot ring[R6IV_RING];
    /* M8.1 */
    int test;                /* 1 = the last / current run is "Test: 1 picture" */
    int cnt_last;            /* total_shots_count / file number after ML's last picture (or at the start) */
    int fn_last;
    /* M8.2 */
    uint32_t menu_blocked;   /* ML menu opens refused while a picture runs (r6_iv_menu_block, src/menu.c) */
    /* M9.2 */
    uint32_t bulb_warn;      /* OR of IV_BW_* over the bulb pictures of the current / last run (reset at the start) */
};

/* M8.1: one read-only snapshot of the CameraConductor and the switch path (r6_interval.c iv_cc_read) */
#define R6IV_V_CC    0x01    /* cc passed the pointer check: the CC fields are valid */
#define R6IV_V_P     0x02    /* p = [cc+0x34FC] valid */
#define R6IV_V_PO    0x04    /* po = [0x10788] valid: relcnt valid */
#define R6IV_V_SW    0x08    /* mgr = [0x1E444] and arr = [mgr] valid: cbr[] valid */
#define R6IV_V_STS   0x10    /* sts = [mgr+4] valid: sts[] valid */

struct r6_iv_cc {
    uint32_t valid;          /* R6IV_V_* */
    uint32_t cc;             /* [0x10AB8] */
    uint32_t copy[9];        /* [V] 0x68C4 0x108A0 0x10874 0x108D4, [I] 0x108C0 0x1E424 0x1E41C 0x10898 0x1087C */
    uint32_t evflag;         /* [cc+0x008] */
    uint32_t table;          /* [cc+0x00C] */
    uint32_t pend;           /* [cc+0x018] class-2 pending */
    uint32_t work;           /* [cc+0x050] class-2 working */
    uint32_t dispatch;       /* [cc+0x0C0] */
    uint8_t  df, e0;         /* bytes [cc+0xDF] / [cc+0xE0]: SW1 / SW2 held */
    uint8_t  snap_df, snap_e0; /* bytes [cc+0x16B] / [cc+0x16C]: per-round copies */
    uint8_t  lvkind;         /* byte [cc+0x2CE] */
    uint8_t  pad[3];
    uint32_t state;          /* [cc+0x294] */
    uint32_t state_prev;     /* [cc+0x298] */
    uint32_t af;             /* [cc+0x2C0] [I] */
    uint32_t swgate;         /* [cc+0x2E0] */
    uint32_t r3b4;           /* [cc+0x3B4] [I] */
    uint32_t remote;         /* [cc+0x580] */
    uint32_t r5e8;           /* [cc+0x5E8] */
    uint32_t r730;           /* [cc+0x730] [I] */
    uint32_t r888;           /* [cc+0x888] */
    uint32_t shoottype;      /* [cc+0x9EC] */
    uint32_t shmode;         /* [cc+0xA40] ShutterMode */
    uint32_t a68;            /* [cc+0xA68] */
    uint32_t a6c;            /* [cc+0xA6C] */
    uint32_t b54;            /* [cc+0xB54] */
    uint32_t bfc;            /* [cc+0xBFC] */
    uint32_t p;              /* [cc+0x34FC]; M8.2: read with r6_fy_ram_ok (cached RAM or the verified uncached alias) */
    uint32_t p_ae;           /* [p+0x008] AE mode 0x80000001 [V id] */
    uint32_t p_drive;        /* [p+0x4C0] drive 0x80000003 [V id] */
    uint32_t p_tv;           /* (u16) [p+0x61E] PROP_SET_TV 0x80000005 [V id] */
    uint32_t p_92c;          /* [p+0x92C] PROP_MECHALESS 0x80020061 [V id] */
    uint32_t swhack_q;       /* [0x1E43C] */
    uint32_t relseq;         /* [0x108AC] */
    uint32_t relcb;          /* [0x108A8] */
    uint32_t aux2;           /* [0x12FAD4] */
    uint32_t mgr;            /* [0x1E444] */
    uint32_t cbr[2];         /* [[mgr]+0/4] */
    uint32_t sts[2];         /* [[mgr+4]+0/4] */
    uint32_t po;             /* [0x10788] */
    uint32_t relcnt;         /* [po+0x1A08] */
    uint32_t jobs;           /* [0x9DC0] */
    uint32_t jid;            /* [0x9DB8] */
    uint32_t jdone;          /* [0x9DBC] */
    uint32_t access;         /* [0x9DD4] */
    uint32_t faneo[2];       /* [0x180D0] / [0x180D4] (both 0x180D0 = empty) */
    int tsc;                 /* total_shots_count (ML) */
    int fn;                  /* file number (ML) */
    int gui;                 /* CURRENT_GUI_MODE */
    /* M8.2 */
    uint32_t p_lenr;         /* [p+0x54C] long exposure NR 0x80000048 [V id, I values] */
    uint32_t p_itimer;       /* byte [p+0x50A] 0x8004005A [I interval timer] */
    uint32_t p_fbrk;         /* byte [p+0x634] 0x80040064 (timer setting, h:m:s at +1..+3; log key name kept) */
    uint32_t b98;            /* [cc+0xB98] */
    uint32_t c830;           /* [cc+0x830] */
    uint32_t r5f8;           /* [cc+0x5F8] (log only) */
    int lv;                  /* r6_interval.c iv_t_lv >= 0 (Canon LV running) */
    int gstate;              /* gui_state */
    /* M9.2: bulb (cc words only with R6IV_V_CC, bulbw only with R6IV_V_P, the timer RAM always, read after the cc block) */
    uint32_t d30;            /* [cc+0xD30] */
    uint32_t ba0;            /* [cc+0xBA0] */
    uint32_t sec;            /* [cc+0x99C] */
    uint32_t bulbw;          /* [p+0x648] */
    uint32_t ta, ta_ms, tb, tb_ms;   /* [0x1E5BC] [0x1E5C0] [0x1E5C4] [0x1E5C8] */
    uint32_t bulb_ms;        /* [0x1E568] */
};

/* M8.1: detail of the last picture (reset at every SW1 ON, kept until the next one) */
#define R6IV_TRACE 64                    /* M8.2: 48 -> 64 */
#define R6IV_PTRACE 64
struct r6_iv_tr { uint32_t dt; uint32_t v; char code; char pad[3]; };   /* M8.2: dt 32 bit (ms since SW1 ON) */
struct r6_iv_ptr { uint32_t t; char code; char pad[3]; uint32_t v; };
struct r6_iv_last {
    struct r6_iv_cc pre, off, post;  /* before SW1 ON, when the OFFs went out, after the settle */
    int n, lost;                     /* trace entries / dropped */
    struct r6_iv_tr tr[R6IV_TRACE];
};

const struct r6_iv_stats * r6_iv_stats(void);
const struct r6_iv_last * r6_iv_last(void);
int r6_iv_running(void);
const char * r6_iv_stop_name(int s);
const char * r6_iv_cam_name(int c);
const char * r6_iv_canon_name(int c);
int r6_iv_cam_check(void);             /* enum r6_iv_cam */
int r6_iv_canon_check(int * hard);     /* enum r6_iv_canon; *hard = 1: stops a series; menu display:
                                        * ignores the ML menu and what it causes (LV stopped, gui_state, CC state) */
int r6_iv_tv_prop(int * seen, int * len); /* PROP_SHUTTER as delivered (int16, own handler); len = delivered length */
int r6_iv_tv_ev768(int * ev);          /* 1 = the delivered Tv decoded: *ev = Tv in EV * 768 (1 s = 0, 1/125 s = 5376) */
int r6_iv_aeb(int * seen);             /* PROP_AEB as delivered (own handler), 0 = off */
void r6_iv_cc_read(struct r6_iv_cc * s); /* M8.1: one read-only snapshot (no Canon call) */
int r6_iv_press_info(uint32_t n[4], uint32_t * refused);  /* returns ML's held switches (bit 1 SW1, 2 SW2), must be 0 */
int r6_iv_ptrace(int i, struct r6_iv_ptr * e);   /* passive trace, i = 0 oldest; returns the count (<= R6IV_PTRACE) */
int r6_iv_gate_cc_settings(void);      /* R6IV_GATE_CC_SETTINGS */
/* M8.2 */
int r6_iv_tv_min(void);                /* R6IV_TV_EV768_MIN (EV * 768 of the longest Tv allowed, 30 s) */
int r6_iv_menu_block(void);            /* src/menu.c menu_open(): 1 = a picture is running (counts the attempt) */
void r6_iv_budget(int * tv_ub, int * busy, int * need, int * need_nr, int * fit);   /* for the log: Tv bound (ms,
                                        * -1 = no Tv), budget add-on, interval need / NR need (ms), interval fit 0 / 1 / 2
                                        * (M9.2, B: T from Canon's bulb timer, busy 2 T + 5 s + T / 16) */
/* M9.2 */
int r6_iv_bulb_t(const struct r6_iv_cc * s);   /* T (s) from [p+0x648] if p valid, enable 1, m / s <= 59; else -1 */
int r6_iv_bulb_min_s(void);                    /* R6IV_BULB_MIN_S (31) */
int r6_iv_bulb_max_s(void);                    /* R6IV_BULB_MAX_S (60) */
int r6_iv_bulb_running(void);                  /* 1 = ML's bulb picture runs, or Canon's bulb after an overrun stop (overlays pause) */

/* Canon RAM read by r6_interval.c (read-only) */
#define R6IV_RAM_JOBCNT    0x9DC0      /* live job count [0x9D9C+0x24]: CreateJob checks it (cbz 0xE07C9E5E),
                                        * TrashJob decrements it and logs "Job Done" at 0 (0xE07CCCDC..0xE07CCD2A) */
#define R6IV_RAM_JOB_LASTID 0x9DB8     /* [0x9D9C+0x1C] newest job ID (CreateJob: ID = [0x9DB8]+1, 0xE07C9E56) */
#define R6IV_RAM_JOB_DONEID 0x9DBC     /* [0x9D9C+0x20] ID of the last fully trashed job (0xE07CCDFA) */
#define R6IV_RAM_JOB_ACCESS 0x9DD4     /* [0x9D9C+0x38] ACCESS_REMAIN (CreateJob += n 0xE07C9EF2, TrashJob -1 0xE07CCC8E) */
/* M8.0 factory words: log only since M8.1 (they protected call("Release"), which is gone) */
#define R6IV_RAM_FASY_SEM  0x9C40      /* "FaSyReleaseSemaphore" [0x9C20+0x20]: created by FaSyRelease 0xE01FB15E,
                                        * set to 0 after delete (0xE01FB1A0) */
#define R6IV_RAM_FA_LOG    0x9B3C      /* factory log: 1 once FA_Initialize set it up (0xE070F502 -> 0xE070F51A) */
#define R6IV_RAM_FA_LOGMODE 0x9B40     /* [0x9B3C+4] FAPrintf mode (tbb 0xE070F04C): only +-2 writes the file
                                        * B:/AdjLog.txt (0xE070F084 / 0xE070F08E), 0 = RAM buffer only */
#define R6IV_RAM_FA_LOGFILE 0x9B54     /* [0x9B3C+0x18] its open file handle */
#define R6IV_RAM_FA_STORE  0xE708      /* FA TransparentMemory (RAM only): 1 once initialised (0xE027EC10 ->
                                        * 0xE027EC3E, from FA_Initialize before "Release" is registered) */
#define R6IV_RAM_FANEO     0x180D0     /* [0x180A4+0x2C] FA-Neo list head (circular, points to itself when empty;
                                        * set up by FARmt init 0xE026C2A6); M8.1 gate: [0x180D0] == [0x180D4] == 0x180D0
                                        * (a factory job keeps a node here, unit-mask flag 0x8 at 0xE07C9D14) */
#define R6IV_RAM_FANEO_TAIL 0x180D4    /* [0x180D0+4] oldest node's link (node = link - 0x14) */
#define R6IV_RAM_FANEO_SEM 0x180BC     /* [0x180A4+0x18] FARmtReleaseSem: FANeoReleaseExecute gives it
                                        * (b.w give_semaphore 0xE026C75E), NULL asserts */
#define R6IV_RAM_CC_SW1    0x108DC     /* [0x108D0+0xC]: CC remote SW1 handler, 0xE029661B once registered (0xE029803C ->
                                        * setter 0xE06D3186, ID 0x40); before: default 0xE06D32DB (log only) */
#define R6IV_RAM_CC_SW2    0x108E0     /* [0x108D0+0x10]: CC remote SW2 handler, 0xE029665D once registered (ID 0x41) */
#define R6IV_RAM_CC_OBJ    0x10AB8     /* CameraConductor object (set 0xE0295DCA) */

/* M8.1: CameraConductor (cc) and the physical shutter switch path */
#define R6IV_RAM_CC          0x10AB8   /* cc as the shutter callbacks read it (0xE0296518 ldr =0x10AB8); set 0xE0295DCA */
#define R6IV_RAM_CC_MASTER   0x68C4    /* [0x68C0+4] CC master: post 0xE0287BB0, CC init 0xE0288492 [V] */
#define R6IV_RAM_CC_RELSEQ   0x108A0   /* ActRelSeq's copy (setter 0xE031DAE8) [V] */
#define R6IV_RAM_CC_STATEGET 0x10874   /* state getter's copy (setter 0xE07020B0) [V] */
#define R6IV_RAM_CC_SWWRAP   0x108D4   /* switch wrapper's copy (setter 0xE06D3070) [V] */
#define R6IV_RAM_CC_SHMODE   0x108C0   /* ShutterMode / re-post copy [I] */
#define R6IV_RAM_CC_LVH1     0x1E424   /* LV handlers' copy [I] */
#define R6IV_RAM_CC_LVH2     0x1E41C   /* dispatcher's copy [I] */
#define R6IV_RAM_CC_JSW2OFF  0x10898   /* JudgeSw2Off's copy [I] */
#define R6IV_RAM_CC_JSW1OFF  0x1087C   /* SW1-OFF judge's copy [I] */
#define R6IV_RAM_RELSEQ      0x108AC   /* [0x108A0+0xC]: 1 ActReleaseSequenceStart 0xE031DD18 .. 0 ActRelSeqComp 0xE031E15E / abort 0xE031E0C6 */
#define R6IV_RAM_RELCB       0x108A8   /* [0x108A0+8]: self-release macro completion cb (setter 0xE031DCF8), 0 = none */
#define R6IV_RAM_SWMGR       0x1E444   /* [0x1E440+4] MPU switch manager: [mgr] cb array, [mgr+4] status array (0xE033A9B8 / 0xE033AA0E) */
#define R6IV_RAM_SWHACK_Q    0x1E43C   /* CC_SWHACK queue, != 0 diverts switch events (0xE0376E2E) */
#define R6IV_RAM_SWHACK_HOOK 0x1E438   /* CC_SWHACK hook, run before the diverted callback (log only) */
#define R6IV_RAM_AUX2        0x12FAD4  /* 0x12FA5C+0x70+4*2: aux deferred word of class 2, the post clears partners there (0xE02AF210) */
#define R6IV_RAM_PO          0x10788   /* PhotoOperator; [po+0x1A08] release counter (0xE074BF0C, 0xE0C2F204) */
#define R6IV_RAM_PO2         0x1079C   /* [[0x1079C]+0x1BC]: copy of the release counter (taskB, log only) */
#define R6IV_PO_RELCNT       0x1A08
#define R6IV_PO_SIZE         0x1A0C
#define R6IV_CC_SIZE         0x3500    /* highest field used: 0x34FC */
#define R6IV_P_SIZE          0x930     /* highest field used: 0x92C */
#define R6IV_CC_EVFLAG       0x008     /* event flag handle: SetEventFlag 0xE07C7AC4 asserts on 0 / odd */
#define R6IV_CC_TABLE        0x00C     /* current state table; LV still = R6IV_LV_STILL_TABLE (0xE02C3936 / 0xE05E0DF2) */
#define R6IV_CC_PEND2        0x018     /* class-2 pending [cc+0x10+4*2] */
#define R6IV_CC_WORK2        0x050     /* class-2 working */
#define R6IV_CC_DISPATCH     0x0C0     /* 1 = dispatch on (0xE02AF5EE, set 0xE0287E18) */
#define R6IV_CC_SW1BYTE_W    0x0DC     /* word holding byte 0xDF (SW1 held, callback strb 0xE0296518) */
#define R6IV_CC_SW2BYTE_W    0x0E0     /* byte 0xE0 (SW2 held, callback strb 0xE029652E) */
#define R6IV_CC_SNAP1_W      0x168     /* word holding byte 0x16B (per-round copy of DF, 0xE02AF614) */
#define R6IV_CC_SNAP2_W      0x16C     /* byte 0x16C (per-round copy of E0, 0xE02E955A / 0xE05A5F8A) */
#define R6IV_CC_STATE        0x294     /* CC state (setter 0xE07020D8) */
#define R6IV_CC_STATE_PREV   0x298
#define R6IV_CC_AF           0x2C0     /* AF active [I] (MF -> 0) */
#define R6IV_CC_LVKIND_W     0x2CC     /* word holding byte 0x2CE: 1 = LV table family */
#define R6IV_CC_SWGATE       0x2E0     /* != 0: wrapper 0xE06D30BA drops switch events */
#define R6IV_CC_R3B4         0x3B4     /* [I] */
#define R6IV_CC_REMOTE       0x580     /* 2 enables the re-post at sequence end (ActRelSeqComp) */
#define R6IV_CC_R5E8         0x5E8     /* re-post builder gate */
#define R6IV_CC_R730         0x730     /* [I] "no picture" gate in 0xE06435B8 */
#define R6IV_CC_R888         0x888     /* SW1 released during the sequence (0xE05E2FA8 / 0xE05E307A / 0xE05E30B8) */
#define R6IV_CC_SHOOTTYPE    0x9EC     /* 3 = LV still, 4 = movie (SW2 handler 0xE02F9CF8) */
#define R6IV_CC_SHMODE       0xA40     /* ShutterMode: button path 1 (0xE05E328E), factory Release 10 */
#define R6IV_CC_A68          0xA68     /* bit 1: SW1 holder (set 0xE06DA472, cleared 0xE06DA6B0) */
#define R6IV_CC_A6C          0xA6C     /* metering holder; re-post if 2 and [cc+0x888] == 1 */
#define R6IV_CC_B54          0xB54     /* 1 from ActReleaseSequenceStart (0xE031DD14) to ActRelSeqComp (0xE031E15A) */
#define R6IV_CC_BFC          0xBFC     /* re-post builder gate */
#define R6IV_CC_P            0x34FC    /* p: CC copies of the settings [I] */
#define R6IV_P_AE            0x008     /* CC copy of AE mode 0x80000001 (3 = M) (CC master table 0xE0FE3344) [V id] */
#define R6IV_P_DRIVE         0x4C0     /* CC copy of drive 0x80000003 (0 = single) [V id] */
#define R6IV_P_TV_W          0x61C     /* word holding the s16 CC copy of PROP_SET_TV 0x80000005 at 0x61E (EV * 768,
                                        * 0xDF00 / 0xDC00 = Bulb; state 12 for AE 3 with 0xDF00, 0xE031D82A..0xE031D84E) [V id] */
#define R6IV_P_ITIMER_W      0x508     /* word holding byte 0x50A (byte 2) = CC copy of 0x8004005A: != 0 makes
                                        * JudgeIntervalTimerShooting 0xE0643C82 (ldrb 0xE0643C9E, from 0xE031DD90 at the
                                        * start of the release sequence) set [cc+0x830] = 1 [V code, I "interval timer"] */
#define R6IV_P_LENR          0x54C     /* CC copy of 0x80000048, long exposure NR (CC master table; ATest get / set
                                        * 0xE06569EA / 0xE065699E) [V id, I values 0 / 1 / 2] */
#define R6IV_P_FBRK_W        0x634     /* word holding byte 0x634 (byte 0) = CC copy of 0x80040064: != 0 sets
                                        * [cc+0xB98] = 1 (ldrb 0xE0643EBA, str 0xE0643ED2) [V code]; M9.1: it carries
                                        * h:m:s at +1..+3 (0xE0643E78, 0xE070D602), i.e. a timer setting; the M8.2 name
                                        * "focus bracketing" is doubtful (texts renamed, the gate is unchanged) */
#define R6IV_P_92C           0x92C     /* CC copy of PROP_MECHALESS 0x80020061 (CC table idx 332 {0x34FC, 0x92C, 4}) [V id];
                                        * SW2 OFF in state 5 (0xE05E3498): RELEASE_STOP if 1, else deferred to state 6 [V] */
#define R6IV_CC_B98          0xB98     /* SW1 OFF in state 5 (0xE05E3094): == 1 runs the state-6 handler 0xE05E2FE6 at once
                                        * instead of deferring; set by the [p+0x634] path 0xE0643ED2 [V] */
#define R6IV_CC_C830         0x830     /* JudgeIntervalTimerShooting fShooting: set to 1 at 0xE0643CB0 [V] */
#define R6IV_CC_R5F8         0x5F8     /* SW1 OFF in state 5: == 1 sets [cc+0xBB8] (0xE05E30A4); the [p+0x634] path
                                        * sets [cc+0xB98] only while it is 0 (0xE0643EC2) [V]; log only */
#define R6IV_LV_STILL_TABLE  0xE1C399DC /* LV still state table (CC state rows 0xE1C3609C .. 0xE1C3615C) */
#define R6IV_SW_EVENT_MASK   0x3C00    /* class-2 bits 0x400 SW1 ON, 0x800 SW1 OFF, 0x1000 SW2 ON, 0x2000 SW2 OFF */
#define R6IV_SWMGR_SIZE      0x10      /* [mgr] arr, [mgr+4] sts, [mgr+0xC] hook */
#define R6IV_JOBCLASS_SIG    0xE1F95BF4 /* address of the ROM string "JobClass": [jobref] / [jobdata] (0xE07D42E6) */

/* M9.2: bulb with Canon's bulb timer (read only; spec m9/m92 spec_m92.md, camera log M9.1 LVP00056) */
#define R6IV_CC_SEC          0x99C     /* bulb seconds: ++ every 1 s from EXPOSURE_START (tick 0xE070D5E8), 0 at the ends that go
                                        * through 0xE070DD9E (0xE070DDAA; 0xE031DF14 calls it only in state 12, skeptic R2) */
#define R6IV_CC_BA0          0xBA0     /* 1 = Canon's bulb timer armed: SHOOT_START into 12 with byte [p+0x648] != 0 (0xE031D87C);
                                        * 0 by timer A (0xE070E076), a full press in 12 (Sw2OnBulbing 0xE05E3380), 0xE0642376
                                        * (0xE06423A4); SHOOT_START never clears it, RELEASE_CANCEL leaves it 1 (skeptic C2) */
#define R6IV_CC_D30          0xD30     /* 1 = bulb running: SHOOT_START into 12 (0xE031D864); 0 by the bulb end 0xE02EB9BE (0xE02EB9F0)
                                        * and SW1 OFF in 2 / 3 / 6 (0xE05E2EEA / 0xE05E3048); RELEASE_CANCEL 0xE031E0AA / ERROR leave it 1 */
#define R6IV_P_BULBT         0x648     /* CC copy of PROP 0x8004005B (CC table idx 237): byte 0 enable, 1 h, 2 m, 3 s
                                        * (0xE0643E22..0xE0643E2E; camera LVP00056 0x23000001 = on, 0:00:35) */
#define R6IV_RAM_BULB_MS     0x1E568   /* [0x1E548+0x20]: 0 at SHOOT_START (0xE02F6F36), (end - start) / 100 * 100, min 100, at the
                                        * end (0xE02F6F7C), before RELEASE_STOP and D30 = 0 */
#define R6IV_RAM_TA          0x1E5BC   /* [0x1E548+0x74] timer A armed (0xE02F7340); cleared only by its cancel 0xE02F7360 (camera:
                                        * 0 again at the end, K8) */
#define R6IV_RAM_TA_MS       0x1E5C0   /* [0x1E548+0x78] timer A ms = s * 1000 (0xE02F734C) */
#define R6IV_RAM_TB          0x1E5C4   /* [0x1E548+0x7C] timer B armed (0xE02F7396); cancel 0xE02F73BA */
#define R6IV_RAM_TB_MS       0x1E5C8   /* [0x1E548+0x80] timer B ms (0xE02F73A8) */
#define R6IV_TV_BULB         (-8448)   /* (s16) 0xDF00: PROP_SHUTTER and [p+0x61E] in B (camera LVP00056) */

#endif /* _r6_interval_h_ */
