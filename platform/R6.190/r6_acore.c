/*
 * R6.190 M14.1: the computation core of the astro clock (spec m14/m141 spec_m141.md Rev. 2, 3; research m14/plan
 * A_clock.md 3). Pure arithmetic: no Canon access, no ML call, no global state (only constants), no pointer cast, no
 * double. Time is int32 minutes (positions) or seconds (events) since J2000.0 = 2000-01-01 12:00 UTC (2020..2059 in range);
 * every angle c0 + rate * t is reduced exactly in int64 pico-degrees, then float32 with sinf / cosf / asinf / atan2f.
 *  - sun and moon: Astronomical Almanac low precision (sun 0.01 deg; moon lon 0.3, lat 0.2, parallax 0.003 deg);
 *  - GMST 280.46061837 + 360.98564736629 deg / day (UTC for UT1, mean place);
 *  - precession IAU 1976 for Polaris (J2000 37.95454 / 89.26411) and Sgr A* (266.4168 / -29.0078);
 *  - the night [t0, t0 + 24 h] from the local noon: one scan every 10 min (sun, moon - h0, centre), every crossing
 *    refined (halvings to 1 min, then linear to s), extrema close to a threshold refined (3 halvings), the
 *    sun's night per threshold = the longest run below it, the Milky Way window = the longest run of (sun below its limit
 *    and centre above its altitude);
 *  - phases: elongation search in day steps (at most 31), halvings to 1 min.
 * The caller's yield callback runs after at most AC_CHUNK evaluations (the task sleeps there); != 0 aborts.
 * In ML only config-defines.h (switches only): without CONFIG_R6_ACLOCK this unit is empty (no --gc-sections).
 */
#ifndef AC_HOST
#include "config-defines.h"
#endif
#include <stdint.h>
#include "r6_acore.h"

#if defined(CONFIG_R6_ACLOCK) || defined(AC_HOST)

float sinf(float);
float cosf(float);
float asinf(float);
float atan2f(float, float);

#define AC_D2R  0.0174532925f
#define AC_R2D  57.2957795f
#define AC_PD   1000000000000LL      /* pico-degrees per degree */

/* c0 + rate * t (pico-degrees, rate per minute) reduced to [0, 360) exactly, then micro-degrees -> float degrees */
static float ac_ang(int64_t c0, int64_t rate, int32_t t)
{
    int64_t a = (c0 + rate * (int64_t) t) % (360 * AC_PD);
    if (a < 0) a += 360 * AC_PD;
    return (float) (int32_t) (a / 1000000) * 1e-6f;
}

/* the sign bit of a float (the soft-float compare call is 3x larger; -0 counts as negative, no NaN here) */
static int ac_neg(float x) { union { float f; int32_t i; } u; u.f = x; return u.i < 0; }

static float ac_sind(float d) { return sinf(d * AC_D2R); }
static float ac_cosd(float d) { return cosf(d * AC_D2R); }
static float ac_asind(float x) { if (x > 1.f) x = 1.f; if (x < -1.f) x = -1.f; return asinf(x) * AC_R2D; }
static float ac_atan2d(float y, float x) { float a = atan2f(y, x) * AC_R2D; return ac_neg(a) ? a + 360.f : a; }
static float ac_wrap360(float a) { while (ac_neg(a)) a += 360.f; while (a >= 360.f) a -= 360.f; return a; }

int32_t ac_floordiv(int32_t a, int32_t b)
{
    int32_t q = a / b;
    if ((a % b) && ((a < 0) != (b < 0))) q--;
    return q;
}

static int32_t ac_clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
static int32_t ac_round(float x) { return ac_neg(x) ? -(int32_t) (0.5f - x) : (int32_t) (x + 0.5f); }

/* ---- calendar 2000..2099 (every 4th year a leap year), integers only ---- */
static const uint8_t ac_mlen[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
static int32_t ac_ylen(int y) { return 365 + !(y & 3); }
static int32_t ac_mdays(int y, int m) { return ac_mlen[m - 1] + (m == 2 && !(y & 3)); }

int32_t ac_utc_s(int y, int mo, int d, int h, int mi, int s, int32_t off_min)
{
    int32_t days = d - 1;
    for (int k = 2000; k < y; k++) days += ac_ylen(k);
    for (int k = 1; k < mo && k <= 12; k++) days += ac_mdays(y, k);
    return days * 86400 - 43200 + h * 3600 + mi * 60 + s - off_min * 60;
}

void ac_civil(int32_t t_s, int32_t off_min, int * y, int * mo, int * d, int * h, int * mi, int * sec, int * wd)
{
    int32_t x = t_s + 43200 + off_min * 60;
    int32_t days = ac_floordiv(x, 86400), r = x - days * 86400;
    int yy = 2000, m = 1;
    *wd = (int) ((days + 6) % 7);                         /* 2000-01-01 was a Saturday; 0 = Sunday (days >= 0) */
    while (days >= ac_ylen(yy)) days -= ac_ylen(yy++);
    while (m < 12 && days >= ac_mdays(yy, m)) days -= ac_mdays(yy, m++);
    *y = yy;
    *mo = m;
    *d = (int) days + 1;
    *h = (int) (r / 3600);
    *mi = (int) (r / 60 % 60);
    *sec = (int) (r % 60);
}

/* ---- positions (t = minutes since J2000.0) ---- */
static void ac_sun(int32_t t, float * ra, float * dec, float * lam)
{
    float L = ac_ang(280460000000000LL, 684477361LL, t);      /* 280.460 + 0.9856474 / day */
    float g = ac_ang(357528000000000LL, 684444653LL, t);      /* 357.528 + 0.9856003 / day */
    float l = L + 1.915f * ac_sind(g) + 0.020f * ac_sind(2.f * g);
    float e = 23.439f - 2.7777778e-10f * (float) t;            /* - 0.0000004 / day */
    float sl = ac_sind(l);
    *ra = ac_atan2d(ac_cosd(e) * sl, ac_cosd(l));
    *dec = ac_asind(ac_sind(e) * sl);
    *lam = l;
}

/* the moon's arguments: c0 (pico-deg) and rate (pico-deg / min = deg per century / 52596000) */
static const int64_t ac_mo_c0[11] = { 135000000000000LL, 259300000000000LL, 235700000000000LL, 269900000000000LL,
                                      357500000000000LL, 186500000000000LL, 93300000000000LL, 228200000000000LL,
                                      318300000000000LL, 217600000000000LL, 218320000000000LL };
static const int64_t ac_mo_r[11] = { 9072911818LL, -7858684311LL, 16931595939LL, 18145823637LL, 684444635LL,
                                     18374097460LL, 9187048825LL, 18259960643LL, 114137007LL, -7744547304LL,
                                     9150275325LL };
static const float ac_mo_la[6] = { 6.29f, -1.27f, 0.66f, 0.21f, -0.19f, -0.11f };
static const float ac_mo_pa[4] = { 0.0518f, 0.0095f, 0.0078f, 0.0028f };
static const float ac_mo_ba[4] = { 5.13f, 0.28f, -0.28f, -0.17f };

/* the moon's ecliptic longitude (and, with bet / par, latitude and parallax) */
static float ac_moon_lbp(int32_t t, float * bet, float * par)
{
    float lam = ac_ang(ac_mo_c0[10], ac_mo_r[10], t), p = 0.9508f, b = 0;
    for (int i = 0; i < 6; i++)
    {
        float a = ac_ang(ac_mo_c0[i], ac_mo_r[i], t);
        lam += ac_mo_la[i] * ac_sind(a);
        if (par && i < 4) p += ac_mo_pa[i] * ac_cosd(a);
    }
    if (bet)
    {
        for (int i = 0; i < 4; i++) b += ac_mo_ba[i] * ac_sind(ac_ang(ac_mo_c0[6 + i], ac_mo_r[6 + i], t));
        *bet = b;
    }
    if (par) *par = p;
    return lam;
}

static void ac_moon(int32_t t, float * ra, float * dec, float * par)
{
    float b, l = ac_moon_lbp(t, &b, par);
    float cb = ac_cosd(b), sb = ac_sind(b), sl = ac_sind(l), cl = ac_cosd(l);
    *ra = ac_atan2d(0.9175f * cb * sl - 0.3978f * sb, cb * cl);
    *dec = ac_asind(0.3978f * cb * sl + 0.9175f * sb);
}

static float ac_gst(int32_t t) { return ac_ang(280460618370000LL, 250684477338LL, t); }

/* IAU 1976 precession J2000 -> date (linear terms: the T^2 terms are below 0.00003 deg until 2059) */
static void ac_precess(float ra0, float dec0, int32_t t, float * ra, float * dec)
{
    float T = (float) t * (1.f / 52596000.f);
    float zeta = 0.6406161f * T, z = zeta, th = 0.5567530f * T;
    float cd = ac_cosd(dec0), sd = ac_sind(dec0), ca = ac_cosd(ra0 + zeta), sa = ac_sind(ra0 + zeta);
    float ct = ac_cosd(th), st = ac_sind(th);
    *ra = ac_wrap360(ac_atan2d(cd * sa, ct * cd * ca - st * sd) + z);
    *dec = ac_asind(st * cd * ca + ct * sd);
}

static void ac_setloc(struct ac_work * w, const struct ac_loc * loc, ac_yield_fn yield, void * ctx)
{
    w->lat = (float) ac_clamp(loc->lat_e2, -9000, 9000) * 0.01f;
    w->lon = (float) ac_clamp(loc->lon_e2, -18000, 18000) * 0.01f;
    w->slat = ac_sind(w->lat);
    w->clat = ac_cosd(w->lat);
    w->yield = yield;
    w->ctx = ctx;
    w->n_eval = w->chunks = 0;
    w->since = w->abort = 0;
}

static float ac_alt(const struct ac_work * w, float ra, float dec, float gst)
{
    return ac_asind(w->slat * ac_sind(dec) + w->clat * ac_cosd(dec) * ac_cosd(gst + w->lon - ra));
}

/* one evaluation more: the yield callback after AC_CHUNK of them; 1 = aborted */
static int ac_tick(struct ac_work * w)
{
    if (w->abort) return 1;
    if (w->since >= AC_CHUNK)
    {
        w->since = 0;
        w->chunks++;
        if (w->yield && w->yield(w->ctx)) { w->abort = 1; return 1; }
    }
    w->since++;
    w->n_eval++;
    return 0;
}

enum { AC_B_SUN, AC_B_MOON, AC_B_GC };

/* the body's value: sun altitude, moon altitude - h0 (0.7275 parallax - 0.5667, upper limb with refraction), centre altitude */
static float ac_body(const struct ac_work * w, int b, int32_t t)
{
    float ra, dec, x, gst = ac_gst(t);
    if (b == AC_B_SUN) { ac_sun(t, &ra, &dec, &x); return ac_alt(w, ra, dec, gst); }
    if (b == AC_B_MOON) { ac_moon(t, &ra, &dec, &x); return ac_alt(w, ra, dec, gst) - (0.7275f * x - 0.5667f); }
    return ac_alt(w, w->gra, w->gdec, gst);
}

/* elongation of the moon from the sun (0..360), and the moon's latitude */
static float ac_elong(int32_t t, float * bet)
{
    float ra, dec, ls;
    ac_sun(t, &ra, &dec, &ls);
    return ac_wrap360(ac_moon_lbp(t, bet, 0) - ls);
}

/* illuminated fraction 1/1000 of the moon, waxing */
static int ac_illum(int32_t t, int * waxing)
{
    float b, e = ac_elong(t, &b);
    if (waxing) *waxing = e < 180.f;
    return (int) ac_round(500.f * (1.f - ac_cosd(b) * ac_cosd(e)));
}

/* ---- events ---- */
struct ac_ev { int32_t t; int8_t up; };
struct ac_iv { int32_t a, b; };

/* the crossing of v = body - h in [ta, tb] minutes (va, vb of opposite sign): halvings to 1 min, then linear, in s */
static int32_t ac_cross(struct ac_work * w, int b, float h, int32_t ta, float va, int32_t tb, float vb)
{
    while (tb - ta > 1)
    {
        if (ac_tick(w)) return AC_NONE;
        int32_t tc = ta + (tb - ta) / 2;
        float vc = ac_body(w, b, tc) - h;
        if (ac_neg(va) != ac_neg(vc)) { tb = tc; vb = vc; }
        else { ta = tc; va = vc; }
    }
    int32_t ds = va != vb ? (int32_t) (60.f * va / (va - vb)) : 0;
    return ta * 60 + ac_clamp(ds, 0, 60) * (tb - ta);
}

/* pattern search around an extreme sample at minute *tv (vv = its value - h): steps 4, 2, 1 min inside (lo, lo + 20), the
 * true extreme lies within +-5 min of the extreme sample; stops once vv changes its sign (a crossing) */
static float ac_peak(struct ac_work * w, int b, float h, int mx, int32_t lo, int32_t * tv, float vv)
{
    const int s0 = ac_neg(vv);
    for (int k = 4; k >= 1 && ac_neg(vv) == s0; k >>= 1)
        for (int sgn = -1; sgn <= 1; sgn += 2)
        {
            int32_t tt = *tv + sgn * k;
            if (tt <= lo || tt >= lo + 20 || ac_tick(w)) continue;
            float vt = ac_body(w, b, tt) - h;
            if (mx ? vt > vv : vt < vv) { vv = vt; *tv = tt; break; }
        }
    return vv;
}

static void ac_ev_add(struct ac_ev * ev, int * n, int32_t t, int up)
{
    if (t == AC_NONE || *n >= AC_NEV) return;
    ev[*n].t = t;
    ev[*n].up = up;
    (*n)++;
}

/* all crossings of body b over h in the window, in time order; an extremum (all three samples on one side, closer than
 * 0.5 deg / moon 1.0 deg) is refined by 3 halvings around the extreme sample (steps 4, 2, 1 min: +-7 min, the true
 * extreme lies within +-5 min of it); if it crosses h, its two crossings are added */
static int ac_events(struct ac_work * w, int b, float h, struct ac_ev * ev)
{
    const float * v = b == AC_B_SUN ? w->s : b == AC_B_MOON ? w->m : w->g;
    const float mg = b == AC_B_MOON ? 1.0f : 0.5f;
    int n = 0;
    for (int i = 1; i < AC_NS && !w->abort; i++)
    {
        float a = v[i - 1] - h, c = v[i] - h;
        int32_t ta = w->t0m + 10 * (i - 1);
        if (ac_neg(a) != ac_neg(c)) ac_ev_add(ev, &n, ac_cross(w, b, h, ta, a, ta + 10, c), !ac_neg(c));
        if (i >= AC_NS - 1) continue;
        float d = v[i + 1] - h;
        int na = ac_neg(a), nc = ac_neg(c), nd = ac_neg(d);
        int mn = !na && !nc && !nd && c < a && c <= d && c < mg;
        int mx = na && nc && nd && c > a && c >= d && c > -mg;
        if (!mn && !mx) continue;
        int32_t tv = ta + 10;
        float vv = ac_peak(w, b, h, mx, ta, &tv, c);
        if (ac_neg(vv) == nc || w->abort) continue;
        ac_ev_add(ev, &n, ac_cross(w, b, h, ta, a, tv, vv), mx);
        ac_ev_add(ev, &n, ac_cross(w, b, h, tv, vv, ta + 20, d), mn);
    }
    return n;
}

/* the runs "in" (below h if !in_up, above h if in_up) from the window start state and the crossings */
static int ac_runs(const struct ac_ev * ev, int n, int in, int in_up, int32_t t0, int32_t t1, struct ac_iv * iv)
{
    int k = 0;
    int32_t a = t0;
    for (int i = 0; i < n; i++)
    {
        int enter = ev[i].up == in_up;
        if (enter && !in) { a = ev[i].t; in = 1; }
        else if (!enter && in) { if (k < AC_NEV) { iv[k].a = a; iv[k].b = ev[i].t; k++; } in = 0; }
    }
    if (in && k < AC_NEV) { iv[k].a = a; iv[k].b = t1; k++; }
    return k;
}

/* the longest run (ties: the one containing tref); -1 if none */
static int ac_longest(const struct ac_iv * iv, int k, int32_t tref)
{
    int best = -1;
    for (int i = 0; i < k; i++)
    {
        int32_t l = iv[i].b - iv[i].a, lb = best < 0 ? -1 : iv[best].b - iv[best].a;
        if (l > lb || (l == lb && iv[i].a <= tref && tref <= iv[i].b)) best = i;
    }
    return best;
}

static int ac_state(const struct ac_iv * r, int32_t t0, int32_t t1, int32_t * from, int32_t * to)
{
    *from = *to = AC_NONE;
    if (!r) return AC_ALWAYS_UP;
    if (r->a <= t0 && r->b >= t1) return AC_ALWAYS_DOWN;
    if (r->a <= t0) { *to = r->b; return AC_FROM_START; }
    *from = r->a;
    if (r->b >= t1) return AC_PARTIAL;
    *to = r->b;
    return AC_CROSS;
}

/* the extreme of body b in the window (the extreme sample, refined by ac_peak inside the window); *at = its minute */
static float ac_ext(struct ac_work * w, int b, int mx, int32_t * at)
{
    const float * v = b == AC_B_SUN ? w->s : w->g;
    int j = 0;
    for (int i = 1; i < AC_NS; i++) if (mx ? v[i] > v[j] : v[i] < v[j]) j = i;
    *at = w->t0m + 10 * j;
    if (j == 0 || j == AC_NS - 1) return v[j];
    const float off = mx ? -1000.f : 1000.f;           /* keeps the sign: ac_peak never stops early */
    return ac_peak(w, b, off, mx, *at - 10, at, v[j] - off) + off;
}

static int16_t ac_e1(float x) { return (int16_t) ac_clamp(ac_round(10.f * x), -32000, 32000); }

/* the longest of k runs or 0 */
static struct ac_iv * ac_iv_pick(struct ac_iv * iv, int k, int32_t tref) { int i = ac_longest(iv, k, tref); return i < 0 ? 0 : &iv[i]; }

int ac_night(const struct ac_loc * loc, const struct ac_crit * crit, int32_t t0, struct ac_work * w,
             struct ac_night * o, ac_yield_fn yield, void * ctx)
{
    static const float hs[AC_H_MW] = { -0.833f, -6.f, -12.f, -18.f };
    struct ac_ev ev[AC_NEV];
    struct ac_iv iv[AC_NEV], sv[AC_NEV], gv[AC_NEV], mv[AC_NEV];
    int ns = 0, ng = 0, nm = 0, n;
    int32_t at;
    mv[0].a = mv[0].b = 0;
    ac_setloc(w, loc, yield, ctx);
    w->t0m = ac_floordiv(t0, 60);
    const int32_t t0s = w->t0m * 60, t1s = t0s + 86400;
    const float hmw = (float) ac_clamp(crit->sun_max_deg, -18, -12), hgc = (float) ac_clamp(crit->gc_min_deg, 5, 15);
    ac_precess(266.4168f, -29.0078f, w->t0m, &w->gra, &w->gdec);
    for (int i = 0; i < AC_NS; i++)
    {
        if (ac_tick(w)) return AC_ABORTED;
        int32_t t = w->t0m + 10 * i;
        w->s[i] = ac_body(w, AC_B_SUN, t);
        w->m[i] = ac_body(w, AC_B_MOON, t);
        w->g[i] = ac_body(w, AC_B_GC, t);
    }
    o->t0 = t0s;
    o->sun_min_e1 = ac_e1(ac_ext(w, AC_B_SUN, 0, &at));
    o->sun_min_t = at * 60;
    o->sun_max_e1 = ac_e1(ac_ext(w, AC_B_SUN, 1, &at));

    /* the sun: per threshold the longest run below it (ties: the one with the sun's minimum) */
    for (int k = 0; k < AC_NH; k++)
    {
        const float h = k < AC_H_MW ? hs[k] : hmw;
        struct ac_iv * r = k == AC_H_MW ? sv : iv;
        n = ac_events(w, AC_B_SUN, h, ev);
        int ni = ac_runs(ev, n, ac_neg(w->s[0] - h), 0, t0s, t1s, r);
        if (k == AC_H_MW) ns = ni;
        o->sun_state[k] = ac_state(ac_iv_pick(r, ni, o->sun_min_t), t0s, t1s, &o->sun_dn[k], &o->sun_up[k]);
    }

    /* the moon: the first rise and the first set */
    n = ac_events(w, AC_B_MOON, 0.f, ev);
    o->moon_rise = o->moon_set = AC_NONE;
    for (int i = 0; i < n; i++)
    {
        if (ev[i].up && o->moon_rise == AC_NONE) o->moon_rise = ev[i].t;
        if (!ev[i].up && o->moon_set == AC_NONE) o->moon_set = ev[i].t;
    }
    o->moon_state = o->moon_rise != AC_NONE ? (o->moon_set != AC_NONE ? AC_M_BOTH : AC_M_NOSET)
                  : o->moon_set != AC_NONE ? AC_M_NORISE : !ac_neg(w->m[0]) ? AC_M_UP : AC_M_DOWN;

    /* the galactic centre above its altitude */
    n = ac_events(w, AC_B_GC, hgc, ev);
    ng = ac_runs(ev, n, !ac_neg(w->g[0] - hgc), 1, t0s, t1s, gv);
    o->gc_max_e1 = ac_e1(ac_ext(w, AC_B_GC, 1, &at));
    o->gc_max_t = at * 60;
    /* the centre's highest altitude while the sun is below the MW limit: the samples inside, the edges of the runs */
    float dm = -90.f;
    for (int i = 0; i < AC_NS; i++) if (w->s[i] < hmw && w->g[i] > dm) dm = w->g[i];
    for (int i = 0; i < 2 * ns && !ac_tick(w); i++)
    {
        float x = ac_body(w, AC_B_GC, ac_floordiv((i & 1 ? sv[i >> 1].b : sv[i >> 1].a) + 30, 60));
        if (x > dm) dm = x;
    }
    o->mw_dark_gc_max_e1 = ac_e1(dm);
    if (w->abort) return AC_ABORTED;

    /* the Milky Way: (sun below its limit) and (centre above its altitude), the longest run */
    for (int i = 0; i < ns; i++)
        for (int j = 0; j < ng; j++)
        {
            int32_t lo = sv[i].a > gv[j].a ? sv[i].a : gv[j].a, hi = sv[i].b < gv[j].b ? sv[i].b : gv[j].b;
            if (lo < hi && nm < AC_NEV) { mv[nm].a = lo; mv[nm].b = hi; nm++; }
        }
    int j = ac_longest(mv, nm, 0);
    o->mw_more = nm > 1;
    o->mw_moon = 0;
    o->mw_moon_pct = 0;
    o->mw_min = 0;
    o->mw_none = 0;
    if (j < 0)
    {
        o->mw_state = 0;
        o->mw_from = o->mw_to = AC_NONE;
        o->mw_none = !ns ? AC_MW_NONE_SUN : !ng ? AC_MW_NONE_GC : AC_MW_NONE_DARK;
        return 0;
    }
    o->mw_state = ac_state(&mv[j], t0s, t1s, &o->mw_from, &o->mw_to);
    o->mw_min = (int16_t) ((mv[j].b - mv[j].a) / 60);
    for (int i = 0; i < AC_NS; i++)
    {
        int32_t ts = (w->t0m + 10 * i) * 60;
        if (ts >= mv[j].a && ts <= mv[j].b && !ac_neg(w->m[i])) o->mw_moon = 1;
    }
    int32_t mid = ac_floordiv(mv[j].a + (mv[j].b - mv[j].a) / 2, 60);
    if (ac_tick(w)) return AC_ABORTED;
    if (!ac_neg(ac_body(w, AC_B_MOON, mid))) o->mw_moon = 1;
    o->mw_moon_pct = (int16_t) ((ac_illum(mid, 0) + 5) / 10);
    return 0;
}

/* elongation - target wrapped to (-180, 180] */
static float ac_pd(int32_t t, float target)
{
    float b, d = ac_elong(t, &b) - target;
    while (d > 180.f) d -= 360.f;
    while (d <= -180.f) d += 360.f;
    return d;
}

/* the next (dir 1) or last (dir -1) time the elongation passes target (from < 0 to >= 0 in time order; the jump at
 * +-180 is no event): day steps (at most 31), halvings to 1 min, then linear; UTC s or AC_NONE */
static int32_t ac_phase(struct ac_work * w, int32_t tm, float target, int dir)
{
    if (ac_tick(w)) return AC_NONE;
    int32_t ta = tm;
    float da = ac_pd(tm, target);
    for (int k = 1; k <= 31; k++)
    {
        if (ac_tick(w)) return AC_NONE;
        int32_t tb = tm + dir * 1440 * k;
        float db = ac_pd(tb, target);
        int32_t tl = dir > 0 ? ta : tb, th = dir > 0 ? tb : ta;
        float dl = dir > 0 ? da : db, dh = dir > 0 ? db : da;
        if (ac_neg(dl) && !ac_neg(dh) && dh - dl < 180.f)
        {
            while (th - tl > 1)
            {
                if (ac_tick(w)) return AC_NONE;
                int32_t tc = tl + (th - tl) / 2;
                float dc = ac_pd(tc, target);
                if (ac_neg(dc)) { tl = tc; dl = dc; }
                else { th = tc; dh = dc; }
            }
            int32_t ds = dh != dl ? (int32_t) (60.f * -dl / (dh - dl)) : 0;
            return tl * 60 + ac_clamp(ds, 0, 60);
        }
        ta = tb;
        da = db;
    }
    return AC_NONE;
}

int ac_now(const struct ac_loc * loc, int32_t t_s, int phases, struct ac_work * w, struct ac_now * o,
           ac_yield_fn yield, void * ctx)
{
    float ra, dec, x;
    int wx;
    ac_setloc(w, loc, yield, ctx);
    const int32_t tm = ac_floordiv(t_s + 30, 60);         /* positions at the rounded minute */
    if (ac_tick(w)) return AC_ABORTED;
    const float gst = ac_gst(tm);
    o->t = t_s;
    ac_sun(tm, &ra, &dec, &x);
    o->sun_alt_e1 = ac_e1(ac_alt(w, ra, dec, gst));
    ac_moon(tm, &ra, &dec, &x);
    float hm = ac_alt(w, ra, dec, gst);
    o->moon_up = !ac_neg(hm - (0.7275f * x - 0.5667f));
    o->moon_alt_e1 = ac_e1(hm - x * ac_cosd(hm));          /* topocentric */
    ac_precess(266.4168f, -29.0078f, tm, &ra, &dec);
    o->gc_alt_e1 = ac_e1(ac_alt(w, ra, dec, gst));
    o->moon_pm = (int16_t) ac_illum(tm, &wx);
    o->waxing = wx;
    ac_precess(37.95454f, 89.26411f, tm, &ra, &dec);
    float ha = ac_wrap360(gst + w->lon - ra);
    int32_t q = ac_round(4.f * ha), p = ac_round(720.f - 2.f * ha);   /* ha in [0, 360): q in [0, 1440], p in [0, 720] */
    o->pol_ha_min = (int16_t) (q >= 1440 ? 0 : q);
    if (p >= 720) p = 0;                                  /* sky: 12 h - HA / 2 on the 12-h dial (minutes) */
    o->pol_clock_min[0] = (int16_t) p;
    o->pol_clock_min[1] = (int16_t) (p < 360 ? p + 360 : p - 360);     /* scope 180 deg */
    o->pol_clock_min[2] = (int16_t) (p ? 720 - p : 0);                 /* mirrored */
    if (phases)
    {
        int32_t a = ac_phase(w, tm, 0.f, -1), b = ac_phase(w, tm, 0.f, 1), c = ac_phase(w, tm, 180.f, 1);
        if (w->abort) return AC_ABORTED;
        o->prev_new = a;
        o->next_new = b;
        o->next_full = c;
    }
    return w->abort ? AC_ABORTED : 0;
}

#endif /* CONFIG_R6_ACLOCK || AC_HOST */
