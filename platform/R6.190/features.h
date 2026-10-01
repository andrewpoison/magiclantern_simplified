#define FEATURE_VRAM_RGBA

/* Like R5: no dedicated layer (compositor.c has no DIGIC X implementation);
 * we draw into Canon's 2048x1080 GUI layer, LCD area 720x480 at (152,30). */
#define CONFIG_COMPOSITOR_XCM

/* M6: first full ML boot. Subset of platform/R5.152/features.h, only
 * read-only diagnostics. Deliberately left out for now:
 * FEATURE_DONT_CLICK_ME (call("dumpf")), FEATURE_SHOW_FREE_MEMORY
 * (grabs shoot memory, stack probing tasks), FEATURE_SHOW_TASKS and
 * FEATURE_SHOW_CPU_USAGE (need CONFIG_TSKMON). */
#define FEATURE_SHOW_SHUTTER_COUNT
#define FEATURE_SHOW_GUI_EVENTS   // Debug > Show GUI events: verify button codes

/* R6 bring-up diagnostics (boot-stage LED + status line), remove after M6 */
#define CONFIG_R6_BRINGUP

/* no Debug > "Dump ROM and RAM" / "Dump image buffers" (src/debug.c):
 * one SET press would write ROM (private) or LV buffers to the card */
#define CONFIG_NO_DUMP_MENUS

/* tskmon runs inside the DryOS scheduler hook on both cores and would
 * bmp_printf a stack warning for the 256-byte R6 idle stacks every second
 * (src/tskmon.c:185-213). Off until tested. */
#undef CONFIG_TSKMON

#undef CONFIG_CRASH_LOG
#undef CONFIG_PROP_REQUEST_CHANGE
#undef CONFIG_AUTOBACKUP_ROM
