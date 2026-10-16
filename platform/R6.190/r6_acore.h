/*
 * R6.190 M14.1: the computation core of the astro clock (r6_acore.c; spec m14/m141 spec_m141.md Rev. 2, 3). Pure
 * arithmetic on its parameters: sun, moon, galactic centre and Polaris, the night's events, the moon's phases, civil
 * time <-> seconds. float32 with an exact int64 angle reduction, no double, no state outside the caller's buffers.
 * Compiled for the camera (CONFIG_R6_ACLOCK) and unchanged on the PC (-DAC_HOST, host test of the stage).
 */
#ifndef _r6_acore_h_
#define _r6_acore_h_

#include <stdint.h>

#define AC_NONE     INT32_MIN   /* no event / no value */
#define AC_ABORTED  (-1)        /* ac_night / ac_now: the yield callback asked to stop (a newer key) */
#define AC_CHUNK    8           /* evaluations between two yield calls, at most */
#define AC_NS       145         /* samples of the night: [t0, t0 + 24 h] every 10 min */
#define AC_NEV      8           /* crossings kept per body and threshold */

/* sun thresholds (sun_dn / sun_up / sun_state index) */
enum { AC_H_RISE, AC_H_CIVIL, AC_H_NAUT, AC_H_ASTRO, AC_H_MW, AC_NH };
/* the state of a sun threshold and of the Milky Way window (0 = none for the window) */
enum { AC_CROSS = 1, AC_FROM_START, AC_PARTIAL, AC_ALWAYS_UP, AC_ALWAYS_DOWN };
/* the moon in the window */
enum { AC_M_BOTH = 1, AC_M_NORISE, AC_M_NOSET, AC_M_UP, AC_M_DOWN };
/* why there is no Milky Way window */
enum { AC_MW_NONE_SUN = 1, AC_MW_NONE_GC, AC_MW_NONE_DARK };

struct ac_loc  { int32_t lat_e2, lon_e2; };              /* 0.01 deg, N / E positive */
struct ac_crit { int8_t gc_min_deg, sun_max_deg; };      /* 5 / 10 / 15, -18 / -15 / -12 */

struct ac_night
{
    int32_t t0;                                  /* window start (UTC s since J2000.0), the local noon */
    int32_t sun_dn[AC_NH], sun_up[AC_NH];        /* -0.833, -6, -12, -18, the MW sun limit: edges of the longest run below */
    int8_t  sun_state[AC_NH];                    /* AC_CROSS .. AC_ALWAYS_DOWN */
    int16_t sun_min_e1, sun_max_e1;              /* 0.1 deg */
    int32_t sun_min_t;
    int32_t moon_rise, moon_set;                 /* the first rise / set in the window */
    int8_t  moon_state;                          /* AC_M_* */
    int32_t mw_from, mw_to;                      /* the longest Milky Way run */
    int16_t mw_min;                              /* its length, minutes */
    int8_t  mw_more, mw_moon, mw_state, mw_none; /* more runs, moon above the horizon in it, state (0 = none), AC_MW_NONE_* */
    int16_t mw_moon_pct;                         /* moon illuminated at the window's middle, % */
    int16_t mw_dark_gc_max_e1;                   /* the centre's highest altitude while the sun is below the MW limit */
    int16_t gc_max_e1;
    int32_t gc_max_t;
};

struct ac_now
{
    int32_t t;                                   /* UTC s */
    int16_t sun_alt_e1, moon_alt_e1, gc_alt_e1;  /* geometric altitudes, 0.1 deg (moon topocentric) */
    int16_t moon_pm;                             /* moon illuminated, 1/1000 */
    int8_t  waxing, moon_up;                     /* moon_up: above the rise / set horizon */
    int32_t prev_new, next_new, next_full;       /* UTC s; ac_now with phases = 0 leaves them */
    int16_t pol_ha_min;                          /* Polaris hour angle 0..1439 min */
    int16_t pol_clock_min[3];                    /* 0..719 on a 12-h dial: sky, scope 180 deg, mirrored */
};

typedef int (*ac_yield_fn)(void * ctx);          /* != 0: abort */

struct ac_work                                   /* the caller's buffer (static, not on a task stack) */
{
    float s[AC_NS], m[AC_NS], g[AC_NS];          /* sun altitude, moon altitude - h0, centre altitude per sample */
    float lat, lon, slat, clat;
    float gra, gdec;                             /* the galactic centre of the night */
    int32_t t0m;                                 /* window start, minutes since J2000.0 */
    ac_yield_fn yield;
    void * ctx;
    uint32_t n_eval, chunks;                     /* evaluations, yield calls (this run) */
    uint8_t since, abort;
};

int32_t ac_utc_s(int y, int mo, int d, int h, int mi, int s, int32_t off_min);       /* civil local -> UTC s */
void    ac_civil(int32_t t_s, int32_t off_min, int * y, int * mo, int * d, int * h, int * mi, int * sec, int * wd);
int32_t ac_floordiv(int32_t a, int32_t b);
int     ac_night(const struct ac_loc * loc, const struct ac_crit * crit, int32_t t0, struct ac_work * w,
                 struct ac_night * o, ac_yield_fn yield, void * ctx);                /* 0 = done, AC_ABORTED */
int     ac_now(const struct ac_loc * loc, int32_t t_s, int phases, struct ac_work * w, struct ac_now * o,
               ac_yield_fn yield, void * ctx);                                      /* 0 = done, AC_ABORTED */

#endif
