/** \file
 * Entry points into the firmware image.
 *
 * These are the functions that we can call from our tasks
 * in the Canon 1.9.0 firmware for the EOS R6
 *
 * Addresses found in the 1.9.0 ROM dump and independently verified,
 * see docs/stubs_r6_190.md in the porting workspace.
 */
/*
 * Copyright (C) 2026 Magic Lantern Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the
 * Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include <stub.h>

/* All functions are Thumb. */


/** Startup **/
NSTUB(MAIN_FIRMWARE_ADDR, firmware_entry)                 // also known as 2nd stage loader, boot2 or DryOS loader
THUMB_FN(0xe07d4e60,  dcache_clean)                       // call from firmware_entry() at 0xE0100058 / 0xE010008E; loop with MCR p15 c7,c10,1; DSB
THUMB_FN(0xe07d4f34,  icache_invalidate)                  // call from firmware_entry() at 0xE0100062 / 0xE0100098; loop with MCR p15 c7,c5,1; c7,c1,6; c7,c1,0; ISB
THUMB_FN(0xe01000ec,  cstart)                             // call from firmware_entry() at 0xE0100022 (via BR_BR_CSTART); calls bzero32 and create_init_task(..., init_task, ...)
THUMB_FN(0xe07db168,  bzero32)                            // call from cstart() at 0xE0100138; zeros out a data structure
THUMB_FN(0xe0277618,  create_init_task)                   // call from cstart() at 0xE010019A only on core0; low-level DryOS initialization
THUMB_FN(0xe010021e,  init_task)                          // 2nd param to create_init_task() (0xE010021F); USER_MEM size checking, dmSetup, termDriverInit...

/** Startup - Digic X specials **/
THUMB_FN(0xe0005b18,  fixup_boot_memory)                  // called from 1st stage loader main 0xE000590E just before boot decision; reloads "CMIF" CamIF core blobs (ROM 0xE3E40000) via DMA

/** Tasks **/
DATA_PTR(    0x1010,  current_interrupt)                  // from interrupt handler (VBAR 0xE02FE320 + 0x18 -> 0xE022E018 str irq_id,[0x1010]); "ASSERT : Vector 0x%lx"
DATA_PTR(    0x1028,  current_task)                       // from task switch/scheduler start 0xE07C16C2 str task,[0x1028]; pointer to the current task structure
DATA_PTR(    0x4100,  task_max)                           // entry in SysConfig struct 0x40EC+0x14 (0xE01652F0 copies dryos_init_info.task_max); used by task pool init 0xE011E810
THUMB_FN(0xe07c1cda,  msleep)                             // SystemIF::KerSys.c; registered as eventproc "SleepTask" (0xE011271A)
THUMB_FN(0xe07b93a6,  task_create)                        // SystemIF::KerTask.c; used to start TaskMain (0xE0100B46), GuiMainTask (0xE04DE4C6)

/** File I/O **/
THUMB_FN(0xe07b3676,  _FIO_OpenFile)                      // Storage::Storage.c Open
THUMB_FN(0xe07b3736,  _FIO_CreateFile)                    // Storage::Storage.c Create
THUMB_FN(0xe07b38a8,  _FIO_RemoveFile)                    // Storage::Storage.c Remove
THUMB_FN(0xe07b393c,  _FIO_ReadFile)                      // Storage::Storage.c Read
THUMB_FN(0xe07b3d5e,  FIO_SeekSkipFile)                   // Storage::Storage.c Search
THUMB_FN(0xe07b3e24,  _FIO_WriteFile)                     // Storage::Storage.c Write
THUMB_FN(0xe07b459a,  FIO_CloseFile)                      // Storage::Storage.c Close
THUMB_FN(0xe07b47b2,  _FIO_GetFileSize64)                 // Storage::Storage.c AcqSize
THUMB_FN(0xe07b51e4,  _FIO_RenameFile)                    // Storage::Storage.c Rename
THUMB_FN(0xe07b5608,  _FIO_CreateDirectory)               // Storage::Storage.c CreateDir
THUMB_FN(0xe07b591a,  FIO_Flush)                          // Storage::Storage.c Flush
THUMB_FN(0xe07b5afc,  _FIO_FindFirstEx)                   // Storage::Storage.c FirstEnt
THUMB_FN(0xe07b5c0c,  FIO_FindNextEx)                     // Storage::Storage.c NextEnt
THUMB_FN(0xe07b5ccc,  FIO_FindClose)                      // Storage::Storage.c CloseEnt

/** Debug messages **/
THUMB_FN(0xe07da350,  DryosDebugMsg)                      // r0=class r1=level r2=format; e.g. "InitializeIntercom (%#x)" at 0xE0100AE4

/** Eventprocs (call by name) **/
THUMB_FN(0xe07c715e,  call)                               // ExecuteEventProcedure, registered at 0xE0112722

/** M8.1: CameraConductor callbacks of the physical shutter switch (approved 2026-10-03) **/
THUMB_FN(0xe0296518,  CC_SW1_CBR)                         // MPU switch table 0xE0FE5C84 row {7, 0xE0296519} @0xE0FE5CA4, registered by CC init 0xE028854E -> 0xE029803C -> loop 0xE0298014 -> 0xE06D315E -> 0xE033A9B8 (ccid 7 -> slot 0, MPU key 5 = SW1); strb r0,[[0x10AB8]+0xDF], post 0x20000400 (r0 == 1) else 0x20000800 via 0xE0287C2E
THUMB_FN(0xe029652e,  CC_SW2_CBR)                         // same table, row {8, 0xE029652F} @0xE0FE5CAC (ccid 8 -> slot 1, MPU key 6 = SW2); strb r0,[[0x10AB8]+0xE0], post 0x20001000 (r0 == 1) else 0x20002000 via 0xE0287C2E

/** GUI timers **/
THUMB_FN(0xe08587d8,  SetHPTimerAfterNow)                 // System::HPTimer.c; see "comAdapterDelayMicroSecond : SetHPTimerAfterNow" (0xE067A574)
THUMB_FN(0xe085885e,  SetHPTimerNextTick)                 // System::HPTimer.c; next function, same worker (e085870c) as SetHPTimerAfterNow
THUMB_FN(0xe07c61e8,  SetTimerAfter)                      // SystemIF::Timer1m.c
THUMB_FN(0xe07c629e,  CancelTimer)                        // SystemIF::Timer1m.c block; CancelRefreshTimer (0xE0B835D4)

/** Memory allocation **/
THUMB_FN(0xe07d54d4,  _AllocateMemory)                    // NG AllocateMemory
THUMB_FN(0xe07d568c,  _FreeMemory)                        // uses the same heap [0x5348]; wrapper -> 0xE07D556A
THUMB_FN(0xe07d535e,  GetMemoryInformation)               // called from _AllocateMemory(); "[MEM] ERROR GetMemoryInformation"
THUMB_FN(0xe07d5332,  GetSizeOfMaxRegion)                 // called from _AllocateMemory(); "[MEM] ERROR GetSizeOfMaxRegion"
THUMB_FN(0xe07c33cc,  _alloc_dma_memory)                  // allocates 64 bytes more, aligns at 32 and sets the uncacheable bit (SystemIF::Cache.c)
THUMB_FN(0xe07c33d2,  _free_dma_memory)                   // paired with the above; clears the uncacheable bit and frees the raw pointer
THUMB_FN(0xe0a0b674,  _malloc)                            // Just before conditional uart_printf("can't malloc(%d)") (0xE029B65E)
THUMB_FN(0xe0a0b6ac,  _free)                              // After malloc() in ROM; same pool 0xFCF48

/** Memory info **/
THUMB_FN(0xe07b2ef8,  res_memshow)                        // DIGIX X replacement of smemShowFix(); eventproc "res_memshow", arg is POINTER to bitmask (1/2/4)
THUMB_FN(0xe02edfbc,  memmap_info)                        // Exception vector, DRYOS system memory etc
THUMB_FN(0xe0327abc,  malloc_info)                        // Malloc Information
THUMB_FN(0xe0327b54,  sysmem_info)                        // System Memory Information

/** Misc **/
THUMB_FN(0xe07c3318,  uart_printf)                        // in init task, prints "%s READY\n" (0xE010075C)
THUMB_FN(0xe0123774,  vsnprintf)                          // called by LogFile_printf(..., "*Data Lost*\n", ...) at 0xE01242EA

/** Tasks (2) **/
DATA_PTR(    0x413c,  task_dispatch_hook)                 // called in task switch 0xE07C1396/0xE07C13FE as hook(reason, prev_task, next_task) before current_task is updated
THUMB_FN(0xe011e900,  task_trampoline)                    // task_create -> e03c432c -> prepare_task_trampoline (e011e920) passes task_trampoline+1 to e014258e
THUMB_FN(0xe02cac34,  _get_task_info_by_id)               // called just before " CORE : %d\n" in exception dump (0xE01AEAAE); fills task_attr V5

/** Recursive locks **/
THUMB_FN(0xe07d5cc8,  CreateRecursiveLock)                // SystemIF::KerMisc.c; called with "PCOMNET_WINE CBMan" (0xE0AC58D4)
THUMB_FN(0xe07c1f08,  AcquireRecursiveLock)               // SystemIF::KerRLock.c
THUMB_FN(0xe07c1fd2,  ReleaseRecursiveLock)               // SystemIF::KerRLock.c

/** Semaphores **/
THUMB_FN(0xe07d9eb4,  create_named_semaphore)             // SystemIF::KerSem.c; init_task calls it with "PU0Wait", "PU1Wait", "CCInit" (0xE0100B4E)
THUMB_FN(0xe07da02a,  give_semaphore)                     // SystemIF::KerSem.c, lines 0x162/0x167
THUMB_FN(0xe07d9fb0,  take_semaphore)                     // SystemIF::KerSem.c, lines 0x133/0x136
THUMB_FN(0xe07d9f80,  take_semaphore_now)                 // SystemIF::KerSem.c; take with zero timeout, just before take_semaphore

/** GUI **/
DATA_PTR(    0x5460,  gui_main_struct)                    // via string "QueueClearFlg Clear", accessed by that function; queue at +0x1C
DATA_PTR(   0x182ac,  gui_task_list)                      // via WINSYS_CreateController() (0xE0A9E8E6)
THUMB_FN(0xe04de3f8,  gui_main_task)                      // via task_create("GuiMainTask"...) in UiOpe_Initialize (0xE04DE4C4)
THUMB_FN(0xe04de52e,  gui_enqueue_message)                // via string "warning! QueueLength=" and "GUI::GUI.c" in one function
THUMB_FN(0xe059193c,  gui_massive_event_loop)             // various "GUICMD_"* strings; tail-called from GMT_FUNCTABLE[0]
THUMB_FN(0xe079a6c0,  SetGUIRequestMode)                  // by debug message "SetGUIRequestMode(0x%08x)". New style (RP+)
// gui_init_end and GUI_Control DNE. Implemented via function_overrides.

/** Dialog API **/
THUMB_FN(0xe078ded8,  dialog_redraw)                      // WINSYS_CreateDialogBox (0xE078DEAC)+0x2C like R5; checks pDialog signature (Dialog.c line 0x2BA)
THUMB_FN(0xe0799b50,  dialog_set_property_str)            // call with "RF-TEST" as 3rd arg (0xE0446DC0)

/** GUI timers (2) **/
THUMB_FN(0xe09e2e2e,  StartDateTimer)                     // by debug message "MenuMain.c StartDateTimer(%#x)"
THUMB_FN(0xe09e2eea,  CancelDateTimer)                    // by debug message "CancelDateTimer"

/** Queue **/
THUMB_FN(0xe07b8f4c,  msg_queue_create)                   // SystemIF::KerQueue.c; used in UiOpe_Initialize() with "GuiMainQueue" (0xE04DE4B0)
THUMB_FN(0xe07b9010,  msg_queue_receive)                  // SystemIF::KerQueue.c; used in gui_main_task() (0xE04DE416)
THUMB_FN(0xe07b916e,  msg_queue_post)                     // SystemIF::KerQueue.c; used in gui_enqueue_message() (0xE04DE562)
THUMB_FN(0xe07b91a4,  msg_queue_count)                    // SystemIF::KerQueue.c; before "warning! QueueLength (%d)" (0xE0A9E110)

/** Memory **/
THUMB_FN(0xe07e5b40,  AllocateMemoryResource)             // System::RscWrap.c, dispatch via [0x527C+0]; "[RD ERROR] AllocateMemoryResource failed" after call
THUMB_FN(0xe07e5b6e,  AllocateContinuousMemoryResource)   // System::RscWrap.c, dispatch via [0x527C+4]; "AllocateContinuousMemoryResource failed" after call
THUMB_FN(0xe07e5b9c,  FreeMemoryResource)                 // System::RscWrap.c, dispatch via [0x527C+8]
THUMB_FN(0xe07bc480,  GetFirstChunkFromSuite)             // In ConcatMemorySuite-like fn 0xE07BCDC4, first function called; PackMem.c
THUMB_FN(0xe07bcd02,  GetNextMemoryChunk)                 // In ConcatMemorySuite-like fn 0xE07BCDC4, called in loop with param2 as 1st arg
THUMB_FN(0xe07bc222,  GetMemoryAddressOfMemoryChunk)      // uses chunk from GetNextMemoryChunk(); returns [chunk+0x18]
THUMB_FN(0xe07bbeae,  CreateMemoryChunk)                  // just after GetMemoryAddressOfMemoryChunk() in 0xE07BCDC4; also called from CreateMemorySuite(); malloc(0x30)
THUMB_FN(0xe07bc3fc,  AddMemoryChunk)                     // just after CreateMemoryChunk() in 0xE07BCDC4; appends to tail
THUMB_FN(0xe07bc8c0,  CreateMemorySuite)                  // One of few xrefs of CreateMemoryChunk(); "System::PackMem.c", calls malloc(0x18)
THUMB_FN(0xe07bc630,  DeleteMemorySuite)                  // DebugMsg "DeleteMemorySuite 0x%x"
// disabled using CONFIG_MEMORY_SRM_NOT_WORKING
// DIGIC X does not use RscMgr anymore, those are dummy functions that just assert!
//THUMB_FN(0xe080bb28,  SRM_AllocateMemoryResourceFor1stJob)// DUMMY on DIGIC X: asserts in Resource::RscMgrOldApiStub.c line 43 - keep disabled (CONFIG_MEMORY_SRM_NOT_WORKING)
//THUMB_FN(0xe080bb32,  SRM_FreeMemoryResourceFor1stJob)    // DUMMY on DIGIC X: asserts in Resource::RscMgrOldApiStub.c line 45 - keep disabled

/** Uncategorized **/
DATA_PTR(   0x1f518,  sd_device)                          // From SD_DeviceCreate: (0xE01E3072), array of 2 device ptrs indexed by StorageID
THUMB_FN(0xe0a84d60,  _LoadCalendarFromRTC)               // Used in GUI_SetStartMovieRecDate() (0xE08CD214)
THUMB_FN(0xe0bfdda4,  ErrForCamera_handler)               // via debug msg: "DlgErrForCamera.c TERMINATE_WINSYS"

/** Properties **/
THUMB_FN(0xe07c2d30,  _prop_cleanup)                      // PropertyMgr.c cmd 5 (token, prop); prop_register_slave+0x5AA like R5/850D/M6II
THUMB_FN(0xe07c2786,  prop_register_slave)                // PropertyMgr.c cmd 3, copies propID list (list,count,handler,priv,token_handler)
THUMB_FN(0xe07c2854,  _prop_request_change)               // PropertyMgr.c cmd 6 (prop,addr,len); called after "update inner version string. \"%s\"" (0xE01015DC)

/** General FEATURE_VRAM_RGBA stubs **/
//THUMB_FN(0xe0260a16,  VMIX_TransferRectangleToVram)       // via debug message "VMIX_TransferRectangleToVram %d %d"; not needed - reference for winsys_sem/XimrExe/_pXCM
//THUMB_FN(0xe0233cc6,  RENDERER_InitializeScreen)          // via debug message "RENDERER_InitializeScreen %#x, %d, %#x"; not needed - reference for _rgb_vram_info
DATA_PTR(   0x18408,  winsys_sem)                         // Used in VMIX_TransferRectangleToVram() around XimrExe call (take_semaphore([0x18404+4],5000)); created as "Winsys_Wait_v" in VMIX_InitializeScreen
DATA_PTR(   0x183d4,  _rgb_vram_info)                     // in RENDERER_InitializeScreen(), written just after MARVs are created / "JediWrapperClearSurface vram:%d" (0x183C4+0x10)
THUMB_FN(0xe01a2688,  XimrExe)                            // In VMIX_TransferRectangleToVram(), just before "XimrExe" debug message; Graphic::XimrApiSub.c

/**
 * Things needed for CONFIG_COMPOSITOR_XCM.
 *
 * Like R5: two RGBA layers of 2048x1080, LCD area 720x480 at (152,30)
 * (WINSYS_SetRGBAVramActualSizeAndOffset 0xE078C82C).
 * With CONFIG_COMPOSITOR_XCM we ask XCM via XCM_GetSourceSurface() for layer 0.
 */
THUMB_FN(0xe01755b0,  XCM_GetSourceSurface)               // by debug message "XCM_GetSourceSurface layerIndex = %d"
//THUMB_FN(0xe01750fa,  XCM_MakeContext)                    // by debug message "XCM_MakeContext Illegal handle"; not used in code, reference for _pXCM
DATA_PTR(  0x14b2d8,  _pXCM)                              // param1 to XCM_MakeContext() when called from VMIX_TransferRectangleToVram() (ldr =0x14B2E4; subs #12)

/** Wrong on purpose **/
DATA_PTR(       0x0,  LCD_Palette)                        // DIGIC X has no palette

/** App handlers **/
THUMB_FN(0xe0744ce6,  PlayMain_handler)                   // via debug string "PlayMain_StartPlayMain", handler passed to WINSYS_CreateDialogBox
THUMB_FN(0xe08ec40a,  ShootOlcApp_handler)                // via debug string "StartShootOlcApp PushPalette(%d)"
THUMB_FN(0xe05dec48,  LiveViewApp_handler)                // via debug string "StartLiveViewApp(%#x)"
//THUMB_FN(0xe0ba0a10,  LiveViewShutterApp_handler)         // via debug string "StartLiveViewShutterApp"
THUMB_FN(0xe07ef818,  PlayMovieGuideApp_handler)          // via debug string "PlayMovieGuide_Start" (DlgPlayMovieGuide.c), handler passed to WINSYS_CreateDialogBox

/** Dialog pointers of the apps above (result of WINSYS_CreateDialogBox) **/
//DATA_PTR(   0x16de4,  PlayMain_dialog)                    // via debug string "PlayMain_StartPlayMain", result of WINSYS_CreateDialogBox (0x16DD4+0x10)
//DATA_PTR(   0x21300,  ShootOlcApp_dialog)                 // via debug string "StartShootOlcApp PushPalette(%d)", result of WINSYS_CreateDialogBox
DATA_PTR(   0x1624c,  LiveViewApp_dialog)                 // via debug string "StartLiveViewApp(%#x)", result of WINSYS_CreateDialogBox (0x16248+4)
//DATA_PTR(   0x2081c,  LiveViewShutterApp_dialog)          // via debug string "StartLiveViewShutterApp", result of WINSYS_CreateDialogBox

/** UNUSED but referenced elsewhere **/
//THUMB_FN(0xe078deac,  WINSYS_CreateDialogBox)             // Equiv. of CreateDialogBox(); forces layer ID 0; called before "Start*App WINSYS_CreateDialogBox(%#x)" messages
//THUMB_FN(0xe078dec2,  WINSYS_CreateDialogBoxEx)           // CreateDialogBox with extra layer ID arg (6 args); wraps core 0xE078DC58 (8 args); used with layer 1 by StartLiveViewSilentShutterApp
//THUMB_FN(0xe089f256,  MemifWindow_GetIBusAddress)         // Peripheral::MemifWindow.c: CPU addr -> 64-bit IBus addr (r0:r1), asserts addr < 0xC0000000
//THUMB_FN(0xe09499d4,  dump_file)                          // Edet::FmidCtrlDebug.c: saves to "A:/%s" or "B:/%s" ("Card is not mounted"); calls FIO_RemoveFile/CreateFile/WriteFile/CloseFile/Flush
