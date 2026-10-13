/*
 * R6.190 M12.0: "Astro red", mode "ML only" (r6_astro.c, CONFIG_R6_ASTRO; docs/m12_plan.md, spec m12/m120 spec_m120.md
 * Rev. 2, user answers of 2026-10-11 E1 a, E2, E5, E6, E8 c, E9, E13 a): ML's own pixels in red by meaning, and the
 * read-only display probe "Tor 0f" (status word of the LCD / EVF output always, its 13 colour registers only while that
 * output is on and shown, on Canon's display core, inside one IRQ lock). Used by r6_layer.c (the palette, once per
 * conversion) and lvprobe.c (menu entries, ASTnnnnn.LOG, the main log line, the shooting-screen test).
 * Nothing here writes MMIO, Canon RAM or a property. M12.1 (spec m12/m121 Rev. 2): mode "Full" over Tor 2 (E3 a), the
 * display matrix of the LCD / EVF in red, written by r6_astro.c's astro task only; the prototypes are unchanged (the main
 * log line has one field more, "<t2>:<stores>").
 */
#ifndef _r6_astro_h_
#define _r6_astro_h_

#include <stdint.h>

/* r6_layer.c r6l_build_lut, once per conversion: bit 0 "ML only", bit 1 colour-key test active, bits 8..15 the level in %
 * of the output shown (disp_mask 0x10: EVF level, else LCD level); 0 = colours as before */
uint32_t r6_astro_pal(void);
/* the palette entry i (v = indexed2rgb(i)) for the word pal of r6_astro_pal; pure (reads only its own table) */
uint32_t r6_astro_map(uint32_t pal, int i, uint32_t v);

/* lvprobe.c Debug > LV probe "Display regs" (not saved, OFF at boot) */
void r6_astro_probe_enable(int on);
int r6_astro_probe_on(void);
const char * r6_astro_probe_msg(void);
/* lvprobe.c Debug > LV probe "Key test": 1 = started, 0 = busy */
int r6_astro_key_start(void);
const char * r6_astro_key_msg(void);

/* lvprobe.c snapshot capture (read only, no card, no notice, no drawing): the AST text into p (at most cap - 1 bytes,
 * NUL-terminated); returns its length, *trunc = 1 if it did not fit. MMIO only through the probe's read function and
 * only while "Display regs" is ON */
int r6_astro_ast(char * p, int cap, int * trunc);
/* lvprobe.c capture, after r6_astro_ast: the fields of the main log line "astro=ASTnnnnn.LOG,<fields>" (spec 4.1) */
int r6_astro_main(char * s, int n);

/* lvprobe.c (M12.0 spec 4.3, F5): Canon's shooting screen with live view running; dm_want 0 = LCD or EVF, else exactly
 * that display mask. RAM reads only. Also the colour-key test's screen check (LCD: 0x01) */
int lp_shooting(uint32_t dm_want);

#endif /* _r6_astro_h_ */
