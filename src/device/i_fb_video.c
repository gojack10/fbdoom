#include "i_video.h"
#include "d_event.h"
#include "d_main.h"
#include "r_defs.h"
#include "hu_stuff.h"
#include "m_swap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <linux/fb.h>
#define FB_WIDTH  480
#define FB_HEIGHT 800

#include <sys/mman.h>
#include <stdint.h>
#include <linux/input.h>
#include <poll.h>
#include <math.h>
#include <sys/wait.h>
#include <time.h>
#include <sys/resource.h>

/* =====================================================
 * Terminal / debug panel
 * Position: x=0, y=420, width=278, height=110
 * Header: 18px (FPS/CPU/RAM), Log: 92px (~10 lines)
 * Font: 8x8 monospace bitmap, color #aaa on #161616
 * ===================================================== */
#define TERM_X 0
#define TERM_Y 300
#define TERM_W 278
#define TERM_H 110
#define TERM_HEADER_H 18
#define TERM_LOG_Y (TERM_Y + TERM_HEADER_H)
#define TERM_LOG_H (TERM_H - TERM_HEADER_H)
#define TERM_BG 0xFF161616    /* #161616 ARGB */
#define TERM_BORDER 0xFF777777 /* #777777 */
#define TERM_TEXT 0xFFAAAAAA   /* #aaa */
#define TERM_HEADER_TEXT 0xFFCCCCC /* #ccc */
#define TERM_FONT_W 8
#define TERM_FONT_H 16
#define TERM_MAX_LINES 10
#define TERM_MAX_COLS 34   /* (278-4)/8 = 34 chars */
#define TERM_LINE_LEN 80

/* Forward declarations for framebuffer globals (defined later) */
extern char *fbp;
extern uint32_t fb_back[FB_HEIGHT][FB_WIDTH];
uint32_t fb_back[FB_HEIGHT][FB_WIDTH]; /* actual allocation */
extern struct fb_fix_screeninfo finfo;
extern struct fb_var_screeninfo vinfo;

/* 8x16 VGA-style bitmap font for ASCII 0x20-0x7f */
static const uint8_t tty_font[96][16] = {{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x00, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x24, 0x24, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x24, 0x24, 0x24, 0x7e, 0x24, 0x24, 0x7e, 0x24, 0x24, 0x24, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x10, 0x10, 0x7c, 0x92, 0x90, 0x90, 0x7c, 0x12, 0x12, 0x92, 0x7c, 0x10, 0x10, 0x00, 0x00 },
    { 0x00, 0x00, 0x64, 0x94, 0x68, 0x08, 0x10, 0x10, 0x20, 0x2c, 0x52, 0x4c, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x18, 0x24, 0x24, 0x18, 0x30, 0x4a, 0x44, 0x44, 0x44, 0x3a, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x10, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x08, 0x10, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x10, 0x08, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x20, 0x10, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x10, 0x20, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x24, 0x18, 0x7e, 0x18, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x10, 0x7c, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x10, 0x20, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x04, 0x04, 0x08, 0x08, 0x10, 0x10, 0x20, 0x20, 0x40, 0x40, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x3c, 0x42, 0x42, 0x46, 0x4a, 0x52, 0x62, 0x42, 0x42, 0x3c, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x08, 0x18, 0x28, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x3e, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x3c, 0x42, 0x42, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x7e, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x3c, 0x42, 0x42, 0x02, 0x1c, 0x02, 0x02, 0x42, 0x42, 0x3c, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x02, 0x06, 0x0a, 0x12, 0x22, 0x42, 0x7e, 0x02, 0x02, 0x02, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x7e, 0x40, 0x40, 0x40, 0x7c, 0x02, 0x02, 0x02, 0x42, 0x3c, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x1c, 0x20, 0x40, 0x40, 0x7c, 0x42, 0x42, 0x42, 0x42, 0x3c, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x7e, 0x02, 0x02, 0x04, 0x04, 0x08, 0x08, 0x10, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x3c, 0x42, 0x42, 0x42, 0x3c, 0x42, 0x42, 0x42, 0x42, 0x3c, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x3c, 0x42, 0x42, 0x42, 0x42, 0x3e, 0x02, 0x02, 0x04, 0x38, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x10, 0x00, 0x00, 0x00, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x10, 0x00, 0x00, 0x00, 0x10, 0x10, 0x20, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x00, 0x04, 0x08, 0x10, 0x20, 0x40, 0x20, 0x10, 0x08, 0x04, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x7e, 0x00, 0x00, 0x7e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x00, 0x40, 0x20, 0x10, 0x08, 0x04, 0x08, 0x10, 0x20, 0x40, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x3c, 0x42, 0x42, 0x42, 0x04, 0x08, 0x08, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x7c, 0x82, 0x9e, 0xa2, 0xa2, 0xa2, 0xa6, 0x9a, 0x80, 0x7e, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x3c, 0x42, 0x42, 0x42, 0x42, 0x7e, 0x42, 0x42, 0x42, 0x42, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x7c, 0x42, 0x42, 0x42, 0x7c, 0x42, 0x42, 0x42, 0x42, 0x7c, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x3c, 0x42, 0x42, 0x40, 0x40, 0x40, 0x40, 0x42, 0x42, 0x3c, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x78, 0x44, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x44, 0x78, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x7e, 0x40, 0x40, 0x40, 0x78, 0x40, 0x40, 0x40, 0x40, 0x7e, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x7e, 0x40, 0x40, 0x40, 0x78, 0x40, 0x40, 0x40, 0x40, 0x40, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x3c, 0x42, 0x42, 0x40, 0x40, 0x4e, 0x42, 0x42, 0x42, 0x3c, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x42, 0x42, 0x42, 0x42, 0x7e, 0x42, 0x42, 0x42, 0x42, 0x42, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x38, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x38, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x44, 0x44, 0x38, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x42, 0x44, 0x48, 0x50, 0x60, 0x60, 0x50, 0x48, 0x44, 0x42, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x7e, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x82, 0xc6, 0xaa, 0x92, 0x92, 0x82, 0x82, 0x82, 0x82, 0x82, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x42, 0x42, 0x42, 0x62, 0x52, 0x4a, 0x46, 0x42, 0x42, 0x42, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x3c, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x3c, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x7c, 0x42, 0x42, 0x42, 0x42, 0x7c, 0x40, 0x40, 0x40, 0x40, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x3c, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x4a, 0x3c, 0x02, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x7c, 0x42, 0x42, 0x42, 0x42, 0x7c, 0x50, 0x48, 0x44, 0x42, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x3c, 0x42, 0x40, 0x40, 0x3c, 0x02, 0x02, 0x42, 0x42, 0x3c, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0xfe, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x3c, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x42, 0x42, 0x42, 0x42, 0x42, 0x24, 0x24, 0x24, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x82, 0x82, 0x82, 0x82, 0x82, 0x92, 0x92, 0xaa, 0xc6, 0x82, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x42, 0x42, 0x24, 0x24, 0x18, 0x18, 0x24, 0x24, 0x42, 0x42, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x82, 0x82, 0x44, 0x44, 0x28, 0x10, 0x10, 0x10, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x7e, 0x02, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x40, 0x7e, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x38, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x38, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x40, 0x40, 0x20, 0x20, 0x10, 0x10, 0x08, 0x08, 0x04, 0x04, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x38, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x38, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x10, 0x28, 0x44, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7e, 0x00, 0x00 },
    { 0x10, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x3c, 0x02, 0x3e, 0x42, 0x42, 0x42, 0x3e, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x40, 0x40, 0x40, 0x7c, 0x42, 0x42, 0x42, 0x42, 0x42, 0x7c, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x3c, 0x42, 0x40, 0x40, 0x40, 0x42, 0x3c, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x02, 0x02, 0x02, 0x3e, 0x42, 0x42, 0x42, 0x42, 0x42, 0x3e, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x3c, 0x42, 0x42, 0x7e, 0x40, 0x40, 0x3c, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x0e, 0x10, 0x10, 0x7c, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x3e, 0x42, 0x42, 0x42, 0x42, 0x42, 0x3e, 0x02, 0x02, 0x3c, 0x00 },
    { 0x00, 0x00, 0x40, 0x40, 0x40, 0x7c, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x10, 0x10, 0x00, 0x30, 0x10, 0x10, 0x10, 0x10, 0x10, 0x38, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x04, 0x04, 0x00, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x44, 0x44, 0x38, 0x00 },
    { 0x00, 0x00, 0x40, 0x40, 0x40, 0x42, 0x44, 0x48, 0x70, 0x48, 0x44, 0x42, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x30, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x38, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0xfc, 0x92, 0x92, 0x92, 0x92, 0x92, 0x92, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x7c, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x3c, 0x42, 0x42, 0x42, 0x42, 0x42, 0x3c, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x7c, 0x42, 0x42, 0x42, 0x42, 0x42, 0x7c, 0x40, 0x40, 0x40, 0x00 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x3e, 0x42, 0x42, 0x42, 0x42, 0x42, 0x3e, 0x02, 0x02, 0x02, 0x00 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x5e, 0x60, 0x40, 0x40, 0x40, 0x40, 0x40, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x3e, 0x40, 0x40, 0x3c, 0x02, 0x02, 0x7c, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x10, 0x10, 0x10, 0x7c, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0e, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x3e, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x42, 0x42, 0x42, 0x24, 0x24, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x82, 0x82, 0x92, 0x92, 0x92, 0x92, 0x7c, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x42, 0x42, 0x24, 0x18, 0x24, 0x42, 0x42, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x3e, 0x02, 0x02, 0x3c, 0x00 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x7e, 0x04, 0x08, 0x10, 0x20, 0x40, 0x7e, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x0c, 0x10, 0x10, 0x10, 0x20, 0x10, 0x10, 0x10, 0x10, 0x0c, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x30, 0x08, 0x08, 0x08, 0x04, 0x08, 0x08, 0x08, 0x08, 0x30, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x62, 0x92, 0x8c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
};


/* Ignore SIGPIPE by closing pipe on write error */
static void setup_sigpipe(void) { }

/* Log buffer */
static char term_lines[TERM_MAX_LINES][TERM_LINE_LEN];
static int term_line_count = 0;
static int term_line_idx = 0;

/* stderr capture */
static int term_stderr_pipe[2] = {-1, -1};

/* FPS / CPU / RAM tracking */
static int term_fps = 0;
static int term_cpu = 0;
static int term_ram_mb = 0;
static unsigned long term_last_ticks = 0;
static time_t term_last_update = 0;

/* Draw a single character at (x,y) with given color */
static void tty_draw_char(int x, int y, char c, uint32_t color)
{
    int row, col;
    if (c < 0x20 || c > 0x7f) c = '?';
    const uint8_t *g = tty_font[c - 0x20];
    for (row = 0; row < TERM_FONT_H; row++) {
        uint8_t bits = g[row];
        for (col = 0; col < TERM_FONT_W; col++) {
            if (bits & (1 << (7 - col))) {
                int px = x + col;
                int py = y + row;
                if (px >= TERM_X && px < TERM_X + TERM_W &&
                    py >= TERM_Y && py < TERM_Y + TERM_H) {
                    uint32_t *dst = &fb_back[py][px];
                    *dst = color;
                }
            }
        }
    }
}

/* Draw a string at (x,y) with given color, returns x + width */
static int tty_draw_str(int x, int y, const char *s, uint32_t color)
{
    while (*s) {
        tty_draw_char(x, y, *s, color);
        x += TERM_FONT_W;
        s++;
    }
    return x;
}

/* Draw a string truncated to max columns */
static void tty_draw_str_clipped(int x, int y, int max_cols, const char *s, uint32_t color)
{
    int cols = 0;
    while (*s && cols < max_cols) {
        tty_draw_char(x + cols * TERM_FONT_W, y, *s, color);
        s++;
        cols++;
    }
}

/* Add a line to the terminal log buffer */
void term_log(const char *msg)
{
    /* Dedup: skip identical consecutive lines */
    if (term_line_count > 0 && strcmp(msg, term_lines[term_line_idx]) == 0)
        return;
    /* Copy message, truncate if needed */
    char *line = term_lines[term_line_idx];
    int len = 0;
    while (*msg && len < TERM_LINE_LEN - 1) {
        line[len++] = *msg++;
    }
    line[len] = '\0';

    term_line_idx = (term_line_idx + 1) % TERM_MAX_LINES;
    if (term_line_count < TERM_MAX_LINES)
        term_line_count++;
}

/* Read available data from stderr pipe */
static void term_read_stderr(void)
{
    if (term_stderr_pipe[0] < 0) return;
    char buf[256];
    struct pollfd pfd;
    pfd.fd = term_stderr_pipe[0];
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
        int n = read(term_stderr_pipe[0], buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            /* Split on newlines and add each line */
            char *p = buf;
            char *nl;
            while ((nl = strchr(p, '\n'))) {
                *nl = '\0';
                if (*p) term_log(p);
                p = nl + 1;
            }
            if (*p) term_log(p);
        }
    }
}

/* Get CPU usage from /proc/stat */
static void term_update_cpu(void)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return;
    char line[256];
    if (fgets(line, sizeof(line), f)) {
        unsigned long u, n, s, i;
        if (sscanf(line + 4, "%lu %lu %lu %lu", &u, &n, &s, &i) == 4) {
            unsigned long total = u + n + s + i;
            unsigned long busy = u + s;
            static unsigned long prev_total = 0, prev_busy = 0;
            if (prev_total) {
                unsigned long dt = total - prev_total;
                unsigned long db = busy - prev_busy;
                if (dt > 0) {
                    /* term_cpu stores tenths of percent (e.g., 75 = 7.5%) */
                    term_cpu = (int)((db * 1000) / dt);
                }
            }
            prev_total = total;
            prev_busy = busy;
        }
    }
    fclose(f);
}

/* Get RAM usage */
static void term_update_ram(void)
{
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    term_ram_mb = ru.ru_maxrss / 1024; /* KB -> MB on Linux */
}

/* Update FPS counter */
static void term_update_fps(int *frame_count, unsigned int *last_time)
{
    static int fps_count = 0;
    static unsigned int fps_start = 0;
    static struct timespec fps_ts_start;

    fps_count++;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    unsigned int now_ms = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

    if (!fps_start) {
        fps_start = now_ms;
        fps_ts_start = ts;
    }

    unsigned int elapsed = now_ms - fps_start;
    if (elapsed >= 1000) {
        term_fps = (int)(fps_count * 1000.0 / elapsed);
        fps_count = 0;
        fps_start = now_ms;
    }
}

/* Draw the entire terminal panel */
static void draw_terminal(void)
{
    int i, x, y;
    /* Background */
    for (y = TERM_Y; y < TERM_Y + TERM_H; y++) {
        for (x = TERM_X; x < TERM_X + TERM_W; x++) {
            uint32_t *dst = &fb_back[y][x];
            *dst = TERM_BG;
        }
    }

    /* Border - top and bottom */
    for (x = TERM_X; x < TERM_X + TERM_W; x++) {
        uint32_t *dst = &fb_back[TERM_Y][x];
        *dst = TERM_BORDER;
        dst = &fb_back[TERM_Y + TERM_H - 1][x];
        *dst = TERM_BORDER;
    }
    /* Border - left and right */
    for (y = TERM_Y; y < TERM_Y + TERM_H; y++) {
        uint32_t *dst = &fb_back[y][TERM_X];
        *dst = TERM_BORDER;
        dst = &fb_back[y][TERM_X + TERM_W - 1];
        *dst = TERM_BORDER;
    }

    /* Header separator line */
    for (x = TERM_X + 1; x < TERM_X + TERM_W - 1; x++) {
        uint32_t *dst = &fb_back[TERM_Y + TERM_HEADER_H - 1][x];
        *dst = TERM_BORDER;
    }

    /* Header: FPS CPU RAM */
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "FPS %d      CPU %d.%d%%     RAM %dMB",
             term_fps, term_cpu / 10, term_cpu % 10, term_ram_mb);
    tty_draw_str(TERM_X + 4, TERM_Y + 3, hdr, TERM_HEADER_TEXT);

    /* Log lines */
    int start_idx = term_line_idx;
    for (i = 0; i < term_line_count; i++) {
        int idx = (start_idx - term_line_count + i + TERM_MAX_LINES) % TERM_MAX_LINES;
        int ly = TERM_LOG_Y + 2 + i * (TERM_FONT_H + 1);
        if (ly + TERM_FONT_H > TERM_Y + TERM_H - 2) break;
        tty_draw_str_clipped(TERM_X + 4, ly, TERM_MAX_COLS - 1, term_lines[idx], TERM_TEXT);
    }
}

/* Initialize terminal (stderr pipe, initial log) */
static void term_init(void)
{
    /* Clear log buffer */
    memset(term_lines, 0, sizeof(term_lines));

    /* Set up stderr capture pipe (disabled - causes SIGPIPE on Android) */
    /*
    if (pipe(term_stderr_pipe) == 0) {
        int flags = fcntl(term_stderr_pipe[0], F_GETFL, 0);
        fcntl(term_stderr_pipe[0], F_SETFL, flags | O_NONBLOCK);
        dup2(term_stderr_pipe[1], 1);
        dup2(term_stderr_pipe[1], 2);
        close(term_stderr_pipe[1]);
    }
    */

    /* Initial state */
    term_last_ticks = 0;
    term_update_cpu(); /* prime the tick counter */
    term_update_ram();
    term_log("DOOM mobile initialized");
}
#include "v_video.h"

static int fbfd = 0;
struct fb_var_screeninfo vinfo;
struct fb_fix_screeninfo finfo;
static long int screensize = 0;
char *fbp = 0;

/* Match phone framebuffer exactly */

/* Offscreen back buffer for double buffering (eliminates flicker) */

/* Game area: 320x200 -> 480x300 (1.5x scale) */
#define GAME_W  480
#define GAME_H  300

/* Button layout: action buttons + strafe buttons */
#define BTN_COUNT 5

typedef struct {
    const char *label;
    int key;
    int pressed;
    int sx0, sy0, sx1, sy1;
} btn_t;

/* Action unit (top-right) + strafe unit (bottom-right) */
static btn_t buttons[BTN_COUNT] = {
    { "MENU",  27,  0,  300, 314, 377, 366 },
    { "MAP",   KEY_TAB, 0,  385, 314, 462, 366 },
    { "USE",   ' ', 0,  300, 374, 462, 404 },
    { "STRAFE L", ',', 0, 282, 540, 367, 640 },
    { "STRAFE R", '.', 0, 372, 540, 457, 640 },
};

/* Joystick: bottom-left */
#define JS_CENTER_X  121
#define JS_CENTER_Y  670
#define JS_RADIUS    94
#define JS_STICK_R   27
#define JS_MAX_OFF   58
#define JS_DEADZONE  0.2f

static struct {
    int active;
    int last_raw_x, last_raw_y;
    float nx, ny;  /* normalized vector (-1..1) */
    int stick_x, stick_y;  /* screen position of stick center */
} js = {0};

/* Joystick readout: "x 0.00 / y 0.00" at bottom-left of controls area */
#define READOUT_X 18
#define READOUT_Y 775  /* 799 - 10 - 14 */
#define READOUT_COLOR 0xFF888888  /* #888 */

static void draw_readout(void)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "x %.2f / y %.2f", js.nx, js.ny);
    tty_draw_str(READOUT_X, READOUT_Y, buf, READOUT_COLOR);
}

/* Touchscreen + keypad state */
static int ts_fd = -1;
static int kbd_fd = -1;

/* Palette colors for framebuffer */
static uint32_t colors32[256];

extern boolean menuactive;

/* HUD font from hu_stuff.c */
extern patch_t *hu_font[HU_FONTSIZE];

/* Doom key bindings (ensure defaults are set) */
extern int key_up;
extern int key_down;
extern int key_left;
extern int key_right;
extern int key_fire;
extern int key_use;
extern int key_strafeleft;
extern int key_straferight;

/* Convert screen-space button rects to touch-space bounds */
static void I_InitButtons(void)
{
    /* Ensure Doom key bindings are set (M_LoadDefaults may not run reliably) */
    key_up = 0xad;      /* KEY_UPARROW */
    key_down = 0xaf;    /* KEY_DOWNARROW */
    key_left = 0xac;    /* KEY_LEFTARROW */
    key_right = 0xae;   /* KEY_RIGHTARROW */
    key_fire = 0x9d;    /* KEY_RCTRL */
    key_use = ' ';      /* SPACE */
    key_strafeleft = ',';
    key_straferight = '.';
}

void I_InitGraphics (void)
{
    fbfd = open("/dev/graphics/fb0", O_RDWR);
    if (!fbfd) { printf("Error: cannot open framebuffer.\n"); exit(1); }
    printf("Framebuffer opened.\n");

    /* Enable s3cfb window power for fb0.
     * surfaceflinger switches to fb2/fb3 for overlay composition and leaves
     * fb0's window powered OFF. When surfaceflinger stops, fb0 stays off —
     * doom writes to fb0 but the LCD shows fb2 (stale content). Fix: enable
     * fb0's window so doom's framebuffer actually reaches the display. */
    {
        FILE *wp = fopen("/sys/devices/platform/s3cfb/win_power", "w");
        if (wp) {
            fprintf(wp, "[fb0] on\n");
            fclose(wp);
            printf("s3cfb: fb0 window enabled.\n");
        }
    }

    if (ioctl(fbfd, FBIOGET_FSCREENINFO, &finfo)) { printf("Error: fixed info.\n"); exit(2); }
    if (ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo)) { printf("Error: var info.\n"); exit(3); }

    screensize = vinfo.xres * vinfo.yres * vinfo.bits_per_pixel / 8;
    printf("Screen: %dx%d @ %dbpp\n", vinfo.xres, vinfo.yres, vinfo.bits_per_pixel);
    printf("Stride: %d, virtual: %d,%d\n", finfo.line_length, vinfo.xres_virtual, vinfo.yres_virtual);
    printf("RGB: r=%d/%d g=%d/%d b=%d/%d\n",
           vinfo.red.offset, vinfo.red.length,
           vinfo.green.offset, vinfo.green.length,
           vinfo.blue.offset, vinfo.blue.length);
    printf("screensize: %ld\n", screensize);

    fbp = (char *)mmap(0, screensize, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    if ((int64_t)fbp == -1) { printf("Error: mmap failed.\n"); exit(4); }
    printf("Framebuffer mapped.\n");

    memset(fbp, 0, screensize);

    ts_fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
    if (ts_fd >= 0) printf("Touchscreen opened.\n");
    else printf("Warning: no touchscreen.\n");

    kbd_fd = open("/dev/input/event7", O_RDONLY | O_NONBLOCK);
    if (kbd_fd >= 0) printf("Keypad opened (FIRE=vol down).\n");
    else printf("Warning: no keypad.\n");

    I_InitButtons();
    setup_sigpipe(); /* Ignore SIGPIPE from broken stderr pipe */
    term_init();
    I_SetTermLog(term_log); /* connect game code -> terminal */
}

void I_ShutdownGraphics(void)
{
    if (kbd_fd >= 0) close(kbd_fd);
    if (ts_fd >= 0) close(ts_fd);
    munmap(fbp, screensize);
    close(fbfd);
}

void I_StartFrame (void) { }

void I_SetPalette (byte* palette)
{
    byte c;
    int i;
    for (i = 0; i < 256; i++) {
        c = gammatable[usegamma][*palette++];
        uint8_t r = (c << 8) + c;
        c = gammatable[usegamma][*palette++];
        uint8_t g = (c << 8) + c;
        c = gammatable[usegamma][*palette++];
        uint8_t b = (c << 8) + c;
        colors32[i] = (0xff << 24) | (r << 16) | (g << 8) | b;
    }
}

void I_UpdateNoBlit (void) { }

/* Draw a Doom patch (font char) to framebuffer */
static void draw_patch_fb(int x, int y, patch_t *patch)
{
    int col;
    column_t *column;
    byte *source;
    int w = SHORT(patch->width);
    byte *patch_end = (byte *)patch + 256;

    x -= SHORT(patch->leftoffset);
    y -= SHORT(patch->topoffset);

    for (col = 0; col < w; col++) {
        column = (column_t *)((byte *)patch + LONG(patch->columnofs[col]));
        int cy = y;
        while (column->topdelta != 0xff) {
            if ((byte *)column + 4 > patch_end) break;
            source = (byte *)column + 2;
            int count = column->length;
            cy += column->topdelta;
            while (count--) {
                if (source < patch_end) {
                    uint32_t color = colors32[*source++];
                    if (x + col >= 0 && x + col < FB_WIDTH && cy >= 0 && cy < FB_HEIGHT) {
                        fb_back[cy][x + col] = color;
                    }
                }
                cy++;
            }
            column = (column_t *)((byte *)column + column->length + 4);
        }
    }
}

/* Draw a filled circle (solid) */
static void draw_filled_circle(int cx, int cy, int r, uint32_t color)
{
    int dy, dx;
    for (dy = -r; dy <= r; dy++) {
        int h = (int)(sqrtf((float)(r * r - dy * dy)) + 0.5f);
        for (dx = -h; dx <= h; dx++) {
            int px = cx + dx, py = cy + dy;
            if (px >= 0 && px < FB_WIDTH && py >= 0 && py < FB_HEIGHT) {
                fb_back[py][px] = color;
            }
        }
    }
}

/* Draw a circle outline */
static void draw_circle_outline(int cx, int cy, int r, uint32_t color)
{
    int dy, dx;
    for (dy = -r; dy <= r; dy++) {
        for (dx = -r; dx <= r; dx++) {
            float d = sqrtf((float)(dx * dx + dy * dy));
            if (d >= (float)(r - 1) && d <= (float)(r + 1)) {
                int px = cx + dx, py = cy + dy;
                if (px >= 0 && px < FB_WIDTH && py >= 0 && py < FB_HEIGHT) {
                    fb_back[py][px] = color;
                }
            }
        }
    }
}

/* Draw crosshair lines inside joystick */
static void draw_js_crosshair(int cx, int cy, int r)
{
    int i;
    uint32_t color = 0xFF444444;
    int margin = 10;
    for (i = margin; i < r - margin; i++) {
        /* vertical */
        if (cx >= 0 && cx < FB_WIDTH && cy - i >= 0 && cy - i < FB_HEIGHT) {
            fb_back[cy - i][cx] = color;
        }
        if (cx >= 0 && cx < FB_WIDTH && cy + i >= 0 && cy + i < FB_HEIGHT) {
            fb_back[cy + i][cx] = color;
        }
        /* horizontal */
        if (cx - i >= 0 && cx - i < FB_WIDTH && cy >= 0 && cy < FB_HEIGHT) {
            fb_back[cy][cx - i] = color;
        }
        if (cx + i >= 0 && cx + i < FB_WIDTH && cy >= 0 && cy < FB_HEIGHT) {
            fb_back[cy][cx + i] = color;
        }
    }
}

/* Draw a button background + border + HUD font label */
static void draw_button(int b)
{
    int bx0 = buttons[b].sx0;
    int bx1 = buttons[b].sx1 - 1;
    int by0 = buttons[b].sy0;
    int by1 = buttons[b].sy1 - 1;

    uint32_t bg = buttons[b].pressed ? 0xFF80FF40 : 0xFF404040;
    uint32_t border = buttons[b].pressed ? 0xFF00FF00 : 0xFF808080;

    int dy, dx;
    for (dy = by0; dy <= by1 && dy < FB_HEIGHT; dy++) {
        for (dx = bx0; dx <= bx1 && dx < FB_WIDTH; dx++) {
            if (dy == by0 || dy == by1 || dx == bx0 || dx == bx1)
                fb_back[dy][dx] = border;
            else
                fb_back[dy][dx] = bg;
        }
    }

    /* Label using HUD font */
    if (hu_font[0]) {
        int label_w = 0, ci;
        for (ci = 0; buttons[b].label[ci]; ci++) {
            if (buttons[b].label[ci] >= HU_FONTSTART && buttons[b].label[ci] <= HU_FONTEND)
                label_w += SHORT(hu_font[buttons[b].label[ci] - HU_FONTSTART]->width) + 1;
        }
        label_w = label_w > 0 ? label_w - 1 : 0;

        int tx = bx0 + (bx1 - bx0 - label_w) / 2;
        int ty = by0 + (by1 - by0 - 10) / 2;

        for (ci = 0; buttons[b].label[ci]; ci++) {
            char c = buttons[b].label[ci];
            if (c >= HU_FONTSTART && c <= HU_FONTEND) {
                draw_patch_fb(tx, ty, hu_font[c - HU_FONTSTART]);
                tx += SHORT(hu_font[c - HU_FONTSTART]->width) + 1;
            }
        }
    }
}

/* Draw the joystick overlay */
static void draw_joystick(void)
{
    /* Fill joystick base with dark background (clears old stick position) */
    draw_filled_circle(JS_CENTER_X, JS_CENTER_Y, JS_RADIUS, 0xFF2A2A2A);

    /* Circle outline + crosshair */
    draw_circle_outline(JS_CENTER_X, JS_CENTER_Y, JS_RADIUS, 0xFF666666);
    draw_js_crosshair(JS_CENTER_X, JS_CENTER_Y, JS_RADIUS);

    /* Stick: gray dot with border */
    draw_circle_outline(js.stick_x, js.stick_y, JS_STICK_R, 0xFFAAAAAA);
    draw_filled_circle(js.stick_x, js.stick_y, JS_STICK_R - 1, 0xFF444444);

    /* Small white center dot on stick */
    draw_filled_circle(js.stick_x, js.stick_y, 5, 0xFFEEEEEE);
}

void I_FinishUpdate (void)
{
    static int frame_count = 0;
    int sx, sy, dx, dy;
    frame_count++;

    /* Sanity check: fbp must be valid */
    if (!fbp) {
        /* printf("FATAL: fbp is NULL in I_FinishUpdate\n"); */
        return;
    }

    /* Render game at top: 320x200 -> 480x300 (1.5x scale) */
    for (sy = 0; sy < SCREENHEIGHT; sy++) {
        uint32_t src_row_pixels[SCREENWIDTH];
        for (sx = 0; sx < SCREENWIDTH; sx++)
            src_row_pixels[sx] = colors32[*(screens[0] + sy * SCREENWIDTH + sx)];

        int dy0 = sy * 3 / 2;
        int dy1 = dy0 + 1;
        if (dy1 >= FB_HEIGHT) dy1 = FB_HEIGHT - 1;

        for (dy = dy0; dy <= dy1; dy++) {
            int dx = 0;
            for (sx = 0; sx < SCREENWIDTH && dx < FB_WIDTH; sx++) {
                fb_back[dy][dx] = src_row_pixels[sx];
                dx++;
                if ((sx & 1) == 0 && dx < FB_WIDTH) {
                    fb_back[dy][dx] = src_row_pixels[sx];
                    dx++;
                }
            }
        }
    }

    /* Draw buttons */
    int b;
    for (b = 0; b < BTN_COUNT; b++)
        draw_button(b);

    /* Draw joystick */
    draw_joystick();

    /* Terminal / debug panel */
    term_read_stderr();
    term_update_fps(&frame_count, NULL);
    if (frame_count % 30 == 0) { /* update stats every ~1 second */
        term_update_cpu();
        term_update_ram();
    }
    draw_terminal();
    draw_readout();

    /* Frame 1: debug info */
    if (frame_count == 1) {
        FILE *log = fopen("/data/local/tmp/fb_debug.log", "w");
        if (log) {
            fprintf(log, "vinfo: %dx%d stride=%d bpp=%d\n",
                    vinfo.xres, vinfo.yres, finfo.line_length, vinfo.bits_per_pixel);
            fprintf(log, "JS: cx=%d cy=%d r=%d\n", JS_CENTER_X, JS_CENTER_Y, JS_RADIUS);
            fclose(log);
        }
    }

    /* Copy back buffer to framebuffer (atomic, eliminates flicker) */
    {
        int y;
        for (y = 0; y < FB_HEIGHT; y++)
            memcpy(fbp + y * finfo.line_length, fb_back[y], FB_WIDTH * 4);
    }
}

/* Post FIRE key (enter + rctrl for shooting) */
static void fire_press(void)
{
    event_t e = {ev_keydown, 13, 0, 0};
    D_PostEvent(&e);
    event_t e2 = {ev_keydown, 0x9d, 0, 0};
    D_PostEvent(&e2);
}

static void fire_release(void)
{
    event_t e = {ev_keyup, 13, 0, 0};
    D_PostEvent(&e);
    event_t e2 = {ev_keyup, 0x9d, 0, 0};
    D_PostEvent(&e2);
}

/* Check if screen coords are inside joystick zone */
static int js_hit_test(int sx, int sy)
{
    int dx = sx - JS_CENTER_X;
    int dy = sy - JS_CENTER_Y;
    return (dx * dx + dy * dy) <= (JS_RADIUS * JS_RADIUS);
}

/* Convert raw touch coords to screen coords */
static void ts_to_screen(int raw_x, int raw_y, int *sx, int *sy)
{
    *sx = raw_x * FB_WIDTH / 1024;
    *sy = raw_y * FB_HEIGHT / 1024;
}

/* Update joystick from screen-space touch position */
static void js_update(int sx, int sy)
{
    int dx = sx - JS_CENTER_X;
    int dy = sy - JS_CENTER_Y;
    float dist = sqrtf((float)(dx * dx + dy * dy));
    float max = (float)JS_MAX_OFF;

    if (dist < JS_DEADZONE * max) {
        js.nx = 0; js.ny = 0;
        js.stick_x = JS_CENTER_X;
        js.stick_y = JS_CENTER_Y;
    } else {
        float clamped = dist > max ? max : dist;
        js.nx = (dx / dist) * (clamped / max);
        js.ny = -(dy / dist) * (clamped / max);  /* flip y: up = positive */
        js.stick_x = JS_CENTER_X + (int)(js.nx * max);
        js.stick_y = JS_CENTER_Y - (int)(js.ny * max);
    }
    js.last_raw_x = sx;
    js.last_raw_y = sy;
}

/* Post joystick key events based on current vector */
static int js_fwd_posted = 0, js_back_posted = 0;
static int js_left_posted = 0, js_right_posted = 0;
static FILE *js_dbg = NULL;

static void js_post_keys(void)
{
    float dead = JS_DEADZONE;
    int changed = 0;

    if (!js_dbg) js_dbg = fopen("/data/local/tmp/doom_debug.log", "a");

    /* Forward/back (y axis) */
    if (js.ny > dead) {
        if (!js_fwd_posted) { js_fwd_posted = 1; D_PostEvent(&(event_t){ev_keydown, 0xad, 0, 0}); changed |= 1; }
    } else {
        if (js_fwd_posted) { js_fwd_posted = 0; D_PostEvent(&(event_t){ev_keyup, 0xad, 0, 0}); changed |= 2; }
    }
    if (js.ny < -dead) {
        if (!js_back_posted) { js_back_posted = 1; D_PostEvent(&(event_t){ev_keydown, 0xaf, 0, 0}); changed |= 4; }
    } else {
        if (js_back_posted) { js_back_posted = 0; D_PostEvent(&(event_t){ev_keyup, 0xaf, 0, 0}); changed |= 8; }
    }

    /* Turn left/right (x axis) */
    if (js.nx < -dead) {
        if (!js_left_posted) { js_left_posted = 1; D_PostEvent(&(event_t){ev_keydown, 0xac, 0, 0}); changed |= 16; }
    } else {
        if (js_left_posted) { js_left_posted = 0; D_PostEvent(&(event_t){ev_keyup, 0xac, 0, 0}); changed |= 32; }
    }
    if (js.nx > dead) {
        if (!js_right_posted) { js_right_posted = 1; D_PostEvent(&(event_t){ev_keydown, 0xae, 0, 0}); changed |= 64; }
    } else {
        if (js_right_posted) { js_right_posted = 0; D_PostEvent(&(event_t){ev_keyup, 0xae, 0, 0}); changed |= 128; }
    }

    if (changed && js_dbg) {
        fprintf(js_dbg, "INPUT nx=%.2f ny=%.2f evt=0x%02x fwd=%d bck=%d lft=%d rgt=%d\n",
                js.nx, js.ny, changed,
                js_fwd_posted, js_back_posted, js_left_posted, js_right_posted);
        fflush(js_dbg);
    }
}

/* Release all joystick keys and reset */
static void js_reset(void)
{
    js.nx = 0; js.ny = 0;
    js.stick_x = JS_CENTER_X;
    js.stick_y = JS_CENTER_Y;
    js.active = 0;
    js_post_keys();  /* releases any held keys (now that vector is zero) */
}

/* Hit-test screen coords against button rects */
static int btn_hit_test(int sx, int sy)
{
    int b;
    for (b = 0; b < BTN_COUNT; b++) {
        if (sx >= buttons[b].sx0 && sx < buttons[b].sx1 &&
            sy >= buttons[b].sy0 && sy < buttons[b].sy1)
            return b;
    }
    return -1;
}

/* Track what the current finger is interacting with */
static int ts_mode = 0;  /* 0=none, 1=joystick, 2..6=button index+1 */
static int ts_btn_down = -1;  /* button index pressed on finger-down */

/* Poll touchscreen (joystick + buttons) + keypad (volume=fire) */
void I_PollTouch(void)
{
    struct input_event ev;

    /* --- Keypad: volume down = FIRE --- */
    static int fire_held = 0;
    if (kbd_fd >= 0) {
        while (read(kbd_fd, &ev, sizeof(ev)) > 0) {
            if (ev.type == EV_KEY && ev.code == KEY_VOLUMEDOWN) {
                if (ev.value == 1 && !fire_held) { fire_held = 1; fire_press(); }
                else if (ev.value == 0 && fire_held) { fire_held = 0; fire_release(); }
            }
        }
    }

    /* --- Touchscreen --- */
    if (ts_fd < 0) return;

    int raw_x = 0, raw_y = 0, tracking_id = -2;

    while (read(ts_fd, &ev, sizeof(ev)) > 0) {
        if (ev.type == EV_ABS) {
            if (ev.code == ABS_MT_POSITION_X) raw_x = ev.value;
            if (ev.code == ABS_MT_POSITION_Y) raw_y = ev.value;
            if (ev.code == ABS_MT_TRACKING_ID) tracking_id = ev.value;
        }
        if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
            int finger_down = (tracking_id >= 0);

            if (finger_down) {
                int sx, sy;
                ts_to_screen(raw_x, raw_y, &sx, &sy);

                if (ts_mode == 0) {
                    /* New finger-down: decide joystick or button */
                    if (js_hit_test(sx, sy)) {
                        ts_mode = 1;  /* joystick */
                        js.active = 1;
                        js_update(sx, sy);
                        js_post_keys();
                    } else {
                        int hit = btn_hit_test(sx, sy);
                        if (hit >= 0) {
                            ts_mode = hit + 2;
                            ts_btn_down = hit;
                            buttons[hit].pressed = 1;
                            D_PostEvent(&(event_t){ev_keydown, buttons[hit].key, 0, 0});
                            /* USE also selects menu items */
                            if (hit == 2 && menuactive) {
                                D_PostEvent(&(event_t){ev_keydown, 13, 0, 0});
                            }
                        }
                    }
                } else if (ts_mode == 1) {
                    /* Moving on joystick */
                    js_update(sx, sy);
                    js_post_keys();
                }
                /* else: button hold — ignore movement (toggle button) */
            } else {
                /* Finger up */
                if (ts_mode == 1) {
                    js_reset();
                } else if (ts_btn_down >= 0) {
                    buttons[ts_btn_down].pressed = 0;
                    D_PostEvent(&(event_t){ev_keyup, buttons[ts_btn_down].key, 0, 0});
                    /* USE also releases menu select */
                    if (ts_btn_down == 2 && menuactive) {
                        D_PostEvent(&(event_t){ev_keyup, 13, 0, 0});
                    }
                    ts_btn_down = -1;
                }
                ts_mode = 0;
            }
        }
    }
}

void I_ReadScreen (byte* scr)
{
    memcpy(scr, screens[0], SCREENWIDTH * SCREENHEIGHT);
}

/* Terminal log callback bridge */
static term_log_fn term_log_cb = NULL;
void I_SetTermLog(term_log_fn fn) { term_log_cb = fn; }
void I_TermLog(const char *msg) { if (term_log_cb) term_log_cb(msg); }
