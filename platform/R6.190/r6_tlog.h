/*
 * R6.190 M14.0: the Tor 0g read-only log (r6_tlog.c, CONFIG_R6_TLOG; docs/m14_plan.md, spec m14/m140 spec_m140.md Rev. 2,
 * user answers of 2026-10-15): Canon's TempSvc table, its copy, error counters, period and over-temperature status, the
 * EXIF temperature byte and the time-zone struct, read only, every pointer checked; a ring of samples kept by a small task.
 * Used by lvprobe.c (lp_capture: the tp / tz section of ASTnnnnn.LOG). No write, no property, no new Canon call.
 * M14.1 (spec m14/m141 2.1): read-only exports of the newest poll, the newest RTC pair and the time-zone struct for
 * r6_temp.c and r6_aclock.c, and the line printers for their tb.* / ac.* lines of the snapshot.
 */
#ifndef _r6_tlog_h_
#define _r6_tlog_h_

#include <stdint.h>

/* lvprobe.c lp_capture (snapshot capture, read only, no card, no wait): the tp / tz text into p (at most cap - 1 bytes,
 * NUL-terminated if cap >= 2; nothing at all if cap < 2); *trunc = 1 if it was cut; returns its length */
int r6tl_ast(char * p, int cap, int * trunc);

/* M14.1: the snapshot's text buffer and its printers (r6tl_i / r6tl_u: own decimal digits; Canon's vsnprintf prints
 * neither %u nor INT_MIN), for the tb / ac lines that r6tl_ast appends */
struct r6tl_out { char * p; int len, cap, trunc; };
void r6tl_pf(struct r6tl_out * o, const char * fmt, ...);
void r6tl_i(struct r6tl_out * o, const char * pre, int32_t v);
void r6tl_u(struct r6tl_out * o, const char * pre, uint32_t v);
/* t16 (1/16 degC) as degC with one decimal, half away from zero, never "-0.0"; s needs 12 bytes */
const char * r6tl_c(char * s, int t16);

/* M14.1: the newest poll of r6_tlog_task (a seqlock copy, no wait); 1 = consistent and at least one poll */
struct r6tl_now
{
    int16_t t16[8];             /* DRAM, MIMIL, MIMIR, A, TFT, SH, IISA, IISB */
    uint8_t ok, st_ok;          /* bit i: sensor i read; the status read */
    int8_t warn, hyst;          /* r6_temp_poll of this poll: warning, threshold state */
    uint32_t st;                /* Canon's over-temperature status */
    int t, polls;               /* ML's ms clock of the poll, polls since boot */
};
int r6tl_now(struct r6tl_now * o);

/* M14.1: the newest RTC pair: Canon's calendar (ML's struct tm), ML's ms clock after the call, pairs since boot */
struct tm;
int r6tl_rtc(struct tm * tm, int * t_ms, int * n);

/* M14.1: the time-zone struct now (Tor 0g, double read, every pointer checked; only r6_aclock_task calls it) */
enum { R6TL_TZF_PTR = 1, R6TL_TZF_TAB, R6TL_TZF_TORN, R6TL_TZF_NOZONE, R6TL_TZF_RANGE, R6TL_TZF_DST };
struct r6tl_tz
{
    int zone, dst, off, total;  /* Canon's zone index, DST 0 / 1, the zone's offset (min), offset + 60 DST (valid only) */
    int zi, zok, tries;         /* table index (-1 none), double read 1 equal / -1 torn, tries */
    int valid;                  /* the M14.0 rule (tz.sel) */
    int valid_ac;               /* for the clock: valid, read equal, zone in Canon's table */
    int fail;                   /* R6TL_TZF_* when not valid_ac */
};
int r6tl_tz_now(struct r6tl_tz * z);

#endif
