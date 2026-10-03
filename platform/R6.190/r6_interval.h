#ifndef _r6_interval_h_
#define _r6_interval_h_

/* R6.190 M8.0 / M8.1: interval release ("R6 Interval", r6_interval.c) */

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
    R6IV_NSTOPS
};

/* camera settings checked before the start and before every picture */
enum r6_iv_cam {
    R6IV_CAM_OK = 0,
    R6IV_CAM_MOVIE,          /* movie mode (is_movie_mode / PROP_LV_MOVIE_SELECT) */
    R6IV_CAM_MODE,           /* shooting mode not M (Tv mode: Canon's safety shift can lengthen the exposure) */
    R6IV_CAM_TV,             /* Tv (PROP_SHUTTER as delivered) unknown or longer than 1 s */
    R6IV_CAM_DRIVE,          /* drive mode not single shot */
    R6IV_CAM_FOCUS,          /* not MF ((af_mode & 0xF) != 3) */
    R6IV_CAM_CARD,           /* no card / card full (avail_shot == 0) */
    R6IV_CAM_AEB,            /* exposure bracketing on (PROP_AEB step != 0): lengthens Tv in M */
    /* M8.1: CameraConductor's own copies [I]; gate only with R6IV_GATE_CC_SETTINGS 1 (0 in this build: logged) */
    R6IV_CAM_CC_MODE,        /* [[cc+0x34FC]+8] != 3 (M) */
    R6IV_CAM_CC_DRIVE,       /* [[cc+0x34FC]+0x4C0] != 0 (single) */
    R6IV_CAM_CC_BULB,        /* (u16) [[cc+0x34FC]+0x61E] == 0xDF00 (Bulb) */
    R6IV_CAM_CC_AF,          /* [cc+0x2C0] == 1 (AF active) */
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
    uint16_t seq_ms;         /* OFFs -> Canon idle again; 0xFFFF = not idle */
    uint8_t  off_why;        /* why the OFFs went out (r6_interval.c IV_WHY_*) */
    uint8_t  off_state;      /* CC state when the OFFs went out */
    uint16_t states;         /* bit s = CC state s seen during the picture (0..15) */
    int16_t  d_rel;          /* RAM release counter delta; -32768 = not used */
    int16_t  d_jid;          /* [0x9DB8] delta */
    uint8_t  sm_before;      /* ShutterMode [cc+0xA40] before / after */
    uint8_t  sm_after;
    uint8_t  late;           /* late-watch mask (after a timeout OFF) */
    uint8_t  second;         /* 1 = a second release seen after Canon was idle */
    uint16_t final;          /* failed final checks (bits, r6_interval.c IV_FIN_*) */
    uint16_t pad;
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
    uint32_t p;              /* [cc+0x34FC] */
    uint32_t p_ae;           /* [p+0x008] [I] */
    uint32_t p_drive;        /* [p+0x4C0] [I] */
    uint32_t p_tv;           /* (u16) [p+0x61E] [I] */
    uint32_t p_92c;          /* [p+0x92C] */
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
};

/* M8.1: detail of the last picture (reset at every SW1 ON, kept until the next one) */
#define R6IV_TRACE 48
#define R6IV_PTRACE 64
struct r6_iv_tr { uint16_t dt; char code; char pad; uint32_t v; };
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
#define R6IV_P_AE            0x008     /* AE mode (3 = M) [I] */
#define R6IV_P_DRIVE         0x4C0     /* drive (0 = single) [I] */
#define R6IV_P_TV_W          0x61C     /* word holding the u16 Tv at 0x61E (0xDF00 = Bulb) [I] */
#define R6IV_P_92C           0x92C     /* SW2 OFF in state 5: RELEASE_STOP if 1, else deferred [V] use */
#define R6IV_LV_STILL_TABLE  0xE1C399DC /* LV still state table (CC state rows 0xE1C3609C .. 0xE1C3615C) */
#define R6IV_SW_EVENT_MASK   0x3C00    /* class-2 bits 0x400 SW1 ON, 0x800 SW1 OFF, 0x1000 SW2 ON, 0x2000 SW2 OFF */
#define R6IV_SWMGR_SIZE      0x10      /* [mgr] arr, [mgr+4] sts, [mgr+0xC] hook */
#define R6IV_JOBCLASS_SIG    0xE1F95BF4 /* address of the ROM string "JobClass": [jobref] / [jobdata] (0xE07D42E6) */

#endif /* _r6_interval_h_ */
