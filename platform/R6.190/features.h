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

#undef CONFIG_CRASH_LOG
#undef CONFIG_PROP_REQUEST_CHANGE
#undef CONFIG_AUTOBACKUP_ROM
