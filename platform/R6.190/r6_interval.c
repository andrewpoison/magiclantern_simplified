/*
 * R6.190 M8.0 / M8.1: interval release, ML menu Shoot > "R6 Interval".
 *
 * M8.1, approved by the user on 2026-10-03: ML takes a picture by pressing
 * the physical shutter switch the way Canon's MPU interrupt does. It calls
 * the two CameraConductor (CC) callbacks of the switch table, CC_SW1_CBR
 * 0xE0296518 (half press) and CC_SW2_CBR 0xE029652E (full press), with the
 * constants 1 / 0, and nothing else new. call("Release") (M8.0, Canon's
 * factory eventproc: ShutterMode 10 marked every job as a factory job and
 * kept it open until reboot) is gone. No property write
 * (CONFIG_PROP_REQUEST_CHANGE stays off, also for auto power off and AF), no
 * write to Canon RAM, no MMIO, no file, no other Canon call. Read-only RAM
 * reads of the CC, the switch manager, the release counter and the job
 * manager; every pointer taken from a Canon structure is range-checked
 * before it is dereferenced (iv_ram_ok). Analysis (ROM, task A/B/C plus a
 * skeptic): spec M8.1 (scratchpad m8/cbr). Not upstream's intervalometer
 * (FEATURE_INTERVALOMETER): that one also calls SetGUIRequestMode(0), blinks
 * the card LED (MMIO), autostarts at boot from the saved setting, writes
 * script files and ignores the outcome of the release.
 *
 * Call path [V]: MPU message cmd 3, key 5 / 6 -> SIO3_ISR 0xE0231244 ->
 * switch manager 0xE033AA0E -> wrapper 0xE06D30AE / 0xE0376E1E -> callback
 * (switch table 0xE0FE5C84 rows {7, 0xE0296519} / {8, 0xE029652F},
 * registered by CC init 0xE028854E -> 0xE029803C -> 0xE06D315E ->
 * 0xE033A9B8 into [[0x1E444]]). Each callback is a leaf: strb status into
 * [[0x10AB8]+0xDF] (SW1) / [+0xE0] (SW2), then the post 0xE0287C2E ->
 * 0xE0287BB0 -> 0xE0287B72: clear the partner event (ON <-> OFF of the same
 * switch) in the class-2 pending word [cc+0x18] and the aux word 0x12FAD4,
 * OR the new event in (0x400 SW1 ON, 0x800 SW1 OFF, 0x1000 SW2 ON, 0x2000
 * SW2 OFF), SetEventFlag([cc+8], 1). ON is "r0 == 1" on the full word while
 * the byte stored is the low byte: ML passes only 0 and 1. No NULL check
 * ([0x10AB8] = 0 writes to page 0: crash): precheck CCPTR, and iv_press
 * checks the pointer again before every call. Canon runs the callbacks in
 * interrupt context, so a call from a task is safe; no wait, no lock besides
 * two short IRQ-off sections. ML skips the wrapper: it drops the event if
 * [cc+0x2E0] != 0 and diverts it into the CC_SWHACK queue if [0x1E43C] != 0;
 * both are read-only preconditions here (SWPATH). The op-log recorder
 * 0xE09007BE only logs. The MPU status array [[0x1E444]+4] keeps the real
 * button state (stale only while ML holds). Canon's own self-release macro
 * (0xE05E883A / 0xE05E8804) does the same: byte 1 + SW1 ON, byte 1 + SW2 ON,
 * at completion byte 0 + SW2 OFF, byte 0 + SW1 OFF.
 *
 * One picture (iv_shoot; only r6_iv_task presses, every call goes through
 * the noinline iv_press, which enforces the order SW1 ON, SW2 ON, SW2 OFF,
 * SW1 OFF and never sends an OFF for a switch ML did not press):
 *  0  the checks of the tick on two snapshots in a row (equal gating fields)
 *  A  SW1 ON; within R6IV_SW1_MS CC must take it (state 2 or [cc+0xA68] bit 1)
 *  B  hold R6IV_SW1_GAP_MS; then all checks again on a fresh snapshot
 *     (state 2, DF 1, E0 0, no SW event pending, no release sequence)
 *  C  SW2 ON; at the first sign that the shot is under way (SHOOT_START
 *     state 5, EXPOSURE_END 6, bulb 12, RELEASE_END, release counter, job
 *     count, new job ID) or at the latest after R6IV_SW2_MS: SW2 OFF, then
 *     SW1 OFF in the same tick. State 4 and the rising edge of the release
 *     sequence are not triggers (a SW2 OFF in state 4 sends RELEASE_STOP).
 *     The real button changing ML's bytes, or a shutdown, drops both at once.
 *  D  after a timeout OFF: R6IV_LATEPIC_MS late watch (a deferred ON can
 *     still be accepted by a dispatch round that was already running)
 *  E  Canon idle again (state 1 / 3, no sequence, bytes 0, no SW events)
 *  F  the picture (counter / file number), Canon's job count back to 0
 *  G  final state checks; anything unexpected stops the series.
 * Expected [V]: state 1/3 -> 2 (SW1) -> 4 (ActRelStart, ShutterMode 1) ->
 * [0x108AC] = [cc+0xB54] = 1 (RELEASE_START) -> 5 (SHOOT_START: the OFFs;
 * both deferred, 888 = 1) -> 6 (EXPOSURE_END) -> RELEASE_END (sequence 0)
 * -> ML's deferred SW1 OFF runs -> state 3.
 *
 * Why there is no second picture [V]: a release starts only when a SW2 ON
 * event is consumed in state 2 / 3 / 6 (0xE05E3234 -> ActRelStart); ML posts
 * one per picture and never presses again within a series. A held byte
 * becomes an event again only through a re-post at ActRelSeqComp
 * (0xE0298972, only for bytes == 1): with [cc+0x580] == 2 (refused before
 * the picture, REMOTE), or with [cc+0xA6C] == 2 && [cc+0x888] == 1, and 888
 * is set only by a SW1 OFF deferred while [cc+0xB54] == 1. ML sends that
 * OFF only after its SW2 OFF, whose callback wrote E0 = 0 before it posted,
 * so the re-post has nothing to send for SW2. ML holds SW2 only until the
 * first committed signal (<= 1 s; normally SHOOT_START, before state 6).
 * Detected anyway (stop EXTRA): release counter / PROP_RELEASE_COUNTER /
 * file number step > 1, or the sequence starting again after Canon was
 * idle. A picture between two slots that ML did not take stops the series
 * (FOREIGN), a late release after a timeout OFF too (LATE).
 *
 * [I] (inferred, not traced to the end) facts are only logged ("log first"):
 * the CC copies of AE mode, drive, Tv and the AF flag (R6IV_GATE_CC_SETTINGS
 * 0: shown in the menu, not checked), the [I] CC pointer copies, [cc+0x730],
 * [cc+0x3B4], the aux word, ACCESS_REMAIN. Fixed limits: M, Tv <= 1 s,
 * single shot, MF, LV running, no Canon menu, at most 50 pictures.
 *
 * Canon's screen saver / eco mode (GUI mode 0xE08E / 0xE08F) ends a series:
 * ML cannot wake the camera without a new Canon input. Canon's auto power
 * off and screen off cannot be held off without a write: the user sets them
 * to Disable / the longest time; an ML shutdown stops the series. Stop
 * conditions are checked every 50 ms (every 10 ms inside a picture). Status
 * in the ML menu only; details in Debug > LV probe > Log snapshot (refused
 * while a series runs). Nothing is saved except interval, count and start
 * delay; a series never starts by itself.
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
#include "r6_interval.h"

#ifdef CONFIG_R6_INTERVAL

#ifdef FEATURE_INTERVALOMETER
#error "R6 M8.0: upstream's intervalometer is not approved (SetGUIRequestMode(0), LED MMIO, boot autostart, script files)"
#endif
#ifdef CONFIG_PROP_REQUEST_CHANGE
#error "R6 M8.0: property writes stay off"
#endif

extern int lv_action_raw_get(uint32_t * value, int * len);   /* propvalues.c */

/* stubs.S (M8.1, approved 2026-10-03). Canon calls them from SIO3_ISR (MPU msg cmd 3, key 5 / 6) through the
 * switch wrapper 0xE06D30AE. status: 1 = pressed, anything else = released (ML passes only 0 / 1).
 * Returns SetEventFlag([cc+8], 1) = 0. No wait, no lock besides two short IRQ-off sections.
 * Only caller: iv_press(). */
extern int CC_SW1_CBR(int status);   /* [cc+0xDF] = status; post 0x20000400 / 0x20000800 */
extern int CC_SW2_CBR(int status);   /* [cc+0xE0] = status; post 0x20001000 / 0x20002000 */

#define R6IV_EARLY_MS      20000   /* no picture in the first 20 s after boot */
#define R6IV_LV_MS          2000   /* Canon LV must run for this long */
#define R6IV_MENU_HOLD_MS   3000   /* no picture until this long after the ML menu closed */
#define R6IV_GUI_GRACE_MS   3000   /* Canon's GUI mode follows the ML menu (0x2003) with a delay */
#define R6IV_LATE_MS        2000   /* a picture this late after its slot is skipped (no catch-up) */
#define R6IV_WAIT_MAX_MS   60000   /* camera not ready this long (ML menu excluded): stop */
#define R6IV_POLL_MS          10   /* poll period inside a picture */
#define R6IV_BTN_MS         30000    /* real button moved in phase A / B: wait this long for the user to let go */
#define R6IV_SW1_MS          300   /* SW1 ON must be taken (state 2 or [cc+0xA68] bit 1) */
#define R6IV_SW1_GAP_MS      100   /* SW1 held before SW2 ON. ROM: optional, Canon's macro uses 0, factory 50 */
#define R6IV_SW2_MS         1000   /* latest OFFs after SW2 ON (= FactorySW2's budget up to RELEASE_START) */
#define R6IV_LATEPIC_MS     3000   /* after a timeout OFF: watch for a late release (Tv <= 1 s + 2 s) */
#define R6IV_SEQ_MS         3000   /* after the OFFs: Canon's release sequence idle again (Tv <= 1 s + 2 s) */
#define R6IV_IDLE_MS        1000   /* after an abort before SW2: wait for state 1 / 3 (log only) */
#define R6IV_PIC_MS        15000   /* new picture via property, counted from SW1 ON */
#define R6IV_JOB_MS        30000   /* Canon's job count [0x9DC0] == 0, counted from SW1 ON */
#define R6IV_SETTLE_MS       300   /* after the job: late counter updates (extra pictures) */
#define R6IV_MAX_PICS         50   /* per series, until a long test showed no leak */
#define R6IV_TEST_MS        3000   /* "Test: 1 picture": delay after the command */
#define R6IV_GATE_CC_SETTINGS  0   /* [I] CC copies of AE mode / drive / Tv / AF: 0 = logged only (flip after T1-T3) */
/* R6 PROP_SHUTTER = Canon's PROP_SET_TV (camera test 2026-10-03, LVP00037,
 * and the ROM): exactly 2 bytes (CC master size table 0xE0FE3E0C; writer
 * ManualSafetyShift 0xE0946A5A, readers ldrh+sxth 0xE066FDCE / 0xE0750948),
 * a signed 16-bit Tv in EV * 768 (Canon "Code768"), 256 per 1/3 stop, 384
 * per 1/2 stop: 1 s = 0x0000, 1/80 = 0x1300, 1/100 = 0x1400, 1/125 = 0x1500
 * (7 EV), 4 s = 0xFA00 (-2 EV), 30 s = 0xF100, Bulb 0xDF00, 0xDC00 (flag).
 * The bytes after the 2 delivered ones are undefined. Not ML's raw 56 + 8 *
 * EV (upstream lens.c keeps the low byte, 0 on the R6). Accepted: a multiple
 * of 128 in 0 (1 s) .. 10752 (14 EV = 1/16000 s); anything else (also a
 * delivery that is not 2 bytes long) counts as unknown. */
#define R6IV_TV_EV768_MIN      0   /* 1 s */
#define R6IV_TV_EV768_MAX  10752   /* 1/16000 s */
#define R6IV_GUI_REVIEW   0x4033   /* Canon's quick review (SetGUIMode 0xE0BD35E4) */
#define R6IV_GUI_SR       0x4034   /* LvReview's SR mode (GUI_RequestSR from action_None_NotifySR 0xE0B9BD1A) */
#define R6IV_GUI_SAVER    0xE08E   /* START_SCREEN_SAVER (GuiUtilTftPowerSaveController 0xE0BA6F74 / 0xE0BA6ECA) */
#define R6IV_GUI_ECO      0xE08F   /* START_ECO_MODE (0xE0BA6CFA) */

/* why the OFFs went out (r6_iv_shot.off_why, trace code X) */
enum {
    IV_WHY_NONE = 0,
    IV_WHY_S5,               /* 1: state 5 (SHOOT_START) */
    IV_WHY_S6,               /* 2: state 6 (EXPOSURE_END) */
    IV_WHY_S12,              /* 3: state 12 (bulb; not expected, stops the series afterwards) */
    IV_WHY_RELEND,           /* 4: release sequence seen 1, now 0 (RELEASE_END) */
    IV_WHY_RELCNT,           /* 5: RAM release counter stepped (only if it matched PROP_RELEASE_COUNTER) */
    IV_WHY_JOB,              /* 6: [0x9DC0] != 0 (PoCreateJob in the scenario) */
    IV_WHY_JID,              /* 7: [0x9DB8] stepped (CreateJob) */
    IV_WHY_TIMEOUT,          /* 8: R6IV_SW2_MS, nothing accepted */
    IV_WHY_STUCK4,           /* 9: R6IV_SW2_MS, accepted but not committed (state 4) */
    IV_WHY_BUTTON,           /* 10: the real button changed DF / E0 */
    IV_WHY_SHUTDOWN,         /* 11: ML shutdown */
    IV_WHY_RET,              /* 12: a switch call returned != 0 or was refused */
};
#define IV_X_ABORT  100      /* trace X before SW2 ON: 100 + enum r6_iv_stop */

/* failed idle / final checks (r6_iv_shot.final, stop_val of SEQ / CCSTATE) */
#define IV_FIN_STATE   0x001 /* state not 1 / 3 (or CC not readable) */
#define IV_FIN_RELSEQ  0x002 /* [0x108AC] */
#define IV_FIN_B54     0x004 /* [cc+0xB54] */
#define IV_FIN_DF      0x008 /* SW1 byte */
#define IV_FIN_E0      0x010 /* SW2 byte */
#define IV_FIN_SWEV    0x020 /* SW bits pending or working */
#define IV_FIN_A68     0x040 /* [cc+0xA68] & 2 (SW1 holder) */
#define IV_FIN_SHMODE  0x080 /* ShutterMode != 1 */
#define IV_FIN_FANEO   0x100 /* FA-Neo list not empty */
#define IV_FIN_RELCNT  0x200 /* release counter step != 1 (only if used) */
#define IV_FIN_RELCB   0x400 /* [0x108A8] != 0 (self-release macro in flight) */

static const int iv_intervals[] = { 3, 5, 10, 15, 20, 30, 45, 60, 90, 120, 180, 300, 600, 900, 1800, 3600 };
static const char * iv_interval_names[] = { "3s", "5s", "10s", "15s", "20s", "30s", "45s", "1m", "1m30s",
                                            "2m", "3m", "5m", "10m", "15m", "30m", "1h" };
static const int iv_delays[] = { 3, 5, 10, 30, 60 };
static const char * iv_delay_names[] = { "3s", "5s", "10s", "30s", "1m" };

/* saved: only the settings; a running series is never saved and never resumed */
CONFIG_INT("r6.iv.interval", r6_iv_interval_idx, 2);    /* 10 s */
CONFIG_INT("r6.iv.count",    r6_iv_count, 10);
CONFIG_INT("r6.iv.delay",    r6_iv_delay_idx, 0);       /* 3 s */

static struct r6_iv_stats st;
static struct r6_iv_last L;              /* last picture: snapshots and trace (r6_iv_task writes) */
static volatile int iv_cmd = 0;          /* menu -> task: 1 = start, 2 = stop, 3 = test (last press wins) */
static int iv_refused_cam = R6IV_CAM_OK; /* why the last Start / Test was refused (menu) */
static volatile int iv_t_menu = -100000; /* last time the task saw the ML menu open */
static int iv_t_lv = -1;                 /* Canon LV running since (ms), -1 = not running */
static int iv_gui_seen = 0;              /* foreign Canon GUI mode seen during the series (latched), 0 = none */
static int iv_due = 0;                   /* ms of the next picture slot */
static uint32_t iv_wait_ms = 0;          /* camera not ready (ML menu excluded) since the last picture */
static uint32_t iv_wait_mask = 0;
static volatile int iv_wait_menu = 0;    /* the task waits for the ML menu (status line) */
static volatile int iv_in_pic = 0;       /* a picture is running (status "pressing", passive trace off) */
static volatile int iv_tv_prop = 0;      /* PROP_SHUTTER as delivered, int16 (lens.c drops it while ML still sees Av / P) */
static volatile int iv_tv_seen = 0;
static volatile int iv_tv_len = 0;
static volatile uint32_t iv_aeb = 0;     /* PROP_AEB as delivered (shoot.c keeps its copy static) */
static volatile int iv_aeb_seen = 0;

/* the switch calls (iv_press) */
enum { IV_SW1 = 1, IV_SW2 = 2 };
static int iv_held = 0;                  /* bit IV_SW1 / IV_SW2: ML's own press, set after the ON call, cleared after the OFF call */
static uint32_t iv_press_n[4];           /* SW1 ON, SW2 ON, SW2 OFF, SW1 OFF calls since boot (log) */
static uint32_t iv_press_refused;        /* iv_press refused a call (log; must stay 0) */
static struct r6_iv_shot * iv_e = 0;     /* ring entry of the running picture: switch returns are ORed into ret */
static int iv_t1 = 0;                    /* ms of SW1 ON of the running / last picture (trace dt) */

/* picture context (r6_iv_task only) */
static struct r6_iv_cc iv_s, iv_s2;      /* poll snapshot; second read of the tick check */
static struct {
    int cnt0, fn0;                       /* total_shots_count / file number right before SW1 ON */
    uint32_t jid0, relcnt0;              /* [0x9DB8], RAM release counter right before SW1 ON */
    int use_rel;                         /* the release counter matched total_shots_count: it counts */
} iv_ref;

/* trace: S R B D E P J C I M G T F (value changes), 1..4 the switch calls, X off_why / abort */
#define IV_NTR 13
static const char iv_tr_codes[IV_NTR + 1] = "SRBDEPJCIMGTF";
static uint32_t iv_trl[IV_NTR];          /* last recorded values, picture trace (deltas) */
static uint32_t iv_ptl[IV_NTR];          /* last recorded values, passive trace (absolute) */
static int iv_ptl_ok = 0;
static struct r6_iv_ptr iv_pt[R6IV_PTRACE];
static uint32_t iv_pt_n = 0;             /* passive entries since boot */

/* read-only copies of already registered properties (lens.c / shoot.c have the other handlers) */
PROP_HANDLER(PROP_SHUTTER)
{
    iv_tv_len = len;
    if (len != 2) { iv_tv_seen = 0; return; }   /* unknown layout: Start stays refused */
    int16_t t;
    memcpy(&t, buf, 2);                          /* only the delivered bytes */
    iv_tv_prop = t;
    iv_tv_seen = 1;
}

/* exposure bracketing: in M it lengthens the exposure (1 s at +3 EV = 8 s).
 * 4 bytes (CC master size table); signed low halfword = bracketing step,
 * 0x100 per 1/3 EV (GUI tables 0xE1F022D8 / 0xE1F02534), 0 = off; byte 2 =
 * two-shot type (CameraConductor keeps it only with a step, 0xE03B2D84..
 * 0xE03B2DB2), byte 3 unused. Canon's own "AEB on" tests use the low
 * halfword only (0xE05B38D4, 0xE0AB088A, 0xE0B12EB8), and so does ML. */
PROP_HANDLER(PROP_AEB)
{
    iv_aeb = buf[0];
    iv_aeb_seen = 1;
}

static const char * const iv_stop_names[R6IV_NSTOPS] = {
    "-", "done", "stopped (menu)", "ML shutdown", "camera setting", "Canon state", "Canon GUI",
    "display off (power saving)", "camera not ready", "switch call returned / refused", "no new picture", "extra picture",
    "Canon job open", "half press not taken", "aborted before full press", "full press not taken",
    "release did not start", "late picture", "shutter button used", "Canon sequence not idle",
    "Canon state after picture", "picture outside ML",
};
static const char * const iv_cam_names[R6IV_NCAMS] = {
    "ok", "movie mode", "not M", "Tv > 1s or unknown", "not single shot", "not MF", "no card / full", "AEB on",
    "Canon CC: not M", "Canon CC: not single", "Canon CC: Bulb", "Canon CC: AF active",
};
static const char * const iv_canon_names[R6IV_NCANON] = {
    "ready", "CameraConductor not ready", "shutter callbacks not registered", "Canon switch path diverted",
    "CC event flag invalid", "factory release mark, reboot", "Canon remote sequence",
    "boot < 20 s", "LV not running", "review / GUI state", "Canon job running", "buffer full",
    "UI locked", "sensor cleaning", "ML menu", "shutter held / pending", "release sequence active",
    "CC not in LV still",
};

const struct r6_iv_stats * r6_iv_stats(void) { return &st; }
const struct r6_iv_last * r6_iv_last(void) { return &L; }
int r6_iv_running(void) { return st.running || iv_cmd == 1 || iv_cmd == 3; }
const char * r6_iv_stop_name(int s) { return (s >= 0 && s < R6IV_NSTOPS) ? iv_stop_names[s] : "?"; }
const char * r6_iv_cam_name(int c) { return (c >= 0 && c < R6IV_NCAMS) ? iv_cam_names[c] : "?"; }
const char * r6_iv_canon_name(int c) { return (c >= 0 && c < R6IV_NCANON) ? iv_canon_names[c] : "?"; }
int r6_iv_gate_cc_settings(void) { return R6IV_GATE_CC_SETTINGS; }
int r6_iv_tv_prop(int * seen, int * len)
{
    if (seen) *seen = iv_tv_seen;
    if (len) *len = iv_tv_len;
    return iv_tv_prop;
}

int r6_iv_tv_ev768(int * ev)
{
    if (!iv_tv_seen) return 0;
    int e = iv_tv_prop;
    if (e % 128) return 0;                  /* neither 1/3 nor 1/2 stops (and not Canon's flag values) */
    *ev = e;
    return 1;
}

int r6_iv_aeb(int * seen) { if (seen) *seen = iv_aeb_seen; return iv_aeb; }

int r6_iv_press_info(uint32_t n[4], uint32_t * refused)
{
    for (int i = 0; i < 4; i++) n[i] = iv_press_n[i];
    *refused = iv_press_refused;
    return iv_held;
}

int r6_iv_ptrace(int i, struct r6_iv_ptr * e)
{
    uint32_t n = iv_pt_n;
    int cnt = n < R6IV_PTRACE ? (int) n : R6IV_PTRACE;
    if (i >= 0 && i < cnt) *e = iv_pt[(n - cnt + i) % R6IV_PTRACE];
    return cnt;
}

/* EV * 768 -> ML's raw Tv (56 + 8 * EV) like Canon's own conversion
 * (0xE09B643E, + 56; exact for its 82 table entries); for lens_format_shutter */
static int iv_ev768_to_raw(int t)
{
    int x = t / 32;
    return 56 + (x + (x >= 0 ? 1 : -1)) / 3;
}

static int iv_interval_s(void) { return iv_intervals[COERCE(r6_iv_interval_idx, 0, COUNT(iv_intervals) - 1)]; }
static int iv_delay_s(void) { return iv_delays[COERCE(r6_iv_delay_idx, 0, COUNT(iv_delays) - 1)]; }
static int iv_count(void) { return COERCE(r6_iv_count, 1, R6IV_MAX_PICS); }

static int iv_file_number(void)
{
    struct card_info * c = get_shooting_card();
    return c ? c->file_number : 0;
}

/* ---- Canon RAM (read-only) ---- */

/* same rule as lvprobe.c ram_ok() region 1: cached RAM, page 0 unmapped, 4-aligned, no wrap */
static int iv_ram_ok(uint32_t a, uint32_t n)
{
    return !(a & 3) && n && a >= 0x1000 && a + n - 1 > a && a + n - 1 < 0x40000000;
}
#define IV_RD(a) (*(volatile uint32_t *) (a))   /* only after iv_ram_ok, or for the fixed globals below */

/* [V] copies (the post, ActRelSeq, the state getter, the switch wrapper), then [I] copies */
static const uint32_t iv_copy_addr[9] = {
    R6IV_RAM_CC_MASTER, R6IV_RAM_CC_RELSEQ, R6IV_RAM_CC_STATEGET, R6IV_RAM_CC_SWWRAP,
    R6IV_RAM_CC_SHMODE, R6IV_RAM_CC_LVH1, R6IV_RAM_CC_LVH2, R6IV_RAM_CC_JSW2OFF, R6IV_RAM_CC_JSW1OFF,
};

/* one snapshot: about 60 words, no Canon call. Byte fields through their aligned word. */
void r6_iv_cc_read(struct r6_iv_cc * s)
{
    memset(s, 0, sizeof(*s));
    uint32_t cc = IV_RD(R6IV_RAM_CC);
    s->cc = cc;
    for (int i = 0; i < 9; i++) s->copy[i] = IV_RD(iv_copy_addr[i]);
    s->swhack_q = IV_RD(R6IV_RAM_SWHACK_Q);
    s->relseq = IV_RD(R6IV_RAM_RELSEQ);
    s->relcb = IV_RD(R6IV_RAM_RELCB);
    s->aux2 = IV_RD(R6IV_RAM_AUX2);
    s->jobs = IV_RD(R6IV_RAM_JOBCNT);
    s->jid = IV_RD(R6IV_RAM_JOB_LASTID);
    s->jdone = IV_RD(R6IV_RAM_JOB_DONEID);
    s->access = IV_RD(R6IV_RAM_JOB_ACCESS);
    s->faneo[0] = IV_RD(R6IV_RAM_FANEO);
    s->faneo[1] = IV_RD(R6IV_RAM_FANEO_TAIL);
    s->tsc = total_shots_count;
    s->fn = iv_file_number();
    s->gui = CURRENT_GUI_MODE;
    if (iv_ram_ok(cc, R6IV_CC_SIZE))
    {
        s->valid |= R6IV_V_CC;
        s->evflag = IV_RD(cc + R6IV_CC_EVFLAG);
        s->table = IV_RD(cc + R6IV_CC_TABLE);
        s->pend = IV_RD(cc + R6IV_CC_PEND2);
        s->work = IV_RD(cc + R6IV_CC_WORK2);
        s->dispatch = IV_RD(cc + R6IV_CC_DISPATCH);
        s->df = IV_RD(cc + R6IV_CC_SW1BYTE_W) >> 24;               /* [cc+0xDF] */
        s->e0 = IV_RD(cc + R6IV_CC_SW2BYTE_W) & 0xFF;              /* [cc+0xE0] */
        s->snap_df = IV_RD(cc + R6IV_CC_SNAP1_W) >> 24;            /* [cc+0x16B] */
        s->snap_e0 = IV_RD(cc + R6IV_CC_SNAP2_W) & 0xFF;           /* [cc+0x16C] */
        s->lvkind = (IV_RD(cc + R6IV_CC_LVKIND_W) >> 16) & 0xFF;   /* [cc+0x2CE] */
        s->state = IV_RD(cc + R6IV_CC_STATE);
        s->state_prev = IV_RD(cc + R6IV_CC_STATE_PREV);
        s->af = IV_RD(cc + R6IV_CC_AF);
        s->swgate = IV_RD(cc + R6IV_CC_SWGATE);
        s->r3b4 = IV_RD(cc + R6IV_CC_R3B4);
        s->remote = IV_RD(cc + R6IV_CC_REMOTE);
        s->r5e8 = IV_RD(cc + R6IV_CC_R5E8);
        s->r730 = IV_RD(cc + R6IV_CC_R730);
        s->r888 = IV_RD(cc + R6IV_CC_R888);
        s->shoottype = IV_RD(cc + R6IV_CC_SHOOTTYPE);
        s->shmode = IV_RD(cc + R6IV_CC_SHMODE);
        s->a68 = IV_RD(cc + R6IV_CC_A68);
        s->a6c = IV_RD(cc + R6IV_CC_A6C);
        s->b54 = IV_RD(cc + R6IV_CC_B54);
        s->bfc = IV_RD(cc + R6IV_CC_BFC);
        uint32_t p = IV_RD(cc + R6IV_CC_P);
        s->p = p;
        if (iv_ram_ok(p, R6IV_P_SIZE))
        {
            s->valid |= R6IV_V_P;
            s->p_ae = IV_RD(p + R6IV_P_AE);
            s->p_drive = IV_RD(p + R6IV_P_DRIVE);
            s->p_tv = IV_RD(p + R6IV_P_TV_W) >> 16;                /* (u16) [p+0x61E] */
            s->p_92c = IV_RD(p + R6IV_P_92C);
        }
    }
    uint32_t po = IV_RD(R6IV_RAM_PO);
    s->po = po;
    if (iv_ram_ok(po, R6IV_PO_SIZE))
    {
        s->valid |= R6IV_V_PO;
        s->relcnt = IV_RD(po + R6IV_PO_RELCNT);
    }
    uint32_t mgr = IV_RD(R6IV_RAM_SWMGR);
    s->mgr = mgr;
    if (iv_ram_ok(mgr, R6IV_SWMGR_SIZE))
    {
        uint32_t arr = IV_RD(mgr), sts = IV_RD(mgr + 4);
        if (iv_ram_ok(arr, 8))
        {
            s->valid |= R6IV_V_SW;
            s->cbr[0] = IV_RD(arr);
            s->cbr[1] = IV_RD(arr + 4);
        }
        if (iv_ram_ok(sts, 8))
        {
            s->valid |= R6IV_V_STS;
            s->sts[0] = IV_RD(sts);
            s->sts[1] = IV_RD(sts + 4);
        }
    }
}

static int iv_faneo_empty(const struct r6_iv_cc * s)
{
    return s->faneo[0] == R6IV_RAM_FANEO && s->faneo[1] == R6IV_RAM_FANEO;
}

/* hard checks (spec 3.1), in this order; 0 = all pass. noinline: the one place that compares Canon's
 * registered callbacks [[0x1E444]] with the stub addresses (compared, never called) */
static int __attribute__((noinline)) iv_cc_gate(const struct r6_iv_cc * s)
{
    if (!(s->valid & R6IV_V_CC) || s->copy[0] != s->cc || s->copy[1] != s->cc || s->copy[2] != s->cc || s->copy[3] != s->cc)
        return R6IV_CANON_CCPTR;
    if (!(s->valid & R6IV_V_SW) || s->cbr[0] != (uint32_t) &CC_SW1_CBR || s->cbr[1] != (uint32_t) &CC_SW2_CBR)
        return R6IV_CANON_SWREG;
    if (s->swgate || s->swhack_q) return R6IV_CANON_SWPATH;
    if (!s->evflag || (s->evflag & 1)) return R6IV_CANON_EVFLAG;
    if (s->shmode == 10 || !iv_faneo_empty(s)) return R6IV_CANON_FACTORY;
    if (s->remote) return R6IV_CANON_REMOTE;
    return R6IV_CANON_OK;
}

/* CC in the LV still table family with dispatch on (state not checked here). noinline: the only
 * comparison with the ROM address of the LV-still state table */
static int __attribute__((noinline)) iv_cc_lvstill(const struct r6_iv_cc * s)
{
    return (s->valid & R6IV_V_CC) && s->table == R6IV_LV_STILL_TABLE && s->lvkind == 1 && s->shoottype == 3
        && s->dispatch == 1;
}

/* no shutter byte, no SW event pending / working, no self-release macro in flight */
static int iv_cc_swfree(const struct r6_iv_cc * s)
{
    return !s->df && !s->e0 && !((s->pend | s->work) & R6IV_SW_EVENT_MASK) && !s->relcb;
}

/* gating fields of two snapshots equal (torn read check before SW1 ON) */
static int iv_cc_same(const struct r6_iv_cc * a, const struct r6_iv_cc * b)
{
    for (int i = 0; i < 4; i++) if (a->copy[i] != b->copy[i]) return 0;
    return a->valid == b->valid && a->cc == b->cc && a->evflag == b->evflag && a->table == b->table
        && ((a->pend ^ b->pend) & R6IV_SW_EVENT_MASK) == 0 && ((a->work ^ b->work) & R6IV_SW_EVENT_MASK) == 0
        && a->dispatch == b->dispatch && a->df == b->df
        && a->e0 == b->e0 && a->lvkind == b->lvkind && a->state == b->state && a->swgate == b->swgate
        && a->remote == b->remote && a->shoottype == b->shoottype && a->shmode == b->shmode
        && a->b54 == b->b54 && a->swhack_q == b->swhack_q && a->relseq == b->relseq && a->relcb == b->relcb
        && a->mgr == b->mgr && a->cbr[0] == b->cbr[0] && a->cbr[1] == b->cbr[1]
        && a->faneo[0] == b->faneo[0] && a->faneo[1] == b->faneo[1] && a->jobs == b->jobs;
}

/* [I] CC copies of the settings (spec 3.3): logged first, gate only with R6IV_GATE_CC_SETTINGS */
static int iv_cc_settings(const struct r6_iv_cc * s)
{
    if (!(s->valid & R6IV_V_P) || s->p_ae != 3) return R6IV_CAM_CC_MODE;
    if (s->p_drive != 0) return R6IV_CAM_CC_DRIVE;
    if (s->p_tv == 0xDF00) return R6IV_CAM_CC_BULB;
    if (s->af == 1) return R6IV_CAM_CC_AF;
    return R6IV_CAM_OK;
}

int r6_iv_cam_check(void)
{
    if (is_movie_mode() || lv_movie_select == LVMS_ENABLE_MOVIE) return R6IV_CAM_MOVIE;
    if (shooting_mode != SHOOTMODE_M) return R6IV_CAM_MODE;
    int ev = 0;
    if (!r6_iv_tv_ev768(&ev) || ev < R6IV_TV_EV768_MIN || ev > R6IV_TV_EV768_MAX) return R6IV_CAM_TV;
    if (drive_mode != DRIVE_SINGLE) return R6IV_CAM_DRIVE;
    if ((af_mode & 0xF) != AF_MODE_MANUAL_FOCUS) return R6IV_CAM_FOCUS;
    if (avail_shot <= 0) return R6IV_CAM_CARD;
    if (iv_aeb_seen && (int16_t) (iv_aeb & 0xFFFF)) return R6IV_CAM_AEB;
    if (R6IV_GATE_CC_SETTINGS)
    {
        struct r6_iv_cc s;
        r6_iv_cc_read(&s);
        int c = iv_cc_settings(&s);
        if (c != R6IV_CAM_OK) return c;
    }
    return R6IV_CAM_OK;
}

static int iv_lv_ok(int now)
{
    return iv_t_lv >= 0 && now - iv_t_lv >= R6IV_LV_MS;
}

/* EARLY, then the hard checks, then the waits, all on the snapshot s. task = 0: for the menu
 * display, which is only seen with the ML menu open: skip the ML menu and what it causes (Canon
 * MENU mode: LV stopped, gui_state, CC state). task = 1: s2 = a second read right after s; any
 * gating field that differs counts as CCSTATE (torn read) */
static int iv_canon_check_snap(int * hard, int task, struct r6_iv_cc * s, struct r6_iv_cc * s2)
{
    int now = get_ms_clock();
    *hard = 0;
    r6_iv_cc_read(s);
    if (task) r6_iv_cc_read(s2);
    if (now < R6IV_EARLY_MS) return R6IV_CANON_EARLY;
    int k = iv_cc_gate(s);
    if (k != R6IV_CANON_OK) { *hard = 1; return k; }
    if (task)
    {
        if (gui_menu_shown() || now - iv_t_menu < R6IV_MENU_HOLD_MS) return R6IV_CANON_MENU;
        if (!iv_lv_ok(now)) return R6IV_CANON_LV;
        int m = CURRENT_GUI_MODE;
        if (gui_state != GUISTATE_IDLE || m == R6IV_GUI_REVIEW || m == R6IV_GUI_SR) return R6IV_CANON_REVIEW;
    }
    if (s->jobs) return R6IV_CANON_JOBS;
    if (burst_count == 0) return R6IV_CANON_BUFFER;
    if (icu_uilock & 0xFF) return R6IV_CANON_UILOCK;
    if (sensor_cleaning) return R6IV_CANON_CLEANING;
    if (!iv_cc_swfree(s)) return R6IV_CANON_SWHELD;
    if (s->relseq || s->b54) return R6IV_CANON_RELSEQ;
    if (task)
    {
        if (!iv_cc_lvstill(s) || (s->state != 1 && s->state != 3)) return R6IV_CANON_CCSTATE;
        if (!iv_cc_same(s, s2)) return R6IV_CANON_CCSTATE;
    }
    return R6IV_CANON_OK;
}

static int iv_canon_check(int * hard, int task)
{
    struct r6_iv_cc a, b;
    return iv_canon_check_snap(hard, task, &a, &b);
}

int r6_iv_canon_check(int * hard)
{
    return iv_canon_check(hard, 0);
}

static void iv_end(int stop, int detail, int32_t val)
{
    st.stop = stop;
    st.stop_cam = stop == R6IV_STOP_CAMERA ? detail : 0;
    st.stop_canon = (stop == R6IV_STOP_CANON || stop == R6IV_STOP_WAIT || stop == R6IV_STOP_ABORT) ? detail : 0;
    st.stop_val = val;
    st.t_stop = get_ms_clock();
    st.running = 0;
}

static uint32_t iv_test_shots0 = 0;      /* st.shots when the last test started (Test line RINFO) */

static void iv_begin(int now, int test)
{
    int c = r6_iv_cam_check();
    if (c != R6IV_CAM_OK)
    {
        iv_refused_cam = c;
        st.refused++;
        return;
    }
    iv_refused_cam = R6IV_CAM_OK;
    st.test = test;
    if (test) iv_test_shots0 = st.shots;
    st.interval_s = test ? 0 : iv_interval_s();   /* test: unused, n_target 1 ends with DONE */
    st.n_target = test ? 1 : iv_count();
    st.n_done = 0;
    st.skipped = 0;
    st.stop = R6IV_STOP_NONE;
    st.stop_cam = st.stop_canon = 0;
    st.stop_val = 0;
    st.t_start = now;
    st.cnt_last = total_shots_count;
    st.fn_last = iv_file_number();
    iv_due = now + (test ? R6IV_TEST_MS : iv_delay_s() * 1000);
    iv_wait_ms = 0;
    iv_wait_mask = 0;
    iv_wait_menu = 0;
    iv_gui_seen = 0;
    st.running = 1;
}

/* ---- traces ---- */

static void iv_tr_vals(const struct r6_iv_cc * s, uint32_t * v, int rel)
{
    v[0] = s->state;
    v[1] = s->relseq;
    v[2] = s->b54;
    v[3] = s->df;
    v[4] = s->e0;
    v[5] = (s->pend | s->work) & R6IV_SW_EVENT_MASK;
    v[6] = s->jobs;
    v[7] = (s->valid & R6IV_V_PO) ? s->relcnt - (rel ? iv_ref.relcnt0 : 0) : 0;
    v[8] = s->jid - (rel ? iv_ref.jid0 : 0);
    v[9] = s->shmode;
    v[10] = s->gui;
    v[11] = s->tsc - (rel ? iv_ref.cnt0 : 0);
    v[12] = s->fn - (rel ? iv_ref.fn0 : 0);
}

static void iv_tr_add(char code, uint32_t v)
{
    if (L.n >= R6IV_TRACE) { L.lost++; return; }
    int dt = get_ms_clock() - iv_t1;
    L.tr[L.n].dt = COERCE(dt, 0, 0xFFFF);
    L.tr[L.n].code = code;
    L.tr[L.n].v = v;
    L.n++;
}

/* picture trace: every field that differs from its last recorded value */
static void iv_trace(const struct r6_iv_cc * s)
{
    uint32_t v[IV_NTR];
    iv_tr_vals(s, v, 1);
    for (int i = 0; i < IV_NTR; i++)
        if (v[i] != iv_trl[i])
        {
            iv_trl[i] = v[i];
            iv_tr_add(iv_tr_codes[i], v[i]);
        }
}

/* passive trace (only while no picture runs): Canon's own shots, e.g. with a finger */
static void iv_ptrace(int now)
{
    struct r6_iv_cc s;
    uint32_t v[IV_NTR];
    r6_iv_cc_read(&s);
    iv_tr_vals(&s, v, 0);
    if (!iv_ptl_ok) { memcpy(iv_ptl, v, sizeof(v)); iv_ptl_ok = 1; return; }
    for (int i = 0; i < IV_NTR; i++)
        if (v[i] != iv_ptl[i])
        {
            iv_ptl[i] = v[i];
            struct r6_iv_ptr * e = &iv_pt[iv_pt_n % R6IV_PTRACE];
            e->t = now;
            e->code = iv_tr_codes[i];
            e->v = v[i];
            iv_pt_n++;
        }
}

/* ---- the switch calls ---- */

/* every call of the two Canon callbacks goes through here (canon_gate: edges iv_press -> CC_SW1_CBR /
 * CC_SW2_CBR). Rules: on is 0 / 1, sw is SW1 / SW2, only r6_iv_task; SW1 ON only with nothing held,
 * SW2 ON only with SW1 held, SW2 OFF only if ML holds SW2, SW1 OFF only with SW1 alone held (no OFF
 * for a switch ML did not press); [0x10AB8] valid and == [0x68C4] (the callback and the post
 * dereference them without a check). Refused: -1, no Canon call. */
static int __attribute__((noinline)) iv_press(int sw, int on)
{
    int i = on ? (sw == IV_SW1 ? 0 : 1) : (sw == IV_SW2 ? 2 : 3);
    int ok = (on == 0 || on == 1) && (sw == IV_SW1 || sw == IV_SW2)
        && strcmp(get_current_task_name(), "r6_iv_task") == 0;
    if (ok)
    {
        if (on) ok = sw == IV_SW1 ? iv_held == 0 : iv_held == IV_SW1;
        else ok = sw == IV_SW2 ? (iv_held & IV_SW2) != 0 : iv_held == IV_SW1;
    }
    if (ok)
    {
        uint32_t cc = IV_RD(R6IV_RAM_CC);
        ok = iv_ram_ok(cc, R6IV_CC_SIZE) && IV_RD(R6IV_RAM_CC_MASTER) == cc;
    }
    if (!ok)
    {
        iv_press_refused++;
        iv_tr_add('1' + i, (uint32_t) -1);
        if (iv_e) iv_e->ret |= -1;
        return -1;
    }
    int r = (sw == IV_SW1) ? CC_SW1_CBR(on ? 1 : 0) : CC_SW2_CBR(on ? 1 : 0);
    if (on) iv_held |= sw; else iv_held &= ~sw;
    iv_press_n[i]++;
    iv_tr_add('1' + i, r);
    if (iv_e) iv_e->ret |= r;
    return r;
}

/* the only way to let go: SW2 OFF, then SW1 OFF, back to back (no msleep in between) */
static void iv_release_all(void)
{
    if (iv_held & IV_SW2) iv_press(IV_SW2, 0);
    if (iv_held & IV_SW1) iv_press(IV_SW1, 0);
}

/* what the task watches: every tick, and while iv_shoot waits (otherwise an
 * ML menu, an LV restart or a Canon MENU visit inside a picture would go
 * unseen). Canon MENU, playback, Q, screen saver, ...: latched for iv_tick;
 * not while the ML menu (it sets 0x2003 itself) is open or just closed.
 * M8.1: the passive trace while no picture runs */
static void iv_sample(int now)
{
    int menu = gui_menu_shown();
    if (menu) iv_t_menu = now;
    uint32_t v = 1;
    int len = 0;
    if (lv_action_raw_get(&v, &len) && len == 4 && v == 0)
    {
        if (iv_t_lv < 0 || now < iv_t_lv) iv_t_lv = now;
    }
    else iv_t_lv = -1;
    int mode = CURRENT_GUI_MODE;
    if (st.running && !iv_gui_seen && mode != 0 && mode != R6IV_GUI_REVIEW && mode != R6IV_GUI_SR
        && !menu && now - iv_t_menu >= R6IV_GUI_GRACE_MS)
        iv_gui_seen = mode;
    if (!iv_in_pic) iv_ptrace(now);
}

/* camera ready late: skip the slots it missed (no catch-up pictures);
 * "Test: 1 picture" has no interval: it only postpones */
static void iv_skip_late(int now)
{
    if (st.interval_s <= 0) { iv_due = now; return; }
    while (now - iv_due > R6IV_LATE_MS)
    {
        iv_due += st.interval_s * 1000;
        st.skipped++;
    }
}

/* ---- one picture ---- */

/* one poll inside a picture: snapshot, trace, the task's sample, states seen */
static int iv_poll(struct r6_iv_shot * e)
{
    r6_iv_cc_read(&iv_s);
    iv_trace(&iv_s);
    int now = get_ms_clock();
    iv_sample(now);
    if (iv_s.state < 16) e->states |= 1 << iv_s.state;
    return now;
}

static int iv_state13(const struct r6_iv_cc * s) { return (s->valid & R6IV_V_CC) && (s->state == 1 || s->state == 3); }

/* Canon took the full press (any sign of the release) */
static int iv_accepted(const struct r6_iv_cc * s)
{
    return (s->state >= 4 && s->state <= 6) || s->state == 12 || s->relseq || s->b54
        || (iv_ref.use_rel && (s->valid & R6IV_V_PO) && s->relcnt != iv_ref.relcnt0)
        || s->jobs || s->jid != iv_ref.jid0;
}

/* the shot is under way, SW2 is no longer needed: off_why 1..7, else 0. State 4 and the rising
 * edge of the release sequence are not triggers (SW2 OFF in state 4 = RELEASE_STOP) */
static int iv_committed(const struct r6_iv_cc * s, int rose)
{
    if (s->state == 5) return IV_WHY_S5;
    if (s->state == 6) return IV_WHY_S6;
    if (s->state == 12) return IV_WHY_S12;
    if (rose && !s->relseq && !s->b54) return IV_WHY_RELEND;
    if (iv_ref.use_rel && (s->valid & R6IV_V_PO) && s->relcnt != iv_ref.relcnt0) return IV_WHY_RELCNT;
    if (s->jobs) return IV_WHY_JOB;
    if (s->jid != iv_ref.jid0) return IV_WHY_JID;
    return IV_WHY_NONE;
}

/* failed idle conditions (IV_FIN_* bits 0..5); 0 = Canon idle */
static int iv_idle_bits(const struct r6_iv_cc * s)
{
    int m = 0;
    if (!iv_state13(s)) m |= IV_FIN_STATE;
    if (s->relseq) m |= IV_FIN_RELSEQ;
    if (s->b54) m |= IV_FIN_B54;
    if (s->df) m |= IV_FIN_DF;
    if (s->e0) m |= IV_FIN_E0;
    if ((s->pend | s->work) & R6IV_SW_EVENT_MASK) m |= IV_FIN_SWEV;
    return m;
}

/* after Canon was idle: a second release (state 4 / 5 / 6 / 12 or the sequence again) */
static void iv_check_second(struct r6_iv_shot * e, int idle)
{
    int s = iv_s.state;
    if (idle && ((iv_s.valid & R6IV_V_CC) && (s == 4 || s == 5 || s == 6 || s == 12 || iv_s.relseq)))
        e->second = 1;
}

/* end of a picture (every path): the last detail, the passive trace continues from here */
static void iv_pic_done(struct r6_iv_shot * e)
{
    L.post = iv_s;
    e->sm_after = iv_s.shmode;
    e->d_jid = iv_s.jid - iv_ref.jid0;
    if (iv_ref.use_rel && (iv_s.valid & R6IV_V_PO)) e->d_rel = COERCE((int) (iv_s.relcnt - iv_ref.relcnt0), -32767, 32767);
    if (!e->d_cnt) e->d_cnt = total_shots_count - iv_ref.cnt0;
    if (!e->d_fn) e->d_fn = MAX(iv_file_number() - iv_ref.fn0, 0);
    iv_tr_vals(&iv_s, iv_ptl, 0);
    iv_ptl_ok = 1;
    iv_e = 0;
    iv_in_pic = 0;
}

/* stop inside a picture before the full-press outcome (phases A / B, shutdown, button): let go
 * at once; x: trace X value (< 0: none); wait_idle: then wait <= 1 s for state 1 / 3 (log only) */
static void iv_pic_stop(struct r6_iv_shot * e, int stop, int detail, int32_t val, int x, int wait_idle)
{
    iv_release_all();
    int t = get_ms_clock();
    if (!e->dur) e->dur = MAX(t - iv_t1, 0);
    if (x >= 0) iv_tr_add('X', x);
    iv_poll(e);
    while (wait_idle && !iv_state13(&iv_s) && get_ms_clock() - t < R6IV_IDLE_MS)
    {
        msleep(R6IV_POLL_MS);
        iv_poll(e);
    }
    iv_pic_done(e);
    iv_end(stop, detail, val);
}

/* M8.1 review: the real shutter button moved while ML holds only SW1 (phases A / B). The button
 * writes the same bytes, and the MPU switch path posts its OFFs when the user lets go; ML's own
 * SW1 OFF while the user holds SW2 would leave "SW2 held, SW1 released" in CameraConductor (E0 1,
 * DF 0). So ML sends nothing: it waits until the user let go (DF 0 and E0 0, at most
 * R6IV_BTN_MS), then forgets its own press (the user's SW1 OFF ended it), trace 'U' */
static void iv_button_stop(struct r6_iv_shot * e)
{
    int t = get_ms_clock();
    int v = iv_s.df | iv_s.e0 << 8;
    iv_tr_add('X', IV_X_ABORT + R6IV_STOP_BUTTON);
    while ((iv_s.df || iv_s.e0) && get_ms_clock() - t < R6IV_BTN_MS && get_ms_clock() >= t)
    {
        msleep(R6IV_POLL_MS);
        iv_poll(e);
    }
    iv_tr_add('U', iv_s.df | iv_s.e0 << 8);
    iv_held = 0;
    if (!e->dur) e->dur = MAX(get_ms_clock() - iv_t1, 0);
    iv_pic_done(e);
    iv_end(R6IV_STOP_BUTTON, 0, v);
}

/* after a timeout OFF: did a release start anyway? (bits: 1 state 5/6/12 or 4 newly entered,
 * 2 sequence rising, 4 release counter step, 8 job count, 0x10 job ID step, 0x20 counter / file) */
static int iv_late_watch(struct r6_iv_shot * e, int t3)
{
    uint32_t prev_state = L.off.state;
    int prev_rel = L.off.relseq || L.off.b54, m = 0;
    for (;;)
    {
        msleep(R6IV_POLL_MS);
        int now = iv_poll(e);
        uint32_t s = iv_s.state;
        int rel = iv_s.relseq || iv_s.b54;
        if (s == 5 || s == 6 || s == 12 || (s == 4 && prev_state != 4)) m |= 0x01;
        if (rel && !prev_rel) m |= 0x02;
        if (iv_ref.use_rel && (iv_s.valid & R6IV_V_PO) && iv_s.relcnt != L.off.relcnt) m |= 0x04;
        if (iv_s.jobs) m |= 0x08;
        if (iv_s.jid != L.off.jid) m |= 0x10;
        if (iv_s.tsc != iv_ref.cnt0 || iv_s.fn != iv_ref.fn0) m |= 0x20;
        prev_state = s;
        prev_rel = rel;
        if (m || now - t3 >= R6IV_LATEPIC_MS || now < t3) return m;
    }
}

/* one picture; called by the task only, with the camera checked as ready on the snapshot pre */
static void iv_shoot(const struct r6_iv_cc * pre)
{
    /* a picture that ML did not take since the start / ML's last picture */
    int dcnt = total_shots_count - st.cnt_last, dfn = iv_file_number() - st.fn_last;
    if (dcnt || dfn) { iv_end(R6IV_STOP_FOREIGN, 0, dcnt ? dcnt : dfn); return; }
    if (ml_shutdown_requested) { iv_end(R6IV_STOP_SHUTDOWN, 0, 0); return; }

    /* phase 0: prepare */
    struct r6_iv_shot * e = &st.ring[st.shots % R6IV_RING];
    memset(e, 0, sizeof(*e));
    e->wait_ms = iv_wait_ms;
    e->wait_mask = iv_wait_mask;
    e->sw1_ms = e->acc_ms = e->seq_ms = 0xFFFF;
    e->d_rel = -32768;
    e->js_before = lens_info.job_state;
    e->sm_before = pre->shmode;
    iv_ref.cnt0 = total_shots_count;
    iv_ref.fn0 = iv_file_number();
    iv_ref.jid0 = pre->jid;
    iv_ref.relcnt0 = pre->relcnt;
    iv_ref.use_rel = (pre->valid & R6IV_V_PO) && (int) pre->relcnt == iv_ref.cnt0;
    memset(&L, 0, sizeof(L));
    L.pre = *pre;
    if (pre != &iv_s) iv_s = *pre;
    iv_tr_vals(pre, iv_trl, 1);
    iv_in_pic = 1;
    iv_e = e;
    int t1 = iv_t1 = get_ms_clock();
    e->t = t1;
    st.calls++;
    st.shots++;

    /* phase A: half press, CC must take it */
    if (iv_press(IV_SW1, 1)) { iv_pic_stop(e, R6IV_STOP_RET, 0, e->ret, IV_X_ABORT + R6IV_STOP_RET, 0); return; }
    int now = t1;
    for (;;)
    {
        now = iv_poll(e);
        if (ml_shutdown_requested) { iv_pic_stop(e, R6IV_STOP_SHUTDOWN, 0, 0, IV_X_ABORT + R6IV_STOP_SHUTDOWN, 0); return; }
        if (iv_s.df != 1 || iv_s.e0 != 0)
        {
            iv_button_stop(e);                              /* real button: no OFF from ML */
            return;
        }
        if (iv_s.state == 2 || (iv_s.a68 & 2)) break;
        if (now - t1 >= R6IV_SW1_MS || now < t1)
        {
            /* the OFF deletes the pending SW1 ON under Canon's lock [V] */
            iv_pic_stop(e, R6IV_STOP_SW1, 0, iv_s.state, IV_X_ABORT + R6IV_STOP_SW1, 1);
            return;
        }
        msleep(R6IV_POLL_MS);
    }
    e->sw1_ms = MIN(now - t1, 0xFFFE);

    /* phase B: hold SW1, then every check again on a fresh snapshot. A Canon GUI mode change is
     * only traced (G); iv_gui_seen stops the series after the picture */
    int tb = now;
    for (;;)
    {
        msleep(R6IV_POLL_MS);
        now = iv_poll(e);
        if (ml_shutdown_requested) { iv_pic_stop(e, R6IV_STOP_SHUTDOWN, 0, 0, IV_X_ABORT + R6IV_STOP_SHUTDOWN, 0); return; }
        if (iv_s.df != 1 || iv_s.e0 != 0)
        {
            iv_button_stop(e);                              /* real button: no OFF from ML */
            return;
        }
        if (gui_menu_shown())
        {
            iv_pic_stop(e, R6IV_STOP_ABORT, R6IV_CANON_MENU, iv_s.state, IV_X_ABORT + R6IV_STOP_ABORT, 1);
            return;
        }
        if (iv_t_lv < 0)
        {
            iv_pic_stop(e, R6IV_STOP_ABORT, R6IV_CANON_LV, iv_s.state, IV_X_ABORT + R6IV_STOP_ABORT, 1);
            return;
        }
        if (now - tb >= R6IV_SW1_GAP_MS || now < tb) break;
    }
    int k = iv_cc_gate(&iv_s);
    if (k == R6IV_CANON_OK && (!iv_cc_lvstill(&iv_s) || iv_s.state != 2)) k = R6IV_CANON_CCSTATE;
    if (k == R6IV_CANON_OK && (((iv_s.pend | iv_s.work) & R6IV_SW_EVENT_MASK) || iv_s.relcb)) k = R6IV_CANON_SWHELD;
    if (k == R6IV_CANON_OK && (iv_s.relseq || iv_s.b54)) k = R6IV_CANON_RELSEQ;
    if (k != R6IV_CANON_OK)
    {
        iv_pic_stop(e, R6IV_STOP_ABORT, k, iv_s.state, IV_X_ABORT + R6IV_STOP_ABORT, 1);
        return;
    }
    int cam = r6_iv_cam_check();
    if (cam != R6IV_CAM_OK)
    {
        iv_pic_stop(e, R6IV_STOP_ABORT, 0, iv_s.state, IV_X_ABORT + R6IV_STOP_ABORT, 1);
        st.stop_cam = cam;
        return;
    }

    /* phase C: full press; let go at the first committed signal, a button change, a shutdown,
     * or after R6IV_SW2_MS. The first read comes right after the call. */
    int t2 = get_ms_clock();
    int why = iv_press(IV_SW2, 1) ? IV_WHY_RET : IV_WHY_NONE;
    int acc = 0, rose = 0;
    while (why == IV_WHY_NONE)
    {
        now = iv_poll(e);
        if (!acc && iv_accepted(&iv_s)) { acc = 1; e->acc_ms = MIN(MAX(now - t2, 0), 0xFFFE); }
        rose |= iv_s.relseq || iv_s.b54;
        if (ml_shutdown_requested) why = IV_WHY_SHUTDOWN;
        else if (iv_s.df != 1 || iv_s.e0 != 1) why = IV_WHY_BUTTON;
        else if ((why = iv_committed(&iv_s, rose)) != IV_WHY_NONE) break;
        else if (now - t2 >= R6IV_SW2_MS || now < t2) why = acc ? IV_WHY_STUCK4 : IV_WHY_TIMEOUT;
        if (why == IV_WHY_NONE) msleep(R6IV_POLL_MS);
    }
    iv_release_all();                                       /* SW2 OFF, SW1 OFF: same tick */
    int t3 = get_ms_clock();
    L.off = iv_s;
    iv_tr_add('X', why);
    if (e->ret && why != IV_WHY_SHUTDOWN && why != IV_WHY_BUTTON && why != IV_WHY_RET)
    {
        why = IV_WHY_RET;                                   /* an OFF returned != 0 / was refused */
        iv_tr_add('X', why);
    }
    e->off_ms = MIN(MAX(t3 - t2, 0), 0xFFFF);
    e->off_why = why;
    e->off_state = iv_s.state;
    e->dur = MAX(t3 - t1, 0);

    /* phase D: outcome */
    if (why == IV_WHY_SHUTDOWN) { iv_pic_stop(e, R6IV_STOP_SHUTDOWN, 0, 0, -1, 0); return; }
    if (why == IV_WHY_BUTTON)
    {
        /* the user has taken over and may still hold the button */
        iv_pic_stop(e, R6IV_STOP_BUTTON, 0, L.off.df | L.off.e0 << 8, -1, 0);
        return;
    }
    if (why == IV_WHY_TIMEOUT || why == IV_WHY_STUCK4) e->late = iv_late_watch(e, t3);

    /* phase E: Canon idle again (ML's SW1 OFF is deferred while b54 == 1 and runs after RELEASE_END) */
    int idle = 0, seq_bits = 0;
    for (;;)
    {
        now = iv_poll(e);
        seq_bits = iv_idle_bits(&iv_s);
        if (!seq_bits) { idle = 1; e->seq_ms = MIN(MAX(now - t3, 0), 0xFFFE); break; }
        if (now - t3 >= R6IV_SEQ_MS || now < t3) break;
        msleep(20);
    }

    /* phase F: the picture (property), Canon's job, settle; a second release is watched once idle */
    int d_cnt = 0, d_fn = 0;
    if (why != IV_WHY_TIMEOUT || e->late)
    {
        for (;;)
        {
            d_cnt = total_shots_count - iv_ref.cnt0;
            d_fn = MAX(iv_file_number() - iv_ref.fn0, 0);
            if (d_cnt || d_fn) { e->pic_ms = MAX(get_ms_clock() - t1, 0); break; }
            now = get_ms_clock();
            if (now - t1 >= R6IV_PIC_MS || now < t1) break;
            msleep(20);
            iv_poll(e);
            iv_check_second(e, idle);
        }
    }
    uint32_t jobs = 0;
    for (;;)
    {
        jobs = iv_s.jobs;
        if (!jobs) { e->job_ms = MAX(get_ms_clock() - t1, 0); break; }
        now = get_ms_clock();
        if (now - t1 >= R6IV_JOB_MS || now < t1) break;
        msleep(20);
        iv_poll(e);
        iv_check_second(e, idle);
    }
    e->jobs_after = jobs;
    for (int ts = get_ms_clock(); get_ms_clock() - ts < R6IV_SETTLE_MS; )
    {
        msleep(20);
        iv_poll(e);
        iv_check_second(e, idle);
    }
    iv_poll(e);
    iv_check_second(e, idle);
    e->js_after = lens_info.job_state;
    e->d_cnt = d_cnt = total_shots_count - iv_ref.cnt0;
    e->d_fn = d_fn = MAX(iv_file_number() - iv_ref.fn0, 0);

    /* phase G: final checks (log; decisions below) */
    int fin = iv_idle_bits(&iv_s);
    if (iv_s.a68 & 2) fin |= IV_FIN_A68;
    if (iv_s.shmode != 1) fin |= IV_FIN_SHMODE;
    if (!iv_faneo_empty(&iv_s)) fin |= IV_FIN_FANEO;
    int d_rel = (int) (iv_s.relcnt - iv_ref.relcnt0);
    if (iv_ref.use_rel && d_rel != 1) fin |= IV_FIN_RELCNT;
    if (iv_s.relcb) fin |= IV_FIN_RELCB;
    e->final = fin;
    st.cnt_last = total_shots_count;
    st.fn_last = iv_file_number();
    iv_pic_done(e);

    if (why == IV_WHY_TIMEOUT)
    {
        if (e->late) iv_end(R6IV_STOP_LATE, 0, e->late);
        else iv_end(R6IV_STOP_SW2, 0, L.off.state);
        return;
    }
    if (why == IV_WHY_STUCK4) { iv_end(R6IV_STOP_STUCK, 0, L.off.state); return; }
    if (why == IV_WHY_RET) { iv_end(R6IV_STOP_RET, 0, e->ret); return; }
    int rel_extra = iv_ref.use_rel && d_rel > 1;
    if (e->second || d_cnt > 1 || d_fn > 1 || rel_extra)
    {
        int dmax = MAX(d_cnt, d_fn);
        if (iv_ref.use_rel) dmax = MAX(dmax, d_rel);
        iv_end(R6IV_STOP_EXTRA, 0, dmax);
        return;
    }
    if (!idle) { iv_end(R6IV_STOP_SEQ, 0, seq_bits); return; }
    if (!d_cnt && !d_fn) { iv_end(R6IV_STOP_NOPIC, 0, 0); return; }
    if (jobs) { iv_end(R6IV_STOP_JOB, 0, jobs); return; }
    if (fin || why == IV_WHY_S12) { iv_end(R6IV_STOP_CCSTATE, 0, fin | (why == IV_WHY_S12) << 15); return; }

    st.n_done++;
    iv_wait_ms = 0;
    iv_wait_mask = 0;
    if (st.n_done >= st.n_target) { iv_end(R6IV_STOP_DONE, 0, 0); return; }
    iv_due = t1 + st.interval_s * 1000;
}

static void iv_tick(int now, int dt)
{
    int c = iv_cmd;
    if (c)
    {
        iv_cmd = 0;
        if ((c == 1 || c == 3) && !st.running) iv_begin(now, c == 3);
        else if (c == 2 && st.running) iv_end(R6IV_STOP_USER, 0, 0);
    }
    if (!st.running) return;

    /* stop conditions: every tick, also between slots */
    if (ml_shutdown_requested) { iv_end(R6IV_STOP_SHUTDOWN, 0, 0); return; }
    int cam = r6_iv_cam_check();
    if (cam != R6IV_CAM_OK) { iv_end(R6IV_STOP_CAMERA, cam, 0); return; }
    /* Canon MENU, playback, Q, ...: the user took over; screen saver / eco
     * mode: Canon's power saving (latched by iv_sample, also during a picture) */
    int mode = iv_gui_seen;
    if (mode)
    {
        iv_end((mode == R6IV_GUI_SAVER || mode == R6IV_GUI_ECO) ? R6IV_STOP_PWRSAVE : R6IV_STOP_GUI, 0, mode);
        return;
    }
    int hard = 0;
    int k = iv_canon_check_snap(&hard, 1, &iv_s, &iv_s2);
    if (hard) { iv_end(R6IV_STOP_CANON, k, 0); return; }

    int due = now - iv_due >= 0;
    iv_wait_menu = 0;
    if (k != R6IV_CANON_OK)
    {
        /* not ready: counts at a slot, and between slots once a slot found
         * the camera not ready (no picture since) */
        if (k == R6IV_CANON_MENU || (k == R6IV_CANON_LV && now - iv_t_menu < R6IV_MENU_HOLD_MS + R6IV_LV_MS + 1000))
        {
            /* the ML menu, and Canon's LV restart after it: postpone, never skip */
            iv_wait_menu = 1;
            if (due) iv_due = now;
            return;
        }
        if (!due && !iv_wait_ms) return;
        iv_wait_mask |= 1 << k;
        iv_wait_ms += dt;
        if (iv_wait_ms >= R6IV_WAIT_MAX_MS) { iv_end(R6IV_STOP_WAIT, k, iv_wait_ms); return; }
        if (due) iv_skip_late(now);
        return;
    }
    if (!due) return;
    if (now - iv_due > R6IV_LATE_MS) { iv_skip_late(now); return; }
    iv_shoot(&iv_s);
}

/* never ends; the only task that presses (iv_press checks the task name) */
static void r6_iv_task(void * unused)
{
    int last = get_ms_clock();
    for (;;)
    {
        msleep(50);
        int now = get_ms_clock();
        int dt = now - last;
        last = now;
        if (dt < 0 || dt > 1000) dt = 50;     /* clock glitch (SMP) */
        iv_sample(now);
        iv_tick(now, dt);
        if (iv_held)
        {
            /* never expected: iv_shoot lets go on every path */
            iv_release_all();
            iv_end(R6IV_STOP_RET, 0, -3);
        }
    }
}

TASK_CREATE("r6_iv_task", r6_iv_task, 0, 0x1a, 0x4000);

/* ---- menu ---- */
static MENU_SELECT_FUNC(iv_start_stop)
{
    if (iv_cmd == 2) return;                 /* Stop pressed, not taken over by the task yet */
    if (st.running || iv_cmd == 1 || iv_cmd == 3) { iv_cmd = 2; return; }
    int c = r6_iv_cam_check();
    if (c != R6IV_CAM_OK)
    {
        iv_refused_cam = c;
        st.refused++;
        return;
    }
    iv_refused_cam = R6IV_CAM_OK;
    iv_cmd = 1;
}

/* running: "pressing" / "waiting: close ML menu" (the slot is due, the ML menu holds it) / time to the slot */
static void iv_run_status(char * buf, int n, const char * next)
{
    int due = (iv_due - get_ms_clock() + 999) / 1000;
    if (iv_in_pic) snprintf(buf, n, "pressing");
    else if (iv_wait_menu && due <= 0) snprintf(buf, n, "waiting: close ML menu");
    else snprintf(buf, n, "%s %ds", next, MAX(due, 0));
}

static MENU_UPDATE_FUNC(iv_start_update)
{
    if (iv_cmd == 2)
    {
        MENU_SET_NAME("Stop");
        MENU_SET_VALUE("stopping");
        return;
    }
    if (st.running || iv_cmd == 1 || iv_cmd == 3)
    {
        MENU_SET_NAME("Stop");
        char s[32];
        iv_run_status(s, sizeof(s), "next");
        if (iv_cmd) MENU_SET_VALUE("starting");
        else MENU_SET_VALUE("%d/%d, %s", st.n_done, st.n_target, s);
        MENU_SET_ICON(MNI_ON, 0);
        return;
    }
    MENU_SET_NAME("Start");
    MENU_SET_ICON(MNI_ACTION, 0);
    if (iv_refused_cam != R6IV_CAM_OK)
    {
        MENU_SET_VALUE("refused");
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Not started: %s (see Camera).", r6_iv_cam_name(iv_refused_cam));
    }
}

/* the warning of "Last series" (also "Test: 1 picture"). Bit tests on 1 << stop, not a switch or an
 * if-chain on the value: gcc makes a tbb jump table of those, whose bytes read as high "constants" in
 * canon_gate */
#define IV_B(x) (1u << (x))
static void iv_stop_warning(struct menu_entry * entry, struct menu_display_info * info)
{
    int s = st.stop;
    uint32_t b = (s >= 0 && s < R6IV_NSTOPS) ? IV_B(s) : 0;
    const uint32_t snap = IV_B(R6IV_STOP_RET) | IV_B(R6IV_STOP_EXTRA) | IV_B(R6IV_STOP_JOB) | IV_B(R6IV_STOP_SW1)
        | IV_B(R6IV_STOP_SW2) | IV_B(R6IV_STOP_STUCK) | IV_B(R6IV_STOP_LATE) | IV_B(R6IV_STOP_SEQ) | IV_B(R6IV_STOP_CCSTATE);
    if (b & IV_B(R6IV_STOP_CAMERA))
        MENU_SET_WARNING(MENU_WARN_INFO, "Stopped: %s.", r6_iv_cam_name(st.stop_cam));
    if (b & (IV_B(R6IV_STOP_CANON) | IV_B(R6IV_STOP_WAIT)))
        MENU_SET_WARNING(MENU_WARN_INFO, "Stopped: %s (%s).", r6_iv_stop_name(s), r6_iv_canon_name(st.stop_canon));
    if (b & IV_B(R6IV_STOP_GUI))
        MENU_SET_WARNING(MENU_WARN_INFO, "Stopped: Canon GUI mode 0x%x.", (int) st.stop_val);
    if (b & IV_B(R6IV_STOP_PWRSAVE))
        MENU_SET_WARNING(MENU_WARN_INFO, "Stopped: Canon screen off / eco mode. Set Screen off to the longest time.");
    if (b & IV_B(R6IV_STOP_ABORT))
        MENU_SET_WARNING(MENU_WARN_INFO, "Stopped before the full press: %s (state %d). Log snapshot, please.",
            st.stop_cam ? r6_iv_cam_name(st.stop_cam) : r6_iv_canon_name(st.stop_canon), (int) st.stop_val);
    if (b & IV_B(R6IV_STOP_NOPIC))
        MENU_SET_WARNING(MENU_WARN_INFO, "Stopped: no new picture seen. Log snapshot, please.");
    if (b & IV_B(R6IV_STOP_BUTTON))
        MENU_SET_WARNING(MENU_WARN_INFO, "Stopped: shutter button used (0x%x). Log snapshot, please.", (int) st.stop_val);
    if (b & IV_B(R6IV_STOP_FOREIGN))
        MENU_SET_WARNING(MENU_WARN_INFO, "Stopped: picture outside ML (%d). Log snapshot, please.", (int) st.stop_val);
    if (b & snap)
        MENU_SET_WARNING(MENU_WARN_INFO, "Stopped: %s %d (0x%x). Log snapshot, please.", r6_iv_stop_name(s),
            (int) st.stop_val, (int) st.stop_val);
}

static MENU_SELECT_FUNC(iv_test_select)
{
    if (st.running || iv_cmd != 0) return;
    int c = r6_iv_cam_check();
    if (c != R6IV_CAM_OK)
    {
        iv_refused_cam = c;
        st.refused++;
        return;
    }
    iv_refused_cam = R6IV_CAM_OK;
    iv_cmd = 3;
}

static MENU_UPDATE_FUNC(iv_test_update)
{
    if (iv_cmd == 3 || (st.running && st.test))
    {
        char s[32];
        iv_run_status(s, sizeof(s), "in");
        MENU_SET_VALUE("%s", iv_cmd == 3 ? "starting" : s);
        return;
    }
    if (!st.test || st.stop == R6IV_STOP_NONE)
    {
        MENU_SET_VALUE("-");
        return;
    }
    if (st.stop == R6IV_STOP_DONE) MENU_SET_VALUE("ok, 1 picture");
    else
    {
        MENU_SET_VALUE("%s", r6_iv_stop_name(st.stop));
        iv_stop_warning(entry, info);
    }
    if (st.shots > iv_test_shots0)                          /* only a picture of this test */
    {
        const struct r6_iv_shot * e = &st.ring[(st.shots - 1) % R6IV_RING];
        MENU_SET_RINFO("hold %dms %d", (int) e->off_ms, (int) e->off_why);
    }
}

static const char * iv_mode_name(int m)
{
    switch (m)
    {
        case SHOOTMODE_P: return "P";
        case SHOOTMODE_TV: return "Tv";
        case SHOOTMODE_AV: return "Av";
        case SHOOTMODE_M: return "M";
        case SHOOTMODE_BULB: return "B";
    }
    return "?";
}

static MENU_UPDATE_FUNC(iv_cam_update)
{
    int seen = 0, len = 0, ev = 0, tv = 0;
    int tvp = r6_iv_tv_prop(&seen, &len);
    if (r6_iv_tv_ev768(&ev) && ev >= -0xF00 && ev <= R6IV_TV_EV768_MAX) tv = iv_ev768_to_raw(ev);   /* 30 s .. 1/16000 */
    MENU_SET_VALUE("%s, %s, %s, %s", iv_mode_name(shooting_mode),
        drive_mode == DRIVE_SINGLE ? "single" : "drive?",
        (af_mode & 0xF) == AF_MODE_MANUAL_FOCUS ? "MF" : "AF",
        (tv >= 15 && tv <= 168) ? lens_format_shutter(tv) : "Tv?");
    struct r6_iv_cc s;
    r6_iv_cc_read(&s);
    if (s.valid & R6IV_V_P) MENU_SET_RINFO("CC: %d,%d,%d", (int) s.p_ae, (int) s.p_drive, (int) s.af);
    else MENU_SET_RINFO("CC: -");
    int c = r6_iv_cam_check();
    if (c == R6IV_CAM_OK)
    {
        int k = iv_cc_settings(&s);
        if (k != R6IV_CAM_OK)
            MENU_SET_WARNING(MENU_WARN_INFO, "%s (Tv 0x%x): logged only in this build.", r6_iv_cam_name(k), (int) s.p_tv);
        return;
    }
    switch (c)
    {
        case R6IV_CAM_MODE:
            MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Mode %d: set M.", shooting_mode); break;
        case R6IV_CAM_TV:
            MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Tv %d (0x%x, %s, len %d): set 1 s or shorter, turn the main dial.",
                tvp, tvp & 0xFFFF, seen ? "seen" : "not seen", len); break;
        case R6IV_CAM_AEB:
            MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "AEB 0x%x: switch exposure bracketing off.", (int) iv_aeb); break;
        case R6IV_CAM_DRIVE:
            MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Drive 0x%x: set single shot (0 expected).", drive_mode); break;
        case R6IV_CAM_FOCUS:
            MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Focus 0x%x: set MF (low nibble 3 expected).", af_mode & 0xFFFF); break;
        case R6IV_CAM_CARD:
            MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Shots left %d: card missing or full.", avail_shot); break;
        default:
            MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "%s.", r6_iv_cam_name(c)); break;
    }
}

static MENU_UPDATE_FUNC(iv_canon_update)
{
    int hard = 0;
    struct r6_iv_cc s, s2;
    int k = iv_canon_check_snap(&hard, 0, &s, &s2);
    MENU_SET_VALUE("%s", r6_iv_canon_name(k));
    if (s.valid & R6IV_V_CC) MENU_SET_RINFO("cc %d jobs %d", (int) s.state, (int) s.jobs);
    else MENU_SET_RINFO("cc - jobs %d", (int) s.jobs);
    MENU_SET_HELP("sw %d%d pend 0x%x relseq %d/%d sm %d rel %d", s.df, s.e0,
        (int) ((s.pend | s.work) & R6IV_SW_EVENT_MASK), (int) s.relseq, (int) s.b54, (int) s.shmode,
        (s.valid & R6IV_V_PO) ? (int) s.relcnt : -1);
    if (hard) MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "No release possible: %s.", r6_iv_canon_name(k));
}

static MENU_UPDATE_FUNC(iv_last_update)
{
    if (st.stop == R6IV_STOP_NONE && !st.running)
    {
        MENU_SET_VALUE("-");
        return;
    }
    MENU_SET_VALUE("%d/%d, %s", st.n_done, st.n_target, st.running ? "running" : r6_iv_stop_name(st.stop));
    if (st.shots)
    {
        const struct r6_iv_shot * e = &st.ring[(st.shots - 1) % R6IV_RING];
        MENU_SET_RINFO("%dms r%d", (int) e->dur, (int) e->ret);
    }
    if (st.running) return;
    iv_stop_warning(entry, info);
}

static MENU_UPDATE_FUNC(iv_apo_update)
{
    MENU_SET_VALUE("%d", auto_power_off_time);
}

static struct menu_entry iv_menu[] = {
    {
        .name   = "R6 Interval",
        .select = menu_open_submenu,
        .help   = "M8.1: pictures at an interval (shutter callbacks, no settings written).",
        .help2  = "Limits: M, Tv 1 s or shorter, single shot, MF, at most 50 pictures.",
        .children = (struct menu_entry[]) {
            {
                .name   = "Start",
                .select = iv_start_stop,
                .update = iv_start_update,
                .help   = "Start / stop a series. Canon MENU, playback, Q or screen off stop it.",
                .help2  = "First picture after the start delay, at least 3 s after this menu closed.",
            },
            {
                .name   = "Test: 1 picture",
                .select = iv_test_select,
                .update = iv_test_update,
                .icon_type = IT_ACTION,
                .help   = "One picture through the shutter callbacks, same checks as Start.",
                .help2  = "Close this menu; the picture comes 3 s later. Then Debug > LV probe > Log snapshot.",
            },
            {
                .name   = "Interval",
                .priv   = &r6_iv_interval_idx,
                .max    = COUNT(iv_intervals) - 1,
                .choices = iv_interval_names,
                .help   = "Time from one picture to the next. A late slot is skipped.",
                .help2  = "Canon's Screen off must be longer than this (screen off ends a series).",
            },
            {
                .name   = "Pictures",
                .priv   = &r6_iv_count,
                .min    = 1,
                .max    = R6IV_MAX_PICS,
                .help   = "Pictures per series (1..50).",
            },
            {
                .name   = "Start delay",
                .priv   = &r6_iv_delay_idx,
                .max    = COUNT(iv_delays) - 1,
                .choices = iv_delay_names,
                .help   = "Time from Start to the first picture.",
            },
            {
                .name   = "Camera",
                .update = iv_cam_update,
                .icon_type = IT_ALWAYS_ON,
                .help   = "How ML reads mode, drive, focus and Tv. Start only if all fit (M only).",
                .help2  = "Right: Canon CC copies (AE mode, drive, AF), logged only in this build.",
            },
            {
                .name   = "Canon",
                .update = iv_canon_update,
                .icon_type = IT_ALWAYS_ON,
                .help   = "Canon state before a picture (read only).",
            },
            {
                .name   = "Last series",
                .update = iv_last_update,
                .icon_type = IT_ALWAYS_ON,
                .help   = "Pictures / target and why it ended; right: SW1 ON to the OFFs, return.",
            },
            {
                .name   = "Auto power off",
                .update = iv_apo_update,
                .icon_type = IT_ALWAYS_ON,
                .help   = "Canon's value (raw). ML cannot keep the camera on during a series:",
                .help2  = "set Canon's auto power off to Disable for long series.",
            },
            MENU_EOL
        },
    },
};

static void iv_init()
{
    menu_add("Shoot", iv_menu, COUNT(iv_menu));
}

INIT_FUNC("r6_interval", iv_init);

#endif /* CONFIG_R6_INTERVAL */
