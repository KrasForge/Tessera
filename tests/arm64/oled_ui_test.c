/* tests/arm64/oled_ui_test.c - host unit tests for the OLED UI model (Theme E,
 * issue #121).
 *
 * Build/run via:  make test-arm-oled-ui
 */

#include "oled_ui.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

/* Extract grid row `r` as a NUL-terminated, right-trimmed string. */
static void row(const char *grid, int r, char *out)
{
    memcpy(out, grid + r * OLED_COLS, OLED_COLS);
    out[OLED_COLS] = '\0';
    for (int i = OLED_COLS - 1; i >= 0 && out[i] == ' '; i--) out[i] = '\0';
}

/* Whole grid contains substring `s`? */
static int grid_has(const char *grid, const char *s)
{
    char line[OLED_COLS + 1];
    for (int r = 0; r < OLED_ROWS; r++) {
        memcpy(line, grid + r * OLED_COLS, OLED_COLS);
        line[OLED_COLS] = '\0';
        if (strstr(line, s)) return 1;
    }
    return 0;
}

static void test_bar(void)
{
    printf("- bar meter fills proportionally\n");
    char b[16];
    oled_ui_bar(b, 10, 0);    b[10] = 0; CHECK(strcmp(b, "[--------]") == 0, "0%% is empty");
    oled_ui_bar(b, 10, 1000); b[10] = 0; CHECK(strcmp(b, "[########]") == 0, "100%% is full");
    oled_ui_bar(b, 10, 500);  b[10] = 0; CHECK(strcmp(b, "[####----]") == 0, "50%% is half");
    oled_ui_bar(b, 10, 1500); b[10] = 0; CHECK(strcmp(b, "[########]") == 0, "over-range clamps full");
}

static void test_home(void)
{
    printf("- home screen shows title, meters, and current patch\n");
    oled_ui_t ui; oled_ui_init(&ui);
    oled_ui_set_meters(&ui, 334, 666);
    char grid[OLED_ROWS * OLED_COLS], line[OLED_COLS + 1];
    oled_ui_render(&ui, grid);
    row(grid, 0, line); CHECK(strcmp(line, "TESSERA") == 0, "row 0 is the title");
    CHECK(grid_has(grid, "CPU"), "CPU label present");
    CHECK(grid_has(grid, "33.4%"), "CPU percentage rendered from per-mille");
    CHECK(grid_has(grid, "66.6%"), "headroom percentage rendered");
    CHECK(grid_has(grid, "-none-"), "no patch loaded yet");
}

static void test_navigation(void)
{
    printf("- four buttons navigate home -> patches -> params -> back\n");
    oled_ui_t ui; oled_ui_init(&ui);
    const char *patches[] = { "Clean", "Crunch", "Lead", "Ambient" };
    oled_ui_set_patches(&ui, patches, 4);

    /* Since issue #187, up/down from home cycle the analysis views
     * (spectrum / tuner); back returns.  The patch flow is unchanged. */
    CHECK(oled_ui_input(&ui, OLED_BTN_DOWN) == OLED_SCREEN_SPECTRUM,
          "home + down enters the spectrum view (issue #187)");
    CHECK(oled_ui_input(&ui, OLED_BTN_BACK) == OLED_SCREEN_HOME,
          "back returns home from the spectrum view");
    CHECK(oled_ui_input(&ui, OLED_BTN_SELECT) == OLED_SCREEN_PATCH, "select enters patch list");

    char grid[OLED_ROWS * OLED_COLS];
    oled_ui_render(&ui, grid);
    CHECK(grid_has(grid, "Patches"), "patch screen header");
    CHECK(grid_has(grid, ">Clean"), "first patch selected by default");

    oled_ui_input(&ui, OLED_BTN_DOWN);
    oled_ui_input(&ui, OLED_BTN_DOWN);      /* -> Lead */
    oled_ui_render(&ui, grid);
    CHECK(grid_has(grid, ">Lead"), "down moves the selection");

    const char *pnames[] = { "Gain", "Tone", "Level" };
    int32_t pvals[] = { 70, 45, 100 };
    oled_ui_set_params(&ui, pnames, pvals, 3);
    CHECK(oled_ui_input(&ui, OLED_BTN_SELECT) == OLED_SCREEN_PARAM, "select loads patch -> params");
    CHECK(ui.cur_patch == 2, "current patch is Lead (index 2)");
    oled_ui_render(&ui, grid);
    CHECK(grid_has(grid, "Params"), "param screen header");
    CHECK(grid_has(grid, "Gain"), "param name shown");
    CHECK(grid_has(grid, "70"), "param value shown");

    CHECK(oled_ui_input(&ui, OLED_BTN_BACK) == OLED_SCREEN_PATCH, "back returns to patches");
    CHECK(oled_ui_input(&ui, OLED_BTN_BACK) == OLED_SCREEN_HOME, "back again returns home");
    oled_ui_render(&ui, grid);
    CHECK(grid_has(grid, "Lead"), "home now shows the loaded patch name");
}

static void test_wrap_and_scroll(void)
{
    printf("- selection wraps and the view scrolls to keep it visible\n");
    oled_ui_t ui; oled_ui_init(&ui);
    const char *names[12];
    char store[12][8];
    for (int i = 0; i < 12; i++) { snprintf(store[i], 8, "P%d", i); names[i] = store[i]; }
    oled_ui_set_patches(&ui, names, 12);
    oled_ui_input(&ui, OLED_BTN_SELECT);           /* enter patch list */

    /* Up from the first entry wraps to the last, which must be visible. */
    oled_ui_input(&ui, OLED_BTN_UP);
    char grid[OLED_ROWS * OLED_COLS];
    oled_ui_render(&ui, grid);
    CHECK(ui.patches.sel == 11, "up from top wraps to the last entry");
    CHECK(grid_has(grid, ">P11"), "the wrapped selection scrolled into view");
    CHECK(!grid_has(grid, ">P0"), "the top of the list scrolled away");

    /* Back down wraps to the first, scrolling the view back up. */
    oled_ui_input(&ui, OLED_BTN_DOWN);
    oled_ui_render(&ui, grid);
    CHECK(ui.patches.sel == 0, "down from bottom wraps to the first");
    CHECK(grid_has(grid, ">P0"), "the first entry is visible again");
}

int main(void)
{
    printf("=== Tessera OLED UI tests (Theme E, #121) ===\n");
    test_bar();
    test_home();
    test_navigation();
    test_wrap_and_scroll();
    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
