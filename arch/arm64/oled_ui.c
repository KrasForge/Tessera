/* arch/arm64/oled_ui.c - on-device OLED user interface (Theme E, issue #121).
 * See oled_ui.h. */

#include "oled_ui.h"

/* ---- small string helpers (no libc on the audio-adjacent path) ----------- */

static int str_copy(char *dst, const char *src, int cap)
{
    int i = 0;
    for (; src && src[i] && i < cap; i++) dst[i] = src[i];
    dst[i] = '\0';
    return i;
}

/* Copy `s` into a grid row at column `col`, not past the row end. */
static void put(char *grid, int row, int col, const char *s)
{
    for (int i = 0; s[i] && col + i < OLED_COLS; i++)
        grid[row * OLED_COLS + col + i] = s[i];
}

/* Unsigned integer to decimal; returns length written. */
static int u32_dec(char *out, uint32_t v)
{
    char tmp[10];
    int n = 0;
    do { tmp[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = '\0';
    return n;
}

/* Signed integer to decimal. */
static void i32_dec(char *out, int32_t v)
{
    if (v < 0) { out[0] = '-'; u32_dec(out + 1, (uint32_t)(-(int64_t)v)); }
    else       u32_dec(out, (uint32_t)v);
}

/* "NN.N%" from a 0..1000 per-mille value, e.g. 334 -> "33.4%". */
static void percent_str(char *out, uint32_t permille)
{
    if (permille > 1000u) permille = 1000u;
    int n = u32_dec(out, permille / 10u);
    out[n++] = '.';
    out[n++] = (char)('0' + permille % 10u);
    out[n++] = '%';
    out[n]   = '\0';
}

/* ---- lists --------------------------------------------------------------- */

static void list_clear(oled_list_t *l) { l->n = 0; l->sel = 0; l->top = 0; l->has_values = 0; }

/* Keep the selection visible in a window of `rows` visible entries. */
static void list_scroll(oled_list_t *l, int rows)
{
    if (l->sel < l->top)            l->top = l->sel;
    else if (l->sel >= l->top + rows) l->top = l->sel - rows + 1;
    if (l->top < 0) l->top = 0;
}

static void list_move(oled_list_t *l, int delta, int rows)
{
    if (l->n == 0) return;
    l->sel += delta;
    if (l->sel < 0)       l->sel = l->n - 1;   /* wrap */
    else if (l->sel >= l->n) l->sel = 0;
    list_scroll(l, rows);
}

/* ---- public API ---------------------------------------------------------- */

void oled_ui_init(oled_ui_t *ui)
{
    ui->screen = OLED_SCREEN_HOME;
    list_clear(&ui->patches);
    list_clear(&ui->params);
    ui->cur_patch = -1;
    ui->cpu_permille = 0;
    ui->headroom_permille = 1000;
    ui->n_bars = 0;
    for (int i = 0; i < OLED_MAX_BARS; i++) {
        ui->bars[i] = 0;
        ui->bar_peaks[i] = 0;
    }
    ui->tuner_note  = -1;
    ui->tuner_cents = 0;
    ui->tuner_dhz   = 0;
}

void oled_ui_set_patches(oled_ui_t *ui, const char *const *names, int n)
{
    if (n > OLED_MAX_ITEMS) n = OLED_MAX_ITEMS;
    ui->patches.n = n;
    ui->patches.has_values = 0;
    for (int i = 0; i < n; i++)
        str_copy(ui->patches.names[i], names[i], OLED_NAME_LEN);
    if (ui->patches.sel >= n) ui->patches.sel = n ? n - 1 : 0;
    list_scroll(&ui->patches, OLED_ROWS - 1);
}

void oled_ui_set_params(oled_ui_t *ui, const char *const *names,
                        const int32_t *values, int n)
{
    if (n > OLED_MAX_ITEMS) n = OLED_MAX_ITEMS;
    ui->params.n = n;
    ui->params.has_values = 1;
    for (int i = 0; i < n; i++) {
        str_copy(ui->params.names[i], names[i], OLED_NAME_LEN);
        ui->params.values[i] = values[i];
    }
    if (ui->params.sel >= n) ui->params.sel = n ? n - 1 : 0;
    list_scroll(&ui->params, OLED_ROWS - 1);
}

void oled_ui_set_meters(oled_ui_t *ui, uint32_t cpu_permille,
                        uint32_t headroom_permille)
{
    ui->cpu_permille      = cpu_permille > 1000u ? 1000u : cpu_permille;
    ui->headroom_permille = headroom_permille > 1000u ? 1000u : headroom_permille;
}

void oled_ui_set_spectrum(oled_ui_t *ui, const uint32_t *bars,
                          const uint32_t *peaks, int n)
{
    if (n > OLED_MAX_BARS) n = OLED_MAX_BARS;
    ui->n_bars = n;
    for (int i = 0; i < n; i++) {
        ui->bars[i]      = bars[i]  > 1000u ? 1000u : bars[i];
        ui->bar_peaks[i] = peaks[i] > 1000u ? 1000u : peaks[i];
    }
}

void oled_ui_set_tuner(oled_ui_t *ui, int note, int cents, uint32_t dhz)
{
    if (cents < -50) cents = -50;
    if (cents >  50) cents =  50;
    ui->tuner_note  = note;
    ui->tuner_cents = cents;
    ui->tuner_dhz   = dhz;
}

oled_screen_t oled_ui_input(oled_ui_t *ui, oled_btn_t btn)
{
    const int rows = OLED_ROWS - 1;   /* one line reserved for a header */
    switch (ui->screen) {
    case OLED_SCREEN_HOME:
        if (btn == OLED_BTN_SELECT)     ui->screen = OLED_SCREEN_PATCH;
        else if (btn == OLED_BTN_DOWN)  ui->screen = OLED_SCREEN_SPECTRUM;
        else if (btn == OLED_BTN_UP)    ui->screen = OLED_SCREEN_TUNER;
        break;
    case OLED_SCREEN_PATCH:
        if (btn == OLED_BTN_UP)         list_move(&ui->patches, -1, rows);
        else if (btn == OLED_BTN_DOWN)  list_move(&ui->patches, +1, rows);
        else if (btn == OLED_BTN_SELECT) {
            if (ui->patches.n) { ui->cur_patch = ui->patches.sel; ui->screen = OLED_SCREEN_PARAM; }
        } else if (btn == OLED_BTN_BACK) ui->screen = OLED_SCREEN_HOME;
        break;
    case OLED_SCREEN_PARAM:
        if (btn == OLED_BTN_UP)         list_move(&ui->params, -1, rows);
        else if (btn == OLED_BTN_DOWN)  list_move(&ui->params, +1, rows);
        else if (btn == OLED_BTN_BACK)  ui->screen = OLED_SCREEN_PATCH;
        break;
    case OLED_SCREEN_SPECTRUM:
        /* The view cycle: HOME -> (down) SPECTRUM -> (down) TUNER -> HOME. */
        if (btn == OLED_BTN_DOWN)       ui->screen = OLED_SCREEN_TUNER;
        else if (btn == OLED_BTN_UP ||
                 btn == OLED_BTN_BACK)  ui->screen = OLED_SCREEN_HOME;
        break;
    case OLED_SCREEN_TUNER:
        if (btn == OLED_BTN_DOWN ||
            btn == OLED_BTN_BACK)       ui->screen = OLED_SCREEN_HOME;
        else if (btn == OLED_BTN_UP)    ui->screen = OLED_SCREEN_SPECTRUM;
        break;
    }
    return ui->screen;
}

void oled_ui_bar(char *out, int w, uint32_t permille)
{
    if (w < 2) { for (int i = 0; i < w; i++) out[i] = ' '; return; }
    if (permille > 1000u) permille = 1000u;
    int inner  = w - 2;
    int filled = (int)((permille * (uint32_t)inner + 500u) / 1000u);
    out[0] = '[';
    for (int i = 0; i < inner; i++) out[1 + i] = i < filled ? '#' : '-';
    out[w - 1] = ']';
}

static void render_list(const oled_ui_t *ui, const oled_list_t *l,
                        const char *title, char *grid)
{
    put(grid, 0, 0, title);
    const int rows = OLED_ROWS - 1;
    for (int r = 0; r < rows; r++) {
        int idx = l->top + r;
        if (idx >= l->n) break;
        int row = r + 1;
        /* Selection marker; the driver may also invert the whole row. */
        grid[row * OLED_COLS + 0] = (idx == l->sel) ? '>' : ' ';
        put(grid, row, 1, l->names[idx]);
        if (l->has_values) {
            char num[12];
            i32_dec(num, l->values[idx]);
            int len = 0; while (num[len]) len++;
            int col = OLED_COLS - len;
            if (col < 2) col = 2;
            put(grid, row, col, num);
        }
    }
}

void oled_ui_render(const oled_ui_t *ui, char *grid)
{
    for (int i = 0; i < OLED_ROWS * OLED_COLS; i++) grid[i] = ' ';

    switch (ui->screen) {
    case OLED_SCREEN_HOME: {
        char buf[OLED_COLS + 1];
        put(grid, 0, 0, "TESSERA");
        /* CPU meter row: "CPU [####----] 42.0%" */
        put(grid, 2, 0, "CPU");
        oled_ui_bar(buf, 10, ui->cpu_permille); buf[10] = '\0';
        put(grid, 2, 4, buf);
        percent_str(buf, ui->cpu_permille);
        put(grid, 2, 15, buf);
        /* Headroom meter row. */
        put(grid, 3, 0, "HDR");
        oled_ui_bar(buf, 10, ui->headroom_permille); buf[10] = '\0';
        put(grid, 3, 4, buf);
        percent_str(buf, ui->headroom_permille);
        put(grid, 3, 15, buf);
        /* Current patch. */
        put(grid, 5, 0, "Patch:");
        if (ui->cur_patch >= 0 && ui->cur_patch < ui->patches.n)
            put(grid, 5, 7, ui->patches.names[ui->cur_patch]);
        else
            put(grid, 5, 7, "-none-");
        break;
    }
    case OLED_SCREEN_PATCH:
        render_list(ui, &ui->patches, "Patches", grid);
        break;
    case OLED_SCREEN_PARAM:
        render_list(ui, &ui->params, "Params", grid);
        break;
    case OLED_SCREEN_SPECTRUM: {
        put(grid, 0, 0, "Spectrum");
        /* One column per bar over the 7 rows below the title: '#' up to the
         * bar level, '-' marking the held peak above it. */
        const int rows = OLED_ROWS - 1;                     /* 7 */
        for (int b = 0; b < ui->n_bars && b < OLED_MAX_BARS; b++) {
            int h  = (int)((ui->bars[b]      * (uint32_t)rows + 500u) / 1000u);
            int ph = (int)((ui->bar_peaks[b] * (uint32_t)rows + 500u) / 1000u);
            for (int y = 0; y < h; y++)
                grid[(OLED_ROWS - 1 - y) * OLED_COLS + b] = '#';
            if (ph > h && ph <= rows)
                grid[(OLED_ROWS - ph) * OLED_COLS + b] = '-';
        }
        break;
    }
    case OLED_SCREEN_TUNER: {
        char buf[OLED_COLS + 1];
        put(grid, 0, 0, "Tuner");
        if (ui->tuner_note < 0) {
            put(grid, 3, 6, "-listen-");
            break;
        }
        /* Note name + octave, e.g. "A4" (MIDI 69), "C#3". */
        static const char *const names[12] = {
            "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
        };
        const char *nm = names[ui->tuner_note % 12];
        int oct = ui->tuner_note / 12 - 1;
        int col = 8;
        put(grid, 2, col, nm);
        col += nm[1] ? 2 : 1;
        if (oct < 0) { put(grid, 2, col, "-"); col++; oct = -oct; }
        u32_dec(buf, (uint32_t)oct);
        put(grid, 2, col, buf);
        /* Cents needle: 21 columns spanning -50..+50, '|' at centre pitch,
         * '*' at the current offset. */
        for (int i = 0; i < OLED_COLS; i++)
            grid[4 * OLED_COLS + i] = (i == OLED_COLS / 2) ? '|' : '.';
        int pos = (ui->tuner_cents + 50) * (OLED_COLS - 1) / 100;
        grid[4 * OLED_COLS + pos] = '*';
        /* "-12c" and "440.0Hz". */
        i32_dec(buf, ui->tuner_cents);
        int len = 0; while (buf[len]) len++;
        buf[len++] = 'c'; buf[len] = '\0';
        put(grid, 6, 0, buf);
        int n = u32_dec(buf, ui->tuner_dhz / 10u);
        buf[n++] = '.';
        buf[n++] = (char)('0' + ui->tuner_dhz % 10u);
        buf[n++] = 'H'; buf[n++] = 'z'; buf[n] = '\0';
        put(grid, 6, OLED_COLS - (n), buf);
        break;
    }
    }
}
