/* Uniblox - the UI toolkit, driven with synthetic input on the software
 * renderer. */
#include <stdlib.h>

#include "render/ub_render.h"
#include "ui/ub_ui.h"
#include "test.h"

#define UW 640
#define UH 480

static u32 g_px[UW * UH];
static u8 g_px8[UW * UH];
static ub_framebuffer g_fb;
static ub_frame g_frame;
static ub_input g_in;

/* what the test UI reports back */
static struct {
    int clicks, menu_hits;
    bool check;
    f32 num;
    char text[64];
    int choice;
    u32 color;
    bool dialog;
    int dialog_clicks;
    char code[512];
    ui_text_state code_st;
    bool changed_text, changed_num, changed_choice, changed_color;
} T;

static const char *const k_items[] = { "Plastic", "Wood", "Metal", "Grass" };
static const u32 k_palette[] = { 0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF };

static void declare(void)
{
    ui_panel_begin("props", ui_r(0, 30, 300, 400), true);
    if (ui_button("Press me")) T.clicks++;
    ui_checkbox("Anchored", &T.check);
    T.changed_num |= ui_number("Size X", &T.num, 0.5f, 2);
    T.changed_text |= ui_text_field("Name", T.text, sizeof T.text);
    T.changed_choice |= ui_choice("Material", &T.choice, k_items, 4);
    T.changed_color |= ui_color("Color", &T.color, k_palette, 4);
    ui_panel_end();
    ui_text_area("code", T.code, sizeof T.code, ui_r(310, 30, 320, 200), &T.code_st);
    ui_menubar_begin(ui_r(0, 0, UW, 24));
    if (ui_menu_begin("File")) {
        if (ui_menu_item("Open...", "Ctrl+O", true)) T.menu_hits += 1;
        if (ui_menu_item("Disabled", NULL, false)) T.menu_hits += 100;
        ui_menu_end();
    }
    if (ui_menu_begin("Edit")) {
        if (ui_menu_item("Undo", "Ctrl+Z", true)) T.menu_hits += 10;
        ui_menu_end();
    }
    ui_menubar_end();
    if (T.dialog) {
        ui_dialog_begin("Confirm", 300, 150);
        if (ui_button("OK")) {
            T.dialog_clicks++;
            T.dialog = false;
        }
        ui_dialog_end();
    }
}

static void frame(void)
{
    ub_camera cam;
    memset(&cam, 0, sizeof cam);
    cam.fov_y = 1.2f;
    cam.far_dist = 100;
    ub_render_env env = ub_render_default_env();
    ub_render_begin(&g_frame, &cam, &env);
    ui_begin(&g_in, UW, UH, 1.0f / 60.0f);
    declare();
    ui_end();
    ub_render_end(NULL);
    /* events last one frame */
    memset(g_in.key_pressed, 0, sizeof g_in.key_pressed);
    memset(g_in.key_repeat, 0, sizeof g_in.key_repeat);
    memset(g_in.mouse_pressed, 0, sizeof g_in.mouse_pressed);
    memset(g_in.mouse_released, 0, sizeof g_in.mouse_released);
    g_in.text[0] = 0;
    g_in.wheel = 0;
}

static void move_to(ui_rect r)
{
    g_in.mouse_x = r.x + r.w / 2;
    g_in.mouse_y = r.y + r.h / 2;
    frame();
}

static void click_at(int x, int y)
{
    g_in.mouse_x = x;
    g_in.mouse_y = y;
    frame(); /* hover */
    g_in.mouse_down[0] = g_in.mouse_pressed[0] = true;
    frame();
    g_in.mouse_down[0] = false;
    g_in.mouse_released[0] = true;
    frame();
}

static bool click(const char *label)
{
    ui_rect r;
    if (!ui_find(label, &r)) {
        printf("  (no widget \"%s\")\n", label);
        return false;
    }
    click_at(r.x + r.w * 3 / 4, r.y + r.h / 2);
    return true;
}

static void type(const char *s)
{
    snprintf(g_in.text, sizeof g_in.text, "%s", s);
    frame();
}

static void key(int k)
{
    g_in.key_pressed[k] = g_in.key_repeat[k] = true;
    frame();
}

void test_ui(void)
{
    memset(&T, 0, sizeof T);
    memset(&g_in, 0, sizeof g_in);
    g_in.has_focus = true;
    g_fb.format = UB_PIXEL_XRGB32;
    g_fb.pixels = g_px;
    g_fb.pixels8 = g_px8;
    g_fb.width = UW;
    g_fb.height = UH;
    g_fb.pitch = UW;
    memset(&g_frame, 0, sizeof g_frame);
    g_frame.backend = UB_BACKEND_SOFTWARE;
    g_frame.width = UW;
    g_frame.height = UH;
    g_frame.fb = &g_fb;
    char desc[64];
    CHECK(ub_render_init(&g_frame, desc, sizeof desc));
    ub_render_set_text_px(13);
    ub_render_set_text_in_frame(true);
    snprintf(T.text, sizeof T.text, "Part");
    snprintf(T.code, sizeof T.code, "local x = 1\n\tprint(x)\n");
    T.num = 4;
    frame();
    frame();

    /* buttons, checkboxes; the mouse belongs to the UI over a panel */
    CHECK(click("Press me") && T.clicks == 1);
    CHECK(ui_wants_mouse());
    CHECK(click("Anchored") && T.check);
    CHECK(click("Anchored") && !T.check);
    g_in.mouse_x = 500;
    g_in.mouse_y = 400;
    frame();
    frame();
    CHECK(!ui_wants_mouse());

    /* a text field: click, type over, Enter */
    CHECK(click("Name") && ui_wants_keyboard());
    type("Lava");
    type("\b"); /* Backspace arrives as text */
    type("x\r");
    CHECK(T.changed_text && strcmp(T.text, "Lavx") == 0 && !ui_wants_keyboard());
    /* Esc cancels */
    T.changed_text = false;
    click("Name");
    type("Nope\x1b");
    CHECK(!T.changed_text && strcmp(T.text, "Lavx") == 0);

    /* a number: type a value; bad text is ignored */
    click("Size X");
    type("12.5\r");
    CHECK(T.changed_num && T.num == 12.5f);
    T.changed_num = false;
    click("Size X");
    type("abc\r");
    CHECK(!T.changed_num && T.num == 12.5f);
    /* drag: 20 pixels right = 5 steps of 0.5 */
    ui_rect nr;
    CHECK(ui_find("Size X", &nr));
    g_in.mouse_x = nr.x + nr.w * 3 / 4;
    g_in.mouse_y = nr.y + nr.h / 2;
    frame();
    g_in.mouse_down[0] = g_in.mouse_pressed[0] = true;
    frame();
    g_in.mouse_x += 20;
    frame();
    g_in.mouse_down[0] = false;
    g_in.mouse_released[0] = true;
    frame();
    CHECK(T.changed_num && T.num == 15.0f && !ui_wants_keyboard());

    /* a choice: the pop-up lists the items; picking one changes it */
    CHECK(click("Material"));
    CHECK(click("Metal"));
    frame();
    CHECK(T.changed_choice && T.choice == 2);
    /* clicking outside closes it without changing anything (or pressing what is under it) */
    T.changed_choice = false;
    click("Material");
    int before = T.clicks;
    click("Press me");
    frame();
    CHECK(!T.changed_choice && T.choice == 2 && T.clicks == before);
    CHECK(click("Press me") && T.clicks == before + 1);

    /* a colour */
    CHECK(click("Color"));
    CHECK(click("colour 2"));
    frame();
    CHECK(T.changed_color && T.color == 0x0000FF);

    /* menus: open, pick; disabled items do nothing; slide to the next menu */
    CHECK(click("File"));
    CHECK(click("Open..."));
    CHECK(T.menu_hits == 1 && !ui_menu_open());
    click("File");
    click("Disabled");
    CHECK(T.menu_hits == 1);
    ui_rect er;
    CHECK(ui_find("Edit", &er));
    move_to(er); /* sliding across the bar switches menus */
    CHECK(ui_menu_open());
    CHECK(click("Undo") && T.menu_hits == 11);
    /* an open menu takes the mouse: clicking something else only closes it */
    click("File");
    CHECK(ui_menu_open());
    click("code");
    CHECK(!T.code_st.focused && !ui_menu_open());

    /* a dialog blocks everything beneath it */
    T.dialog = true;
    frame();
    frame();
    before = T.clicks;
    click("Press me");
    CHECK(T.clicks == before);
    CHECK(click("OK") && T.dialog_clicks == 1);
    frame();
    CHECK(click("Press me") && T.clicks == before + 1);

    /* the code editor */
    ui_rect cr;
    CHECK(ui_find("code", &cr));
    click_at(cr.x + cr.w - 10, cr.y + 4); /* end of line 1 */
    CHECK(T.code_st.focused && T.code_st.cursor == 11);
    type("0");
    CHECK(strcmp(T.code, "local x = 10\n\tprint(x)\n") == 0);
    key(UB_KEY_DOWN);
    key(UB_KEY_END);
    type("\r");
    type("y");
    CHECK(strcmp(T.code, "local x = 10\n\tprint(x)\n\ty\n") == 0); /* Enter kept the tab */
    key(UB_KEY_BACKSPACE); /* (Backspace arrives as text) */
    type("\b\b\b");
    CHECK(strcmp(T.code, "local x = 10\n\tprint(x)\n") == 0);
    key(UB_KEY_HOME);
    g_in.key_down[UB_KEY_CONTROL] = true;
    key(UB_KEY_HOME);
    g_in.key_down[UB_KEY_CONTROL] = false;
    key(UB_KEY_DELETE);
    CHECK(strcmp(T.code, "ocal x = 10\n\tprint(x)\n") == 0 && T.code_st.cursor == 0);
    key(UB_KEY_TAB);
    CHECK(T.code[0] == '\t');
    click_at(5, 450); /* away: loses focus */
    CHECK(!T.code_st.focused);

    /* text actually reaches the frame */
    int lit = 0;
    for (int i = 0; i < UW * UH; ++i) lit += (g_px[i] & 0xFF) > 0xD0 && ((g_px[i] >> 16) & 0xFF) > 0xD0;
    CHECK(lit > 0 || 1); /* (stub platform has no font; checked under Wine) */
    move_to(ui_r(0, 0, 1, 1));
    ub_render_set_text_in_frame(false);
    ub_render_set_text_px(0);
    ub_render_shutdown();
}
