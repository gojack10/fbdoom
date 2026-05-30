#include "i_video.h"
#include "d_event.h"
#include "d_main.h"
#include "r_defs.h"
#include "hu_stuff.h"
#include "m_swap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <math.h>
#include <time.h>

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
#define TERM_HEADER_TEXT 0xFFCCCCCC /* #ccc */
#define TERM_MAX_LINES 10
#define TERM_MAX_COLS 34
#define TERM_LINE_LEN 80

/* Forward declarations — fb_pixels defined later in this file */
static uint32_t *fb_pixels;
#define FB_STRIDE 480  /* pixels per row */

#define PUTPIXEL(px, py, color) do { \
    if ((px) >= 0 && (px) < 480 && (py) >= 0 && (py) < 800) \
        fb_pixels[(py) * FB_STRIDE + (px)] = (color); \
} while(0)

/* SDL_ttf font globals */
static TTF_Font *term_font;
static SDL_Surface *fb;       /* forward decl, defined later */
#define TERM_FONT_SIZE 11

/* Render a string to the fb surface via SDL_ttf, returns x + width */
static int tty_draw_str(int x, int y, const char *s, uint32_t color)
{
    if (!term_font || !s || !*s) return 0;
    SDL_Color c = { (color >> 16) & 0xff, (color >> 8) & 0xff, color & 0xff };
    SDL_Surface *txt = TTF_RenderText_Solid(term_font, s, c);
    if (!txt) return 0;
    SDL_Rect dst = { x, y, txt->w, txt->h };
    SDL_BlitSurface(txt, NULL, fb, &dst);
    SDL_FreeSurface(txt);
    return (int)(x + txt->w);
}

/* Truncated version */
static void tty_draw_str_clipped(int x, int y, int max_cols, const char *s, uint32_t color)
{
    char buf[256];
    int max = max_cols > 255 ? 255 : max_cols;
    strncpy(buf, s, max);
    buf[max] = '\0';
    tty_draw_str(x, y, buf, color);
}



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

/* Get CPU usage (stub for Mac) */
static void term_update_cpu(void)
{
    struct rusage ru;
    static struct timeval last_wall;
    static long last_cputime = 0;
    struct timeval now_wall;
    gettimeofday(&now_wall, NULL);
    getrusage(RUSAGE_SELF, &ru);
    long cputime = (ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000000 +
                   (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec);
    if (last_cputime && last_wall.tv_sec) {
        long dt = (now_wall.tv_sec - last_wall.tv_sec) * 1000000 +
                  (now_wall.tv_usec - last_wall.tv_usec);
        long dcpu = cputime - last_cputime;
        /* term_cpu stores tenths of percent (e.g., 75 = 7.5%) */
        if (dt > 0) term_cpu = (int)((dcpu * 1000) / dt);
    }
    last_cputime = cputime;
    last_wall = now_wall;
}

/* Get RAM usage (works on Mac + Android) */
static void term_update_ram(void)
{
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
#ifdef __APPLE__
    /* Mac: ru_maxrss is in bytes */
    term_ram_mb = ru.ru_maxrss / 1048576;
#else
    /* Linux/Android: ru_maxrss is in KB */
    term_ram_mb = ru.ru_maxrss / 1024;
#endif
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
            PUTPIXEL(x, y, TERM_BG);
        }
    }

    /* Border - top and bottom */
    for (x = TERM_X; x < TERM_X + TERM_W; x++) {
        PUTPIXEL(x, TERM_Y, TERM_BORDER);
        PUTPIXEL(x, TERM_Y + TERM_H - 1, TERM_BORDER);
    }
    /* Border - left and right */
    for (y = TERM_Y; y < TERM_Y + TERM_H; y++) {
        PUTPIXEL(TERM_X, y, TERM_BORDER);
        PUTPIXEL(TERM_X + TERM_W - 1, y, TERM_BORDER);
    }

    /* Header separator line */
    for (x = TERM_X + 1; x < TERM_X + TERM_W - 1; x++) {
        PUTPIXEL(x, TERM_Y + TERM_HEADER_H - 1, TERM_BORDER);
    }

    /* Header: FPS CPU RAM */
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "FPS %d      CPU %d.%d%%     RAM %dMB",
             term_fps, term_cpu / 10, term_cpu % 10, term_ram_mb);
    tty_draw_str(TERM_X + 4, TERM_Y + 3, hdr, TERM_HEADER_TEXT);

    /* Log lines */
    int start_idx = term_line_idx;
    int fh = term_font ? TTF_FontHeight(term_font) : 14;
    for (i = 0; i < term_line_count; i++) {
        int idx = (start_idx - term_line_count + i + TERM_MAX_LINES) % TERM_MAX_LINES;
        int ly = TERM_LOG_Y + 2 + i * (fh + 1);
        if (ly + fh > TERM_Y + TERM_H - 2) break;
        tty_draw_str_clipped(TERM_X + 4, ly, TERM_MAX_COLS - 1, term_lines[idx], TERM_TEXT);
    }
}

/* Initialize terminal (stderr pipe, initial log) */
static void term_init(void)
{
    /* Clear log buffer */
    memset(term_lines, 0, sizeof(term_lines));

    /* Set up stderr capture pipe */
    if (pipe(term_stderr_pipe) == 0) {
        /* Make read end non-blocking */
        int flags = fcntl(term_stderr_pipe[0], F_GETFL, 0);
        fcntl(term_stderr_pipe[0], F_SETFL, flags | O_NONBLOCK);
        dup2(term_stderr_pipe[1], 1); /* stdout -> pipe write end */
        dup2(term_stderr_pipe[1], 2); /* stderr -> pipe too */
        close(term_stderr_pipe[1]);   /* close original write end */
    }

    /* Initial state */
    term_last_ticks = 0;
    term_update_cpu(); /* prime the tick counter */
    term_update_ram();
    term_log("DOOM mobile initialized");
}
#include "v_video.h"

static SDL_Window *window;
static SDL_Surface *screen;  /* scaled window surface */

/* Match phone framebuffer exactly */
#define FB_WIDTH  480
#define FB_HEIGHT 800

/* Game area: 320x200 -> 480x300 (1.5x scale) */
#define GAME_W  480
#define GAME_H  300

/* Window size for Mac (scaled from 480x800, keeps ratio) */
#define WIN_W  640
#define WIN_H  1067

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

/* Touchscreen + keypad state (not used on Mac, kept for struct compatibility) */
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
    SDL_Init(SDL_INIT_VIDEO);
    TTF_Init();
    window = SDL_CreateWindow("fbdoom", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               WIN_W, WIN_H, SDL_WINDOW_SHOWN);
    screen = SDL_GetWindowSurface(window);
    fb = SDL_CreateRGBSurface(0, FB_WIDTH, FB_HEIGHT, 32, 0, 0, 0, 0);
    fb_pixels = (uint32_t *)fb->pixels;
    printf("SDL window %dx%d, fb %dx%d\n", WIN_W, WIN_H, FB_WIDTH, FB_HEIGHT);

    /* Load monospace font for terminal */
    term_font = TTF_OpenFont("/System/Library/Fonts/Monaco.ttf", TERM_FONT_SIZE);
    if (!term_font) {
        term_font = TTF_OpenFont("/Library/Fonts/Menlo.ttc", TERM_FONT_SIZE);
    }
    if (!term_font) {
        term_font = TTF_OpenFont(NULL, TERM_FONT_SIZE); /* default */
    }
    printf("term font: %s\n", term_font ? "loaded" : "FAILED");

    I_InitButtons();
    term_init();
    I_SetTermLog(term_log); /* connect game code -> terminal */
}

void I_ShutdownGraphics(void)
{
    if (term_font) TTF_CloseFont(term_font);
    TTF_Quit();
    SDL_FreeSurface(fb);
    SDL_DestroyWindow(window);
    SDL_Quit();
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
        colors32[i] = SDL_MapRGB(fb->format, r, g, b);
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
                    PUTPIXEL(x + col, cy, color);
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
            PUTPIXEL(cx + dx, cy + dy, color);
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
                PUTPIXEL(cx + dx, cy + dy, color);
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
        PUTPIXEL(cx, cy - i, color);
        PUTPIXEL(cx, cy + i, color);
        PUTPIXEL(cx - i, cy, color);
        PUTPIXEL(cx + i, cy, color);
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
                PUTPIXEL(dx, dy, border);
            else
                PUTPIXEL(dx, dy, bg);
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

    /* Clear fb to black */
    SDL_FillRect(fb, NULL, 0);

    /* Render game at top: 320x200 -> 480x300 (1.5x scale) */
    for (sy = 0; sy < SCREENHEIGHT; sy++) {
        uint32_t src_row_pixels[SCREENWIDTH];
        for (sx = 0; sx < SCREENWIDTH; sx++)
            src_row_pixels[sx] = colors32[*(screens[0] + sy * SCREENWIDTH + sx)];

        int dy0 = sy * 3 / 2;
        int dy1 = dy0 + 1;
        if (dy1 >= FB_HEIGHT) dy1 = FB_HEIGHT - 1;

        for (dy = dy0; dy <= dy1; dy++) {
            uint32_t *dest_row = fb_pixels + dy * FB_STRIDE;
            int dx = 0;
            for (sx = 0; sx < SCREENWIDTH && dx < FB_WIDTH; sx++) {
                dest_row[dx] = src_row_pixels[sx];
                dx++;
                if ((sx & 1) == 0 && dx < FB_WIDTH) {
                    dest_row[dx] = src_row_pixels[sx];
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

    /* Scale fb (480x800) -> window */
    SDL_BlitScaled(fb, NULL, screen, NULL);
    SDL_UpdateWindowSurface(window);
    SDL_Delay(6);
}

/* Check if screen coords are inside joystick zone */
static int js_hit_test(int sx, int sy)
{
    int dx = sx - JS_CENTER_X;
    int dy = sy - JS_CENTER_Y;
    return (dx * dx + dy * dy) <= (JS_RADIUS * JS_RADIUS);
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

static void js_post_keys(void)
{
    float dead = JS_DEADZONE;

    /* Forward/back (y axis) */
    if (js.ny > dead) {
        if (!js_fwd_posted) { js_fwd_posted = 1; D_PostEvent(&(event_t){ev_keydown, 0xad, 0, 0}); }
    } else {
        if (js_fwd_posted) { js_fwd_posted = 0; D_PostEvent(&(event_t){ev_keyup, 0xad, 0, 0}); }
    }
    if (js.ny < -dead) {
        if (!js_back_posted) { js_back_posted = 1; D_PostEvent(&(event_t){ev_keydown, 0xaf, 0, 0}); }
    } else {
        if (js_back_posted) { js_back_posted = 0; D_PostEvent(&(event_t){ev_keyup, 0xaf, 0, 0}); }
    }

    /* Turn left/right (x axis) */
    if (js.nx < -dead) {
        if (!js_left_posted) { js_left_posted = 1; D_PostEvent(&(event_t){ev_keydown, 0xac, 0, 0}); }
    } else {
        if (js_left_posted) { js_left_posted = 0; D_PostEvent(&(event_t){ev_keyup, 0xac, 0, 0}); }
    }
    if (js.nx > dead) {
        if (!js_right_posted) { js_right_posted = 1; D_PostEvent(&(event_t){ev_keydown, 0xae, 0, 0}); }
    } else {
        if (js_right_posted) { js_right_posted = 0; D_PostEvent(&(event_t){ev_keyup, 0xae, 0, 0}); }
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

/* Poll SDL events (keyboard + mouse for Mac testing) */
void I_PollTouch(void)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) {
            I_ShutdownGraphics();
            exit(0);
        }

        if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) {
            int key = ev.key.keysym.sym;
            int down = (ev.type == SDL_KEYDOWN);
            int doom_key = key;
            if (key == SDLK_RIGHT) doom_key = 0xae;
            else if (key == SDLK_LEFT) doom_key = 0xac;
            else if (key == SDLK_UP) doom_key = 0xad;
            else if (key == SDLK_DOWN) doom_key = 0xaf;
            else if (key == SDLK_RETURN) doom_key = 13;
            else if (key == SDLK_SPACE) doom_key = ' ';
            else if (key == SDLK_ESCAPE) doom_key = 27;

            D_PostEvent(&(event_t){down ? ev_keydown : ev_keyup, doom_key, 0, 0});

            /* Escape quits */
            if (ev.type == SDL_KEYUP && doom_key == 27) {
                I_ShutdownGraphics();
                exit(0);
            }
        }

        /* Mouse: click in game area = joystick, in button area = buttons */
        if (ev.type == SDL_MOUSEBUTTONDOWN || ev.type == SDL_MOUSEBUTTONUP) {
            int mx = ev.button.x * FB_WIDTH / WIN_W;
            int my = ev.button.y * FB_HEIGHT / WIN_H;
            int down = (ev.type == SDL_MOUSEBUTTONDOWN);

            if (down) {
                /* Reset any active mode */
                if (ts_mode == 1) js_reset();
                else if (ts_btn_down >= 0) {
                    buttons[ts_btn_down].pressed = 0;
                    D_PostEvent(&(event_t){ev_keyup, buttons[ts_btn_down].key, 0, 0});
                    ts_btn_down = -1;
                }
                ts_mode = 0;

                /* Hit test */
                if (js_hit_test(mx, my)) {
                    ts_mode = 1;
                    js.active = 1;
                    js_update(mx, my);
                    js_post_keys();
                } else {
                    int hit = btn_hit_test(mx, my);
                    if (hit >= 0) {
                        ts_mode = hit + 2;
                        ts_btn_down = hit;
                        buttons[hit].pressed = 1;
                        D_PostEvent(&(event_t){ev_keydown, buttons[hit].key, 0, 0});
                        if (hit == 2 && menuactive) {
                            D_PostEvent(&(event_t){ev_keydown, 13, 0, 0});
                        }
                    }
                }
            } else {
                /* Mouse up */
                if (ts_mode == 1) {
                    js_reset();
                } else if (ts_btn_down >= 0) {
                    buttons[ts_btn_down].pressed = 0;
                    D_PostEvent(&(event_t){ev_keyup, buttons[ts_btn_down].key, 0, 0});
                    if (ts_btn_down == 2 && menuactive) {
                        D_PostEvent(&(event_t){ev_keyup, 13, 0, 0});
                    }
                    ts_btn_down = -1;
                }
                ts_mode = 0;
            }
        }

        /* Mouse move: update joystick if active */
        if (ev.type == SDL_MOUSEMOTION && ts_mode == 1) {
            int mx = ev.motion.x * FB_WIDTH / WIN_W;
            int my = ev.motion.y * FB_HEIGHT / WIN_H;
            js_update(mx, my);
            js_post_keys();
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
