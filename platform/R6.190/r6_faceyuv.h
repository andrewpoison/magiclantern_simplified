/*
 * R6.190 M7.4: Canon's FaceYuv image as ML's LiveView image source
 * (r6_faceyuv.c). Shared by lvprobe.c ("FaceYuv sample", log keys fy.*) and
 * src/vram.c (get_yuv422_vram, CONFIG_R6_FACEYUV_LV).
 *
 * FaceYuv (camera M7.3, 2026-10-02): Canon's 640x426 8-bit UYVY image for
 * the face detection, chroma signed (int8, 0 = neutral), 4 buffers on IBus
 * channel 1 (camera: 0x1:11540260 + k * 0x96000), static memif window 5,
 * read only through the uncached alias 0x5xxxxxxx. Approved by the user
 * 2026-10-01 (read only, every pointer checked).
 *
 * M9.3 (r6_motion.c): r6_fy_locate_done() picks the second-newest ring frame
 * F2, whose buffer is not the one being written, and r6_fy_recheck() tells
 * after the pixel read whether that buffer may have been handed out again
 * (rules in r6_faceyuv.c).
 */
#ifndef _r6_faceyuv_h_
#define _r6_faceyuv_h_

#include <stdint.h>

#define R6_FY_W        640
#define R6_FY_H        426
#define R6_FY_PITCH    (R6_FY_W * 2)               /* 8-bit 4:2:2, 2 bytes per pixel */
#define R6_FY_BYTES    (R6_FY_PITCH * R6_FY_H)     /* 0x85200 = burst stride (0xE08093D0) */

/* which buffer holds the newest complete frame, and where the CPU may read it */
struct r6_fy_loc
{
    int ok;                  /* 1 = cpu is the verified uncached alias of the whole buffer */
    const char * why;        /* refusal reason ("" if ok) */
    int ring_i;              /* s8 MEM(0x1ED68): next ring slot */
    uint32_t ring_flag, ring_frame;   /* newest ring entry {flag, FrameNum} */
    int gidx, ridx;          /* ctx[0xB029] getter index, ctx[0xB02A] record index */
    int pick, src;           /* buffer 0..3; src 1 rec, 2 rec_old, 3 get */
    int rec;                 /* record used, -1 none */
    uint32_t rec_frame;
    uint32_t lo, hi;         /* 64-bit IBus address of the picked buffer */
    uint32_t cpu;            /* CPU address (uncached alias), 0 if refused */
    int win;                 /* static memif window, -1 none */
    /* M9.3 (r6_fy_locate_done only; src 4) */
    uint32_t ctx;            /* MEM(0x1ED68+0xC) used */
    uint32_t b048;           /* byte [ctx+0xB048] (0xFF = not readable) */
    uint32_t f1, f2;         /* newest / second-newest ring FrameNum */
    int pick1;               /* buffer of F1's record (0..3), -1 none */
};

/* Locate the current FaceYuv buffer (RAM reads of Canon's controller
 * context, ring and records; nothing written, no Canon call). The rules are
 * the ones of the M7.3 "FaceYuv sample" (camera-verified 2026-10-02): the
 * record whose FrameNum equals the newest ring entry, else the newest older
 * record, else the getter rule. Then: channel 1, aligned, a static window
 * 4..7, the whole buffer inside that window, inside channel 1's RAM end and
 * inside the verified uncached alias. Returns r->ok. */
int r6_fy_locate(struct r6_fy_loc * r);

/* M9.3: the buffer of the second-newest ring frame F2 (src 4), for the motion
 * detector: [ctx+0xB048] & 6 == 0, F1 - F2 in 1..16, a record for F1 and one
 * for F2, each naming one of the 4 buffers, different buffers, and exactly
 * one hand-out since F2 ([ctx+0xB029] = g: F1 in buffer (g+3)&3, F2 in
 * (g+2)&3). Never the getter fallback. Then the pointer checks of
 * r6_fy_locate on that buffer. Returns r->ok. */
int r6_fy_locate_done(struct r6_fy_loc * r);

/* M9.3, after the pixel read of a r6_fy_locate_done result: hand-outs since
 * the locate (0..3), or -1 if r is not an ok src-4 result, the ctx pointer
 * changed, a byte is not readable or [ctx+0xB048] & 6 != 0 */
int r6_fy_recheck(const struct r6_fy_loc * r);

/* [a, a+n) 4-byte aligned and inside cached RAM 0x1000..0x3FFFFFFF, or inside
 * the uncached alias 0x40001000..0x7FFFFFFF if Canon's memif copy verifies
 * it (the lvprobe.c rule); for reads of Canon pointers (r6_gdraw.c) */
int r6_fy_ram_ok(uint32_t a, uint32_t n);

/* ML's LV image (vram.c): 1 and *cpu = uncached alias of the verified buffer
 * if Canon's LV runs (PROP_LV_ACTION == 0 as recorded by propvalues.c),
 * r6_fy_locate succeeds and the ring's FrameNum moved within the last
 * second; else 0 (counted per reason). Cheap: ~100 RAM loads, no pixels. */
int r6_fy_lv_frame(uint32_t * cpu);

enum {
    R6FY_OK = 0,
    R6FY_NO_LV,      /* PROP_LV_ACTION not 0 (or not seen yet) */
    R6FY_LOCATE,     /* r6_fy_locate refused (why: last_why) */
    R6FY_STALE,      /* ring FrameNum unchanged for 1 s */
    R6FY_NREASONS
};

struct r6_fy_stats
{
    uint32_t calls;
    uint32_t reasons[R6FY_NREASONS];
    uint32_t last;           /* R6FY_* of the last call */
    const char * last_why;   /* r6_fy_locate reason of the last refusal */
    uint32_t cpu;            /* last verified buffer (CPU address) */
    uint32_t frame;          /* its ring FrameNum */
    uint32_t switches;       /* calls that returned another buffer than the call before */
};

const struct r6_fy_stats * r6_fy_stats(void);

#endif
