/* arch/arm64/oled_ui.h - on-device OLED user interface (Theme E, issue #121)
 *
 * The stompbox's little screen: browse patches and parameters, and watch live
 * CPU load / headroom meters.  This is the UI *model and layout* - a pure state
 * machine that renders the current screen into a fixed character grid.  The
 * pixel font and the SSD1306/SH1106 bring-up live in the display driver; what is
 * pure and host-tested here is what the screen shows and how the four buttons
 * (up / down / select / back) navigate it.
 *
 * A 128x64 panel with a 6x8 font is 21 columns by 8 rows; the renderer fills a
 * caller-supplied grid of that size with space-padded ASCII, and the driver
 * blits each character through its font.  Integer only, no allocation, no libc.
 */

#ifndef ARM64_OLED_UI_H
#define ARM64_OLED_UI_H

#include <stdint.h>

#define OLED_COLS      21
#define OLED_ROWS      8
#define OLED_MAX_ITEMS 32   /* patches or parameters listed */
#define OLED_NAME_LEN  16   /* max stored name length (excl. NUL) */

typedef enum {
    OLED_SCREEN_HOME = 0,   /* title + CPU/headroom meters + current patch */
    OLED_SCREEN_PATCH,      /* scrollable patch list                       */
    OLED_SCREEN_PARAM,      /* scrollable parameter list for the patch     */
    OLED_SCREEN_SPECTRUM,   /* live spectrum bars + peak-hold (issue #187) */
    OLED_SCREEN_TUNER,      /* note name + cents needle + Hz (issue #187)  */
} oled_screen_t;

#define OLED_MAX_BARS  (OLED_COLS - 1)   /* spectrum bars: one column each */

typedef enum {
    OLED_BTN_UP = 0,
    OLED_BTN_DOWN,
    OLED_BTN_SELECT,
    OLED_BTN_BACK,
} oled_btn_t;

typedef struct {
    char    names[OLED_MAX_ITEMS][OLED_NAME_LEN + 1];
    int32_t values[OLED_MAX_ITEMS];   /* parameter values (unused for patches) */
    int     has_values;
    int     n;
    int     sel;      /* highlighted row            */
    int     top;      /* first visible row (scroll) */
} oled_list_t;

typedef struct {
    oled_screen_t screen;
    oled_list_t   patches;
    oled_list_t   params;
    int           cur_patch;          /* loaded patch index, or -1 */
    uint32_t      cpu_permille;       /* 0..1000 total CPU load    */
    uint32_t      headroom_permille;  /* 0..1000 headroom          */

    /* Spectrum screen state (issue #187): per-mille bar levels + held peaks,
     * produced by the SDK analyser and pushed by the host each update. */
    uint32_t      bars[OLED_MAX_BARS];
    uint32_t      bar_peaks[OLED_MAX_BARS];
    int           n_bars;

    /* Tuner screen state (issue #187): MIDI note (-1 = no signal), cents
     * offset (-50..+50), and frequency in tenths of Hz (integer-only). */
    int           tuner_note;
    int           tuner_cents;
    uint32_t      tuner_dhz;
} oled_ui_t;

void oled_ui_init(oled_ui_t *ui);

/* Replace the patch list (names are copied, truncated to OLED_NAME_LEN). */
void oled_ui_set_patches(oled_ui_t *ui, const char *const *names, int n);

/* Replace the parameter list for the current patch (name + integer value). */
void oled_ui_set_params(oled_ui_t *ui, const char *const *names,
                        const int32_t *values, int n);

/* Update the live meters (per-mille, 0..1000). */
void oled_ui_set_meters(oled_ui_t *ui, uint32_t cpu_permille,
                        uint32_t headroom_permille);

/* Update the spectrum screen: `bars`/`peaks` are per-mille levels (0..1000),
 * up to OLED_MAX_BARS of them (the SDK analyser's bars/peaks arrays). */
void oled_ui_set_spectrum(oled_ui_t *ui, const uint32_t *bars,
                          const uint32_t *peaks, int n);

/* Update the tuner screen: MIDI `note` (-1 = no signal), `cents` (-50..+50),
 * and the frequency in tenths of Hz (4401 = 440.1 Hz). */
void oled_ui_set_tuner(oled_ui_t *ui, int note, int cents, uint32_t dhz);

/* Feed a button press; advances the state machine.  Returns the (possibly new)
 * current screen. */
oled_screen_t oled_ui_input(oled_ui_t *ui, oled_btn_t btn);

/* Render the current screen into `grid` (OLED_ROWS * OLED_COLS chars, row-major,
 * space-padded, no NULs).  The driver blits each cell through the font. */
void oled_ui_render(const oled_ui_t *ui, char *grid);

/* Draw a bracketed bar meter "[####----]" of total width `w` (including the two
 * brackets) filled to `permille`/1000 into `out` (writes exactly `w` chars). */
void oled_ui_bar(char *out, int w, uint32_t permille);

#endif /* ARM64_OLED_UI_H */
