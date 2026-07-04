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

oled_screen_t oled_ui_input(oled_ui_t *ui, oled_btn_t btn)
{
    const int rows = OLED_ROWS - 1;   /* one line reserved for a header */
    switch (ui->screen) {
    case OLED_SCREEN_HOME:
        if (btn == OLED_BTN_SELECT) ui->screen = OLED_SCREEN_PATCH;
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
    }
}
