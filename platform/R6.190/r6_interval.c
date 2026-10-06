/*
 * R6.190 M8.0 / M8.1 / M8.2 / M9.2 / M9.3: interval release, ML menu Shoot > "R6 Interval".
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
 * before it is dereferenced (iv_ram_ok; the CC settings block p: r6_fy_ram_ok,
 * M8.2). Analysis (ROM, task A/B/C plus a
 * skeptic): spec M8.1 (scratchpad m8/cbr). Not upstream's intervalometer
 * (FEATURE_INTERVALOMETER): that one also calls SetGUIRequestMode(0), blinks
 * the card LED (MMIO), autostarts at boot from the saved setting, writes
 * script files and ignores the outcome of the release.
 *
 * M8.2 (spec m8/m83 spec_longtv.md): Tv up to 30 s in M, no Bulb. The waits
 * after the OFFs grow with the exposure: busy = 2 x the Tv bound for Tv >= 1 s
 * (iv_tv_ub_ms, 1 s .. 32 s in 1/6 stops, rounded up; x 2 for a long exposure
 * NR dark frame, whose encoding and position are [I]), 0 below 1 s, added to
 * R6IV_SEQ_MS / R6IV_PIC_MS / R6IV_JOB_MS and fixed per picture at the pre-SW2
 * check from the longer of ML's PROP_SHUTTER and the CC copy. For Tv >= 1 s
 * the OFFs go out only at state 5 with [cc+0xB54] == 1 (Canon defers both OFFs
 * there [V]), 6, 12 or RELEASE_END (iv_committed_long), never on a job / job ID
 * / counter step, and at once if [p+0x92C] (PROP_MECHALESS) != 0 (then SW2 OFF
 * in state 5 is RELEASE_STOP). The CC copies p = [cc+0x34FC] lie in the uncached
 * alias on the camera (0x418E3C64 in LVP00041..45); iv_ram_ok() refused them, so
 * before M8.2 they were never read. Now r6_fy_ram_ok() reads them (the lvprobe
 * rule: cached RAM, or 0x40001000..0x7FFFFFFF once Canon's memif copy verifies,
 * read-only) and they gate Start, Test and every tick (R6IV_GATE_CC_SETTINGS 1):
 * M, single, Tv in range, no AF, PROP_MECHALESS 0, Canon's interval timer and
 * timer setting 0x80040064 bytes 0; [cc+0xB98] / [cc+0x830] make a picture wait. Start
 * is refused if the interval is shorter than Tv + 2 s. src/menu.c does not open
 * the ML menu while a picture runs (r6_iv_menu_block). An ML shutdown is taken
 * in every wait after the OFFs (nothing is held there).
 *
 * M9.2 (spec m9/m92 spec_m92.md with the corrections of spec_m92_skeptic.md;
 * approved 2026-10-04: mode B, design D1): B with Canon's own bulb timer (PROP
 * 0x8004005B, CC copy [p+0x648] = {enable, h, m, s}) set to R6IV_BULB_MIN_S ..
 * R6IV_BULB_MAX_S (31 s .. 30 min since M9.3). Canon's
 * timer measures the exposure. ML presses as in M (SW1 ON, 100 ms, SW2 ON) and
 * lets go (SW2 OFF, then SW1 OFF, same tick) in the first poll that shows state
 * 12 with [cc+0xD30] == 1: SHOOT_START (0xE031D7B4, CC task dispatch) stores the
 * state (0xE031D85C), D30 (0xE031D864) and BA0 (= 1 with the timer enable byte
 * set, 0xE031D87C) with only a RAM store in between, and the callbacks only
 * post, so ML's OFFs are judged with the final BA0: consumed without effect
 * while the timer is armed (SW1 OFF 0xE05E30BE, SW2 OFF 0xE05E34CC). Timer A ends
 * the bulb at T (callback 0xE070E070 -> 0xE05E50E0 -> end 0xE02EB9BE) unless the
 * half press is held at that moment (byte [cc+0x16B]), which ML never does. From
 * the OFFs on ML holds nothing and calls nothing. Phase W only reads (D30, BA0,
 * [cc+0x99C], timer RAM 0x1E568 / 0x1E5BC..0x1E5C8) until Canon's bulb ends (D30
 * 0 or state != 12): classification within R6IV_BULB_CLASS_MS (timer seen or
 * not: BULB_NOTIMER, a very short frame), a check at 3 s (timer A / B armed with
 * T, seconds counting: advice only; no finger and nothing that can end the bulb,
 * i.e. timer A / B 0 and BA0 0 or seconds 0 (EXPOSURE_START, which arms A / B and
 * starts the seconds, never ran): OVERRUN at once, skeptic D1, review F1), an
 * early end (< T - 1.5 s - T / 256: BULB_EARLY), no end at T + 5 s + T / 64 (M9.3:
 * the margins grow with T, ML's clock is not Canon's, m93 skeptic D2)
 * (BULB_OVERRUN at once; the ML menu stays closed with a hint until Canon's bulb
 * ends; no automatic second press: it could race Canon's own end and start a
 * second full bulb, K11), ML shutdown (stop at once). Elapsed time in phase W
 * skips clock steps (skeptic C3), with a poll-count cap as the hard bound.
 * Budgets after the OFFs: busy = 2 T + 5 s + T / 16 (NR dark frame ~ T; the M8.2
 * camera margin of 2.6 s was too small). B gates: AE 4 (CC copy), PROP_SHUTTER
 * and CC Tv 0xDF00, [cc+0x2C0] == 0 (state 12, 0xE0716EDE), [cc+0x5E8] == 0
 * (skeptic D2), timer enable 1 with 31 s .. 30 min, Canon's bulb idle (D30, BA0, timer
 * A / B, seconds 0; refused in M as well: with D30 != 0 Canon's SW1 OFF in 2 / 3
 * / 6 runs the bulb-end copy, skeptic C1), and all M8.2 checks. No property
 * write (also not 0x8004005B / 0x8002000E/F), no IntvlSw1CBR / IntvlSw2CBR, no
 * new Canon call, no MMIO. ML's overlays pause during ML's bulb (r6_gdraw.c,
 * r6_overlays.c); the user's own bulb does not pause them. A series keeps the
 * mode it started in (iv_mode0, review F2): a change between M and B (or to any
 * other mode) stops it in the tick or in phase B with SW1 released (STOP_CAMERA,
 * CAM_MODE, stop_val = the new mode), and
 * phase B picks the M / B path by that mode, not by the live one.
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
 *     Tv >= 1 s (M8.2): only state 5 with b54, 6, 12 and RELEASE_END count;
 *     PROP_MECHALESS lets go at once (off_why 13, stop CCSTATE bit 14).
 *     B (M9.2): state 12 only together with [cc+0xD30] == 1 (iv_committed_bulb).
 *     The real button changing ML's bytes, or a shutdown, drops both at once.
 *  D  after a timeout OFF: R6IV_LATEPIC_MS late watch (a deferred ON can
 *     still be accepted by a dispatch round that was already running)
 *  W  M9.2, after a bulb commit: the read-only watch until Canon's bulb ends
 *  E  Canon idle again (state 1 / 3, no sequence, bytes 0, no SW events)
 *     within R6IV_SEQ_MS + busy
 *  F  the picture (counter / file number) within R6IV_PIC_MS + busy, Canon's
 *     job count back to 0 within R6IV_JOB_MS + busy; E, F and the settle end
 *     at once on an ML shutdown (stop SHUTDOWN, val 1)
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
 * the [I] CC pointer copies, [cc+0x730], [cc+0x3B4], [cc+0x5F8], the aux word,
 * ACCESS_REMAIN, the NR value [p+0x54C]. Since M8.2 the CC copies of AE mode,
 * drive, Tv, PROP_MECHALESS (ids [V], CC master table 0xE0FE3344), the AF flag
 * and the interval timer / timer setting (0x80040064) bytes gate (R6IV_GATE_CC_SETTINGS
 * 1). Fixed limits: M with Tv <= 30 s, or B with Canon's bulb timer 31 s .. 30 min
 * (M9.2 / M9.3), single shot, MF, LV running, no Canon menu, interval >= Tv (bulb) + 2 s,
 * at most 50 pictures.
 *
 * M9.3 (spec m9/m93 spec_m93.md, skeptic corrections): R6IV_BULB_MAX_S 1800 (30
 * min, approved 2026-10-04 after the clean M9.2 camera log LVP00058..63); the
 * phase W margins grow with T (OVER 5 s + T / 64, EARLY 1.5 s + T / 256); after
 * the bulb end the status shows "Canon busy" (NR / saving). R6 Motion
 * (r6_motion.c) runs through this task: one series at a time (st.mode; every
 * Start of the other kind is refused with CAM_BUSY, each menu stops only its own
 * run), command 4 starts it, iv_tick_md runs the stops (shutdown, Canon GUI /
 * power saving, the maximum duration; Release: a picture ML did not take, every
 * tick, the mode, the camera check, Canon's hard reasons) and the detector
 * (r6_md_step, 20 ms ticks). A trigger takes exactly one picture through
 * iv_shoot with all M8.x checks, or is dropped if Canon is not ready (never
 * caught up). Motion never uses B. "Display only" never presses.
 *
 * M10.2 (spec m10/m102 spec_m102.md section 4, Tor 1 approved 2026-10-05, effective 2026-10-06): Mode "Bracket"
 * (only M): every event takes N = 3 / 5 / 7 / 9 pictures whose Tv steps around the user's Tv (the series base) by
 * 1/3 .. 3 EV, order "0 - +" or "- 0 +", and puts the base back after the event. The Tv writes go through
 * r6_propw.c (the one _prop_request_change call site, all its locks, the confirmation window, the restore rules,
 * the marker file); this file only decides when: an event starts at a due slot with Canon ready and Tv stably the
 * base; before each write the wait rows are waited for (no call), after it 200 ms settle, then the member pre-check
 * (Canon ready, Tv stably the member) and the unchanged M8.x picture (iv_shoot; phase B also checks Tv == member,
 * re-read once). Any stop inside an event ends in r6_propw.c's restore decision (now / pending / none); the ML menu
 * stays closed during an event. Between events the hard rows of the write path and the Tv (stably the base, or the
 * user's change with evidence: stop, no restore) are checked every tick. A Tv request that may still be queued in
 * Canon ("open") blocks every ML press in every mode (iv_press, r6_iv_cam_check). Decision D2: Canon's HDR mode,
 * multiple exposure, WB / focus bracketing and multi-shot NR refuse every R6 Interval mode and R6 Motion Release.
 * "Tv back to ..." (command 5) runs r6_propw.c's offer after a cut-off bracket. Start, Test, "Tv writes" and "Tv
 * back to ..." act only when ML's menu key handling calls them (GuiMainTask, src/gui.c ml_gui_main_task).
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
#include "r6_faceyuv.h"                  /* r6_fy_ram_ok(): the CC settings block p (M8.2) */
#ifdef CONFIG_R6_MOTION
#include "r6_motion.h"                   /* M9.3: R6 Motion's detector runs in r6_iv_task */
#endif
#include "r6_propw.h"                    /* M10.2: the Tv write path (Tor 1) */

#ifdef CONFIG_R6_INTERVAL

#ifdef FEATURE_INTERVALOMETER
#error "R6 M8.0: upstream's intervalometer is not approved (SetGUIRequestMode(0), LED MMIO, boot autostart, script files)"
#endif
#ifdef CONFIG_PROP_REQUEST_CHANGE
#error "R6 M8.0: property writes stay off"
#endif
#ifndef CONFIG_R6_PROPW_TV
#error "R6 M10.2: R6 Interval's Bracket mode and the press block need the Tv write path (internals.h CONFIG_R6_PROPW_TV)"
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
#define R6IV_LATEPIC_MS     3000   /* after a timeout OFF: watch for a late release; start signals precede the
                                    * exposure: not Tv-dependent [V 1/125 and 1 s, I beyond] */
#define R6IV_SEQ_MS         3000   /* after the OFFs: Canon's release sequence idle again (+ busy) */
#define R6IV_IDLE_MS        1000   /* after an abort before SW2: wait for state 1 / 3 (log only) */
#define R6IV_PIC_MS        15000   /* new picture via property, counted from SW1 ON (+ busy) */
#define R6IV_MENU_WANT_MS   5000   /* after a Trash press blocked during a picture: hold the next one (review fix) */
#define R6IV_JOB_MS        30000   /* Canon's job count [0x9DC0] == 0, counted from SW1 ON (+ busy) */
#define R6IV_SETTLE_MS       300   /* after the job: late counter updates (extra pictures) */
#define R6IV_MAX_PICS         50   /* per series, until a long test showed no leak */
#define R6IV_TEST_MS        3000   /* "Test: 1 picture": delay after the command */
#define R6IV_GATE_CC_SETTINGS  1   /* CC copies are [V] (CC master table) and readable since M8.2 (r6_fy_ram_ok) */
#define R6IV_PROC_MS        2000   /* M8.2: Start refused if interval < Tv + this */
#define R6IV_PROC_NR_MS     1000   /* M8.2: Interval warning if interval < 2 Tv + this and NR may be on */
#define R6IV_BKT_MEMBER_MS  1000   /* M10.2: per bracket member: job free <= 0.71 s + 0.3 s settle (K31) */
#define R6IV_BKT_WRITE_MS    700   /* M10.2: per Tv write: 500 confirmation + 200 settle */
#define R6IV_BACK_MAX_MS   60000   /* M10.2: "Tv back to ..." waits this long for the ML menu to close, then drops */
#define R6IV_MENU_TASK "GuiMainTask"   /* M10.2: ML's menu key handling runs here (src/gui.c ml_gui_main_task, the
                                        * TASK_OVERRIDE of Canon's gui_main_task, stubs.S task_create("GuiMainTask")) */
/* M9.2: bulb with Canon's bulb timer. The menu texts name the limits as Canon shows them ("0:00:31 .. 0:30:00"):
 * change them together with R6IV_BULB_MIN_S / R6IV_BULB_MAX_S (no division by 60 / 3600 in this file) */
#define R6IV_BULB_MIN_S       31   /* M9.2: Canon bulb timer at least this (shorter: M, Tv up to 30 s) */
#define R6IV_BULB_MAX_S     1800   /* M9.3: 30 min, approved 2026-10-04 after the clean M9.2 camera log LVP00058..63 */
#define R6IV_BULB_CLASS_MS   300   /* after the OFFs: timer bulb (BA0 / timer A seen) or not */
#define R6IV_BULB_POLL_MS    100   /* phase W poll after the classification window */
#define R6IV_BULB_CHECK_MS  3000   /* after the OFFs: timer A / B armed with T * 1000, seconds counting; else a warning */
#define R6IV_BULB_EARLY_MS  1500   /* bulb end before T - this - T / 256 (M9.3): ended early */
#define R6IV_BULB_OVER_MS   5000   /* still running at T + this + T / 64 (M9.3): stop at once, no call */
#define R6IV_BULB_BUSY_MS   5000   /* budget: busy = 2 T + this + T / 16 (NR dark frame ~ T, M8.2 camera: 2.6 s margin was too small) */
#define R6IV_BULB_MS_TOL     300   /* after a timer end [0x1E568] expected T * 1000 .. + this */
#define R6IV_BULB_GLITCH_MS 2000   /* phase W: a clock step back, or forward by more than this in one poll, is skipped (skeptic C3) */
#if R6IV_BULB_MAX_S > 0xFFFF / 2 || R6IV_BULB_POLL_MS != 100
#error "M9.2: bt / sec_max are 16 bit, the phase W poll cap assumes 100 ms polls"
#endif
/* R6 PROP_SHUTTER = Canon's PROP_SET_TV (camera test 2026-10-03, LVP00037,
 * and the ROM): exactly 2 bytes (CC master size table 0xE0FE3E0C; writer
 * ManualSafetyShift 0xE0946A5A, readers ldrh+sxth 0xE066FDCE / 0xE0750948),
 * a signed 16-bit Tv in EV * 768 (Canon "Code768"), 256 per 1/3 stop, 384
 * per 1/2 stop: 1 s = 0x0000, 1/80 = 0x1300, 1/100 = 0x1400, 1/125 = 0x1500
 * (7 EV), 4 s = 0xFA00 (-2 EV), 30 s = 0xF100, Bulb 0xDF00, 0xDC00 (flag).
 * The bytes after the 2 delivered ones are undefined. Not ML's raw 56 + 8 *
 * EV (upstream lens.c keeps the low byte, 0 on the R6). Accepted (M8.2): a
 * multiple of 128 in -3840 (30 s) .. 10752 (14 EV = 1/16000 s); anything
 * else (also a delivery that is not 2 bytes long) counts as unknown. */
#define R6IV_TV_EV768_MIN  (-3840)  /* 30 s = 0xF100 (int16); Bulb 0xDF00 (-8448) / 0xDC00 (-9216) lie below */
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
    IV_WHY_S12,              /* 3: state 12 (bulb; M9.2: the commit of a B picture with D30 1; in M not expected, stops the series) */
    IV_WHY_RELEND,           /* 4: release sequence seen 1, now 0 (RELEASE_END) */
    IV_WHY_RELCNT,           /* 5: RAM release counter stepped (only if it matched PROP_RELEASE_COUNTER) */
    IV_WHY_JOB,              /* 6: [0x9DC0] != 0 (PoCreateJob in the scenario) */
    IV_WHY_JID,              /* 7: [0x9DB8] stepped (CreateJob) */
    IV_WHY_TIMEOUT,          /* 8: R6IV_SW2_MS, nothing accepted */
    IV_WHY_STUCK4,           /* 9: R6IV_SW2_MS, accepted but not committed (state 4) */
    IV_WHY_BUTTON,           /* 10: the real button changed DF / E0 */
    IV_WHY_SHUTDOWN,         /* 11: ML shutdown */
    IV_WHY_RET,              /* 12: a switch call returned != 0 or was refused */
    IV_WHY_MECHA = 13,       /* 13: M8.2, Tv >= 1 s: [p+0x92C] != 0 or p not readable (stop CCSTATE, bit 14) */
};
#define IV_X_ABORT  100      /* trace X before SW2 ON: 100 + enum r6_iv_stop */

/* M9.2: bulb class (r6_iv_shot.bulb), why phase W ended (bend, trace Z), warning bits (bwarn, trace W) */
enum { IV_BCLASS_NONE = 0, IV_BCLASS_TIMER, IV_BCLASS_NOTIMER };
enum { IV_BEND_NONE = 0, IV_BEND_TIMER, IV_BEND_EARLY, IV_BEND_OVERRUN, IV_BEND_SHUTDOWN, IV_BEND_NOTIMER };
#define IV_BW_TA    0x01     /* timer A not armed at R6IV_BULB_CHECK_MS */
#define IV_BW_TAMS  0x02     /* timer A ms != T * 1000 */
#define IV_BW_TB    0x04     /* timer B not armed */
#define IV_BW_SEC   0x08     /* [cc+0x99C] still 0 */
#define IV_BW_D30   0x10     /* state left 12 with [cc+0xD30] still 1 (K9: the next Start is refused with BULB_BUSY) */
#define IV_BW_MS    0x20     /* [0x1E568] not T * 1000 .. + R6IV_BULB_MS_TOL after a timer end */
#define IV_BW_WORD  0x40     /* [p+0x648] changed during the picture (K10) */
#define IV_BW_NOEND 0x80     /* skeptic D1 / review F1: at the 3 s check no finger, timer A / B 0 and BA0 0 or seconds 0
                              * (EXPOSURE_START never ran): nothing can end it, OVERRUN at once */

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

/* M9.3 review F6: 1h30m / 2h appended at the end (saved indices stay valid): a 30-min bulb with NR (~62 min) fits 1h30m */
static const int iv_intervals[] = { 3, 5, 10, 15, 20, 30, 45, 60, 90, 120, 180, 300, 600, 900, 1800, 3600, 5400, 7200 };
static const char * iv_interval_names[] = { "3s", "5s", "10s", "15s", "20s", "30s", "45s", "1m", "1m30s",
                                            "2m", "3m", "5m", "10m", "15m", "30m", "1h", "1h30m", "2h" };
typedef char iv_interval_names_complete[COUNT(iv_interval_names) == COUNT(iv_intervals) ? 1 : -1];
static const int iv_delays[] = { 3, 5, 10, 30, 60 };
static const char * iv_delay_names[] = { "3s", "5s", "10s", "30s", "1m" };

/* saved: only the settings; a running series is never saved and never resumed */
CONFIG_INT("r6.iv.interval", r6_iv_interval_idx, 2);    /* 10 s */
CONFIG_INT("r6.iv.count",    r6_iv_count, 10);
CONFIG_INT("r6.iv.delay",    r6_iv_delay_idx, 0);       /* 3 s */
CONFIG_INT("r6.iv.mode",     r6_iv_mode, 0);            /* M10.2: 0 Single, 1 Bracket */
CONFIG_INT("r6.iv.bkt.n",    r6_iv_bkt_n, 0);           /* M10.2: 3 / 5 / 7 / 9 shots (index 0 = 3) */
CONFIG_INT("r6.iv.bkt.step", r6_iv_bkt_step, 3);        /* M10.2: 1..9 thirds (3 = 1 EV) */
CONFIG_INT("r6.iv.bkt.order", r6_iv_bkt_order, 0);      /* M10.2: 0 "0 - +", 1 "- 0 +" */

static struct r6_iv_stats st;
static struct r6_iv_last L;              /* last picture: snapshots and trace (r6_iv_task writes) */
static volatile int iv_cmd = 0;          /* menu -> task: 1 = start, 2 = stop, 3 = test (last press wins) */
static int iv_refused_cam = R6IV_CAM_OK; /* why the last Start / Test was refused (menu) */
static volatile int iv_t_menu = -100000; /* last time the task saw the ML menu open */
static volatile int iv_t_menu_wanted = -100000; /* last Trash press blocked during a picture (menu task); review F3: and
                                                 * the end of the picture it was blocked in (iv_pic_done) */
static volatile int iv_menu_wanted_pic = 0;     /* review F3: a Trash press was blocked by the running picture (menu task
                                                 * sets, iv_pic_done takes it: the R6IV_MENU_WANT_MS hold runs from the end) */
static int iv_mode0 = -1;                /* review F2: shooting_mode when the series / test started (M or B) */
static int iv_t_lv = -1;                 /* Canon LV running since (ms), -1 = not running */
static int iv_gui_seen = 0;              /* foreign Canon GUI mode seen during the series (latched), 0 = none */
static int iv_due = 0;                   /* ms of the next picture slot */
static uint32_t iv_wait_ms = 0;          /* camera not ready (ML menu excluded) since the last picture */
static uint32_t iv_wait_mask = 0;
static volatile int iv_wait_menu = 0;    /* the task waits for the ML menu (status line) */
static volatile int iv_in_pic = 0;       /* a picture is running (status "pressing", passive trace off) */
static volatile int iv_long = 0;         /* M8.2: the running picture has Tv >= 1 s (set in phase B, cleared in iv_pic_done) */
static volatile int iv_bulb = 0;         /* M9.2: the running picture is a bulb (set in phase B, cleared in iv_pic_done) */
static volatile int iv_bulb_s = 0;       /* M9.2: its T (s), for the status line */
static volatile int iv_bulb_ovr = 0;     /* M9.2: stopped with BULB_OVERRUN and Canon's bulb still runs (menu guard, overlays) */
static volatile int iv_bulb_end_t = 0;   /* M9.3: ms when phase W ended (Canon's NR / saving follows), 0 = not yet */
static volatile int iv_md_shoot_req = 0; /* M9.3: command 4 (motion Start): 1 = Release, 0 = Display only */
static volatile int iv_md_refused = R6IV_CAM_OK;   /* M9.3: why the last motion Start was refused */
static struct r6_iv_bkt bk;              /* M10.2: Bracket settings and event state of the current / last run */
static int iv_refused_pw = 0;            /* M10.2: enum r6pw_lock of the last R6IV_CAM_TVW refusal */
static int iv_refused_bk = 0;            /* M10.2: BKT_LIST: why << 8 | member index; iv_refused_tv: its Tv */
static int iv_refused_tv = 0;
static volatile int iv_menu_wanted_ev = 0;   /* M10.2: a Trash press was blocked by a bracket event (hold from its end) */
static int iv_back_t = 0;                /* M10.2: command 5 taken at (ms | 1), 0 = none */
static const char * iv_back_why = 0;     /* M10.2: why "Tv back to ..." was refused / dropped */
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

/* trace: S R B D E P J C I M G T F L Q N (value changes; M8.2: L LV running, Q gui_state, N [p+0x92C]; M9.2: K [cc+0xD30],
 * A [cc+0xBA0], H [0x1E5BC], O [cc+0x99C] != 0, Y [0x1E568]), 1..4 the switch calls, X off_why / abort; M9.2 markers:
 * W the 3 s check (IV_BW_*), Z the end of phase W (IV_BEND_*) */
#define IV_NTR 21
static const char iv_tr_codes[IV_NTR + 1] = "SRBDEPJCIMGTFLQNKAHOY";
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

static const char * const iv_stop_names[] = {
    "-", "done", "stopped (menu)", "ML shutdown", "camera setting", "Canon state", "Canon GUI",
    "display off (power saving)", "camera not ready", "switch call returned / refused", "no new picture", "extra picture",
    "Canon job open", "half press not taken", "aborted before full press", "full press not taken",
    "release did not start", "late picture", "shutter button used", "Canon sequence not idle",
    "Canon state after picture", "picture outside ML",
    "bulb without Canon timer", "bulb ended early", "bulb did not end", "no bulb started",            /* M9.2 */
    "time limit",                                                                                       /* M9.3 */
    "Tv write stopped", "Tv changed by user",                                                           /* M10.2 */
};
static const char * const iv_cam_names[] = {
    "ok", "movie mode", "not M or B", "Tv > 30s, Bulb or unknown", "not single shot", "not MF", "no card / full", "AEB on",
    "Canon CC: not M", "Canon CC: not single", "Canon CC: Tv Bulb / out of range", "Canon CC: AF active",
    "Canon CC: mechaless mode", "Canon interval timer on", "Canon timer setting (0x80040064) on", "interval < exposure + 2s",
    "B: Tv not Bulb", "Canon CC: not B", "Canon CC: Tv not Bulb", "Canon bulb timer off",                     /* M9.2 */
    "Canon bulb timer not 0:00:31..0:30:00", "Canon bulb not idle", "Canon CC: 0x5E8 set",
    "other series running", "B: not for R6 Motion (M, Tv up to 30 s)", "no memory for the detector",   /* M9.3 */
    "WB bracketing on", "Canon HDR mode on", "multiple exposure on", "focus bracketing on", "multi-shot NR on",   /* M10.2 */
    "bracket: M only", "Tv writes", "bracket Tv list", "bracket: over 50 pictures",
    "Tv write open: no ML pictures until restart", "not from the ML menu",
};
/* every enum value has its name (a missing one would be a NULL %s) */
typedef char iv_stop_names_complete[COUNT(iv_stop_names) == R6IV_NSTOPS ? 1 : -1];
typedef char iv_cam_names_complete[COUNT(iv_cam_names) == R6IV_NCAMS ? 1 : -1];
static const char * const iv_canon_names[R6IV_NCANON] = {
    "ready", "CameraConductor not ready", "shutter callbacks not registered", "Canon switch path diverted",
    "CC event flag invalid", "factory release mark, reboot", "Canon remote sequence",
    "boot < 20 s", "LV not running", "review / GUI state", "Canon job running", "buffer full",
    "UI locked", "sensor cleaning", "ML menu", "shutter held / pending", "release sequence active",
    "CC not in LV still", "Canon CC timer flag (0xB98 / 0x830)",
};

const struct r6_iv_stats * r6_iv_stats(void) { return &st; }
const struct r6_iv_last * r6_iv_last(void) { return &L; }
int r6_iv_running(void) { return st.running || iv_cmd == 1 || iv_cmd == 3 || iv_cmd == 4; }   /* M9.3: 4 = motion */
const char * r6_iv_stop_name(int s) { return (s >= 0 && s < R6IV_NSTOPS) ? iv_stop_names[s] : "?"; }
const char * r6_iv_cam_name(int c) { return (c >= 0 && c < R6IV_NCAMS) ? iv_cam_names[c] : "?"; }
const char * r6_iv_canon_name(int c) { return (c >= 0 && c < R6IV_NCANON) ? iv_canon_names[c] : "?"; }
int r6_iv_gate_cc_settings(void) { return R6IV_GATE_CC_SETTINGS; }
int r6_iv_tv_min(void) { return R6IV_TV_EV768_MIN; }
int r6_iv_bulb_min_s(void) { return R6IV_BULB_MIN_S; }                                     /* M9.2 */
int r6_iv_bulb_max_s(void) { return R6IV_BULB_MAX_S; }
int r6_iv_bulb_running(void) { return (iv_in_pic && iv_bulb) || iv_bulb_ovr; }
int r6_iv_in_pic(void) { return iv_in_pic; }                                                /* M9.3 */
static int iv_md_run(void) { return st.running && st.mode != R6IV_MODE_INTERVAL; }
int r6_iv_md_running(void) { return iv_md_run() || iv_cmd == 4; }
int r6_iv_md_refused(void) { return iv_md_refused; }
#ifdef CONFIG_R6_MOTION
int r6_iv_tick_ms(void) { return iv_md_run() ? R6MD_TICK_MS : 50; }
#else
int r6_iv_tick_ms(void) { return 50; }
#endif

/* M9.2: T (s) of Canon's bulb timer from the CC copy [p+0x648] = {enable, h, m, s} (bytes, no division); -1 = p not
 * readable, enable != 1 or m / s > 59 */
int r6_iv_bulb_t(const struct r6_iv_cc * s)
{
    if (!(s->valid & R6IV_V_P) || (s->bulbw & 0xFF) != 1) return -1;
    int h = (s->bulbw >> 8) & 0xFF, m = (s->bulbw >> 16) & 0xFF, sec = s->bulbw >> 24;
    if (m > 59 || sec > 59) return -1;
    return h * 3600 + m * 60 + sec;
}
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

/* M8.2: ms for Tv = 1 s .. 30 s in 1/6-stop steps (index = -ev / 128): 1000 * 2^s * (1 + j / 6), rounded up.
 * It is >= the true 2^(k/6) s and >= Canon's nominal labels (1.3 1.6 2.5 3.2 5 6 10 13 15 20 25 30; half stops
 * 1.5 3 6 10 20) [V arithmetic]. A table, no division: canon_gate sees no reciprocal constant */
static const uint16_t iv_tv_ub_tab[31] = {
    1000, 1167, 1334, 1500, 1667, 1834, 2000, 2334, 2667, 3000, 3334, 3667, 4000, 4667, 5334, 6000,
    6667, 7334, 8000, 9334, 10667, 12000, 13334, 14667, 16000, 18667, 21334, 24000, 26667, 29334, 32000 };
static int iv_tv_ub_ms(int ev)       /* ev: EV*768, multiple of 128, >= R6IV_TV_EV768_MIN */
{
    if (ev >= 0) return 1000;
    if (ev < R6IV_TV_EV768_MIN) return 32000;
    return iv_tv_ub_tab[(-ev) >> 7];
}
static int iv_cc_tv_ok(int t) { return !(t % 128) && t >= R6IV_TV_EV768_MIN && t <= R6IV_TV_EV768_MAX; }

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
    s->lv = iv_t_lv >= 0;
    s->gstate = gui_state;
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
        s->b98 = IV_RD(cc + R6IV_CC_B98);
        s->c830 = IV_RD(cc + R6IV_CC_C830);
        s->r5f8 = IV_RD(cc + R6IV_CC_R5F8);
        s->d30 = IV_RD(cc + R6IV_CC_D30);                          /* M9.2 */
        s->ba0 = IV_RD(cc + R6IV_CC_BA0);
        s->sec = IV_RD(cc + R6IV_CC_SEC);
        uint32_t p = IV_RD(cc + R6IV_CC_P);
        s->p = p;
        /* M8.2: p lies in the uncached alias on the camera (0x418E3C64, LVP00041..45), which iv_ram_ok refuses:
         * the lvprobe rule (cached RAM, or the uncached alias once Canon's memif copy verifies), read-only */
        if (r6_fy_ram_ok(p, R6IV_P_SIZE))
        {
            s->valid |= R6IV_V_P;
            s->p_ae = IV_RD(p + R6IV_P_AE);
            s->p_drive = IV_RD(p + R6IV_P_DRIVE);
            s->p_tv = IV_RD(p + R6IV_P_TV_W) >> 16;                /* (u16) [p+0x61E] */
            s->p_92c = IV_RD(p + R6IV_P_92C);
            s->p_lenr = IV_RD(p + R6IV_P_LENR);
            s->p_itimer = (IV_RD(p + R6IV_P_ITIMER_W) >> 16) & 0xFF;  /* byte [p+0x50A] */
            s->p_fbrk = IV_RD(p + R6IV_P_FBRK_W) & 0xFF;             /* byte [p+0x634] */
            s->bulbw = IV_RD(p + R6IV_P_BULBT);                      /* M9.2: Canon's bulb timer {enable, h, m, s} */
            s->p_hdr = IV_RD(p + R6IV_P_HDR_W) & 0xFF;              /* M10.2 (D2, ESHUT log) */
            s->p_me = IV_RD(p + R6IV_P_ME_W) & 0xFF;
            uint32_t ba = IV_RD(p + R6IV_P_WBB_BA), gm = IV_RD(p + R6IV_P_WBB_GM);
            s->p_wbb = (ba & 0xFF) | (gm & 0xFF) << 8 | ((ba | gm) > 0xFF ? 0x10000 : 0);
            s->p_fbkt = IV_RD(p + R6IV_P_FBKT);
            s->p_msnr = IV_RD(p + R6IV_P_MSNR);
            s->p_shtype = IV_RD(p + R6IV_P_SHTYPE);
        }
    }
    /* M9.2: Canon's bulb timer RAM (fixed globals), after the cc block: Canon stores [0x1E568] before D30 = 0, so a
     * snapshot that shows D30 0 has the final bulb time (skeptic C4) */
    s->ta = IV_RD(R6IV_RAM_TA);
    s->ta_ms = IV_RD(R6IV_RAM_TA_MS);
    s->tb = IV_RD(R6IV_RAM_TB);
    s->tb_ms = IV_RD(R6IV_RAM_TB_MS);
    s->bulb_ms = IV_RD(R6IV_RAM_BULB_MS);
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
        && a->faneo[0] == b->faneo[0] && a->faneo[1] == b->faneo[1] && a->jobs == b->jobs
        && a->b98 == b->b98 && a->c830 == b->c830
        && a->d30 == b->d30 && a->ba0 == b->ba0 && a->ta == b->ta && a->tb == b->tb && a->bulbw == b->bulbw    /* M9.2 */
        && a->p_hdr == b->p_hdr && a->p_me == b->p_me && a->p_wbb == b->p_wbb && a->p_fbkt == b->p_fbkt   /* M10.2 */
        && a->p_msnr == b->p_msnr && a->p_shtype == b->p_shtype;
}

/* M9.2: Canon's bulb not idle (spec R2, skeptic C5): D30, BA0, timer A / B, seconds. Gated in B and, skeptic C1, in M:
 * with D30 != 0 Canon's SW1 OFF in 2 / 3 / 6 runs the bulb-end copy and sends RELEASE_STOP with a bulb time
 * (0xE05E2EBC / 0xE05E301A), where M sends RELEASE_STOP(0); RELEASE_CANCEL / ERROR leave D30 (and BA0) 1 */
static int iv_bulb_busy(const struct r6_iv_cc * s) { return s->d30 || s->ba0 || s->ta || s->tb || s->sec; }

/* M10.2 decision D2: Canon's multi-shot modes (rows 17..21 of the Tv write locks) refuse every R6 Interval mode and
 * R6 Motion Release; before M10.2 they ended a series only after the first picture (EXTRA / NOPIC / budget) */
static int iv_cc_multi(const struct r6_iv_cc * s)
{
    if (s->p_wbb) return R6IV_CAM_CC_WBB;
    if (s->p_hdr) return R6IV_CAM_CC_HDR;
    if (s->p_me) return R6IV_CAM_CC_ME;
    if (s->p_fbkt) return R6IV_CAM_CC_FBKT;
    if (s->p_msnr == 4) return R6IV_CAM_CC_MSNR;
    return R6IV_CAM_OK;
}

/* CC copies of the settings (ids [V], CC master table 0xE0FE3344), in this order; M8.2: gate (R6IV_GATE_CC_SETTINGS) */
static int iv_cc_settings(const struct r6_iv_cc * s)
{
    if (!(s->valid & R6IV_V_P) || s->p_ae != 3) return R6IV_CAM_CC_MODE;
    if (s->p_drive != 0) return R6IV_CAM_CC_DRIVE;
    if (!iv_cc_tv_ok((int16_t) s->p_tv)) return R6IV_CAM_CC_BULB;   /* also 0xDF00 / 0xDC00 / beyond 30 s */
    if (s->af == 1) return R6IV_CAM_CC_AF;
    if (s->p_92c != 0) return R6IV_CAM_CC_MECHA;
    if (s->p_itimer) return R6IV_CAM_CC_ITIMER;
    if (s->p_fbrk) return R6IV_CAM_CC_FBRK;
    if (iv_bulb_busy(s)) return R6IV_CAM_BULB_BUSY;                 /* M9.2, skeptic C1 */
    return iv_cc_multi(s);                                          /* M10.2 D2 */
}

/* M9.2: CC copies for B, in this order (hard: Start / Test refused, series STOP_CAMERA); *t_s = T on OK */
static int iv_cc_settings_bulb(const struct r6_iv_cc * s, int * t_s)
{
    if (!(s->valid & R6IV_V_P) || s->p_ae != 4) return R6IV_CAM_CC_NOTB;
    if (s->p_drive != 0) return R6IV_CAM_CC_DRIVE;
    if ((int16_t) s->p_tv != R6IV_TV_BULB) return R6IV_CAM_CC_TVB;
    if (s->af != 0) return R6IV_CAM_CC_AF;                 /* 0xE0716EDE: [cc+0x2C0] == 1 would give state 5 */
    if (s->p_92c != 0) return R6IV_CAM_CC_MECHA;
    if (s->p_itimer) return R6IV_CAM_CC_ITIMER;
    if (s->p_fbrk) return R6IV_CAM_CC_FBRK;
    if (s->r5e8) return R6IV_CAM_CC_R5E8;                  /* skeptic D2 */
    if (iv_bulb_busy(s)) return R6IV_CAM_BULB_BUSY;        /* R2 + C5: SHOOT_START never clears BA0, a stale one would arm */
    int m = iv_cc_multi(s);                                /* M10.2 D2 */
    if (m != R6IV_CAM_OK) return m;
    if ((s->bulbw & 0xFF) != 1) return R6IV_CAM_BULB_OFF;
    int t = r6_iv_bulb_t(s);
    if (t < R6IV_BULB_MIN_S || t > R6IV_BULB_MAX_S) return R6IV_CAM_BULB_TIME;
    if (t_s) *t_s = t;
    return R6IV_CAM_OK;
}

/* M9.2: the camera check for B (shooting_mode SHOOTMODE_BULB, camera LVP00056: 4); the CC copy [p+8] == 4 must agree */
static int iv_cam_check_bulb(void)
{
    int seen = 0, len = 0, tv = r6_iv_tv_prop(&seen, &len);
    if (!seen || len != 2 || tv != R6IV_TV_BULB) return R6IV_CAM_TVB;
    if (drive_mode != DRIVE_SINGLE) return R6IV_CAM_DRIVE;
    if ((af_mode & 0xF) != AF_MODE_MANUAL_FOCUS) return R6IV_CAM_FOCUS;
    if (avail_shot <= 0) return R6IV_CAM_CARD;
    if (iv_aeb_seen && (int16_t) (iv_aeb & 0xFFFF)) return R6IV_CAM_AEB;
    struct r6_iv_cc s;
    r6_iv_cc_read(&s);
    if (!(s.valid & R6IV_V_CC)) return R6IV_CAM_OK;        /* invalid cc: the Canon check's CCPTR (hard), as in M */
    return iv_cc_settings_bulb(&s, 0);
}

int r6_iv_cam_check(void)
{
    if (!_r6pw_press_ok()) return R6IV_CAM_TVOPEN;   /* M10.2: a Tv request may still be queued: no ML press (spec 3.7) */
    if (is_movie_mode() || lv_movie_select == LVMS_ENABLE_MOVIE) return R6IV_CAM_MOVIE;
    if (shooting_mode == SHOOTMODE_BULB) return iv_cam_check_bulb();      /* M9.2 */
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
        if (s.valid & R6IV_V_CC)                 /* an invalid cc is the Canon check's job (CCPTR, hard) */
        {
            int c = iv_cc_settings(&s);
            if (c != R6IV_CAM_OK) return c;
        }
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
 * gating field that differs counts as CCSTATE (torn read). task = 2 (M10.2): as 1, inside a bracket event: the
 * R6IV_MENU_WANT_MS hold of a blocked Trash press runs from the event's end, not between its members */
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
        if (gui_menu_shown() || now - iv_t_menu < R6IV_MENU_HOLD_MS || (task != 2 && now - iv_t_menu_wanted < R6IV_MENU_WANT_MS))
            return R6IV_CANON_MENU;
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
    if (s->b98 || s->c830) return R6IV_CANON_CCFLAG;
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

/* M10.2: read-only helpers for r6_propw.c (names _[a-zA-Z]*: not in magiclantern.sym) */
int _r6iv_cc_gate(const struct r6_iv_cc * s) { return iv_cc_gate(s); }
int _r6iv_lvstill(const struct r6_iv_cc * s) { return iv_cc_lvstill(s); }
int _r6iv_cc_same(const struct r6_iv_cc * a, const struct r6_iv_cc * b) { return iv_cc_same(a, b); }
void _r6iv_times(int * t_menu, int * t_lv, int * in_pic) { *t_menu = iv_t_menu; *t_lv = iv_t_lv; *in_pic = iv_in_pic; }
const struct r6_iv_bkt * r6_iv_bkt(void) { return &bk; }

/* ---- M10.2 Bracket: member list, checks, budget (spec 4.2 .. 4.5) ---- */
static int iv_bkt_mode(void) { return r6_iv_mode == 1; }
static int iv_bkt_count(void) { return 3 + 2 * COERCE(r6_iv_bkt_n, 0, 3); }

/* exposure offsets e_k in steps (+ = brighter = longer Tv); Tv_k = base - e_k * step (Code768: larger = shorter) */
static const int8_t iv_bkt_e0[R6IV_BKT_MAXN] = { 0, -1, 1, -2, 2, -3, 3, -4, 4 };     /* "0 - +" */
static int iv_bkt_build(int base, int16_t * list)
{
    int n = iv_bkt_count(), s = COERCE(r6_iv_bkt_step, 1, 9) * 0x100, h = (n - 1) / 2;
    for (int k = 0; k < n; k++) list[k] = base - (r6_iv_bkt_order ? k - h : iv_bkt_e0[k]) * s;   /* "- 0 +": -h .. +h */
    return n;
}

/* the first member that does not fit, in member order, each checked in this order: 1 not on the 1/3 grid, 2 outside
 * 1/8000 .. 30 s, 3 outside Canon's cached range, 4 outside the raw C.Fn range, 5 faster than 1/200 without the
 * no-flash proof (row 43); never clamped. 0 = all fit, *bad_k = the member */
static const char * const iv_bkt_why[6] = { "", "not on the 1/3 grid", "outside 1/8000..30s", "outside Canon's range",
                                            "outside the C.Fn range", "faster than 1/200: flash?" };
static int iv_bkt_list_check(const struct r6pw_rng * r, const int16_t * list, int n, int * bad_k)
{
    for (int k = 0; k < n; k++)
    {
        int v = list[k], why = 0;
        if (v & (R6PW_GRID - 1)) why = 1;
        else if (v < R6PW_TV_MIN || v > R6PW_TV_MAX) why = 2;
        else if (v > r->fast || v < r->slow) why = 3;
        else if (v > r->cf_fast || v < r->cf_slow) why = 4;
        else if (v > R6PW_XSYNC_CAP && !r->noflash) why = 5;
        if (why) { *bad_k = k; return why; }
    }
    return 0;
}

/* 4.5: need = R6IV_PROC_MS + sum ub(Tv_k) + N member + W write; need_nr with 2 ub for Tv_k >= 1 s; W = writes */
static void iv_bkt_budget(const int16_t * list, int n, int base, int * need, int * need_nr, int * writes)
{
    int sum = 0, sum_nr = 0, w = 0, cur = base;
    for (int k = 0; k < n; k++)
    {
        int t = iv_tv_ub_ms(list[k]);
        sum += t;
        sum_nr += list[k] <= 0 ? 2 * t : 1000;
        if (list[k] != cur) w++;
        cur = list[k];
    }
    if (cur != base) w++;                                /* the restore */
    *need = R6IV_PROC_MS + sum + n * R6IV_BKT_MEMBER_MS + w * R6IV_BKT_WRITE_MS;
    *need_nr = R6IV_PROC_NR_MS + sum_nr + n * R6IV_BKT_MEMBER_MS + w * R6IV_BKT_WRITE_MS;
    *writes = w;
}

/* M8.2: the Tv the budgets use (EV * 768): the longer (more negative) of ML's PROP_SHUTTER and the CC copy, each
 * only if it is a Tv of 30 s .. 1/16000; 0 = none known (*ev untouched) */
static int iv_tv_budget_ev(const struct r6_iv_cc * s, int * ev)
{
    int e = 0;
    int ok = r6_iv_tv_ev768(&e) && e >= R6IV_TV_EV768_MIN && e <= R6IV_TV_EV768_MAX;
    if ((s->valid & R6IV_V_P) && iv_cc_tv_ok((int16_t) s->p_tv))
    {
        int c = (int16_t) s->p_tv;
        if (!ok || c < e) e = c;
        ok = 1;
    }
    if (ok) *ev = e;
    return ok;
}

/* 0 ok, 1 warning (NR may skip slots), 2 refused. Tv: the longer of PROP and a valid CC copy.
 * need = (Tv >= 1 s ? T : 1000) + R6IV_PROC_MS, need_nr = Tv >= 1 s ? 2 T + R6IV_PROC_NR_MS : 0 (ms);
 * the warning only if [p+0x54C] != 0 or p is not readable. No Tv known: 0 (the camera check refuses) */
static int iv_interval_fit(int interval_s, int * need, int * need_nr)
{
    struct r6_iv_cc s;
    r6_iv_cc_read(&s);
    int ev = 0, n = 0, nr = 0;
    if (shooting_mode == SHOOTMODE_BULB)                       /* M9.2: T from Canon's bulb timer; 0 (off): the camera check refuses */
    {
        int t = r6_iv_bulb_t(&s);
        if (t > 0) { n = t * 1000 + R6IV_PROC_MS; nr = 2 * t * 1000 + R6IV_PROC_NR_MS; }
    }
    else if (iv_bkt_mode() && shooting_mode == SHOOTMODE_M && iv_tv_budget_ev(&s, &ev))
    {
        int16_t list[R6IV_BKT_MAXN];                           /* M10.2: one bracket event (spec 4.5) */
        int w = 0;
        iv_bkt_budget(list, iv_bkt_build(ev, list), ev, &n, &nr, &w);
    }
    else if (iv_tv_budget_ev(&s, &ev))
    {
        int t = iv_tv_ub_ms(ev);
        n = (ev <= 0 ? t : 1000) + R6IV_PROC_MS;
        nr = ev <= 0 ? 2 * t + R6IV_PROC_NR_MS : 0;
    }
    if (need) *need = n;
    if (need_nr) *need_nr = nr;
    int ms = interval_s * 1000;
    if (ms < n) return 2;
    if (ms < nr && (!(s.valid & R6IV_V_P) || s.p_lenr != 0)) return 1;
    return 0;
}

/* M9.2: the budget add-on of a bulb of t_s seconds (shift: no reciprocal constant); 3 717 500 ms at 30 min */
static int iv_bulb_busy_ms(int t_s) { int t = t_s * 1000; return 2 * t + R6IV_BULB_BUSY_MS + (t >> 4); }
/* M9.3 (m93 skeptic D2): ML's clock (DIGIC us counter, src/shoot.c) is not Canon's (DryOS ms clock, timer A / [0x1E568]),
 * and the camera has data for T 35 s only: the phase W margins grow with T (shifts): OVER 5 s + T / 64 (33.1 s at
 * 30 min), EARLY 1.5 s + T / 256 (8.5 s). Harmless: ML holds nothing in phase W, a later overrun stop only means
 * more exposure; a false OVERRUN would tell the user to end a healthy timer bulb */
static int iv_bulb_over_ms(int tms) { return R6IV_BULB_OVER_MS + (tms >> 6); }
static int iv_bulb_early_ms(int tms) { return R6IV_BULB_EARLY_MS + (tms >> 8); }
/* phase W hard bound: twice the nominal poll count (T at 100 ms plus the overrun margin), the 10 ms classification
 * polls, + 40 (36 732 at 30 min) */
static int iv_bulb_polls_max(int bt)
{
    return 2 * (bt * (1000 / R6IV_BULB_POLL_MS) + iv_bulb_over_ms(bt * 1000) / R6IV_BULB_POLL_MS)
        + R6IV_BULB_CLASS_MS / R6IV_POLL_MS + 40;
}

void r6_iv_budget(int * tv_ub, int * busy, int * need, int * need_nr, int * fit)
{
    struct r6_iv_cc s;
    r6_iv_cc_read(&s);
    int ev = 0, t = -1, b = 0;
    if (shooting_mode == SHOOTMODE_BULB)                       /* M9.2 */
    {
        int bt = r6_iv_bulb_t(&s);
        t = bt > 0 ? bt * 1000 : -1;
        b = bt > 0 ? iv_bulb_busy_ms(bt) : 0;
    }
    else if (iv_tv_budget_ev(&s, &ev))
    {
        t = iv_tv_ub_ms(ev);
        b = ev <= 0 ? 2 * t : 0;
    }
    *tv_ub = t;
    *busy = b;
    *fit = iv_interval_fit(iv_interval_s(), need, need_nr);
}

/* M9.2: Canon's bulb (any, also the user's own) runs: 3 words, no snapshot (menu task stack) */
static int iv_bulb_canon_running(void)
{
    uint32_t cc = IV_RD(R6IV_RAM_CC);
    return iv_ram_ok(cc, R6IV_CC_SIZE) && IV_RD(cc + R6IV_CC_STATE) == 12 && IV_RD(cc + R6IV_CC_D30) == 1;
}

/* src/menu.c menu_open() (M8.2): 1 = blocked (counted), the ML menu stays closed. No SetGUIRequestMode(0x2003) while a
 * picture runs (the GUI mode stays 0 and gui_state idle during a timed exposure [V], so the ML menu would open inside
 * it); M9.2: also after STOP_BULB_OVERRUN while Canon's bulb still runs. M8.2 review fix: a note, and iv_canon_check
 * holds the next picture R6IV_MENU_WANT_MS after the blocked press (postponed like an open ML menu, not skipped), so
 * the next Trash press opens the menu even when long exposures run back to back. M9.2 review F3: for a press blocked
 * by a running picture that hold runs from the END of the picture (iv_pic_done; a bulb ends up to T + 5 s after the
 * press), the press time stamp is kept as well */
int r6_iv_menu_block(void)
{
    int ovr = iv_bulb_ovr && iv_bulb_canon_running();     /* M9.2: after STOP_BULB_OVERRUN while Canon's bulb still runs */
    int evb = _r6pw_ev_on();                              /* M10.2: a bracket event or its restore runs (spec 4.7) */
    if (!iv_in_pic && !ovr && !evb) return 0;
    st.menu_blocked++;
    iv_t_menu_wanted = get_ms_clock();
    if (iv_in_pic) iv_menu_wanted_pic = 1;                /* review F3 */
    if (evb) iv_menu_wanted_ev = 1;                       /* M10.2: the hold runs from the event's end */
    /* M9.3 review F5: the feature whose picture runs (a bulb is always R6 Interval's). Review round 2: every line fits
     * the screen (FONT_LARGE at x = 50: <= 670 px, argnor32 widths: bulb 433 / 503 / 391 px at 1799/1800 s, the others
     * <= 619 px); the bulb text is 94 characters at 1799/1800 s (NotifyBox keeps 98) */
    const char * nm = (iv_in_pic && st.mode != R6IV_MODE_INTERVAL) ? "R6 Motion" : "R6 Interval";
    if (ovr) NotifyBox(3000, "R6 Interval: bulb did not end.\nPress the shutter fully once, or switch off.");
    else if (iv_bulb && !iv_bulb_end_t)                   /* M9.3: up to 30 min */
        NotifyBox(3000, "R6 Interval: bulb %d/%d s.\nAbort: full press, not near the end.\nML menu after the picture.", MAX(get_ms_clock() - iv_t1, 0) / 1000, iv_bulb_s);
    else if (iv_bulb) NotifyBox(3000, "R6 Interval: Canon busy (NR).\nML menu after it.");
    else if (evb && _r6pw_state()->ev_on) NotifyBox(3000, "R6 Interval: bracket %d/%d running.\nML menu after it.", MIN(bk.k + 1, bk.n), bk.n);
    else if (evb) NotifyBox(3000, "R6 Interval: Tv restore running.\nML menu after it.");   /* review round 1, safety L5 */
    else NotifyBox(3000, "%s: picture running.\nPress Trash again right after it.", nm);
    return 1;
}

/* M10.2: a Trash press blocked during a bracket event: the R6IV_MENU_WANT_MS hold runs from the event's end */
static void iv_ev_end_hold(void)
{
    if (iv_menu_wanted_ev && !_r6pw_ev_on())
    {
        iv_menu_wanted_ev = 0;
        iv_t_menu_wanted = get_ms_clock();
    }
}

static void iv_end(int stop, int detail, int32_t val)
{
    st.stop = stop;
    st.stop_cam = stop == R6IV_STOP_CAMERA ? detail : 0;
    st.stop_canon = (stop == R6IV_STOP_CANON || stop == R6IV_STOP_WAIT || stop == R6IV_STOP_ABORT) ? detail : 0;
    st.stop_val = val;
    st.t_stop = get_ms_clock();
    st.running = 0;
#ifdef CONFIG_R6_MOTION
    if (st.mode != R6IV_MODE_INTERVAL) r6_md_end();     /* M9.3: idempotent (iv_end can run twice) */
#endif
    _r6pw_series_end(stop);   /* M10.2: every end of a series / test: event stop (restore now / pending / none), then
                               * disarm or ctx PEND; once per arm (iv_end can run twice) */
    iv_ev_end_hold();
}

static uint32_t iv_test_shots0 = 0;      /* st.shots when the last test started (Test line RINFO) */

/* M10.2: the checks 2..6 of spec 4.3 for Mode Bracket, after r6_iv_cam_check (menu task: Start / Test; task: again in
 * iv_begin); *base = Canon's [p+0x61E] of the read (the task replaces it by the stable-M Tv) */
static int iv_bkt_check(int test, int * base)
{
    if (shooting_mode != SHOOTMODE_M) return R6IV_CAM_BKT_MODE;
    int lock = 0;
    if (_r6pw_locks(R6PW_WANT_NONE, R6PW_CLASS_HARD, &lock)) { iv_refused_pw = lock; return R6IV_CAM_TVW; }
    struct r6pw_rng r;
    if (!_r6pw_ranges(&r)) { iv_refused_pw = R6PW_L_PTR; return R6IV_CAM_TVW; }
    if (base) *base = r.tv;
    int16_t list[R6IV_BKT_MAXN];
    int n = iv_bkt_build(r.tv, list), k = 0, why = iv_bkt_list_check(&r, list, n, &k);
    if (why) { iv_refused_bk = why << 8 | k; iv_refused_tv = list[k]; return R6IV_CAM_BKT_LIST; }
    if (!test && iv_count() * n > R6IV_MAX_PICS) return R6IV_CAM_BKT_MAX;
    if (!test && iv_interval_fit(iv_interval_s(), 0, 0) == 2) return R6IV_CAM_INTERVAL;   /* shorter than one bracket */
    return R6IV_CAM_OK;
}

static void iv_begin(int now, int test)
{
    int mode0 = shooting_mode;               /* review round 2: the mode the checks below see */
    int c = r6_iv_cam_check();
    if (c == R6IV_CAM_OK && shooting_mode != mode0) c = R6IV_CAM_MODE;
    int bkm = iv_bkt_mode(), base = 0;
    if (c == R6IV_CAM_OK && bkm)                 /* M10.2: spec 4.3, in the task; base = the stable-M Tv (row 36) */
    {
        int16_t v = 0;
        c = iv_bkt_check(test, &base);
        if (c == R6IV_CAM_OK && (!_r6pw_tv_stable(&v) || v != base)) { iv_refused_pw = R6PW_L_VIEWS; c = R6IV_CAM_TVW; }
    }
    if (c == R6IV_CAM_OK && !bkm && !test && iv_interval_fit(iv_interval_s(), 0, 0) == 2) c = R6IV_CAM_INTERVAL;   /* M8.2 */
    if (c != R6IV_CAM_OK)
    {
        iv_refused_cam = c;
        st.refused++;
        return;
    }
    iv_refused_cam = R6IV_CAM_OK;
    iv_mode0 = mode0;                        /* review F2: the series keeps this mode (M or B, r6_iv_cam_check passed) */
    st.mode = R6IV_MODE_INTERVAL;            /* M9.3 */
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
    st.bulb_warn = 0;                        /* M9.2 */
    memset(&bk, 0, sizeof(bk));              /* M10.2: the run's bracket (mode 0 = Single) */
    if (bkm)
    {
        bk.mode = 1;
        bk.n = iv_bkt_build(base, bk.list);
        bk.step = COERCE(r6_iv_bkt_step, 1, 9);
        bk.order = r6_iv_bkt_order;
        bk.base = base;
        iv_bkt_budget(bk.list, bk.n, base, &bk.need, &bk.need_nr, &bk.writes);
        bk.fit = test ? 0 : iv_interval_fit(st.interval_s, 0, 0);
        _r6pw_begin(base);
    }
    st.running = 1;
}

#ifdef CONFIG_R6_MOTION
/* M9.3: the camera check of a motion Start. Display only: anything but movie; Release: not B, then R6 Interval's check */
static int iv_md_check(int shoot)
{
    if (!shoot) return (is_movie_mode() || lv_movie_select == LVMS_ENABLE_MOVIE) ? R6IV_CAM_MOVIE : R6IV_CAM_OK;
    if (shooting_mode == SHOOTMODE_BULB) return R6IV_CAM_MD_B;
    return r6_iv_cam_check();
}

/* M9.3: command 4 in the task: the checks again (the mode may have changed), the detector's buffers, the run */
static void iv_begin_md(int now, int shoot)
{
    int mode0 = shooting_mode;
    int c = iv_md_check(shoot);
    if (c == R6IV_CAM_OK && shooting_mode != mode0) c = R6IV_CAM_MODE;
    if (c == R6IV_CAM_OK && r6_md_begin(now, shoot)) c = R6IV_CAM_MD_MEM;
    if (c != R6IV_CAM_OK)
    {
        iv_md_refused = c;
        st.refused++;
        return;
    }
    iv_md_refused = R6IV_CAM_OK;
    iv_mode0 = mode0;
    st.mode = shoot ? R6IV_MODE_MD_SHOOT : R6IV_MODE_MD_VIEW;
    st.test = 0;
    st.interval_s = 0;
    st.n_target = shoot ? r6_md_count() : 0;
    st.n_done = 0;
    st.skipped = 0;
    st.stop = R6IV_STOP_NONE;
    st.stop_cam = st.stop_canon = 0;
    st.stop_val = 0;
    st.t_start = now;
    st.cnt_last = total_shots_count;
    st.fn_last = iv_file_number();
    iv_due = now;
    iv_wait_ms = 0;
    iv_wait_mask = 0;
    iv_wait_menu = 0;
    iv_gui_seen = 0;
    st.bulb_warn = 0;
    memset(&bk, 0, sizeof(bk));              /* M10.2: motion never brackets */
    st.running = 1;
}
#endif

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
    v[13] = s->lv;
    v[14] = s->gstate;
    v[15] = s->p_92c;
    v[16] = s->d30;                          /* M9.2: absolute values; seconds only 0 / 1 (60 s do not fill the trace) */
    v[17] = s->ba0;
    v[18] = s->ta;
    v[19] = s->sec != 0;
    v[20] = s->bulb_ms;
}

static void iv_tr_add(char code, uint32_t v)
{
    if (L.n >= R6IV_TRACE) { L.lost++; return; }
    int dt = get_ms_clock() - iv_t1;
    L.tr[L.n].dt = MAX(dt, 0);               /* M8.2: 32 bit (exposure + NR can pass 65 s) */
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
    if (ok && on && !_r6pw_press_ok()) ok = 0;   /* M10.2: no ON while a Tv request is open; OFFs are never gated (RR2-18) */
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
    if (_r6pw_ev_on()) _r6pw_consume(now);   /* M10.2: the ring consumer, every 10 ms inside a bracket member (3.3) */
    if (iv_s.state < 16) e->states |= 1 << iv_s.state;
    if ((iv_s.state == 5 || (iv_bulb && iv_s.state == 12)) && !e->s5_ms) e->s5_ms = MAX(now - iv_t1, 1);   /* M8.2; M9.2: 12 */
    if (iv_s.state == 6 && e->s5_ms && !e->exp_ms) e->exp_ms = MAX(now - iv_t1 - (int) e->s5_ms, 1);
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

/* M8.2, Tv >= 1 s: a job or job-ID step could be read before the state-5 write lands, and an OFF in state 4 is
 * RELEASE_STOP [I]: only the CC state and the end of the sequence count. Not committed within R6IV_SW2_MS:
 * TIMEOUT / STUCK4 as for short Tv (no picture rather than a cut one) */
static int iv_committed_long(const struct r6_iv_cc * s, int rose)
{
    if (!(s->valid & R6IV_V_P) || s->p_92c) return IV_WHY_MECHA;   /* SW2 OFF in 5 would be RELEASE_STOP: let go at once */
    if (s->state == 5 && s->b54 == 1) return IV_WHY_S5;             /* the deferral path of F2 */
    if (s->state == 6) return IV_WHY_S6;
    if (s->state == 12) return IV_WHY_S12;                          /* bulb: SW1 OFF ends it ~40 ms later [V A] */
    if (rose && !s->relseq && !s->b54) return IV_WHY_RELEND;
    return IV_WHY_NONE;                                             /* RELCNT / JOB / JID: no triggers */
}

/* M9.2: commit only when SHOOT_START took the bulb path: state 12 AND [cc+0xD30] 1 (written after the state, 0xE031D864);
 * the OFFs are then dispatched with the final BA0 (spec R1). State 5 (b54) / 6 / sequence end: not a bulb, let go as
 * M8.2 (stop BULB_NOT12 after the picture) */
static int iv_committed_bulb(const struct r6_iv_cc * s, int rose)
{
    if (!(s->valid & R6IV_V_P) || s->p_92c) return IV_WHY_MECHA;
    if (s->state == 12 && s->d30 == 1) return IV_WHY_S12;
    if (s->state == 5 && s->b54 == 1) return IV_WHY_S5;
    if (s->state == 6) return IV_WHY_S6;
    if (rose && !s->relseq && !s->b54) return IV_WHY_RELEND;
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
    iv_long = 0;
    iv_bulb = 0;                             /* M9.2 */
    iv_bulb_end_t = 0;                       /* M9.3 */
    if (iv_menu_wanted_pic)                  /* review F3: the hold after a blocked Trash press runs from here */
    {
        iv_menu_wanted_pic = 0;
        iv_t_menu_wanted = get_ms_clock();
    }
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

static int iv_bend(struct r6_iv_shot * e, int b) { e->bend = b; iv_tr_add('Z', b); return b; }

/* M9.2 phase W: from the OFFs (t3) until Canon's bulb ends ([cc+0xD30] 0 or state != 12). ML holds nothing and calls
 * nothing; it only reads. 10 ms polls during R6IV_BULB_CLASS_MS, then R6IV_BULB_POLL_MS. Elapsed time el sums the
 * poll steps; a step back, or forward by more than R6IV_BULB_GLITCH_MS, counts as a clock glitch (clk_back) and adds
 * nothing / one poll period (skeptic C3: get_ms_clock can step back under SMP; a false OVERRUN would tell the user to
 * press, which makes a healthy timer bulb manual). Hard bound: a poll count of twice the nominal. Returns IV_BEND_* */
static int iv_bulb_watch(struct r6_iv_shot * e, int t3)
{
    int tms = e->bt * 1000, seen = 0, checked = 0, el = 0, prev = t3, polls = 0;
    int polls_max = iv_bulb_polls_max(e->bt);
    for (;;)
    {
        int now = iv_poll(e);
        int d = now - prev;
        prev = now;
        if (d < 0 || d > R6IV_BULB_GLITCH_MS)
        {
            if (e->clk_back < 0xFFFF) e->clk_back++;
            d = d < 0 ? 0 : R6IV_BULB_POLL_MS;
        }
        el += d;
        polls++;
        int ok = (iv_s.valid & R6IV_V_CC) != 0;
        if (ok && (iv_s.ba0 == 1 || iv_s.ta == 1)) seen = 1;
        if (iv_s.ta == 1 && !e->ta_ms) e->ta_ms = iv_s.ta_ms;
        if (ok && iv_s.sec > e->sec_max) e->sec_max = MIN(iv_s.sec, 0xFFFF);
        if (ok) e->bbtn |= (iv_s.df ? 1 : 0) | (iv_s.e0 ? 2 : 0);      /* the real button: logged, never answered */
        if (ml_shutdown_requested) { e->d30_ms = el; return iv_bend(e, IV_BEND_SHUTDOWN); }
        int running = ok && iv_s.state == 12 && iv_s.d30 == 1;
        if (!e->bulb && (!running || el >= R6IV_BULB_CLASS_MS))
            e->bulb = seen ? IV_BCLASS_TIMER : IV_BCLASS_NOTIMER;
        if (!running)
        {
            e->d30_ms = el;
            e->bulb_ms = IV_RD(R6IV_RAM_BULB_MS);   /* skeptic C4: a fresh read; stored before RELEASE_STOP and D30 = 0 (0xE02F6F7C) */
            if (ok && iv_s.d30) e->bwarn |= IV_BW_D30;
            if (e->bulb == IV_BCLASS_NOTIMER) return iv_bend(e, IV_BEND_NOTIMER);
            if (el < tms - iv_bulb_early_ms(tms)) return iv_bend(e, IV_BEND_EARLY);
            if (e->bulb_ms < (uint32_t) tms || e->bulb_ms > (uint32_t) (tms + R6IV_BULB_MS_TOL)) e->bwarn |= IV_BW_MS;
            return iv_bend(e, IV_BEND_TIMER);
        }
        if (!checked && el >= R6IV_BULB_CHECK_MS)
        {
            checked = 1;
            if (iv_s.ta != 1) e->bwarn |= IV_BW_TA;
            else if (iv_s.ta_ms != (uint32_t) tms) e->bwarn |= IV_BW_TAMS;
            if (iv_s.tb != 1) e->bwarn |= IV_BW_TB;
            if (iv_s.sec < 1) e->bwarn |= IV_BW_SEC;
            /* skeptic D1 / review F1: no finger seen, timers A / B 0, and BA0 0 (nothing armed) or seconds 0 (EXPOSURE_START
             * 0xE05E0E00, which arms A / B with BA0 1 and starts the 1 s tick, never ran): nothing can end Canon's bulb by
             * itself (a full press can: Sw2OnBulbing makes it manual, the release ends it) */
            int noend = !e->bbtn && !iv_s.ta && !iv_s.tb && (!iv_s.ba0 || !iv_s.sec);
            if (noend) e->bwarn |= IV_BW_NOEND;
            iv_tr_add('W', e->bwarn);
            if (noend) { e->d30_ms = el; return iv_bend(e, IV_BEND_OVERRUN); }
        }
        if (el > tms + iv_bulb_over_ms(tms) || polls > polls_max) { e->d30_ms = el; return iv_bend(e, IV_BEND_OVERRUN); }
        msleep(el < R6IV_BULB_CLASS_MS ? R6IV_POLL_MS : R6IV_BULB_POLL_MS);
    }
}

/* one picture; called by the task only, with the camera checked as ready on the snapshot pre */
static void iv_shoot(const struct r6_iv_cc * pre)
{
    /* a picture that ML did not take since the start / ML's last picture */
    int dcnt = total_shots_count - st.cnt_last, dfn = iv_file_number() - st.fn_last;
    if (dcnt || dfn) { iv_end(R6IV_STOP_FOREIGN, 0, dcnt ? dcnt : dfn); return; }
    int evon = _r6pw_ev_on();                               /* M10.2: a bracket member (iv_event) */
    /* M10.2 review round 1 (L4 / L6, spec 3.11): inside an event also an abort signal (PROP_ABORT, TERMINATE_SHUT_REQ)
     * that did not set ml_shutdown_requested stops before any press */
    if (ml_shutdown_requested || (evon && _r6pw_sig())) { iv_end(R6IV_STOP_SHUTDOWN, 0, 0); return; }

    /* phase 0: prepare */
    struct r6_iv_shot * e = &st.ring[st.shots % R6IV_RING];
    memset(e, 0, sizeof(*e));
    e->wait_ms = iv_wait_ms;
    e->wait_mask = iv_wait_mask;
    e->sw1_ms = e->acc_ms = 0xFFFF;
    e->seq_ms = 0xFFFFFFFF;
    e->d_rel = -32768;
    e->tv = e->tv_cc = -32768;                              /* M8.2: set at the pre-SW2 check */
    e->bk = -1;                                             /* M10.2: not a bracket member */
    e->want = -32768;
    e->wr = -1;
    if (evon) { e->bk = bk.k; e->bn = bk.n; e->want = bk.want; e->ev = bk.ev; e->wr = bk.wr; }
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
    iv_menu_wanted_pic = 0;                  /* review F3: a press that raced the last iv_pic_done (its own stamp holds) */
    iv_in_pic = 1;
    iv_e = e;
    int t1 = iv_t1 = get_ms_clock();
    e->t = t1;
    e->slot_late = (st.test || (evon && bk.k)) ? 0 : t1 - iv_due;   /* M10.2: a slot only for member 0 */
    if (evon && !bk.k) bk.t1 = t1;
    st.calls++;
    st.shots++;

    /* phase A: half press, CC must take it */
    if (iv_press(IV_SW1, 1)) { iv_pic_stop(e, R6IV_STOP_RET, 0, e->ret, IV_X_ABORT + R6IV_STOP_RET, 0); return; }
    int now = t1;
    for (;;)
    {
        now = iv_poll(e);
        if (ml_shutdown_requested || (evon && _r6pw_sig()))   /* M10.2 review round 1: an abort signal too (3.11) */
        {
            iv_pic_stop(e, R6IV_STOP_SHUTDOWN, 0, 0, IV_X_ABORT + R6IV_STOP_SHUTDOWN, 0);
            return;
        }
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
        if (ml_shutdown_requested || (evon && _r6pw_sig()))   /* before SW2 (M10.2 review round 1, 3.11) */
        {
            iv_pic_stop(e, R6IV_STOP_SHUTDOWN, 0, 0, IV_X_ABORT + R6IV_STOP_SHUTDOWN, 0);
            return;
        }
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
    if (k == R6IV_CANON_OK && (iv_s.b98 || iv_s.c830)) k = R6IV_CANON_CCFLAG;
    if (k != R6IV_CANON_OK)
    {
        iv_pic_stop(e, R6IV_STOP_ABORT, k, iv_s.state, IV_X_ABORT + R6IV_STOP_ABORT, 1);
        return;
    }
    int cam = r6_iv_cam_check();
    if (cam == R6IV_CAM_OK && shooting_mode != iv_mode0)     /* review F2: changed since the tick (SW1 released) */
    {
        iv_pic_stop(e, R6IV_STOP_CAMERA, R6IV_CAM_MODE, shooting_mode, IV_X_ABORT + R6IV_STOP_ABORT, 1);
        return;
    }
    if (cam != R6IV_CAM_OK)
    {
        iv_pic_stop(e, R6IV_STOP_ABORT, 0, iv_s.state, IV_X_ABORT + R6IV_STOP_ABORT, 1);
        st.stop_cam = cam;
        return;
    }
    if (iv_mode0 == SHOOTMODE_BULB)                         /* review F2: the mode of the series, not the live one */
    {
        /* M9.2: the B gates on this snapshot too (a dial turned since r6_iv_cam_check fails here: CC_NOTB), then T */
        int bt = 0;
        int c2 = iv_cc_settings_bulb(&iv_s, &bt);
        if (c2 != R6IV_CAM_OK) { iv_pic_stop(e, R6IV_STOP_ABORT, 0, iv_s.state, IV_X_ABORT + R6IV_STOP_ABORT, 1); st.stop_cam = c2; return; }
        e->tv = (int16_t) r6_iv_tv_prop(0, 0); e->tv_cc = (int16_t) iv_s.p_tv; e->r92c = iv_s.p_92c; e->nr = iv_s.p_lenr & 0xFF;
        e->cflag = iv_s.b98 | iv_s.c830 << 1;
        e->bt = bt; e->bulbw = iv_s.bulbw;
        iv_bulb = 1; iv_bulb_s = bt; iv_long = 0;
        e->busy = iv_bulb_busy_ms(bt);
    }
    else
    {
        /* M8.2: the CC copies on this snapshot too, then the Tv budget of this picture */
        int ev = 0;
        r6_iv_tv_ev768(&ev);                                    /* in range: r6_iv_cam_check() just passed */
        int c2 = iv_cc_settings(&iv_s);
        if (c2 != R6IV_CAM_OK) { iv_pic_stop(e, R6IV_STOP_ABORT, 0, iv_s.state, IV_X_ABORT + R6IV_STOP_ABORT, 1); st.stop_cam = c2; return; }
        int evc = (int16_t) iv_s.p_tv;
        if (evon && (ev != bk.want || evc != bk.want))
        {
            /* M10.2 (spec 4.4 3.3): Tv must be the member's; one re-read after R6PW_STABLE_MS (a single read never
             * stops, 3.6a); still different: SW1 released, no SW2; TVUSER only with user evidence, else the restore
             * path decides (lock 44) */
            msleep(R6PW_STABLE_MS);
            iv_poll(e);
            ev = 0;
            r6_iv_tv_ev768(&ev);
            evc = (int16_t) iv_s.p_tv;
            if (ev != bk.want || evc != bk.want)
            {
                int16_t u = 0;
                int user = _r6pw_user_evidence(bk.want, &u);
                iv_pic_stop(e, user ? R6IV_STOP_TVUSER : R6IV_STOP_ABORT, 0, user ? u : (int32_t) iv_s.state, IV_X_ABORT + R6IV_STOP_ABORT, 1);
                if (!user) st.stop_cam = R6IV_CAM_TVW;
                return;
            }
        }
        int evb = MIN(ev, evc);
        e->tv = ev; e->tv_cc = evc; e->r92c = iv_s.p_92c; e->nr = iv_s.p_lenr & 0xFF; e->cflag = iv_s.b98 | iv_s.c830 << 1;
        iv_long = evb <= 0;                                     /* 1 s or longer (Canon's LvDispBusy threshold [V]) */
        e->busy = iv_long ? 2 * iv_tv_ub_ms(evb) : 0;           /* x 2: a long exposure NR dark frame [I] */
    }
    e->drive = iv_s.p_drive & 0xFF; e->r5e8 = iv_s.r5e8 & 0xFF;          /* M9.2: log, M and B */

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
        else if ((why = iv_bulb ? iv_committed_bulb(&iv_s, rose) : iv_long ? iv_committed_long(&iv_s, rose)
                  : iv_committed(&iv_s, rose)) != IV_WHY_NONE) break;
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

    /* M9.2 phase W: only a committed bulb; a shutdown or an overrun stops at once (nothing is held, no call) */
    int b = iv_bulb, bend = IV_BEND_NONE;
    if (b && why == IV_WHY_S12)
    {
        bend = iv_bulb_watch(e, t3);
        iv_bulb_end_t = get_ms_clock() | 1;                 /* M9.3: status "Canon busy" (NR / saving) from here */
        if (bend == IV_BEND_SHUTDOWN || bend == IV_BEND_OVERRUN)
        {
            e->bulbw_after = iv_s.bulbw;
            st.bulb_warn |= e->bwarn;
            if (bend == IV_BEND_OVERRUN) iv_bulb_ovr = 1;       /* before iv_pic_done: no gap for the menu guard / overlays */
            iv_pic_done(e);
            if (bend == IV_BEND_OVERRUN) iv_end(R6IV_STOP_BULB_OVERRUN, 0, (int32_t) e->d30_ms);
            else iv_end(R6IV_STOP_SHUTDOWN, 0, 2);                    /* val 2: during a bulb */
            return;
        }
    }

    /* phase E: Canon idle again (ML's SW1 OFF is deferred while b54 == 1 and runs after RELEASE_END);
     * M8.2: within R6IV_SEQ_MS + busy (exposure and a possible NR dark frame lie in between). An ML shutdown
     * ends E, F and the settle at once (nothing is held after the OFFs): stop SHUTDOWN val 1 */
    int idle = 0, seq_bits = 0, shut = 0;
    for (;;)
    {
        now = iv_poll(e);
        seq_bits = iv_idle_bits(&iv_s);
        if (!seq_bits) { idle = 1; e->seq_ms = MAX(now - t3, 0); break; }
        if (ml_shutdown_requested) { shut = 1; break; }
        if (now - t3 >= R6IV_SEQ_MS + (int) e->busy || now < t3) break;
        msleep(20);
    }

    /* phase F: the picture (property), Canon's job, settle; a second release is watched once idle */
    int d_cnt = 0, d_fn = 0;
    if (!shut && (why != IV_WHY_TIMEOUT || e->late))
    {
        for (;;)
        {
            d_cnt = total_shots_count - iv_ref.cnt0;
            d_fn = MAX(iv_file_number() - iv_ref.fn0, 0);
            if (d_cnt || d_fn) { e->pic_ms = MAX(get_ms_clock() - t1, 0); break; }
            if (ml_shutdown_requested) { shut = 1; break; }
            now = get_ms_clock();
            if (now - t1 >= R6IV_PIC_MS + (e->s5_ms ? (int) e->busy : 0) || now < t1) break;   /* busy only after an exposure state */
            msleep(20);
            iv_poll(e);
            iv_check_second(e, idle);
        }
    }
    uint32_t jobs = iv_s.jobs;
    while (!shut)
    {
        jobs = iv_s.jobs;
        if (!jobs) { e->job_ms = MAX(get_ms_clock() - t1, 0); break; }
        if (ml_shutdown_requested) { shut = 1; break; }
        now = get_ms_clock();
        if (now - t1 >= R6IV_JOB_MS + (int) e->busy || now < t1) break;
        msleep(20);
        iv_poll(e);
        iv_check_second(e, idle);
    }
    e->jobs_after = jobs;
    for (int ts = get_ms_clock(); !shut && get_ms_clock() - ts < R6IV_SETTLE_MS; )
    {
        if (ml_shutdown_requested) { shut = 1; break; }
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
    if (b)                                                      /* M9.2 */
    {
        e->bulbw_after = iv_s.bulbw;
        if (e->bulbw_after != e->bulbw) e->bwarn |= IV_BW_WORD;
        st.bulb_warn |= e->bwarn;
    }
    e->final = fin;
    st.cnt_last = total_shots_count;
    st.fn_last = iv_file_number();
    iv_pic_done(e);
    if (shut) { iv_end(R6IV_STOP_SHUTDOWN, 0, 1); return; }      /* M8.2: before every other decision; no switch call */

    if (why == IV_WHY_TIMEOUT)
    {
        if (e->late) iv_end(R6IV_STOP_LATE, 0, e->late);
        else iv_end(R6IV_STOP_SW2, 0, L.off.state);
        return;
    }
    if (why == IV_WHY_STUCK4) { iv_end(R6IV_STOP_STUCK, 0, L.off.state); return; }
    if (why == IV_WHY_RET) { iv_end(R6IV_STOP_RET, 0, e->ret); return; }
    if (why == IV_WHY_MECHA) { iv_end(R6IV_STOP_CCSTATE, 0, fin | 0x4000); return; }   /* M8.2: PROP_MECHALESS / p lost */
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
    if (b && why != IV_WHY_S12) { iv_end(R6IV_STOP_BULB_NOT12, 0, L.off.state); return; }                  /* M9.2 */
    if (b && bend == IV_BEND_NOTIMER) { iv_end(R6IV_STOP_BULB_NOTIMER, 0, (int32_t) e->d30_ms); return; }
    if (b && bend == IV_BEND_EARLY) { iv_end(R6IV_STOP_BULB_EARLY, 0, (int32_t) e->d30_ms); return; }       /* before BUTTON */
    if (b && e->bbtn) { iv_end(R6IV_STOP_BUTTON, 0, e->bbtn | 0x100); return; }                             /* 0x1xx: in a bulb */
    if (fin || (why == IV_WHY_S12 && !b)) { iv_end(R6IV_STOP_CCSTATE, 0, fin | (why == IV_WHY_S12 && !b) << 15); return; }

    iv_wait_ms = 0;
    iv_wait_mask = 0;
    if (evon) return;                        /* M10.2: a bracket member; iv_event counts the event after its restore */
    st.n_done++;
    if (st.n_done >= st.n_target) { iv_end(R6IV_STOP_DONE, 0, 0); return; }
    iv_due = t1 + st.interval_s * 1000;
}

/* ---- M10.2: one bracket event (spec 4.4) ---- */

/* settle after a write: R6PW_SETTLE_MS in 10 ms polls; 1 = stopped (shutdown) */
static int iv_settle(void)
{
    int t0 = get_ms_clock();
    for (;;)
    {
        msleep(R6IV_POLL_MS);
        int now = get_ms_clock();
        iv_sample(now);
        _r6pw_consume(now);
        if (ml_shutdown_requested || _r6pw_sig()) { iv_end(R6IV_STOP_SHUTDOWN, 0, 0); return 1; }   /* review round 1: abort too */
        if (now - t0 >= R6PW_SETTLE_MS || now < t0) return 0;
    }
}

/* member pre-check, every 20 ms up to R6IV_BKT_WAIT_MS, in this order: shutdown, mode, camera check, Canon GUI latch,
 * a picture ML did not take, Canon (hard: stop; soft: wait), Tv by 3.6a (stably the member: ready; user evidence:
 * stop TVUSER; unsettled: wait); 0 = ready (iv_s / iv_s2 hold Canon's state), 1 = stopped */
static int iv_member_ready(int want)
{
    int t0 = get_ms_clock();
    for (;;)
    {
        int now = get_ms_clock();
        iv_sample(now);
        _r6pw_consume(now);
        if (ml_shutdown_requested || _r6pw_sig()) { iv_end(R6IV_STOP_SHUTDOWN, 0, 0); return 1; }   /* review round 1: abort too */
        if (shooting_mode != iv_mode0 || shooting_mode_custom != SHOOTMODE_M)
        {
            iv_end(R6IV_STOP_CAMERA, R6IV_CAM_MODE, shooting_mode != iv_mode0 ? shooting_mode : shooting_mode_custom);
            return 1;
        }
        int cam = r6_iv_cam_check();
        if (cam != R6IV_CAM_OK) { iv_end(R6IV_STOP_CAMERA, cam, 0); return 1; }
        int m = iv_gui_seen;
        if (m) { iv_end((m == R6IV_GUI_SAVER || m == R6IV_GUI_ECO) ? R6IV_STOP_PWRSAVE : R6IV_STOP_GUI, 0, m); return 1; }
        int dcnt = total_shots_count - st.cnt_last, dfn = iv_file_number() - st.fn_last;
        if (dcnt || dfn) { iv_end(R6IV_STOP_FOREIGN, 0, dcnt ? dcnt : dfn); return 1; }
        int hard = 0, k = iv_canon_check_snap(&hard, 2, &iv_s, &iv_s2);
        if (hard) { iv_end(R6IV_STOP_CANON, k, 0); return 1; }
        if ((iv_s.valid & R6IV_V_CC) && (iv_s.df || iv_s.e0))      /* the real shutter button: hard inside an event (4.6) */
        {
            iv_end(R6IV_STOP_BUTTON, 0, iv_s.df | iv_s.e0 << 8);
            return 1;
        }
        int16_t v = 0, u = 0;
        int tv = _r6pw_tv_stable(&v) && v == want;
        if (!tv && _r6pw_user_evidence(want, &u)) { iv_end(R6IV_STOP_TVUSER, 0, u); return 1; }
        if (tv && k == R6IV_CANON_OK) return 0;
        if (now - t0 >= R6IV_BKT_WAIT_MS || now < t0)
        {
            if (!tv) iv_end(R6IV_STOP_TVW, 0, R6PW_R_REFUSED << 8 | R6PW_L_VIEWS);
            else iv_end(R6IV_STOP_WAIT, k, now - t0);
            return 1;
        }
        msleep(20);
    }
}

/* the newest write attempt found the user's value (TOOK_OVER, lock 44): *u = that value */
static int iv_took_over(int32_t * u)
{
    struct r6pw_rec e;
    memset(&e, 0, sizeof(e));
    int n = _r6pw_ring(0, &e);
    if (n) _r6pw_ring(n - 1, &e);
    *u = e.rep;
    return n && e.res == R6PW_R_TOOK_OVER;
}

/* at a due slot with Canon ready: arm (first event: marker file), the members, the restore. Every stop goes through
 * iv_end -> _r6pw_series_end (restore now / pending / none, spec 4.6) */
static void iv_event(int now)
{
    if (!bk.armed)
    {
        int l = _r6pw_arm(R6PW_CTX_BKT, bk.base);
        if (l) { iv_end(R6IV_STOP_CAMERA, R6IV_CAM_TVW, l); return; }
        bk.armed = 1;
    }
    _r6pw_ev_begin(++bk.ev);                 /* ML menu blocked from here */
    int cur = bk.base;
    bk.t1 = now;
    for (int k = 0; k < bk.n; k++)
    {
        int want = bk.list[k];
        bk.k = k;
        bk.want = want;
        bk.wr = -1;
        if (want != cur)
        {
            /* wait rows, no call; a hard row or the timeout stops. Review round 1 (F1 / L1): lock 44 for STEP (Canon's
             * Tv still ML's last value) in the wait and the write; the user's value found there (TOOK_OVER) stops
             * TVUSER and nothing is restored (plan 2.5 "your dial turn wins") */
            int32_t u = 0;
            int l = _r6pw_wait_ready(want, R6PW_K_STEP);
            if (l && iv_took_over(&u)) { iv_end(R6IV_STOP_TVUSER, 0, u); return; }
            if (l) { iv_end(R6IV_STOP_TVW, 0, R6PW_R_REFUSED << 8 | l); return; }
            int r = _r6pw_write(want, R6PW_K_STEP, k, bk.ev);
            bk.wr = (int) _r6pw_ring_n() - 1;
            if (r == R6PW_R_TOOK_OVER && iv_took_over(&u)) { iv_end(R6IV_STOP_TVUSER, 0, u); return; }
            if (r != R6PW_R_OK) { iv_end(R6IV_STOP_TVW, 0, r << 8 | _r6pw_state()->last_lock); return; }
            cur = want;
            if (iv_settle()) return;
        }
        if (iv_member_ready(want)) return;
        iv_shoot(&iv_s);                     /* unchanged M8.x picture; phase B checks Tv == want */
        if (!st.running) return;
    }
    bk.k = bk.n;                             /* restoring */
    int r = _r6pw_write(bk.base, R6PW_K_RESTORE, 0xFF, bk.ev);   /* waits, then the one call (lock 44); ends the event */
    iv_ev_end_hold();
    if (r == R6PW_R_OK || r == R6PW_R_DONE)
    {
        st.n_done++;
        if (st.n_done >= st.n_target) { iv_end(R6IV_STOP_DONE, 0, 0); return; }
        iv_due = bk.t1 + st.interval_s * 1000;
        return;
    }
    int32_t u = 0;
    if (r == R6PW_R_TOOK_OVER && iv_took_over(&u)) { iv_end(R6IV_STOP_TVUSER, 0, u); return; }
    iv_end(R6IV_STOP_TVW, 0, r << 8 | _r6pw_state()->last_lock);
}

/* between events (spec 4.4, after the M8.x stops of iv_tick): the mode of a C1..C3 dial, a picture ML did not take,
 * the hard rows of the write path, then Tv by 3.6a. 1 = stopped; bk.t_mis != 0: no event start this tick */
static int iv_bkt_between(int now)
{
    if (shooting_mode_custom != SHOOTMODE_M) { iv_end(R6IV_STOP_CAMERA, R6IV_CAM_MODE, shooting_mode_custom); return 1; }
    int dcnt = total_shots_count - st.cnt_last, dfn = iv_file_number() - st.fn_last;
    if (dcnt || dfn) { iv_end(R6IV_STOP_FOREIGN, 0, dcnt ? dcnt : dfn); return 1; }
    int lock = 0;
    if (_r6pw_locks(R6PW_WANT_NONE, R6PW_CLASS_HARD, &lock)) { iv_end(R6IV_STOP_CAMERA, R6IV_CAM_TVW, lock); return 1; }
    int16_t v = 0, u = 0;
    if (_r6pw_tv_stable(&v) && v == bk.base) { bk.t_mis = 0; return 0; }
    if (_r6pw_user_evidence(bk.base, &u)) { iv_end(R6IV_STOP_TVUSER, 0, u); return 1; }
    if (!bk.t_mis) bk.t_mis = now | 1;       /* unsettled, or not the base without evidence: wait (no single-read stop) */
    else if (now - bk.t_mis >= R6IV_BKT_WAIT_MS) { iv_end(R6IV_STOP_TVW, 0, R6PW_R_REFUSED << 8 | R6PW_L_VIEWS); return 1; }
    return 0;
}

/* command 5 "Tv back to ...": >= R6IV_TEST_MS after the command and with the ML menu closed >= R6PW_HOLD_MS */
static void iv_back_tick(int now)
{
    int t0 = iv_back_t & ~1;             /* taken at (now | 1): on that tick now can be iv_back_t - 1 (QEMU m102 K2) */
    if (now - t0 > R6IV_BACK_MAX_MS || now < t0) { iv_back_t = 0; iv_back_why = "ML menu not closed"; return; }
    if (now - t0 < R6IV_TEST_MS || gui_menu_shown() || now - iv_t_menu < R6PW_HOLD_MS) return;
    iv_back_t = 0;
    _r6pw_back();
    iv_ev_end_hold();                        /* a Trash press blocked in its window (review round 1, safety L5) */
}

#ifdef CONFIG_R6_MOTION
/* M9.3: one tick of a motion run (r6_iv_task, every R6MD_TICK_MS): the stops, then the detector; a trigger takes one
 * picture through iv_shoot (Release, Canon ready on this tick's snapshot), else it is dropped (never caught up) */
static void iv_tick_md(int now)
{
    if (ml_shutdown_requested) { iv_end(R6IV_STOP_SHUTDOWN, 0, 0); return; }
    if (iv_gui_seen)
    {
        iv_end((iv_gui_seen == R6IV_GUI_SAVER || iv_gui_seen == R6IV_GUI_ECO) ? R6IV_STOP_PWRSAVE : R6IV_STOP_GUI, 0, iv_gui_seen);
        return;
    }
    int lim = r6_md_maxdur_ms();
    if (lim && now - (int) st.t_start >= lim) { iv_end(R6IV_STOP_TIME, 0, lim); return; }
    int shoot = st.mode == R6IV_MODE_MD_SHOOT, k = R6IV_CANON_OK, hard = 0;
    if (shoot)
    {
        /* a picture ML did not take: every tick (plan correction) */
        int dcnt = total_shots_count - st.cnt_last, dfn = iv_file_number() - st.fn_last;
        if (dcnt || dfn) { iv_end(R6IV_STOP_FOREIGN, 0, dcnt ? dcnt : dfn); return; }
        if (shooting_mode != iv_mode0) { iv_end(R6IV_STOP_CAMERA, R6IV_CAM_MODE, shooting_mode); return; }
        int cam = r6_iv_cam_check();
        if (cam != R6IV_CAM_OK) { iv_end(R6IV_STOP_CAMERA, cam, 0); return; }
        k = iv_canon_check_snap(&hard, 1, &iv_s, &iv_s2);
        if (hard) { iv_end(R6IV_STOP_CANON, k, 0); return; }
    }
    struct r6_md_in in = { now, iv_lv_ok(now),
        !gui_menu_shown() && now - iv_t_menu >= R6IV_MENU_HOLD_MS && now - iv_t_menu_wanted >= R6IV_MENU_WANT_MS };
    if (r6_md_step(&in) != R6MD_TRIGGER) return;
    /* review F2 (defence in depth): only a run started as Release presses, by the task's mode and the detector's copy */
    if (!shoot || !r6_md_stats()->release) { r6_md_after(get_ms_clock(), R6MD_R_VIEW); return; }
    if (k != R6IV_CANON_OK) { r6_md_after(get_ms_clock(), R6MD_R_DROP + k); return; }   /* never caught up */
    iv_due = now;                                     /* slot_late ~0 */
    iv_shoot(&iv_s);                                  /* all M8.x checks; may end the run (DONE / any stop) */
    if (st.running) r6_md_after(get_ms_clock(), R6MD_R_SHOT);
}
#endif

static void iv_tick(int now, int dt)
{
    int c = iv_cmd;
    if (c)
    {
        iv_cmd = 0;
        if ((c == 1 || c == 3) && !st.running) iv_begin(now, c == 3);
#ifdef CONFIG_R6_MOTION
        else if (c == 4 && !st.running) iv_begin_md(now, iv_md_shoot_req);    /* M9.3 */
#endif
        else if (c == 2 && st.running) iv_end(R6IV_STOP_USER, 0, 0);
        else if (c == 5 && !st.running) { iv_back_t = now | 1; iv_back_why = 0; }   /* M10.2: "Tv back to ..." */
    }
    if (iv_back_t && !st.running) iv_back_tick(now);
    if (!st.running) return;
#ifdef CONFIG_R6_MOTION
    if (st.mode != R6IV_MODE_INTERVAL) { iv_tick_md(now); return; }
#endif

    /* stop conditions: every tick, also between slots */
    if (ml_shutdown_requested) { iv_end(R6IV_STOP_SHUTDOWN, 0, 0); return; }
    /* review F2: M -> B (or back) between two pictures would take the next one in the other mode, e.g. a bulb that the
     * Start check (interval >= exposure + 2 s) never saw: stop, val = the new mode */
    if (shooting_mode != iv_mode0) { iv_end(R6IV_STOP_CAMERA, R6IV_CAM_MODE, shooting_mode); return; }
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
    if (bk.mode && iv_bkt_between(now)) return;   /* M10.2 */
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
    if (bk.mode)                             /* M10.2: an event starts only with Tv stably the base (iv_bkt_between) */
    {
        if (!bk.t_mis) iv_event(now);
        return;
    }
    iv_shoot(&iv_s);
}

/* never ends; the only task that presses (iv_press checks the task name) */
static void r6_iv_task(void * unused)
{
    int last = get_ms_clock();
    for (;;)
    {
        msleep(r6_iv_tick_ms());                          /* M9.3: 20 ms during a motion run */
        int now = get_ms_clock();
        int dt = now - last;
        last = now;
        if (dt < 0 || dt > 1000) dt = 50;     /* clock glitch (SMP) */
        if (iv_bulb_ovr && !iv_bulb_canon_running()) iv_bulb_ovr = 0;   /* M9.2: Canon's bulb ended after an overrun stop */
        iv_sample(now);
        _r6pw_tick(now);                      /* M10.2: fixed order (spec 3.10): sample, the write path's tick, the series */
        iv_ev_end_hold();                     /* M10.2 review round 1 (safety L5): a Trash press blocked in a pending restore's window */
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

/* M10.2: read-only helpers for r6_propw.c (after r6_iv_task: iv_sample and the run state) */
void _r6iv_sample(int now) { iv_sample(now); }
int _r6iv_bkt_armable(void) { return st.running && st.mode == R6IV_MODE_INTERVAL && iv_mode0 == SHOOTMODE_M && bk.mode == 1; }

/* M10.2 provenance (spec 3.10, L15 b): Start, Test, "Tv writes" and "Tv back to ..." act only when ML's menu key
 * handling calls them, never from a module or Lua's menu API */
static int __attribute__((noinline)) iv_menu_caller(void) { return streq(get_current_task_name(), R6IV_MENU_TASK); }

/* ---- menu ---- */
#ifdef CONFIG_R6_MOTION
/* M9.3: R6 Motion's Start / Stop (menu task); every Start while the other series runs is refused */
int r6_iv_md_start(void)
{
    int shoot = r6_md_shoot_setting();
    int c = (st.running || iv_cmd) ? R6IV_CAM_BUSY : iv_md_check(shoot);
    if (c != R6IV_CAM_OK)
    {
        iv_md_refused = c;
        st.refused++;
        return c;
    }
    iv_md_refused = R6IV_CAM_OK;
    iv_md_shoot_req = shoot;
    iv_cmd = 4;
    return 0;
}

void r6_iv_md_stop(void)
{
    if (r6_iv_md_running()) iv_cmd = 2;
}
#endif

static MENU_SELECT_FUNC(iv_start_stop)
{
    if (iv_cmd == 2) return;                 /* Stop pressed, not taken over by the task yet */
    if (r6_iv_md_running()) { iv_refused_cam = R6IV_CAM_BUSY; st.refused++; return; }   /* M9.3: never stops a motion run */
    if (st.running || iv_cmd == 1 || iv_cmd == 3) { iv_cmd = 2; return; }
    int c = iv_menu_caller() ? r6_iv_cam_check() : R6IV_CAM_CALLER;   /* M10.2: only ML's menu starts */
    if (c == R6IV_CAM_OK && iv_bkt_mode()) c = iv_bkt_check(0, 0);     /* M10.2: spec 4.3 */
    else if (c == R6IV_CAM_OK && iv_interval_fit(iv_interval_s(), 0, 0) == 2) c = R6IV_CAM_INTERVAL;   /* M8.2: < Tv + 2 s */
    if (c != R6IV_CAM_OK)
    {
        iv_refused_cam = c;
        st.refused++;
        return;
    }
    iv_refused_cam = R6IV_CAM_OK;
    iv_cmd = 1;
}

/* M10.2: the refusal text of Start / Test */
static void iv_refused_text(char * t, int n, int c)
{
    char a[12];
    if (c == R6IV_CAM_TVW) _r6pw_lock_text(iv_refused_pw, t, n);
    else if (c == R6IV_CAM_BKT_LIST)
        snprintf(t, n, "bracket member %s %s (see Bracket Tv)", _r6pw_tvs(a, iv_refused_tv), iv_bkt_why[COERCE(iv_refused_bk >> 8, 0, 5)]);
    else if (c == R6IV_CAM_INTERVAL && iv_bkt_mode()) snprintf(t, n, "interval shorter than one bracket (see Interval)");
    else snprintf(t, n, "%s (see %s)", r6_iv_cam_name(c), c == R6IV_CAM_INTERVAL ? "Interval" : "Camera");
}

/* running: "picture <s>s" (Tv >= 1 s, since SW1 ON) / "pressing" / "waiting: close ML menu" (the slot is due,
 * the ML menu holds it) / time to the slot */
static void iv_run_status(char * buf, int n, const char * next)
{
    int now = get_ms_clock();
    int due = (iv_due - now + 999) / 1000;
    int be = iv_bulb_end_t;
    if (_r6pw_ev_on() && bk.k >= bk.n) snprintf(buf, n, "restoring Tv");                                  /* M10.2 */
    else if (_r6pw_ev_on()) snprintf(buf, n, "bracket %d/%d", bk.k + 1, bk.n);
    else if (iv_in_pic && iv_bulb && be) snprintf(buf, n, "Canon busy %ds", MAX(now - be, 0) / 1000);           /* M9.3: NR / saving */
    else if (iv_in_pic && iv_bulb) snprintf(buf, n, "bulb %d/%ds", MAX(now - iv_t1, 0) / 1000, iv_bulb_s); /* M9.2 */
    else if (iv_in_pic && iv_long) snprintf(buf, n, "picture %ds", MAX(now - iv_t1, 0) / 1000);
    else if (iv_in_pic) snprintf(buf, n, "pressing");
    else if (iv_wait_menu && due <= 0) snprintf(buf, n, "waiting: close ML menu");
    else snprintf(buf, n, "%s %ds", next, MAX(due, 0));
}

static MENU_UPDATE_FUNC(iv_start_update)
{
    if (iv_cmd == 2)
    {
        MENU_SET_NAME("Stop");
        MENU_SET_VALUE("%s", iv_in_pic || _r6pw_ev_on() ? "stopping after this picture" : "stopping");
        return;
    }
    if (r6_iv_md_running())                  /* M9.3 */
    {
        MENU_SET_NAME("Start");
        MENU_SET_VALUE("R6 Motion running");
        MENU_SET_ICON(MNI_ACTION, 0);
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
        char t[90];
        iv_refused_text(t, sizeof(t), iv_refused_cam);   /* M10.2: Tv write locks, bracket list */
        MENU_SET_VALUE("refused");
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Not started: %s.", t);
    }
}

/* the warning of "Last series" (also "Test: 1 picture"). Bit tests on 1 << stop, not a switch or an
 * if-chain on the value: gcc makes a tbb jump table of those, whose bytes read as high "constants" in
 * canon_gate */
#define IV_B(x) (1u << (x))
static const char * iv_mode_name(int m);
void r6_iv_stop_warn(struct menu_entry * entry, struct menu_display_info * info)   /* M9.3: also R6 Motion's Last run */
{
    int s = st.stop;
    uint32_t b = (s >= 0 && s < R6IV_NSTOPS) ? IV_B(s) : 0;
    uint32_t bc = (st.stop_cam > 0 && st.stop_cam < 32) ? IV_B(st.stop_cam) : 0;    /* review F4 */
    const uint32_t snap = IV_B(R6IV_STOP_RET) | IV_B(R6IV_STOP_EXTRA) | IV_B(R6IV_STOP_JOB) | IV_B(R6IV_STOP_SW1)
        | IV_B(R6IV_STOP_SW2) | IV_B(R6IV_STOP_STUCK) | IV_B(R6IV_STOP_LATE) | IV_B(R6IV_STOP_SEQ) | IV_B(R6IV_STOP_CCSTATE);
    /* review F4: Canon's bulb not idle gets the conservative hint (C2), a mode change its own line (F2) */
    if ((b & IV_B(R6IV_STOP_CAMERA)) && (bc & IV_B(R6IV_CAM_BULB_BUSY)))
        MENU_SET_WARNING(MENU_WARN_INFO, "Stopped: Canon bulb not idle. Switch to B and half-press once; if it stays, switch off and on.");
    else if ((b & IV_B(R6IV_STOP_CAMERA)) && (bc & IV_B(R6IV_CAM_MODE)))
        MENU_SET_WARNING(MENU_WARN_INFO, "Stopped: mode changed to %s (%d); a series keeps its mode (M or B).",
            iv_mode_name(st.stop_val), (int) st.stop_val);
    else if (b & IV_B(R6IV_STOP_CAMERA))
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
    /* M9.2 */
    int v = st.stop_val > 0 ? (int) st.stop_val : 0;
    if (b & IV_B(R6IV_STOP_BULB_NOTIMER))
        MENU_SET_WARNING(MENU_WARN_INFO, "Stopped: no Canon bulb timer at the release (%d ms). Log snapshot, please.", v);
    if (b & IV_B(R6IV_STOP_BULB_EARLY))
        MENU_SET_WARNING(MENU_WARN_INFO, "Stopped: bulb ended after %d.%d s (button or Canon). Log snapshot, please.",
            v / 1000, v % 1000 / 100);
    if ((b & IV_B(R6IV_STOP_BULB_OVERRUN)) && iv_bulb_ovr)
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Bulb did not end: press the shutter fully once, or switch off.");
    if ((b & IV_B(R6IV_STOP_BULB_OVERRUN)) && !iv_bulb_ovr && (st.bulb_warn & IV_BW_NOEND))     /* review F4 (c) */
        MENU_SET_WARNING(MENU_WARN_INFO, "Stopped: no Canon bulb timer armed at the 3 s check (0x%x). Log snapshot, please.",
            (int) st.bulb_warn);
    if ((b & IV_B(R6IV_STOP_BULB_OVERRUN)) && !iv_bulb_ovr && !(st.bulb_warn & IV_BW_NOEND))
        MENU_SET_WARNING(MENU_WARN_INFO, "Stopped: bulb did not end in time (%d.%d s). Log snapshot, please.",
            v / 1000, v % 1000 / 100);
    if (b & IV_B(R6IV_STOP_BULB_NOT12))
        MENU_SET_WARNING(MENU_WARN_INFO, "Stopped: Canon did not start a bulb (state %d). Log snapshot, please.", v);
    if (b & IV_B(R6IV_STOP_TIME))                         /* M9.3 */
        MENU_SET_WARNING(MENU_WARN_INFO, "Stopped: time limit reached.");
    if ((b & IV_B(R6IV_STOP_CAMERA)) && st.stop_cam == R6IV_CAM_TVW)                    /* M10.2 */
    {
        char t[80];
        _r6pw_lock_text((int) st.stop_val, t, sizeof(t));
        MENU_SET_WARNING(MENU_WARN_INFO, "Stopped: %s.", t);
    }
    if (b & IV_B(R6IV_STOP_TVW))
        MENU_SET_WARNING(MENU_WARN_INFO, "Stopped: Tv write: %s (%s). Log snapshot, please.",
            _r6pw_res_name((int) (st.stop_val >> 8) & 0xFF), _r6pw_lock_name((int) st.stop_val & 0xFF));
    if (b & IV_B(R6IV_STOP_TVUSER))
    {
        char a[12];
        MENU_SET_WARNING(MENU_WARN_INFO, "Stopped: Tv changed by user to %s; ML did not restore it.", _r6pw_tvs(a, (int) st.stop_val));
    }
    if ((b & IV_B(R6IV_STOP_DONE)) && st.bulb_warn)
        MENU_SET_WARNING(MENU_WARN_ADVICE, "Canon bulb timer check 0x%x. Log snapshot, please.", (int) st.bulb_warn);
}

static MENU_SELECT_FUNC(iv_test_select)
{
    if (r6_iv_md_running()) { iv_refused_cam = R6IV_CAM_BUSY; st.refused++; return; }   /* M9.3 */
    if (st.running || iv_cmd != 0) return;
    int c = iv_menu_caller() ? r6_iv_cam_check() : R6IV_CAM_CALLER;   /* M10.2: only ML's menu starts */
    if (c == R6IV_CAM_OK && iv_bkt_mode()) c = iv_bkt_check(1, 0);     /* M10.2: spec 4.3 */
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
    if (iv_bkt_mode()) MENU_SET_NAME("Test: 1 bracket");                /* M10.2 */
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
    if (st.stop == R6IV_STOP_DONE && bk.mode) MENU_SET_VALUE("ok, %d pictures", bk.n);   /* M10.2 */
    else if (st.stop == R6IV_STOP_DONE) MENU_SET_VALUE("ok, 1 picture");
    else MENU_SET_VALUE("%s", r6_iv_stop_name(st.stop));
    r6_iv_stop_warn(entry, info);                           /* M9.2: also for done (bulb timer advice) */
    if (st.shots > iv_test_shots0)                          /* only a picture of this test */
    {
        const struct r6_iv_shot * e = &st.ring[(st.shots - 1) % R6IV_RING];
        uint32_t bms = e->bulb_ms ? e->bulb_ms : e->d30_ms;
        if (e->bulb)                                           /* M9.2: Canon's measured bulb time */
            MENU_SET_RINFO("bulb %d.%ds", (int) (bms / 1000), (int) (bms % 1000 / 100));
        else if (e->exp_ms && e->tv <= 0 && e->tv != -32768)   /* long Tv (1 s or more) */
            MENU_SET_RINFO("exp %d.%ds", (int) (e->exp_ms / 1000), (int) (e->exp_ms % 1000 / 100));
        else MENU_SET_RINFO("hold %dms %d", (int) e->off_ms, (int) e->off_why);
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

/* M9.2: the Camera line in B. Refusals chosen with bit tests (no switch / if-chain on the value: canon_gate tbb noise) */
static void iv_cam_update_bulb(struct menu_entry * entry, struct menu_display_info * info)
{
    struct r6_iv_cc s;
    r6_iv_cc_read(&s);
    const char * dr = drive_mode == DRIVE_SINGLE ? "single" : "drive?";
    const char * fo = (af_mode & 0xF) == AF_MODE_MANUAL_FOCUS ? "MF" : "AF";
    int t = r6_iv_bulb_t(&s);
    int h = (s.bulbw >> 8) & 0xFF, m = (s.bulbw >> 16) & 0xFF, sec = s.bulbw >> 24;
    if (!(s.valid & R6IV_V_P)) MENU_SET_VALUE("B, %s, %s, bulb ?", dr, fo);
    else if (t < 0) MENU_SET_VALUE("B, %s, %s, timer off", dr, fo);
    else MENU_SET_VALUE("B, %s, %s, bulb %d:%02d:%02d", dr, fo, h, m, sec);
    if (s.valid & R6IV_V_P) MENU_SET_RINFO("CC: %d,%d,%d nr %d", (int) s.p_ae, (int) s.p_drive, (int) s.af, (int) (s.p_lenr & 0xFF));
    else MENU_SET_RINFO("CC: -");
    int c = r6_iv_cam_check();
    if (c == R6IV_CAM_OK)
    {
        MENU_SET_WARNING(MENU_WARN_INFO, "Bulb %d:%02d:%02d (x2 with NR). Abort: one full press, not in its last seconds.", h, m, sec);
        return;
    }
    uint32_t bc = (c > 0 && c < 32) ? IV_B(c) : 0;   /* M10.2: 37 codes, bit tests below 32 */
    const uint32_t own = IV_B(R6IV_CAM_TVB) | IV_B(R6IV_CAM_BULB_OFF) | IV_B(R6IV_CAM_BULB_TIME) | IV_B(R6IV_CAM_BULB_BUSY)
        | IV_B(R6IV_CAM_DRIVE) | IV_B(R6IV_CAM_FOCUS) | IV_B(R6IV_CAM_CARD) | IV_B(R6IV_CAM_AEB);
    int seen = 0, len = 0, tvp = r6_iv_tv_prop(&seen, &len);
    if (bc & IV_B(R6IV_CAM_TVB))
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Tv %d (0x%x, %s, len %d): B expects Bulb (0xDF00).",
            tvp, tvp & 0xFFFF, seen ? "seen" : "not seen", len);
    if (bc & IV_B(R6IV_CAM_BULB_OFF))
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Canon bulb timer off: enable it, 0:00:31 .. 0:30:00.");
    if (bc & IV_B(R6IV_CAM_BULB_TIME))
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Canon bulb timer %d:%02d:%02d: set 0:00:31 .. 0:30:00.", h, m, sec);
    if (bc & IV_B(R6IV_CAM_BULB_BUSY))
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Canon bulb not idle (%d %d %d %d %d): half-press once; if it stays, switch off and on.",
            (int) s.d30, (int) s.ba0, (int) s.ta, (int) s.tb, (int) s.sec);
    if (bc & IV_B(R6IV_CAM_DRIVE))
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Drive 0x%x: set single shot (0 expected).", drive_mode);
    if (bc & IV_B(R6IV_CAM_FOCUS))
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Focus 0x%x: set MF (low nibble 3 expected).", af_mode & 0xFFFF);
    if (bc & IV_B(R6IV_CAM_CARD))
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Shots left %d: card missing or full.", avail_shot);
    if (bc & IV_B(R6IV_CAM_AEB))
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "AEB 0x%x: switch exposure bracketing off.", (int) iv_aeb);
    if (!(bc & own))
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "%s.", r6_iv_cam_name(c));
}

static MENU_UPDATE_FUNC(iv_cam_update)
{
    if (shooting_mode == SHOOTMODE_BULB) { iv_cam_update_bulb(entry, info); return; }      /* M9.2 */
    int seen = 0, len = 0, ev = 0, tv = 0;
    int tvp = r6_iv_tv_prop(&seen, &len);
    if (r6_iv_tv_ev768(&ev) && ev >= R6IV_TV_EV768_MIN && ev <= R6IV_TV_EV768_MAX) tv = iv_ev768_to_raw(ev);   /* 30 s .. 1/16000 */
    MENU_SET_VALUE("%s, %s, %s, %s", iv_mode_name(shooting_mode),
        drive_mode == DRIVE_SINGLE ? "single" : "drive?",
        (af_mode & 0xF) == AF_MODE_MANUAL_FOCUS ? "MF" : "AF",
        (tv >= 15 && tv <= 168) ? lens_format_shutter(tv) : "Tv?");
    struct r6_iv_cc s;
    r6_iv_cc_read(&s);
    if (s.valid & R6IV_V_P) MENU_SET_RINFO("CC: %d,%d,%d nr %d", (int) s.p_ae, (int) s.p_drive, (int) s.af, (int) (s.p_lenr & 0xFF));
    else MENU_SET_RINFO("CC: -");
    int c = r6_iv_cam_check();
    if (c == R6IV_CAM_OK)
    {
        int evb = 0;
        if (iv_tv_budget_ev(&s, &evb) && evb <= 0)
            MENU_SET_WARNING(MENU_WARN_INFO, "Tv: up to %d s per picture, x2 with long exposure NR.", (iv_tv_ub_ms(evb) + 999) / 1000);
        return;
    }
    /* review F4 (a): skeptic C1 refuses M while Canon's bulb is not idle; a bit test, not a new case (canon_gate tbb) */
    uint32_t bc = (c > 0 && c < 32) ? IV_B(c) : 0;   /* M10.2: 37 codes, bit tests below 32 */
    if (bc & IV_B(R6IV_CAM_BULB_BUSY))
    {
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Canon bulb not idle (%d %d %d %d %d): switch to B and half-press once; if it stays, switch off and on.",
            (int) s.d30, (int) s.ba0, (int) s.ta, (int) s.tb, (int) s.sec);
        return;
    }
    switch (c)
    {
        case R6IV_CAM_MODE:
            MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Mode %d: set M (Tv up to 30 s) or B (Canon bulb timer).", shooting_mode); break;
        case R6IV_CAM_TV:
            MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Tv %d (0x%x, %s, len %d): set 30 s or shorter (not Bulb), turn the main dial.",
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
    const char * md = st.mode != R6IV_MODE_INTERVAL ? " (motion)" : "";     /* M9.3 */
    if (st.skipped)
        MENU_SET_VALUE("%d/%d, %s, %d skipped%s", st.n_done, st.n_target, st.running ? "running" : r6_iv_stop_name(st.stop), st.skipped, md);
    else
        MENU_SET_VALUE("%d/%d, %s%s", st.n_done, st.n_target, st.running ? "running" : r6_iv_stop_name(st.stop), md);
    if (st.shots)
    {
        const struct r6_iv_shot * e = &st.ring[(st.shots - 1) % R6IV_RING];
        MENU_SET_RINFO("%dms r%d", (int) e->dur, (int) e->ret);
    }
    if (st.running) return;
    r6_iv_stop_warn(entry, info);
}

/* M8.2: the interval against the Tv (the longer of PROP and the CC copy) */
static MENU_UPDATE_FUNC(iv_interval_update)
{
    int need = 0, need_nr = 0;
    int fit = iv_interval_fit(iv_interval_s(), &need, &need_nr);
    if (fit == 2)
    {
        if (shooting_mode == SHOOTMODE_BULB)                /* M9.2 */
            MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Shorter than bulb + 2 s (%d s): Start is refused.", (need + 999) / 1000);
        else if (iv_bkt_mode())                             /* M10.2 */
            MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Shorter than one bracket (%d s): Start is refused.", (need + 999) / 1000);
        else
            MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Shorter than Tv + 2 s (%d s): Start is refused.", (need + 999) / 1000);
    }
    else if (fit == 1)
        MENU_SET_WARNING(MENU_WARN_ADVICE, "Long exposure NR: about %d s per picture, slots will be skipped.", (need_nr + 999) / 1000);
}

/* ---- M10.2 menu: Bracket, Tv writes, Tv back to ... ---- */
static MENU_UPDATE_FUNC(iv_count_update)
{
    if (iv_bkt_mode()) MENU_SET_NAME("Brackets");
}

/* "Bracket Tv": base: shortest .. longest member, the writes per event, the first member that does not fit */
static MENU_UPDATE_FUNC(iv_bkt_update)
{
    struct r6pw_rng r;
    char a[12], b2[12], c2[12];
    if (!_r6pw_ranges(&r)) { MENU_SET_VALUE("-"); return; }
    int16_t list[R6IV_BKT_MAXN];
    int n = iv_bkt_build(r.tv, list), lo = list[0], hi = list[0], need = 0, need_nr = 0, w = 0, k = 0;
    for (int i = 1; i < n; i++) { lo = MIN(lo, list[i]); hi = MAX(hi, list[i]); }
    MENU_SET_VALUE("%s: %s .. %s", _r6pw_tvs(a, r.tv), _r6pw_tvs(b2, hi), _r6pw_tvs(c2, lo));
    iv_bkt_budget(list, n, r.tv, &need, &need_nr, &w);
    MENU_SET_RINFO("%d writes", w);
    int why = iv_bkt_list_check(&r, list, n, &k);
    if (why) MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Member %s %s: Start / Test are refused (never clamped).",
        _r6pw_tvs(a, list[k]), iv_bkt_why[why]);
    else if (!iv_bkt_mode()) MENU_SET_WARNING(MENU_WARN_INFO, "Mode Single: no bracket, no Tv write.");
}

static MENU_SELECT_FUNC(iv_pw_toggle)
{
    if (iv_menu_caller()) _r6pw_toggle();      /* only flips the saved switch; never a write by itself */
}

static MENU_UPDATE_FUNC(iv_pw_update)
{
    const struct r6pw_state * p = _r6pw_state();
    const char * m = _r6pw_msg();
    MENU_SET_ICON(_r6pw_on() ? MNI_ON : MNI_OFF, 0);
    if (!_r6pw_on()) MENU_SET_VALUE("OFF");
    else if (p->open) MENU_SET_VALUE("ON, write open");
    else if (p->dead) MENU_SET_VALUE("ON, off until restart");
    else if (p->lim) MENU_SET_VALUE("ON, only the Tv restore");
    else if (p->pend) MENU_SET_VALUE("ON, restore pending %ds", MAX(get_ms_clock() - (int) p->pend_t0, 0) / 1000);
    else if (p->offer) MENU_SET_VALUE("ON, Tv back offered");
    else if (p->marker == 2 || p->marker == 3) MENU_SET_VALUE("ON, marker kept");
    else MENU_SET_VALUE("ON");
    if (p->open) MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Tv write open (%s): no ML pictures until restart.", _r6pw_res_name(p->dead_why));
    else if (p->dead) MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Writes off until restart (%s). %s", _r6pw_res_name(p->dead_why), m);
    else if (m[0]) MENU_SET_WARNING(MENU_WARN_INFO, "%s", m);
}

static MENU_SELECT_FUNC(iv_back_select)
{
    const struct r6pw_state * p = _r6pw_state();
    const char * why = !iv_menu_caller() ? "not from the ML menu" : !p->offer ? "nothing to put back"
        : !_r6pw_on() ? "Tv writes OFF" : p->dead ? "writes off until restart" : p->lim ? "only the Tv restore"
        : p->open ? "write open"
        : (p->landed_ae && !p->landed_ok && p->landed_ae2 && !p->landed_ok2) ? "ML's Tv in 2 modes: set them first"
        : shooting_mode != SHOOTMODE_M ? "set M first" : p->armed ? "busy"
        : (st.running || iv_cmd) ? "series running" : 0;   /* M10.2 review round 2 (S2-1): two open landed records */
    iv_back_why = why;
    if (!why) iv_cmd = 5;
}

static MENU_UPDATE_FUNC(iv_back_update)
{
    const struct r6pw_state * p = _r6pw_state();
    char a[12], b2[12];
    if (p->offer) MENU_SET_VALUE("%s (now %s)", _r6pw_tvs(a, p->base), _r6pw_tvs(b2, p->boot_rec));
    else MENU_SET_VALUE("-");
    if (iv_back_t) MENU_SET_RINFO("in 3 s");
    else if (iv_back_why) MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Tv back refused: %s.", iv_back_why);
    else if (p->back_res == R6PW_R_OK || p->back_res == R6PW_R_DONE) MENU_SET_RINFO("Tv back: ok");
    else if (p->back_res) MENU_SET_WARNING(MENU_WARN_INFO, "Tv back: %s (%s).", _r6pw_res_name(p->back_res), _r6pw_lock_name(p->back_lock));
}

static MENU_UPDATE_FUNC(iv_apo_update)
{
    MENU_SET_VALUE("%d", auto_power_off_time);
}

static struct menu_entry iv_menu[] = {
    {
        .name   = "R6 Interval",
        .select = menu_open_submenu,
        .help   = "M9.3: pictures at an interval (shutter callbacks, no settings written).",
        .help2  = "M: Tv up to 30 s. B: Canon timer 31 s .. 30 min (ML menu waits for it). Single, MF, max 50.",
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
                .update = iv_interval_update,
                .help   = "Time from one picture to the next, at least Tv (B: bulb) + 2 s. A late slot is skipped.",
                .help2  = "Canon's Screen off must be longer than this (screen off ends a series).",
            },
            {
                .name   = "Pictures",
                .priv   = &r6_iv_count,
                .min    = 1,
                .max    = R6IV_MAX_PICS,
                .update = iv_count_update,
                .help   = "Pictures per series (1..50). Bracket: brackets per series (pictures x shots <= 50).",
            },
            {
                .name   = "Start delay",
                .priv   = &r6_iv_delay_idx,
                .max    = COUNT(iv_delays) - 1,
                .choices = iv_delay_names,
                .help   = "Time from Start to the first picture.",
            },
            {
                .name   = "Mode",
                .priv   = &r6_iv_mode,
                .max    = 1,
                .choices = CHOICES("Single", "Bracket"),
                .help   = "Single: 1 picture per slot. Bracket (M only): an HDR bracket per slot, Tv written",
                .help2  = "by ML (Tor 1: only Tv, only M, 1/3 stops), your Tv put back after each bracket.",
            },
            {
                .name   = "Bracket shots",
                .priv   = &r6_iv_bkt_n,
                .max    = 3,
                .choices = CHOICES("3", "5", "7", "9"),
                .help   = "Pictures per bracket.",
            },
            {
                .name   = "Bracket step",
                .priv   = &r6_iv_bkt_step,
                .min    = 1,
                .max    = 9,
                .choices = CHOICES("1/3 EV", "2/3 EV", "1 EV", "1 1/3 EV", "1 2/3 EV", "2 EV", "2 1/3 EV", "2 2/3 EV", "3 EV"),
                .help   = "Tv step between the bracket members.",
            },
            {
                .name   = "Bracket order",
                .priv   = &r6_iv_bkt_order,
                .max    = 1,
                .choices = CHOICES("0 - +", "- 0 +"),
                .help   = "0 - +: your Tv first, then shorter / longer. - 0 +: shortest to longest.",
            },
            {
                .name   = "Bracket Tv",
                .update = iv_bkt_update,
                .icon_type = IT_ALWAYS_ON,
                .help   = "Your Tv: shortest .. longest member; right: Tv writes per bracket.",
                .help2  = "Members are never clamped: one outside Canon's range refuses Start / Test.",
            },
            {
                .name   = "Tv writes",
                .select = iv_pw_toggle,
                .update = iv_pw_update,
                .help   = "Only Tv, only M, 1/3 stops, 1/8000..30 s. No flash; WLAN, Bluetooth, USB off.",
                .help2  = "If the camera switches off during a bracket, Tv can stay changed: ML shows it next start.",
            },
            {
                .name   = "Tv back to",
                .select = iv_back_select,
                .update = iv_back_update,
                .icon_type = IT_ACTION,
                .help   = "After a cut-off bracket: puts your Tv back (one write, all checks), 3 s after",
                .help2  = "the ML menu closed. Or turn the main dial yourself.",
            },
            {
                .name   = "Camera",
                .update = iv_cam_update,
                .icon_type = IT_ALWAYS_ON,
                .help   = "How ML reads mode, drive, focus and Tv (B: Canon's bulb timer). Start only if all fit.",
                .help2  = "Right: Canon CC copies (AE mode, drive, AF, long exposure NR). They must match (M8.2).",
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
