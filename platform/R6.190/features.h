#define FEATURE_VRAM_RGBA

/* Like R5: no dedicated layer (compositor.c has no DIGIC X implementation);
 * we draw into Canon's 2048x1080 GUI layer, LCD area 720x480 at (152,30). */
#define CONFIG_COMPOSITOR_XCM

/* Subset of platform/R5.152/features.h, only read-only diagnostics.
 * Deliberately left out: FEATURE_DONT_CLICK_ME (call("dumpf")),
 * FEATURE_SHOW_FREE_MEMORY (grabs shoot memory, stack probing tasks). */
#define FEATURE_SHOW_SHUTTER_COUNT
#define FEATURE_SHOW_GUI_EVENTS   // Debug > Show GUI events: button codes

/* tskmon (CONFIG_TSKMON, default on) runs in the DryOS dispatch hook on both
 * cores. Both R6 idle tasks have 256-byte stacks: exception in tskmon.c. */
#define FEATURE_SHOW_TASKS
#define FEATURE_SHOW_CPU_USAGE

/* no Debug > "Dump ROM and RAM" / "Dump image buffers" (src/debug.c):
 * one SET press would write ROM (private) or LV buffers to the card */
#define CONFIG_NO_DUMP_MENUS

/* M7.0: Debug > "LV probe" (lvprobe.c), read-only LiveView census:
 * plain RAM reads of Canon globals, status line in ML's own layer-0 pixels,
 * one new file ML/LOGS/LVPnnnnn.LOG per snapshot. Also times
 * refresh_yuv_from_rgb (bmp.c). No new MMIO by ML; existing M6 accesses:
 * DIGIC timer 0xC0242014 (read more often in this build), Canon XimrExe
 * (also from the status task in "ON, refresh", only while the EVF is on). */
#define FEATURE_LV_BUFFER_PROBE

/* M7.3: Overlay > "Histogram (Canon)" (r6_overlays.c): Canon's own LV
 * histogram bins (RAM reads, confirmed in M7.2a) drawn as a 128x64 box into
 * ML's indexed buffer. Not FEATURE_GLOBAL_DRAW (zebra.c / lvinfo paths are
 * not reviewed for the shared layer). Needs CONFIG_R6_LAYER_OWNERSHIP. */
#define FEATURE_R6_CANON_HISTOGRAM

/* M7.4: ML's own overlays from Canon's FaceYuv image (CONFIG_R6_GLOBAL_DRAW,
 * internals.h), Overlay menu. All default OFF. M7.6: Global Draw is saved
 * (default ON) and the LV flag follows Canon at boot, both only after a clean
 * shutdown (CONFIG_R6_AUTOSTART).
 * M7.6: FEATURE_FOCUS_PEAK (zebra.c draw_zebra_and_focus), dots only, on
 * FaceYuv (get_yuv422_vram, not YUV422_LV_BUFFER_DISPLAY_ADDR = 0); no
 * display-filter modes (FEATURE_FOCUS_PEAK_DISP_FILTER), no grayscale image.
 * Deliberately left out (MMIO, writes into Canon image buffers, Canon GUI
 * calls, files, or not reviewed): FEATURE_ZEBRA_FAST (EngDrvOut),
 * FEATURE_MAGIC_ZOOM (writes the LV buffer), CONFIG_DISPLAY_FILTERS,
 * FEATURE_VECTORSCOPE, FEATURE_CROPMARKS / FEATURE_GHOST_IMAGE (card files,
 * Canon redraws), FEATURE_OVERLAYS_IN_PLAYBACK_MODE, FEATURE_LV_DISPLAY_PRESETS;
 * r6_gdraw.c #errors on them. ML's histogram and "Histogram (Canon)" share
 * the box position and are mutually exclusive (r6_overlays.c). */
#define FEATURE_GLOBAL_DRAW
#define FEATURE_ZEBRA
#define FEATURE_FALSE_COLOR
#define FEATURE_WAVEFORM
#define FEATURE_SPOTMETER
#define FEATURE_HISTOGRAM
#define FEATURE_FOCUS_PEAK

#undef CONFIG_CRASH_LOG
#undef CONFIG_PROP_REQUEST_CHANGE
#undef CONFIG_AUTOBACKUP_ROM
