#ifndef _platform_lens_h
#define _platform_lens_h

/* Digic 8 brings new properties:
 * PROP_LENS_STATIC_DATA  = PROP_LENS + PROP_LENS_NAME + ???
 * PROP_LENS_DYNAMIC_DATA = PROP_LV_LENS + ???
 *
 * Those are quite huge, they size depend on camera.
 * So far we have data from following models:
 *             M50   SX740    R     RP    SX70   250D   850D   R5     R6 1.9.0
 * STATIC     0x138  0x178  0x184  0x184  0x180  0x180  0x1C8  0x1C8  0x1D0
 * DYNAMIC    0x84   0x8C   0x90   0x90   0x90   0x8C   0x90   0x94   0x8C
 * DryOS ICU   P2     P3     P4     P4     P4     P5     P8     P9
 *
 * A lot of PROP_LENS_STATIC_DATA can be decoded via `readid` evshell function.
 * Some can be decoded via ShootingInfoEx (models like SX70 don't have readid...)
 *
 * R6 1.9.0 (M10.1, docs/m10_plan.md section 1): the layouts below replace the R5.152 copy. Evidence (ROM0 1.9.0):
 *  - registration: PROP_LENS_DYNAMIC_DATA 0x80020037 -> master buffer 0x127B88, 0x8C bytes (0xE058A96A..0xE058A974);
 *    PROP_LENS_STATIC_DATA 0x8003009A -> 0x1279B8, 0x1D0 bytes (0xE058A8EC, 0xE058A922..0xE058A928);
 *    "LensDynamicInfo Size:%x LensStaticInfo Size:%x" with 0x8C / 0x1D0 (0xE0316354 / 0xE031636E)
 *  - dynamic field names: Canon's debug print of the structure (0xE0585B3E: "AVEF", "[FL]%d [NearAbs(cm)]%d
 *    [NearInf(cm)]%d [HRFL0.1]%d", "st1..st5", ...), ShootingInfoEx (0xE031550E..0xE03155B0: avo, avmax, abs_inf,
 *    abs_near, fine_focus_pos, ts_*), LensIf_GetFlAndStatusUnCache 0xE0749442 (FL, st1..st5, FcsSt, ZmSt), the FL
 *    getter 0xE0749A40, LensIf_GetISSwitch 0xE0749ADC (st3 bits 2 / 1)
 *  - static field offsets: Canon's `readid` print (0xE0584718..0xE0584E06, base r4 = 0x1279B8 + 0xDE), adapter
 *    firmware check 0xE0749B3C ([+0x1A9] == 1, dlAdp_count); equal to the R5 layout up to 0xF0, then +2 / +4
 *  - camera logs M9.1..M9.3 (Sigma 35 mm F1.4 Art): FL [0x40] = 35, far / near [0x44] / [0x46] in cm, focusPos
 *    [0x4A] 0x07..0x1E, st2 0x80, HRFL [0x7A] = 350; static [0xF6] = 0x1E (focus_pos_size), [0x1C0] = 350
 * Natural alignment (no packed): every u16 is even, every u32 a multiple of 4. The checks at the end make the
 * compiler prove the size and the offsets ML reads. Field names as before (src/lens.c reads them).
 */
struct prop_lens_static_data
{
        uint8_t                 attached;                      // 0x000
        uint8_t                 type;                          // 0x001: 90 EF, 91 RF (R6 log: 0x81)
        uint8_t                 LENSID[38];                    // 0x002 LENSID
        uint16_t                lens_id;                       // 0x028: and yes, this one is lens_id...
        uint16_t                lens_id_ext;                   // 0x02A
        uint16_t                fl_wide;                       // 0x02C
        uint16_t                fl_tele;                       // 0x02E
        uint8_t                 lens_serial[5];                // 0x030
        uint8_t                _unk_02[24];                    // 0x035: not referenced in readid
        uint8_t                 extender_id[6];                // 0x04D (R6 log without extender: 0xFF)
        uint8_t                 lens_firm_ver[3];              // 0x053
        uint8_t                 field_vision;                  // 0x056
        uint8_t                 lens_type;                     // 0x057
        uint8_t                 extenderMountInfo;             // 0x058
        uint8_t                 lens_name_len;                 // 0x059
        char                    lens_name[73];                 // 0x05A (Canon copies up to 0x8A bytes, 0xE05701FC)
        uint8_t                _unk_03[65];                    // 0x0A3: rest of Canon's 0x8A name capacity
        uint8_t                 mount_size;                    // 0x0E4
        uint8_t                 lens_switch_exists;            // 0x0E5
        uint8_t                 lens_af_func_exists;           // 0x0E6
        uint8_t                 lens_mf_funct_exists;          // 0x0E7
        uint8_t                 lens_is_switch_exists;         // 0x0E8
        uint8_t                 lens_is_funct_exists;          // 0x0E9
        uint8_t                 af_speed_setting_available;    // 0x0EA
        uint8_t                 dafLimitFno;                   // 0x0EB
        uint8_t                 distortionCorrectionInfo;      // 0x0EC
        uint8_t                 bcfInfo;                       // 0x0ED
        uint8_t                 lens_id_1292;                  // 0x0EE
        uint8_t                 emd_hot_limit;                 // 0x0EF
        uint8_t                 aberationControl;              // 0x0F0: readid [r4+0x12]
        uint8_t                 popeye_info;                   // 0x0F1: R6 1.9.0, readid [r4+0x13]
        uint8_t                 mlSupportCinemaLens;           // 0x0F2: R6 1.9.0, readid [r4+0x14], GUI copy +0x7EE
        uint8_t                _pad_01;                        // 0x0F3
        uint16_t                zoom_pos_size;                 // 0x0F4: readid [r4+0x16] (R5: 0xF2)
        uint16_t                focus_pos_size;                // 0x0F6 (R6 log: 0x1E)
        uint16_t                fine_focus_size;               // 0x0F8
        uint16_t                av_stop_div;                   // 0x0FA
        uint8_t                 av_slow_enable;                // 0x0FC
        uint8_t                 av_dlp_lens;                   // 0x0FD
        uint16_t                av_dlp_max_spd;                // 0x0FE
        uint16_t                av_dlp_silent_spd;             // 0x100
        uint16_t                av_dlp_min_spd;                // 0x102
        uint8_t                _unk_04[150];                   // 0x104
        uint8_t                 extendMagnificationVal;        // 0x19A: readid [r4+0xBC]
        uint8_t                _unk_05;                        // 0x19B
        uint16_t                ois_shift_max;                 // 0x19C
        uint8_t                 colorBalance;                  // 0x19E
        uint8_t                 pza_exists;                    // 0x19F
        uint8_t                 pza_id[5];                     // 0x1A0
        uint8_t                 pza_firm_ver[3];               // 0x1A5
        uint8_t                 pza_firmup;                    // 0x1A8
        uint8_t                 dlAdp_count;                   // 0x1A9: 0xE0749B3C "count isn't 1"
        uint8_t                _pad_02[2];                     // 0x1AA
        uint32_t                dlAdp1_id;                     // 0x1AC
        uint8_t                 dlAdp1_func1;                  // 0x1B0
        uint8_t                 dlAdp1_firm_ver[3];            // 0x1B1
        uint32_t                dlAdp2_id;                     // 0x1B4
        uint8_t                 dlAdp2_func1;                  // 0x1B8
        uint8_t                 dlAdp2_firm_ver[3];            // 0x1B9
        uint8_t                _unk_06;                        // 0x1BC
        uint8_t                 demandWarnDispFromLens;        // 0x1BD: writer 0xE05700D2
        uint8_t                 demandWarnDispFromAdp;         // 0x1BE: writer 0xE05701AE
        uint8_t                _unk_07;                        // 0x1BF
        uint16_t                high_res_focal_length_tele;    // 0x1C0: R6 1.9.0, 0.1 mm (readid ldrh [r4,#0xe2]!; log 350)
        uint16_t                high_res_focal_length_wide;    // 0x1C2: R6 1.9.0, 0.1 mm (log 350)
        uint8_t                _unk_08[12];                    // 0x1C4
};

SIZE_CHECK_STRUCT( prop_lens_static_data, 0x1D0 );

/*
 * PROP_LENS_DYNAMIC_DATA structure, R6 1.9.0 (0x8C bytes)
 *
 * Canon's own print of this structure names most fields: _prints_dynamic_data(dynamic*, static*) = 0xE0585B3E
 * ("AVEF:0x%x"...). ShootingInfoEx (0xE031550E) embeds a copy ("ShootingInfoEx:LensDynamicInfo avef:%x"...).
 * The camera fills 0x60 (flag), st2 bit 7 without a lens (0xE057036C), st4 bit 0 and [0x88] = 0 (0xE057037A)
 * after the memcpy of the lens process' block (0xE05702A4).
 * Code768 = APEX * 768 (Canon's own conversion from ML-style raw: (raw - 8) * 96, 0xE05702F4).
 */
struct prop_lens_dynamic_data {
        int16_t                 AVEF;             // 0x00: Code768
        int16_t                 AVO;              // 0x02: ShootingInfoEx avo, widest aperture, Code768 (log 0x300 = F1.4)
        int16_t                 AVMAX;            // 0x04: ShootingInfoEx avmax, smallest aperture, Code768 (log 0x1800 = F16)
        int16_t                 AVD;              // 0x06: GUI copy +0x8D2
        int16_t                 NowAvRF;          // 0x08 (0xDF00 = none)
        int16_t                 NowAvEF;          // 0x0A
        uint8_t                _unk_01[4];        // 0x0C
        int16_t                 AVEF_image;       // 0x10
        int16_t                 AVO_image;        // 0x12
        uint16_t                jsstep;           // 0x14
        uint8_t                _unk_02[4];        // 0x16
        uint16_t                IDC;              // 0x1A
        uint8_t                _unk_03[2];        // 0x1C: byte 0x1C checked by the update path (0xE05703B0)
        uint16_t                po;               // 0x1E
        uint16_t                po05;             // 0x20
        uint16_t                po10;             // 0x22
        uint16_t                po00;             // 0x24
        uint16_t                po15;             // 0x26
        uint16_t                Npo;              // 0x28
        uint16_t                po0;              // 0x2A
        uint16_t                po25;             // 0x2C
        uint16_t                po50;             // 0x2E
        uint16_t                po75;             // 0x30
        uint16_t                po100;            // 0x32
        uint16_t                po125;            // 0x34
        uint16_t                po150;            // 0x36
        uint16_t                po175;            // 0x38
        uint16_t                po200;            // 0x3A
        uint8_t                 v0;               // 0x3C: vignetting
        uint8_t                 v1;
        uint8_t                 v2;
        uint8_t                 v3;
        uint16_t                FL;               // 0x40: focal length in mm ("[FL]%d", FL getter 0xE0749A40)
        uint16_t                focal_len_image;  // 0x42: ShootingInfoEx focal_length_image
        uint16_t                focus_far;        // 0x44: cm, "[NearInf(cm)]", ShootingInfoEx abs_inf
        uint16_t                focus_near;       // 0x46: cm, "[NearAbs(cm)]", ShootingInfoEx abs_near
        uint16_t                zoomPos;          // 0x48: ShootingInfoEx zoom_pos
        uint16_t                focusPos;         // 0x4A (log 0x07..0x1E, falls with the distance)
        uint16_t                fineFocusPos;     // 0x4C: ShootingInfoEx fine_focus_pos
        uint16_t                HighResoZoomPos;  // 0x4E: ShootingInfoEx high_res_zoom_pos
        uint16_t                HighResoFocusPos; // 0x50: ShootingInfoEx high_res_focus_pos
        uint8_t                _unk_04[8];        // 0x52
        int8_t                  abstat;           // 0x5A
        uint8_t                 st1;              // 0x5B
        uint8_t                 st2;              // 0x5C: bit 7 set without a lens (0xE057036C); log 0x80 with MF [assumed: MF]
        uint8_t                 st3;              // 0x5D: IS bits 2 / 1 (LensIf_GetISSwitch 0xE0749ADC)
        uint8_t                 st4;              // 0x5E: bit 0 set by the camera (0xE0570384)
        uint8_t                 st5;              // 0x5F
        uint8_t                _unk_05[3];        // 0x60: 0x60 set by the camera, log 0x62 = 1
        uint8_t                 truePfOn;         // 0x63
        uint8_t                 FcsSt1;           // 0x64: FocusStep?
        uint8_t                 FcsSt2;
        uint8_t                 FcsSt3;
        uint8_t                 FcsSt4;
        uint8_t                 ZmSt1;            // 0x68: ZoomStep?
        uint8_t                 ZmSt2;
        uint8_t                 ZmSt3;
        uint8_t                 ZmSt4;
        uint8_t                _unk_06[4];        // 0x6C
        int16_t                 ts_shift;         // 0x70: via ShootingInfoEx
        int8_t                  ts_tilt;          // 0x72: via ShootingInfoEx
        int8_t                  ts_all_revo;      // 0x73: via ShootingInfoEx
        int8_t                  ts_ts_revo;       // 0x74: via ShootingInfoEx
        uint8_t                _unk_07[5];        // 0x75 (log 0x75 = 0x55, 0x79 = 0xA2 / 0x82)
        uint16_t                HRFL;             // 0x7A: focal length in 0.1 mm ("[HRFL0.1]%d", log 350)
        int8_t                  temperature;      // 0x7C: -128 = no value
        uint8_t                _unk_08[3];        // 0x7D
        uint32_t                LENSEr;           // 0x80 (ldr.w, 32 bit)
        uint32_t               _unk_09;           // 0x84
        uint32_t               _cam_88;           // 0x88: set to 0 by the camera (0xE057037A)
};

SIZE_CHECK_STRUCT( prop_lens_dynamic_data, 0x8C );

/* the offsets ML reads (src/lens.c) and a few anchors of the shifted part, checked by the compiler */
#define R6_LENS_OFS(s, f, o) typedef char r6_lens_ofs_##s##_##f[__builtin_offsetof(struct prop_lens_##s##_data, f) == (o) ? 1 : -1]
R6_LENS_OFS(dynamic, AVO, 0x02);
R6_LENS_OFS(dynamic, AVMAX, 0x04);
R6_LENS_OFS(dynamic, FL, 0x40);
R6_LENS_OFS(dynamic, focus_far, 0x44);
R6_LENS_OFS(dynamic, focus_near, 0x46);
R6_LENS_OFS(dynamic, focusPos, 0x4A);
R6_LENS_OFS(dynamic, st2, 0x5C);
R6_LENS_OFS(dynamic, st3, 0x5D);
R6_LENS_OFS(dynamic, ts_shift, 0x70);
R6_LENS_OFS(dynamic, HRFL, 0x7A);
R6_LENS_OFS(dynamic, LENSEr, 0x80);
R6_LENS_OFS(static, lens_id, 0x28);
R6_LENS_OFS(static, fl_wide, 0x2C);
R6_LENS_OFS(static, fl_tele, 0x2E);
R6_LENS_OFS(static, lens_serial, 0x30);
R6_LENS_OFS(static, extender_id, 0x4D);
R6_LENS_OFS(static, lens_name_len, 0x59);
R6_LENS_OFS(static, lens_name, 0x5A);
R6_LENS_OFS(static, mount_size, 0xE4);
R6_LENS_OFS(static, lens_is_funct_exists, 0xE9);
R6_LENS_OFS(static, zoom_pos_size, 0xF4);
R6_LENS_OFS(static, focus_pos_size, 0xF6);
R6_LENS_OFS(static, extendMagnificationVal, 0x19A);
R6_LENS_OFS(static, dlAdp_count, 0x1A9);
R6_LENS_OFS(static, dlAdp1_id, 0x1AC);
R6_LENS_OFS(static, demandWarnDispFromAdp, 0x1BE);
R6_LENS_OFS(static, high_res_focal_length_tele, 0x1C0);
#undef R6_LENS_OFS

/* M10.1 (CONFIG_R6_LENS_DATA, src/lens.c): what ML's lens handlers accepted and rejected, for the Expo log
 * (lvprobe.c expo.lensml). Written only in Canon's property task by the handlers (plain stores), read by the log. */
struct r6_lens_dbg
{
        uint32_t                dyn_n, dyn_bad;         /* PROP_LENS_DYNAMIC_DATA: accepted / length != 0x8C */
        uint32_t                stat_n, stat_bad;       /* PROP_LENS_STATIC_DATA: accepted / length != 0x1D0 */
        uint32_t                av_n, av_bad;           /* PROP_APERTURE: decoded / ignored (length != 2, no ML code) */
        uint32_t                aa_n, aa_bad;           /* PROP_APERTURE_AUTO: the same */
        uint16_t                near, far;              /* last accepted dynamic data: [0x46] / [0x44] in cm */
        uint16_t                av_last, aa_last;       /* last 2-byte value delivered (Code768) */
        uint8_t                 st2, st3;               /* last accepted dynamic data: [0x5C] / [0x5D] */
        uint8_t                 ext;                    /* last accepted static data: extender_id[0] as delivered */
        uint8_t                 fl_bad;                 /* 1: the last FL was rejected (> 2000 mm) */
};
extern struct r6_lens_dbg r6_lens_dbg;

#endif
