#ifndef _r6_interval_h_
#define _r6_interval_h_

/* R6.190 M8.0: interval release ("R6 Interval", r6_interval.c) */

#include <stdint.h>

/* why a series stopped (r6_iv_stats.stop); R6IV_STOP_NONE = never ran */
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
    R6IV_STOP_RET,           /* call("Release") returned != 0 (stop_val = return value) */
    R6IV_STOP_SLOW,          /* call("Release") took >= R6IV_SLOW_MS (stop_val = ms) */
    R6IV_STOP_NOPIC,         /* no new picture seen (counter / file number) */
    R6IV_STOP_EXTRA,         /* more than one new picture (stop_val = delta) */
    R6IV_STOP_JOB,           /* Canon's job count did not return to 0 (stop_val = count) */
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
    R6IV_NCAMS
};

/* Canon state checked right before every call("Release"); the first four
 * stop a series, the others only delay the picture */
enum r6_iv_canon {
    R6IV_CANON_OK = 0,
    R6IV_CANON_CC,           /* Canon's remote switch handlers not (yet) registered */
    R6IV_CANON_FAINIT,       /* Canon's factory part not initialised (FA_Initialize) */
    R6IV_CANON_FASEM,        /* a Release is already in flight ([0x9C40] != 0) */
    R6IV_CANON_FALOG,        /* factory log writes a file (mode [0x9B40] / file [0x9B54] != 0) */
    R6IV_CANON_EARLY,        /* less than R6IV_EARLY_MS since boot */
    R6IV_CANON_LV,           /* Canon LV not running (PROP_LV_ACTION != 0), or for less than R6IV_LV_MS */
    R6IV_CANON_REVIEW,       /* gui_state != idle or Canon's quick review / SR mode (GUI mode 0x4033 / 0x4034) */
    R6IV_CANON_JOBS,         /* Canon job count [0x9DC0] != 0 */
    R6IV_CANON_BUFFER,       /* burst_count == 0 (buffer full) */
    R6IV_CANON_UILOCK,       /* icu_uilock & 0xFF */
    R6IV_CANON_CLEANING,     /* sensor cleaning */
    R6IV_CANON_MENU,         /* ML menu open or closed less than R6IV_MENU_HOLD_MS ago */
    R6IV_NCANON
};

#define R6IV_RING 64

struct r6_iv_shot {
    uint32_t t;              /* ms (get_ms_clock) when call("Release") started */
    int32_t  ret;            /* return value of call("Release") */
    uint32_t dur;            /* ms inside call("Release") */
    int32_t  d_cnt;          /* PROP_RELEASE_COUNTER delta */
    int32_t  d_fn;           /* file number delta */
    uint32_t pic_ms;         /* ms from the start until the new picture was seen */
    uint32_t job_ms;         /* ms from the start until Canon's job count was 0 again */
    uint32_t jobs_after;     /* [0x9DC0] after the wait */
    uint32_t js_before;      /* lens_info.job_state before / after */
    uint32_t js_after;
    uint32_t wait_ms;        /* ms the picture waited for a ready camera */
    uint32_t wait_mask;      /* bit r = waited for r6_iv_canon reason r */
};

struct r6_iv_stats {
    int running;
    int n_done;              /* pictures of the current / last series */
    int n_target;
    int interval_s;          /* of the current / last series */
    uint32_t t_start;        /* ms when Start was pressed */
    int stop;                /* enum r6_iv_stop of the last series */
    int stop_cam;            /* enum r6_iv_cam, for R6IV_STOP_CAMERA */
    int stop_canon;          /* enum r6_iv_canon, for R6IV_STOP_CANON / R6IV_STOP_WAIT */
    int32_t stop_val;
    uint32_t t_stop;
    int skipped;             /* interval slots skipped (camera not ready within R6IV_LATE_MS) */
    uint32_t calls;          /* call("Release") since boot */
    uint32_t shots;          /* ring entries written since boot */
    uint32_t refused;        /* Start refused */
    struct r6_iv_shot ring[R6IV_RING];
};

const struct r6_iv_stats * r6_iv_stats(void);
int r6_iv_running(void);
const char * r6_iv_stop_name(int s);
const char * r6_iv_cam_name(int c);
const char * r6_iv_canon_name(int c);
int r6_iv_cam_check(void);             /* enum r6_iv_cam */
int r6_iv_canon_check(int * hard);     /* enum r6_iv_canon; *hard = 1: stops a series; menu display:
                                        * ignores the ML menu and what it causes (LV stopped, gui_state) */
int r6_iv_tv_prop(int * seen);         /* PROP_SHUTTER as Canon delivered it (r6_interval.c's own handler) */

/* Canon RAM read by r6_interval.c (for the log; read-only) */
#define R6IV_RAM_JOBCNT    0x9DC0      /* live job count [0x9D9C+0x24]: CreateJob checks it (cbz 0xE07C9E5E),
                                        * TrashJob decrements it and logs "Job Done" at 0 (0xE07CCCDC..0xE07CCD2A) */
#define R6IV_RAM_FASY_SEM  0x9C40      /* "FaSyReleaseSemaphore" [0x9C20+0x20]: created by FaSyRelease 0xE01FB15E,
                                        * set to 0 after delete (0xE01FB1A0) */
#define R6IV_RAM_FA_LOG    0x9B3C      /* factory log: 1 once FA_Initialize set it up (0xE070F502 -> 0xE070F51A) */
#define R6IV_RAM_FA_LOGMODE 0x9B40     /* [0x9B3C+4] FAPrintf mode (tbb 0xE070F04C): only +-2 writes the file
                                        * B:/AdjLog.txt (0xE070F084 / 0xE070F08E), 0 = RAM buffer only */
#define R6IV_RAM_FA_LOGFILE 0x9B54     /* [0x9B3C+0x18] its open file handle */
#define R6IV_RAM_FA_STORE  0xE708      /* FA TransparentMemory (RAM only): 1 once initialised (0xE027EC10 ->
                                        * 0xE027EC3E, from FA_Initialize before "Release" is registered) */
#define R6IV_RAM_FANEO     0x180D0     /* [0x180A4+0x2C] FA-Neo list head (circular, points to itself when empty;
                                        * set up by FARmt init 0xE026C2A6): FANeoReleaseExecute stores through
                                        * it (0xE026C74E..0xE026C756), 0 = not set up (would write at 4) */
#define R6IV_RAM_FANEO_SEM 0x180BC     /* [0x180A4+0x18] FARmtReleaseSem: FANeoReleaseExecute gives it
                                        * (b.w give_semaphore 0xE026C75E), NULL asserts */
#define R6IV_RAM_CC_SW1    0x108DC     /* [0x108D0+0xC]: CC SW1 handler, 0xE029661B once registered (0xE029803C ->
                                        * setter 0xE06D3186, ID 0x40); before: default 0xE06D32DB */
#define R6IV_RAM_CC_SW2    0x108E0     /* [0x108D0+0x10]: CC SW2 handler, 0xE029665D once registered (ID 0x41) */
#define R6IV_RAM_CC_OBJ    0x10AB8     /* CameraConductor object (set 0xE0295DCA) */
#define R6IV_CC_SW1_FN     0xE029661B
#define R6IV_CC_SW2_FN     0xE029665D

#endif /* _r6_interval_h_ */
