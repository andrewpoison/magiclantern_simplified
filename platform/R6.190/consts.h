/*
 *  EOS R6 1.9.0 consts
 *
 *  Addresses verified against the 1.9.0 ROM dump, see docs/stubs_r6_190.md
 *  in the porting workspace. Values marked "R5, unverified" are copied from
 *  R5.152 and not yet confirmed on R6.
 */

// Device::LEDDef.c: LED 0 is port 0xC2 (0xE01F7818); the GPIO writer 0xE0891AE4
// uses (port & ~3) + 0xD2239000. On / off values from 0xE01F7546 / 0xE01F75E6.
#define CARD_LED_ADDRESS            0xD22390C0
#define LEDON                       0x24D0002
#define LEDOFF                      0x24C0003

// Identical positions to R6.150 and R5.152; verified in 1.9.0.
#define BR_DCACHE_CLN_1     0xE0100058   /* first call to dcache_clean, before cstart */
#define BR_ICACHE_INV_1     0xE0100062   /* first call to icache_invalidate, before cstart */
#define BR_DCACHE_CLN_2     0xE010008E   /* second call to dcache_clean, before cstart */
#define BR_ICACHE_INV_2     0xE0100098   /* second call to icache_invalidate, before cstart */
#define BR_BR_CSTART        0xE01000AE   // indirect branch to cstart (ldr r0,=0xE0100023; bx r0);
                                         // the first branch is absolute to original, we must patch
#define BR_BZERO32          0xE0100138   /* called from cstart */
#define BR_CREATE_ITASK     0xE010019A   /* called from cstart */

// firmware_entry and cstart are contiguous: 0xE0100000 - 0xE010020F including
// the per-CPU init 0xE01001D0 and literal pools. No CSTART_LEN needed.
#define FIRMWARE_ENTRY_LEN 0x800

/* "Malloc Information", get_malloc_info 0xE037C234 */
#define MALLOC_STRUCT_ADDR 0xFCF48

/* DryOS assert 0xE08D7714 reads the handler from 0x4000, setter 0xE08D7756 */
#define DRYOS_ASSERT_HANDLER        0x4000

// GetGUIMode 0xE079A692: ldr r0,=0x8D00; ldr r0,[r0,#0x70]
#define CURRENT_GUI_MODE            (*(int*)0x8D70)

/**
 * New GUI mode numbering scheme, like R5 (IDLEHandler 0xE0BD3880:
 * START_PLAY_MODE -> SetGUIMode(0x2002), START_MENU_MODE -> SetGUIMode(0x2003))
 */
#define GUIMODE_PLAY 0x2002
#define GUIMODE_MENU 0x2003
// The ML menu always opens over Canon's MENU (0x2003), also in LV, as on RP.160.
// 0xC048 is Canon's Q screen: name table 0xE1C585BC, low byte 0x48 =
// START_UNAVI_MODE (movw + SetGUIRequestMode at 0xE07543B4, compared in the
// PRESS_UNAVI handler 0xE0790296). Requesting it, or any other mode, would be
// a new Canon GUI request that needs separate approval.
#define GUIMODE_ML_MENU GUIMODE_MENU

#define CANON_SHUTTER_RATING 300000 // Canon's official rating for R6

// DispOperator_PropertyMasterSetDisplayTurnOffOn 0xE0751460 caches the requested
// state here (1 = on). Not yet confirmed on the camera.
#define DISPLAY_IS_ON               (*(int *)0xEF48)

#define GMT_FUNCTABLE               0xE0CD1B00           // from gui_main_task 0xE04DE3F8 (memcpy 0x1C)
#define GMT_NFUNCS                  0x7                  // size of table above

// eventprocs lvae_set* at 0xE072A842ff; all field offsets identical to R5
#define LVAE_STRUCT                 0x120334
#define CONTROL_BV      (*(uint16_t*)(LVAE_STRUCT+0x28)) // via "lvae_setcontrolbv"
#define CONTROL_BV_TV   (*(uint16_t*)(LVAE_STRUCT+0x42)) // via "lvae_setcontrolaeparam"
#define CONTROL_BV_AV   (*(uint16_t*)(LVAE_STRUCT+0x44)) // via "lvae_setcontrolaeparam"
#define CONTROL_BV_ISO  (*(uint16_t*)(LVAE_STRUCT+0x46)) // via "lvae_setcontrolaeparam"
#define CONTROL_BV_ZERO (*(uint16_t*)(LVAE_STRUCT+0x48)) // via "lvae_setcontrolaccumh"
#define LVAE_DISP_GAIN  (*(uint16_t*)(LVAE_STRUCT+0x74)) // via "lvae_setdispgain"
#define LVAE_MOV_M_CTRL (*(uint8_t* )(LVAE_STRUCT+0x24)) // via "lvae_setmoviemanualcontrol"

/*
 * LvInfoToggle_Update 0xE08C1B90 ("LvInfoToggle_Update ToggleIndex:%d -> %d").
 * WARNING: R6 has 9 states (0..8) and no "TURNOFF" string; the R5 meaning of
 * the values (3 = clean overlays) is not confirmed for R6.
 */
#define LV_OVERLAYS_MODE MEM(0x20F70)
/* M7.4: values on which ML's overlays may draw (get_global_draw, zebra.c;
 * r6_gdraw.c). Camera (M7.1 census, M7.3 logs, 2026-10-02): 0, 1, 2, 3 on
 * the INFO variants of the LV shooting screen, 6 on Canon's settings screen
 * (OLC, also used by r6_overlays.c). 4, 5, 7, 8 never observed: not allowed.
 * The R5/R180 meaning "3 = clean overlays" is not used on R6. */
#define LV_OVERLAYS_MODE_ML_OK(m) ((uint32_t) (m) <= 3)

// AeProcessSetting 0xE0725E9A; ISOMin via " >> ISOMin:%d" (LVAE_STRUCT - 0x48, as on R5)
#define LVAE_ISO_STRUCT 0x1202EC
#define LVAE_ISO_MIN    (*(uint8_t* )LVAE_ISO_STRUCT + 0x0E ) // via string: ISOMin:%d

/*
 * DISP_VRAM pointers are over 32 bits on DIGIC X (IBus addresses), see R5.
 * LV buffers are not usable yet.
 */
#define YUV422_LV_BUFFER_1   0x0
#define YUV422_LV_BUFFER_2   0x0
#define YUV422_LV_BUFFER_3   0x0

#define YUV422_LV_BUFFER_DISPLAY_ADDR 0x0
#define YUV422_LV_PITCH               1024       // depends on display type

#define YUV422_HD_BUFFER_DMA_ADDR 0x0 // it expects this to be shamem_read(some_DMA_ADDR)

/* WRONG! */
#define HALFSHUTTER_PRESSED         0

#define NUM_PICSTYLES 11 // R5, unverified

#define AUDIO_MONITORING_HEADPHONES_CONNECTED 0
#define INFO_BTN_NAME               "INFO"
#define Q_BTN_NAME                  "Q/SET"

#define MIN_MSLEEP 11
#define PLAY_MODE (gui_state == GUISTATE_PLAYMENU && CURRENT_GUI_MODE == GUIMODE_PLAY)
#define MENU_MODE (gui_state == GUISTATE_PLAYMENU && CURRENT_GUI_MODE == GUIMODE_MENU)

/* WRONG: copied straight from 200d/50d via R5 */
// Definitely wrong / hacks / no testing at all:
#define IMGPLAY_ZOOM_LEVEL_ADDR (0x2CBC) //wrong

extern int winsys_bmp_dirty_bit_neg;
#define WINSYS_BMP_DIRTY_BIT_NEG MEM(&winsys_bmp_dirty_bit_neg) // faked via function_overrides.c
#define FOCUS_CONFIRMATION 0 // wrong, but safe, no focus lock

#define LV_BOTTOM_BAR_DISPLAYED 0x0 // wrong, fake bool
// this block all copied from 50D (via R5), and probably wrong, though likely safe
#define FASTEST_SHUTTER_SPEED_RAW 160
#define MAX_AE_EV 2
#define FLASH_MAX_EV 3
#define FLASH_MIN_EV -10
#define COLOR_FG_NONLV 80
#define AF_BTN_HALFSHUTTER 0
#define AF_BTN_STAR 2
// another block copied from 50D (via R5)
#define GUIMODE_WB 5
// 9 is the 50D value, unverified on R6. With BGMT_Q defined, a match would make
// Q on the shooting screen open the ML focus menu (menu.c) - off until known.
#define GUIMODE_FOCUS_MODE -1
#define GUIMODE_DRIVE_MODE 8
#define GUIMODE_PICTURE_STYLE 4
#define GUIMODE_Q_UNAVI 0x18
#define GUIMODE_FLASH_AE 0x22
#define GUIMODE_PICQ 6

// all these MVR ones are junk, don't try and record video.
// MVR_190_STRUCT is the R5 address and NOT valid on R6; movie features stay disabled.
#define MVR_190_STRUCT (*(void**)0x1ed8)
#define div_maybe(a,b) ((a)/(b))
#define MVR_BUFFER_USAGE 70 /* obviously wrong, don't try and record video */
#define MVR_FRAME_NUMBER (*(int*)(220 + MVR_190_STRUCT))
#define MVR_BYTES_WRITTEN MEM((212 + MVR_190_STRUCT))

// XCM_New 0xE0174FA4 allocates the XCM on the heap, so there is no fixed address.
// XCM_GetContext 0xE0175274 returns xcm + idx*0x7A0 + 0x10; context 0 = XCM + 0x10 (as on R5).
#define XCM_PTR *(unsigned int *)0x14B2D8   // _pXCM
#define XIMR_CONTEXT ((unsigned int *)(XCM_PTR + 0x10))
