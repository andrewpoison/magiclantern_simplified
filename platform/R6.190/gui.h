#ifndef _cameraspecific_gui_h_
#define _cameraspecific_gui_h_

/* R6 1.9.0
 *
 * event->param of type-0 events in gui_main_task (GuiMainQueue, gui_main_struct+0x1C).
 * Source: UiOpe switch callbacks (0xE0108972..0xE01098C4, "Sw*_Button[%d]" strings),
 * posting via 0xE09BD6FC -> 0xE09BD750 -> gui_enqueue_message(0, code, 0, arg).
 * Buttons: press = even code, unpress = code + 1; arg is 1 in both cases.
 * Dials: one code per direction, arg = number of clicks.
 * Cross-checked with gui_massive_event_loop 0xE059193C (jump table 0xE0592208) and
 * the dialog event names it produces. Same values as R5 1.5.2 where R5 defines them.
 */

#define BGMT_WHEEL_DOWN              0x00 // QCD2 (rear, around SET), SwSub_Dial delta<0
#define BGMT_WHEEL_UP                0x01 // SwSub_Dial delta>0; arg = number of clicks

// 0x02/0x03: SUB2 dial, no switch callback exists on R6
// 0x04/0x05: QCD1 (top, "3rd dial"), Sw3rd_Dial; not used by ML
// 0x06/0x07: RF lens control ring, SwLensRing; not used by ML

#define BGMT_WHEEL_LEFT              0x08 // main dial, SwMain_Dial delta<0
#define BGMT_WHEEL_RIGHT             0x09 // SwMain_Dial delta>0

#define BGMT_PRESS_SET               0x0A
#define BGMT_UNPRESS_SET             0x0B
#define BGMT_MENU                    0x0C // UNPRESS 0x0D
#define BGMT_INFO                    0x0E // UNPRESS 0x0F

#define BGMT_PLAY                    0x12 // UNPRESS 0x13
#define BGMT_TRASH                   0x14 // UNPRESS 0x15, opens ML menu

#define BGMT_PRESS_ZOOM_IN           0x1C // magnify button (SwScale)
#define BGMT_UNPRESS_ZOOM_IN         0x1D

// M-Fn 0x22/0x23, movie REC 0x2D/0x2E, AF-ON 0x46/0x47,
// AE lock (*) 0x48/0x49, AF point select 0x4A/0x4B: not used by ML
// AF stop (lens) 0x4E/0x4F, SwAfStop_ButtonCBR 0xE010907E: Canon reports the
// released state (0x4F) once at startup (seen on camera); not used by ML

#define BGMT_Q                       0x2B // SwUNAVI; dialog PRESS_UNAVI_BUTTON
#define BGMT_UNPRESS_Q               0x2C

#define BGMT_RATE                    0x32 // UNPRESS 0x33; in ML menu remapped to Q

/* multi-controller (SwDirectionKeyCBR 0xE0108BAA) */
#define BGMT_UNPRESS_UDLR            0x76 // release, any direction and centre
#define BGMT_PRESS_UP                0x77
#define BGMT_PRESS_UP_RIGHT          0x78
#define BGMT_PRESS_UP_LEFT           0x79
#define BGMT_PRESS_RIGHT             0x7A
#define BGMT_PRESS_LEFT              0x7B
#define BGMT_PRESS_DOWN_RIGHT        0x7C
#define BGMT_PRESS_DOWN_LEFT         0x7D
#define BGMT_PRESS_DOWN              0x7E
#define BGMT_JOY_CENTER              0x7F // press; Canon maps it to PRESS_MLT_CENTER (= SET)

#define BGMT_PRESS_HALFSHUTTER       0x9F // GUICMD_SW1_ON; +1 SW1 off, +2 SW2 on, +3 SW2 off

// needed for correct shutdown from powersave modes
#define GMT_GUICMD_LOCK_OFF          0xAF // GUICMD_LOCK_OFF
#define GMT_GUICMD_OPEN_SLOT_COVER   0xB1 // GUICMD_OPEN_SLOT_COVER
#define GMT_GUICMD_START_AS_CHECK    0xB6 // GUICMD_START_AS_CHECK

#define GMT_OLC_INFO_CHANGED         0xB7 // copyOlcDataToStorage uiCommand(%d)

/* Codes below are WRONG: DNE in R6 */
#define BGMT_LV                      -0xF3 // MILC, always LV.

//#define BGMT_PRESS_ZOOM_OUT          -0xF6
//#define BGMT_UNPRESS_ZOOM_OUT        -0xF7
#endif
