/*
 * R6.190 M14.0: the Tor 0g read-only log (r6_tlog.c, CONFIG_R6_TLOG; docs/m14_plan.md, spec m14/m140 spec_m140.md Rev. 2,
 * user answers of 2026-10-15): Canon's TempSvc table, its copy, error counters, period and over-temperature status, the
 * EXIF temperature byte and the time-zone struct, read only, every pointer checked; a ring of samples kept by a small task.
 * Used by lvprobe.c (lp_capture: the tp / tz section of ASTnnnnn.LOG). No write, no property, no new Canon call.
 */
#ifndef _r6_tlog_h_
#define _r6_tlog_h_

/* lvprobe.c lp_capture (snapshot capture, read only, no card, no wait): the tp / tz text into p (at most cap - 1 bytes,
 * NUL-terminated if cap >= 2; nothing at all if cap < 2); *trunc = 1 if it was cut; returns its length */
int r6tl_ast(char * p, int cap, int * trunc);

#endif
