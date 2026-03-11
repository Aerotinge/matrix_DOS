#include <stdio.h>
#include <stdlib.h>
#include <dos.h>

#include "keyb.h"
#include "keycodes.h"

#include "txtui.h"

#define VIDEO_INT 0x10
#define SET_MODE 0x00
#define VGA_256_COLOR_MODE 0x13
#define VGA_TEXT_MODE 0x03


#define INPUT_STATUS_1 0x03da
#define VRETRACE 0x08

typedef unsigned char byte;
static unsigned int g_rng;
static byte g_col_active[80];
static signed char g_col_head[80];

void wait_retrace() {
    while ((inp(INPUT_STATUS_1) & VRETRACE));
    while (!(inp(INPUT_STATUS_1) & VRETRACE));
}

void putGVal(unsigned char g) {
	outp(0x03c9, 0x00);
	outp(0x03c9, g);
	outp(0x03c9, 0x00);
}

enum {
    VIDEO_CAP_MDA = 0x01,
    VIDEO_CAP_EGA = 0x02,
    VIDEO_CAP_VGA = 0x04
};

unsigned char get_video_caps(void) {
    union REGS regs;
    unsigned char caps = 0;

    /* MDA text mode is 7. */
    regs.h.ah = 0x0F;
    int86(0x10, &regs, &regs);
    if(((unsigned char)regs.h.al) == 0x07u) {
        caps |= VIDEO_CAP_MDA;
    }

    /* EGA/VGA palette register support. */
    regs.h.ah = 0x12;
    regs.h.bl = 0x10;
    int86(VIDEO_INT, &regs, &regs);
    if(regs.h.bl != 0x10) {
        caps |= VIDEO_CAP_EGA;
    }

    /* VGA/MCGA DAC support. */
    regs.x.ax = 0x1A00;
    int86(VIDEO_INT, &regs, &regs);
    if(regs.h.al == 0x1A) {
        caps |= VIDEO_CAP_VGA;
    }

    return caps;
}

void set_ega_palette_reg(byte reg, byte color6) {
    union REGS regs;
    regs.h.ah = 0x10;
    regs.h.al = 0x00;
    regs.h.bl = reg;
    regs.h.bh = color6 & 0x3F;
    int86(VIDEO_INT, &regs, &regs);
}

void set_mode(byte mode)
{
    union REGS regs;
    regs.h.ah = SET_MODE;
    regs.h.al = mode;
    int86(VIDEO_INT, &regs, &regs);
}

/* set up a 16-shade greenscale palette */
void green_palette() {
    static const byte ega_green_ramp[16] = {
        0x00, 0x00, 0x00, 0x02, 0x02, 0x0A, 0x0A, 0x2A,
        0x2A, 0x1A, 0x1A, 0x12, 0x12, 0x3A, 0x3A, 0x3F
    };
    unsigned char g, gstep;
    int i;
    unsigned char caps;
    /* EGA-only remap */
    caps = get_video_caps();
    if(!(caps & VIDEO_CAP_VGA) && (caps & VIDEO_CAP_EGA)) {
        for(i = 0; i < 16; i++) {
            set_ega_palette_reg((byte)i, ega_green_ramp[i]);
        }
        return;
    }

    outp(0x03c8, 0);

    g = 0x00;
    gstep = 0x3F >> 4;
    for(i = 0; i < 6; i++) {
	putGVal(g);
	g += gstep;
    }
    outp(0x03c8, 20);
    putGVal(g);
    g += gstep;
    outp(0x03c8, 7);
    putGVal(g);
    g += gstep;
    outp(0x03c8, 56);
    for(i = 0; i < 7; i ++) {
	putGVal(g);
	g += gstep;
    }
    /* head of the trail is bright white rather than green */
    outp(0x03c9, 0x3F);
    outp(0x03c9, 0x3F);
    outp(0x03c9, 0x3F);
}

int is_whitespace(char c) {
	return ((c == 0) || (c == 32) || (c == 255));
}

byte fast_rand8() {
	/* 16-bit LCG: cheap and 808x-friendly */
	g_rng = (unsigned int)(g_rng * 25173u + 13849u);
	return (byte)(g_rng >> 8);
}

char rnd_printable() {
	byte r;
	do {
		r = fast_rand8();
	} while((r == 0) || (r == 255) || (r == 32));
	return (char)r;
}

void init_stream_state() {
	int col;
	for(col = 0; col < 80; col++) {
		g_col_active[col] = 1;
		g_col_head[col] = -1;
	}
}

void step(int spawn) {
	int row, col;
	unsigned int spawn_threshold;
	char far *col_base;
	char far *ptr;
	signed char head;
	unsigned char currentColor;
	byte col_has_light;

	/* Map 0..99 spawn to 0..255 threshold once per frame. */
	spawn_threshold = ((unsigned int)spawn * 256u + 99u) / 100u;

	for(col = 0; col < 80; col++) {
		col_base = txtmem + (col << 1);
		head = g_col_head[col];
		col_has_light = 0;

		/* Cold columns only need a top-row spawn check. */
		if(!g_col_active[col]) {
			if(((col_base[1] & 0x0F) == 0) && (fast_rand8() >= spawn_threshold)) {
				col_base[1] = 0x0F;
				col_base[0] = rnd_printable();
				g_col_head[col] = 0;
				g_col_active[col] = 1;
			}
			continue;
		}

		/* Fade this column. */
		ptr = col_base + 1;
		for(row = 0; row < 25; row++) {
			currentColor = 0x0F & ptr[0];
			if(currentColor > 0) {
				ptr[0] = (char)(currentColor - 1);
				if(currentColor > 1) {
					col_has_light = 1;
				}
			}
			ptr += 160;
		}

		/* Move the head down one row when the previous glyph is non-whitespace. */
		if(head >= 0) {
			if((head < 24) && !is_whitespace(col_base[(head * 160)])) {
				head++;
				col_base[(head * 160) + 1] = 0x0F;
				col_base[(head * 160)] = rnd_printable();
				col_has_light = 1;
			} else {
				head = -1;
			}
		}

		/* Spawn from top when dark, same threshold logic as before. */
		if(((col_base[1] & 0x0F) == 0) && (fast_rand8() >= spawn_threshold)) {
			col_base[1] = 0x0F;
			col_base[0] = rnd_printable();
			head = 0;
			col_has_light = 1;
		}

		g_col_head[col] = head;
		g_col_active[col] = (byte)((head >= 0) || col_has_light);
	}
}

int main(int argc, char** argv) {
	char keybuf[32];
	int i;
	/* Quick MDA check */
	if (get_video_caps() & VIDEO_CAP_MDA) {
	printf("This program requires CGA or higher.\r\n");
	return 1;
	}
	init_keyboard();
	clear_keybuf(keybuf);
	/* make sure we are in text mode */
	set_mode(VGA_TEXT_MODE);
	green_palette();
	paint_box(0,0,80,25,0x0F);
	paint_box(0,0,80,1, 0x00);
	g_rng = peek(0x0040, 0x006C);
	init_stream_state();
	while(!test_keybuf(keybuf, KEY_ESC)) {
		step(95);
		/* as it turns out, 60Hz is ludicrously fast for this effect */
		wait_retrace();
		wait_retrace();
		wait_retrace();
		wait_retrace();
		get_keys_hit(keybuf);
	}
	deinit_keyboard();
	/* step through the simulation some more with new spawns disabled,
		to allow remaining trails to finish their arc */
	for(i = 0; i < 41; i ++) {
		step(100);
		wait_retrace();
		wait_retrace();
		wait_retrace();
		wait_retrace();
	}
	clear_screen(0x07);
	/* we're already in text_mode but this'll restore the palette */
	set_mode(VGA_TEXT_MODE);
	wait_retrace();
	return 0;
}
