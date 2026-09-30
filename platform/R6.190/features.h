/* First bring-up: load fonts from card, print Hello World, start nothing else.
 * Remove once boot path and basic stubs are confirmed on the camera. */
#define CONFIG_HELLO_WORLD

#define FEATURE_VRAM_RGBA

/* Like R5: no dedicated layer (compositor.c has no DIGIC X implementation);
 * we draw into Canon's 2048x1080 GUI layer, LCD area 720x480 at (152,30). */
#define CONFIG_COMPOSITOR_XCM

#undef CONFIG_CRASH_LOG
#undef CONFIG_PROP_REQUEST_CHANGE
#undef CONFIG_AUTOBACKUP_ROM
