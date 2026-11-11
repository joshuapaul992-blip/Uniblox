/* Uniblox - things the player interacts with (milestone 8B):
 * ClickDetector (clicks and hovering, through the mouse's target),
 * ProximityPrompt (shown near the character, triggered by its key, held
 * for HoldDuration; drawn as an engine-owned BillboardGui in Uniblox's own
 * style under a CoreGui root scripts do not see into) and the Humanoid's
 * state events (StateChanged, Running, Jumping, FreeFalling). */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "gui/ub_gui.h"
#include "physics/ub_pmath.h"
#include "script/ub_sapi.h"
#include "world/ub_classes.h"

#define W (&S.g->world)
#define IN() (&S.g->input)

static bool streq(const char *a, const char *b) { return strcmp(a, b) == 0; }
static const char *cls_name(i32 i) { return ub_world_str(W, W->inst[i].class_name); }
static bool is_class(i32 i, const char *c) { return i >= 0 && ub_world_alive(W, i) && streq(cls_name(i), c); }

static f64 num_prop(i32 i, const char *k, f64 dflt)
{
    const ub_value *v = ub_world_prop_named(W, i, k);
    if (!v) return dflt;
    switch (v->type) {
    case UB_PV_FLOAT: return v->v.f;
    case UB_PV_DOUBLE: return v->v.d;
    case UB_PV_INT: case UB_PV_ENUM: return v->v.i;
    case UB_PV_BOOL: return v->v.b;
    default: return dflt;
    }
}

static const char *str_prop(i32 i, const char *k, u32 *len)
{
    const ub_value *v = ub_world_prop_named(W, i, k);
    if (!v || v->type != UB_PV_STRING) {
        *len = 0;
        return "";
    }
    *len = v->v.str.len;
    return v->v.str.s;
}

/* where the player's character is (Uniblox space): its root, as Roblox measures */
static bool char_pos(vec3 *out)
{
    if (S.g->dead || S.g->char_model < 0) return false;
    *out = v3_add(S.g->feet, v3(0, 3, 0));
    return true;
}

/* -------------------------------------------------------- ClickDetector */

static i32 g_hover = -1, g_down = -1, g_rdown = -1;

/* the ClickDetector a click on `part` reaches: in the part, or in a model around it */
static i32 detector_for(i32 part, vec3 hit_rbx)
{
    vec3 me;
    if (part < 0 || !char_pos(&me)) return -1;
    for (i32 a = part; a >= 0; a = W->inst[a].parent) {
        for (i32 c = W->inst[a].first_child; c >= 0; c = W->inst[c].next_sibling) {
            if (!is_class(c, "ClickDetector")) continue;
            f64 range = num_prop(c, "MaxActivationDistance", 32);
            if (v3_length(v3_sub(rbx_vec(hit_rbx), me)) > range) return -1;
            return c;
        }
        if (a != part && !is_class(a, "Model")) break; /* only models pass clicks on */
    }
    return -1;
}

static void fire_player(lua_State *L, i32 inst, int ev)
{
    if (inst < 0 || !ub_world_alive(W, inst)) return;
    if (S.g->role == UB_ROLE_CLIENT) net_player_event(inst, ev); /* the server's scripts hear it too */
    push_inst(L, S.g->player);
    fire_signal(L, inst, ev, NULL, 1);
}

static void click_pump(lua_State *L)
{
    const ub_game_input *in = IN();
    i32 det = -1;
    if (in->have_cam && !in->gui_mouse) {
        vec3 hit;
        det = detector_for(gui_mouse_target(&hit), hit);
    }
    if (det != g_hover) {
        i32 old = g_hover;
        g_hover = det;
        fire_player(L, old, EV_MOUSELEAVE);
        fire_player(L, det, EV_MOUSEENTER);
    }
    if (in->pressed[0]) g_down = det;
    if (in->pressed[1]) g_rdown = det;
    if (in->released[0]) {
        if (g_down >= 0 && g_down == det) fire_player(L, det, EV_B1CLICK);
        g_down = -1;
    }
    if (in->released[1]) {
        if (g_rdown >= 0 && g_rdown == det) fire_player(L, det, EV_B2CLICK);
        g_rdown = -1;
    }
}

/* ------------------------------------------------------- ProximityPrompt */

#define MAX_PROMPTS 256

typedef struct {
    i32 inst;
    i32 bb;              /* its BillboardGui under CoreGui, -1 none */
    i32 action, object, key, fill;
    bool shown, holding, triggered;
    f32 hold;
    char last_action[64], last_object[64];
    int last_key;
} sprompt;

static sprompt g_p[MAX_PROMPTS];
static int g_np;
static f64 g_scan_t = -1;
static u32 g_scan_epoch;
static i32 g_core = -1;

static ub_value vudim2(f32 sx, i32 ox, f32 sy, i32 oy)
{
    ub_value v;
    memset(&v, 0, sizeof v);
    v.type = UB_PV_UDIM2;
    v.v.ud2.sx = sx;
    v.v.ud2.ox = ox;
    v.v.ud2.sy = sy;
    v.v.ud2.oy = oy;
    return v;
}

static ub_value vcolor(f32 r, f32 g, f32 b)
{
    ub_value v;
    memset(&v, 0, sizeof v);
    v.type = UB_PV_COLOR3;
    v.v.c3.r = r;
    v.v.c3.g = g;
    v.v.c3.b = b;
    return v;
}

static ub_value vref(i32 i)
{
    ub_value v;
    memset(&v, 0, sizeof v);
    v.type = UB_PV_REF;
    v.v.ref = i;
    return v;
}

static void setv(i32 i, const char *k, ub_value v) { ub_world_set_prop_named(W, i, k, &v); }

static i32 make(const char *cls, i32 parent, const char *name)
{
    i32 i = gui_instance_new(cls);
    if (i < 0) return -1;
    ub_world_set_name(W, i, name);
    ub_world_set_parent(W, i, parent);
    W->inst[i].flags |= UB_INST_RUNTIME;
    return i;
}

static void frame_look(i32 f, f32 r, f32 g, f32 b, f32 transparency, i32 corner)
{
    setv(f, "BackgroundColor3", vcolor(r, g, b));
    setv(f, "BackgroundTransparency", ub_vfloat(transparency));
    setv(f, "BorderSizePixel", ub_vint(0));
    if (corner > 0) {
        i32 c = make("UICorner", f, "Corner");
        ub_value u;
        memset(&u, 0, sizeof u);
        u.type = UB_PV_UDIM;
        u.v.ud.o = corner;
        if (c >= 0) setv(c, "CornerRadius", u);
    }
}

static i32 label(i32 parent, const char *name, f32 size, bool bold, f32 r, f32 g, f32 b, int xalign)
{
    i32 t = make("TextLabel", parent, name);
    if (t < 0) return -1;
    setv(t, "BackgroundTransparency", ub_vfloat(1));
    setv(t, "TextColor3", vcolor(r, g, b));
    setv(t, "TextSize", ub_vfloat(size));
    setv(t, "TextXAlignment", ub_venum((u32)xalign));
    ub_value f;
    memset(&f, 0, sizeof f);
    f.type = UB_PV_FONT;
    f.v.font.family = (char *)"rbxasset://fonts/families/SourceSansPro.json";
    f.v.font.weight = bold ? 700 : 400;
    setv(t, "FontFace", f);
    setv(t, "Font", ub_venum(bold ? 4u : 3u)); /* SourceSansBold / SourceSans */
    return t;
}

static void build_billboard(sprompt *p, i32 adornee)
{
    g_core = S.g->core_gui;
    if (g_core < 0 || !ub_world_alive(W, g_core)) return;
    i32 bb = make("BillboardGui", g_core, "Prompt");
    if (bb < 0) return;
    setv(bb, "Adornee", vref(adornee));
    setv(bb, "AlwaysOnTop", ub_vbool(true));
    setv(bb, "Size", vudim2(0, 210, 0, 52));
    setv(bb, "LightInfluence", ub_vfloat(0));
    setv(bb, "Enabled", ub_vbool(false));
    i32 body = make("Frame", bb, "Body");
    setv(body, "Size", vudim2(1, 0, 1, 0));
    frame_look(body, 0.09f, 0.10f, 0.13f, 0.2f, 10);
    i32 key = make("Frame", body, "Key");
    setv(key, "Position", vudim2(0, 9, 0, 9));
    setv(key, "Size", vudim2(0, 34, 0, 34));
    frame_look(key, 0.93f, 0.94f, 0.97f, 0, 7);
    i32 kt = label(key, "KeyText", 20, true, 0.09f, 0.10f, 0.13f, 2);
    setv(kt, "Size", vudim2(1, 0, 1, 0));
    i32 fill = make("Frame", body, "Hold");
    setv(fill, "Position", vudim2(0, 0, 1, -4));
    setv(fill, "Size", vudim2(0, 0, 0, 4));
    frame_look(fill, 0.36f, 0.78f, 0.96f, 0, 2);
    i32 action = label(body, "Action", 19, true, 1, 1, 1, 0);
    i32 object = label(body, "Object", 15, false, 0.72f, 0.75f, 0.80f, 0);
    setv(action, "Position", vudim2(0, 52, 0, 5));
    setv(action, "Size", vudim2(1, -58, 0, 24));
    setv(object, "Position", vudim2(0, 52, 0, 28));
    setv(object, "Size", vudim2(1, -58, 0, 18));
    p->bb = bb;
    p->action = action;
    p->object = object;
    p->key = kt;
    p->fill = fill;
    p->last_key = -1;
    p->last_action[0] = p->last_object[0] = 1; /* never equal: set the first time */
}

/* the part a prompt is shown at, and its position (Uniblox space) */
static i32 prompt_part(i32 prompt, vec3 *pos)
{
    i32 p = W->inst[prompt].parent;
    vec3 off = v3(0, 0, 0);
    if (p >= 0 && is_class(p, "Attachment")) {
        const ub_value *a = ub_world_prop_named(W, p, "Position");
        if (a && a->type == UB_PV_VECTOR3) off = v3(a->v.v3.x, a->v.v3.y, a->v.v3.z);
        p = W->inst[p].parent;
    }
    if (p >= 0 && W->inst[p].part < 0) { /* a model: its first part */
        i32 n;
        i32 *sub = sapi_subtree(p, &n);
        i32 found = -1;
        for (i32 j = 1; sub && j < n && found < 0; ++j)
            if (W->inst[sub[j]].part >= 0) found = sub[j];
        plat_free(sub);
        p = found;
    }
    if (p < 0 || W->inst[p].part < 0) return -1;
    const ub_part *pt = &W->parts[W->inst[p].part];
    *pos = v3_add(pt->pos, mat3_apply(&pt->rot, rbx_vec(off)));
    return p;
}

static void rescan(void)
{
    i32 ws = ub_world_workspace(W);
    bool keep[MAX_PROMPTS] = { false };
    int found = 0;
    for (i32 i = 0; i < W->count; ++i) {
        if (!is_class(i, "ProximityPrompt") || !ub_world_is_descendant(W, i, ws)) continue;
        int k = 0;
        while (k < g_np && g_p[k].inst != i) k++;
        if (k == g_np) {
            if (g_np >= MAX_PROMPTS) continue;
            memset(&g_p[g_np], 0, sizeof g_p[0]);
            g_p[g_np].inst = i;
            g_p[g_np].bb = -1;
            g_np++;
        }
        keep[k] = true;
        found++;
    }
    for (int k = 0; k < g_np;) {
        if (keep[k]) { ++k; continue; }
        if (g_p[k].bb >= 0 && ub_world_alive(W, g_p[k].bb)) ub_world_destroy(W, g_p[k].bb);
        g_p[k] = g_p[--g_np];
        keep[k] = keep[g_np];
    }
    (void)found;
}

static void fire_prompt(lua_State *L, sprompt *p, int ev, bool input_type)
{
    /* on the prompt: (player) or (inputType); on ProximityPromptService: (prompt, the same) */
    if (!input_type && S.g->role == UB_ROLE_CLIENT) net_player_event(p->inst, ev);
    if (input_type) push_enum(L, enum_type_named("ProximityPromptInputType"), 0); /* Keyboard */
    else push_inst(L, S.g->player);
    fire_signal(L, p->inst, ev, NULL, 1);
    push_inst(L, p->inst);
    if (input_type) push_enum(L, enum_type_named("ProximityPromptInputType"), 0);
    else push_inst(L, S.g->player);
    fire_signal(L, VIRT_PROMPTSERVICE, ev, NULL, 2);
}

static void hold_begin(lua_State *L, sprompt *p)
{
    if (p->holding) return;
    p->holding = true;
    p->hold = 0;
    p->triggered = false;
    if (num_prop(p->inst, "HoldDuration", 0) > 0) fire_prompt(L, p, EV_HOLDBEGAN, false);
    else {
        p->triggered = true;
        fire_prompt(L, p, EV_TRIGGERED, false);
    }
}

static void hold_end(lua_State *L, sprompt *p)
{
    if (!p->holding) return;
    p->holding = false;
    bool held = num_prop(p->inst, "HoldDuration", 0) > 0;
    if (p->triggered) fire_prompt(L, p, EV_TRIGGERENDED, false);
    if (held) fire_prompt(L, p, EV_HOLDENDED, false);
    p->hold = 0;
    p->triggered = false;
}

static void set_text(i32 t, char *last, size_t cap, const char *s, u32 n)
{
    if (t < 0) return;
    if (n >= cap) n = (u32)cap - 1;
    if (memcmp(last, s, n) == 0 && last[n] == 0) return;
    memcpy(last, s, n);
    last[n] = 0;
    ub_value v = ub_vstr(s, n);
    setv(t, "Text", v);
}

static void draw_prompt(sprompt *p, i32 part)
{
    bool want = p->shown && num_prop(p->inst, "Style", 0) == 0; /* Default; Custom draws nothing */
    if (!want) {
        if (p->bb >= 0 && ub_world_alive(W, p->bb) && num_prop(p->bb, "Enabled", 0) != 0)
            setv(p->bb, "Enabled", ub_vbool(false));
        return;
    }
    if (p->bb < 0 || !ub_world_alive(W, p->bb)) build_billboard(p, part);
    if (p->bb < 0) return;
    if (num_prop(p->bb, "Enabled", 0) == 0) setv(p->bb, "Enabled", ub_vbool(true));
    const ub_value *ad = ub_world_prop_named(W, p->bb, "Adornee");
    if (!ad || ad->v.ref != part) setv(p->bb, "Adornee", vref(part));
    u32 al, ol;
    const char *action = str_prop(p->inst, "ActionText", &al);
    const char *object = str_prop(p->inst, "ObjectText", &ol);
    set_text(p->action, p->last_action, sizeof p->last_action, action, al);
    set_text(p->object, p->last_object, sizeof p->last_object, object, ol);
    if (p->action >= 0) setv(p->action, "Position", vudim2(0, 52, 0, ol ? 5 : 14));
    int kc = (int)num_prop(p->inst, "KeyboardKeyCode", 101);
    if (kc != p->last_key && p->key >= 0) {
        p->last_key = kc;
        const char *kn = enum_item_name(enum_type_named("KeyCode"), kc);
        if (!kn) kn = "?";
        ub_value v = ub_vstr(kn, (u32)strlen(kn));
        setv(p->key, "Text", v);
        setv(p->key, "TextSize", ub_vfloat(strlen(kn) <= 2 ? 20.0f : 11.0f));
    }
    f64 dur = num_prop(p->inst, "HoldDuration", 0);
    f32 frac = dur > 0 && p->holding ? (f32)(p->hold / dur) : 0;
    if (frac > 1) frac = 1;
    if (p->fill >= 0) setv(p->fill, "Size", vudim2(frac, 0, 0, 4));
}

static void prompt_pump(lua_State *L, f32 dt)
{
    f64 now = script_time();
    if (g_scan_t < 0 || (W->epoch != g_scan_epoch && now - g_scan_t > 0.25) || now - g_scan_t > 2.0) {
        rescan();
        g_scan_t = now;
        g_scan_epoch = W->epoch;
    }
    vec3 me;
    bool alive = char_pos(&me);
    f32 dist[MAX_PROMPTS];
    i32 parts[MAX_PROMPTS];
    bool in_range[MAX_PROMPTS];
    for (int k = 0; k < g_np; ++k) {
        sprompt *p = &g_p[k];
        in_range[k] = false;
        parts[k] = -1;
        if (!alive || !ub_world_alive(W, p->inst) || num_prop(p->inst, "Enabled", 1) == 0) continue;
        vec3 at;
        parts[k] = prompt_part(p->inst, &at);
        if (parts[k] < 0) continue;
        dist[k] = v3_length(v3_sub(at, me));
        in_range[k] = dist[k] <= num_prop(p->inst, "MaxActivationDistance", 10);
    }
    /* Exclusivity: OnePerButton (0) the nearest per key, OneGlobally (1) the nearest, AlwaysShow (2) all */
    for (int k = 0; k < g_np; ++k) {
        if (!in_range[k]) continue;
        int ex = (int)num_prop(g_p[k].inst, "Exclusivity", 0);
        if (ex == 2) continue;
        int key = (int)num_prop(g_p[k].inst, "KeyboardKeyCode", 101);
        for (int j = 0; j < g_np; ++j) {
            if (j == k || !in_range[j] || num_prop(g_p[j].inst, "Exclusivity", 0) == 2) continue;
            if (ex == 0 && (int)num_prop(g_p[j].inst, "KeyboardKeyCode", 101) != key) continue;
            if (dist[j] < dist[k] || (dist[j] == dist[k] && j < k)) {
                in_range[k] = false;
                break;
            }
        }
    }
    const ub_game_input *in = IN();
    for (int k = 0; k < g_np; ++k) {
        sprompt *p = &g_p[k];
        if (!ub_world_alive(W, p->inst)) continue;
        if (in_range[k] != p->shown) {
            if (!in_range[k]) hold_end(L, p);
            p->shown = in_range[k];
            fire_prompt(L, p, p->shown ? EV_PROMPTSHOWN : EV_PROMPTHIDDEN, true);
            if (!ub_world_alive(W, p->inst)) continue;
        }
        if (p->shown) {
            int vk = gui_vk_of((int)num_prop(p->inst, "KeyboardKeyCode", 101));
            if (vk > 0 && in->key_pressed[vk] && !in->gui_keys) hold_begin(L, p);
            if (vk > 0 && in->key_released[vk]) hold_end(L, p);
            if (p->holding && !p->triggered) {
                f64 dur = num_prop(p->inst, "HoldDuration", 0);
                p->hold += dt;
                if (p->hold >= dur) {
                    p->triggered = true;
                    fire_prompt(L, p, EV_TRIGGERED, false);
                }
            }
        }
        if (ub_world_alive(W, p->inst)) draw_prompt(p, parts[k]);
    }
}

/* ProximityPrompt:InputHoldBegin() / InputHoldEnd() */
static sprompt *find_prompt(i32 inst)
{
    for (int k = 0; k < g_np; ++k)
        if (g_p[k].inst == inst) return &g_p[k];
    return NULL;
}

static int m_holdbegin(lua_State *L)
{
    sprompt *p = find_prompt(check_inst(L, 1));
    if (p && p->shown) hold_begin(L, p);
    return 0;
}

static int m_holdend(lua_State *L)
{
    sprompt *p = find_prompt(check_inst(L, 1));
    if (p) hold_end(L, p);
    return 0;
}

static void tools_pump(lua_State *L);
void teams_pump(lua_State *L);
lua_CFunction teams_method(i32 idx, const char *k);
int tools_event(i32 idx, const char *k);
lua_CFunction tools_method(i32 idx, const char *k);
void tools_reset(void);
void get_extra_value(lua_State *L, i32 idx, const char *key);
void set_extra_value(lua_State *L, i32 idx, const char *key, int v);

/* -------------------------------------------------------------- Humanoid */

static int g_state = -1;     /* HumanoidStateType value */
static f32 g_last_speed = -1;

static void set_state(lua_State *L, int st)
{
    if (st == g_state) return;
    int old = g_state;
    g_state = st;
    i32 h = S.g->humanoid;
    if (h < 0) return;
    if (old >= 0) {
        push_enum(L, EN_HUMANOIDSTATE, old);
        push_enum(L, EN_HUMANOIDSTATE, st);
        fire_signal(L, h, EV_STATECHANGED, NULL, 2);
    }
    if (st == 3) { /* Jumping */
        lua_pushboolean(L, 1);
        fire_signal(L, h, EV_JUMPING, NULL, 1);
    }
    if (st == 5 || old == 5) { /* Freefall */
        lua_pushboolean(L, st == 5);
        fire_signal(L, h, EV_FREEFALLING, NULL, 1);
    }
}

static void humanoid_pump(lua_State *L)
{
    if (S.g->humanoid < 0) return;
    if (S.g->dead) {
        set_state(L, 15);
        g_last_speed = -1;
        return;
    }
    if (!S.g->phys_ok) return;
    vec3 v = ub_char_velocity(&S.g->chr, &S.g->phys);
    f32 speed = (f32)sqrt(v.x * v.x + v.z * v.z);
    if (S.g->chr.grounded) {
        if (g_state == 5 || g_state == 3) set_state(L, 7); /* Landed, then Running */
        set_state(L, 8);
        if (g_last_speed < 0 || fabs(speed - g_last_speed) > 0.25f || (speed < 0.01f && g_last_speed >= 0.01f)) {
            g_last_speed = speed < 0.01f ? 0 : speed;
            lua_pushnumber(L, g_last_speed);
            fire_signal(L, S.g->humanoid, EV_RUNNING, NULL, 1);
        }
    } else {
        if (v.y > 0.5f && (g_state == 8 || g_state == 7)) set_state(L, 3); /* left the ground going up */
        else if (v.y <= 0.5f || g_state != 3) set_state(L, 5);
        g_last_speed = -1;
    }
}

int humanoid_state(void) { return g_state < 0 ? 8 : g_state; }

/* ---------------------------------------------------------------- hooks */

int interact_event(i32 idx, const char *k)
{
    int tev = tools_event(idx, k);
    if (tev >= 0) return tev;
    tev = sound_event(idx, k);
    if (tev >= 0) return tev;
    if (idx == VIRT_PROMPTSERVICE || (idx >= 0 && is_class(idx, "ProximityPrompt"))) {
        if (streq(k, "Triggered") || streq(k, "PromptTriggered")) return EV_TRIGGERED;
        if (streq(k, "TriggerEnded") || streq(k, "PromptTriggerEnded")) return EV_TRIGGERENDED;
        if (streq(k, "PromptShown")) return EV_PROMPTSHOWN;
        if (streq(k, "PromptHidden")) return EV_PROMPTHIDDEN;
        if (streq(k, "PromptButtonHoldBegan")) return EV_HOLDBEGAN;
        if (streq(k, "PromptButtonHoldEnded")) return EV_HOLDENDED;
    }
    if (idx >= 0 && is_class(idx, "ClickDetector")) {
        if (streq(k, "MouseClick")) return EV_B1CLICK;
        if (streq(k, "RightMouseClick")) return EV_B2CLICK;
        if (streq(k, "MouseHoverEnter")) return EV_MOUSEENTER;
        if (streq(k, "MouseHoverLeave")) return EV_MOUSELEAVE;
    }
    if (idx >= 0 && idx == S.g->humanoid) {
        if (streq(k, "StateChanged")) return EV_STATECHANGED;
        if (streq(k, "Running")) return EV_RUNNING;
        if (streq(k, "Jumping")) return EV_JUMPING;
        if (streq(k, "FreeFalling")) return EV_FREEFALLING;
    }
    return -1;
}

lua_CFunction interact_method(i32 idx, const char *k)
{
    lua_CFunction tf = tools_method(idx, k);
    if (tf) return tf;
    tf = sound_method(idx, k);
    if (tf) return tf;
    tf = teams_method(idx, k);
    if (tf) return tf;
    if (idx >= 0 && is_class(idx, "ProximityPrompt")) {
        if (streq(k, "InputHoldBegin")) return m_holdbegin;
        if (streq(k, "InputHoldEnd")) return m_holdend;
    }
    return NULL;
}

void interact_pump(lua_State *L, f32 dt)
{
    if (S.g->player < 0) return;
    if (S.g->role != UB_ROLE_SERVER) { /* (a server has no mouse, keys or screen) */
        click_pump(L);
        prompt_pump(L, dt);
    }
    humanoid_pump(L);
    tools_pump(L);
    teams_pump(L);
    sound_pump(L);
}

/* server: the client clicked, hovered or used a prompt */
void interact_remote_event(lua_State *L, i32 inst, int ev)
{
    if (is_class(inst, "ClickDetector") &&
        (ev == EV_MOUSEENTER || ev == EV_MOUSELEAVE || ev == EV_B1CLICK || ev == EV_B2CLICK)) {
        fire_player(L, inst, ev);
    } else if (is_class(inst, "ProximityPrompt") &&
               (ev == EV_TRIGGERED || ev == EV_TRIGGERENDED || ev == EV_HOLDBEGAN || ev == EV_HOLDENDED)) {
        sprompt p;
        memset(&p, 0, sizeof p);
        p.inst = inst;
        fire_prompt(L, &p, ev, false);
    }
}

void interact_reset(void)
{
    tools_reset();
    sound_reset();
    g_hover = g_down = g_rdown = -1;
    g_np = 0;
    g_scan_t = -1;
    g_core = -1;
    g_state = -1;
    g_last_speed = -1;
}

/* --------------------------------------------------- offline services
 * What 2022 games ask these services, answered as for a player offline:
 * no badges, no groups, default policies, English, and paths that go
 * straight to the goal (waypoints every 4 studs). */

static int s_false(lua_State *L)
{
    lua_pushboolean(L, 0);
    return 1;
}

static int s_noop(lua_State *L)
{
    (void)L;
    return 0;
}

static int s_emptylist(lua_State *L)
{
    lua_newtable(L);
    return 1;
}

static int s_policy(lua_State *L)
{
    lua_newtable(L);
    static const char *const no[] = { "AreAdsAllowed", "ArePaidRandomItemsRestricted", "IsSubjectToChinaPolicies",
                                      "IsContentSharingAllowed", NULL };
    for (int i = 0; no[i]; ++i) {
        lua_pushboolean(L, 0);
        lua_setfield(L, -2, no[i]);
    }
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "IsPaidItemTradingAllowed");
    lua_newtable(L);
    lua_setfield(L, -2, "AllowedExternalLinkReferences");
    return 1;
}

static int s_translate(lua_State *L) /* translator:Translate(context, text) */
{
    lua_pushvalue(L, 3);
    return 1;
}

static int s_translator(lua_State *L)
{
    lua_newtable(L);
    lua_pushliteral(L, "en-us");
    lua_setfield(L, -2, "LocaleId");
    lua_pushcfunction(L, s_translate);
    lua_setfield(L, -2, "Translate");
    return 1;
}

static int path_compute(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    vec3 a = check_v3(L, 2), b = check_v3(L, 3);
    vec3 d = v3_sub(b, a);
    f32 len = v3_length(d);
    int steps = (int)(len / 4.0f) + 1;
    if (steps > 1000) steps = 1000;
    lua_newtable(L);
    int walk = enum_type_named("PathWaypointAction");
    for (int i = 0; i <= steps; ++i) {
        lua_newtable(L);
        push_v3(L, v3_add(a, v3_scale(d, (f32)i / (f32)steps)));
        lua_setfield(L, -2, "Position");
        push_enum(L, walk, 0);
        lua_setfield(L, -2, "Action");
        lua_pushliteral(L, "");
        lua_setfield(L, -2, "Label");
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, 1, "_waypoints");
    push_enum(L, enum_type_named("PathStatus"), 0); /* Success */
    lua_setfield(L, 1, "Status");
    return 0;
}

static int path_waypoints(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_newtable(L);
    lua_getfield(L, 1, "_waypoints");
    if (lua_istable(L, -1))
        for (int i = 1, n = (int)lua_objlen(L, -1); i <= n; ++i) {
            lua_rawgeti(L, -1, i);
            lua_rawseti(L, -3, i);
        }
    lua_pop(L, 1);
    return 1;
}

static int path_occlusion(lua_State *L)
{
    lua_pushinteger(L, -1);
    return 1;
}

static int s_createpath(lua_State *L)
{
    lua_newtable(L);
    push_enum(L, enum_type_named("PathStatus"), 5); /* NoPath, until computed */
    lua_setfield(L, -2, "Status");
    static const luaL_Reg m[] = { { "ComputeAsync", path_compute }, { "GetWaypoints", path_waypoints },
                                  { "CheckOcclusionAsync", path_occlusion }, { "Destroy", s_noop }, { NULL, NULL } };
    for (int i = 0; m[i].name; ++i) {
        lua_pushcfunction(L, m[i].func);
        lua_setfield(L, -2, m[i].name);
    }
    push_signal(L, VIRT_PATHFINDING, EV_NEVER, NULL);
    lua_setfield(L, -2, "Blocked");
    push_signal(L, VIRT_PATHFINDING, EV_NEVER, NULL);
    lua_setfield(L, -2, "Unblocked");
    return 1;
}

static int s_offline(lua_State *L)
{
    return luaL_error(L, "%s is not available offline", lua_tostring(L, lua_upvalueindex(1)));
}

/* Chat:Chat(part, message): a bubble over the part; the filters let text through (offline) */
static int s_chat_bubble(lua_State *L)
{
    i32 part = check_inst(L, 2);
    const char *msg = luaL_checkstring(L, 3);
    i32 head = part;
    if (part >= 0 && W->inst[part].part < 0) { /* a model or character: over its Head */
        for (i32 c = W->inst[part].first_child; c >= 0; c = W->inst[c].next_sibling)
            if (W->inst[c].part >= 0 && streq(ub_world_str(W, W->inst[c].name), "Head")) head = c;
    }
    if (head >= 0 && W->inst[head].part >= 0) ub_hud_bubble(S.g, head, msg);
    return 0;
}

static int s_filter(lua_State *L)
{
    lua_pushvalue(L, 2);
    return 1;
}

bool offline_index(lua_State *L, i32 idx, const char *k)
{
    if (idx == VIRT_CHAT) {
        if (streq(k, "Chat")) { lua_pushcfunction(L, s_chat_bubble); return true; }
        if (streq(k, "FilterStringAsync") || streq(k, "FilterStringForBroadcast")) { lua_pushcfunction(L, s_filter); return true; }
        if (streq(k, "BubbleChatEnabled") || streq(k, "LoadDefaultChat")) { lua_pushboolean(L, 1); return true; }
    }
    static const struct { i32 idx; const char *k; lua_CFunction f; } m[] = {
        { VIRT_POLICY, "GetPolicyInfoForPlayerAsync", s_policy },
        { VIRT_BADGE, "UserHasBadgeAsync", s_false }, { VIRT_BADGE, "UserHasBadge", s_false },
        { VIRT_BADGE, "AwardBadge", s_false }, { VIRT_BADGE, "IsLegal", s_false }, { VIRT_BADGE, "IsDisabled", s_false },
        { VIRT_LOCALIZATION, "GetTranslatorForPlayerAsync", s_translator },
        { VIRT_LOCALIZATION, "GetTranslatorForPlayer", s_translator },
        { VIRT_LOCALIZATION, "GetTranslatorForLocaleAsync", s_translator },
        { VIRT_PATHFINDING, "CreatePath", s_createpath },
        { VIRT_GROUP, "GetGroupsAsync", s_emptylist }, { VIRT_GROUP, "GetAlliesAsync", s_emptylist },
        { VIRT_GROUP, "GetEnemiesAsync", s_emptylist },
        { VIRT_SOCIAL, "CanSendGameInviteAsync", s_false }, { VIRT_SOCIAL, "PromptGameInvite", s_noop },
        { VIRT_PROMPTSERVICE, "Enabled", NULL },
    };
    for (size_t i = 0; i < sizeof m / sizeof m[0]; ++i)
        if (m[i].idx == idx && streq(m[i].k, k)) {
            if (m[i].f) lua_pushcfunction(L, m[i].f);
            else lua_pushboolean(L, 1);
            return true;
        }
    if (idx == VIRT_LOCALIZATION && (streq(k, "RobloxLocaleId") || streq(k, "SystemLocaleId"))) {
        lua_pushliteral(L, "en-us");
        return true;
    }
    if (idx == VIRT_BADGE && streq(k, "GetBadgeInfoAsync")) {
        lua_pushstring(L, k);
        lua_pushcclosure(L, s_offline, 1);
        return true;
    }
    if (idx == VIRT_ANALYTICS || idx == VIRT_SOCIAL || idx == VIRT_POLICY || idx == VIRT_BADGE ||
        idx == VIRT_LOCALIZATION || idx == VIRT_GROUP || idx == VIRT_PATHFINDING || idx == VIRT_PROMPTSERVICE) {
        int ev = interact_event(idx, k);
        if (ev >= 0) {
            push_signal(L, idx, ev, NULL);
            return true;
        }
    }
    return false;
}

/* ---------------------------------------------------------------- tools
 * StarterPack's tools are copied into the Backpack at every spawn; keys 1
 * to 9 equip the tool in that slot of the hotbar (again to put it away);
 * the left mouse button activates the held tool. Equipping parents the
 * tool to the character and grips its Handle in the right hand with a
 * Weld named RightGrip (C1 = the tool's Grip), as Roblox does. The hotbar
 * is drawn under CoreGui in Uniblox's own style. */

#define MAX_SLOTS 9

static i32 g_slot[MAX_SLOTS];
static i32 g_equipped = -1, g_grip = -1;
static u32 g_seen_spawn;
static bool g_activated;
static i32 g_client_held = -1;   /* a client: the tool it last saw held */
static i32 g_hotbar = -1, g_hotbar_frames[MAX_SLOTS], g_hotbar_labels[MAX_SLOTS];
static u32 g_hotbar_sig;
#define g_backpack_hidden (S.g->hud.core_off[UB_CORE_BACKPACK])

static bool is_tool(i32 i)
{
    return i >= 0 && ub_world_alive(W, i) && (is_class(i, "Tool") || is_class(i, "HopperBin"));
}

static i32 find_child_named(i32 parent, const char *name)
{
    for (i32 c = W->inst[parent].first_child; c >= 0; c = W->inst[c].next_sibling)
        if (ub_world_alive(W, c) && streq(ub_world_str(W, W->inst[c].name), name)) return c;
    return -1;
}

static void set_parent_l(lua_State *L, i32 inst, i32 parent)
{
    push_inst(L, parent);
    set_prop(L, inst, "Parent", lua_gettop(L));
    lua_pop(L, 1);
}

static void refill_backpack(lua_State *L)
{
    i32 bp = S.g->backpack;
    if (bp < 0) return;
    /* the old ones go (held and in the backpack) */
    if (g_equipped >= 0 && ub_world_alive(W, g_equipped)) ub_world_destroy(W, g_equipped);
    if (g_grip >= 0 && ub_world_alive(W, g_grip)) ub_world_destroy(W, g_grip);
    g_equipped = g_grip = -1;
    for (i32 c = W->inst[bp].first_child; c >= 0;) {
        i32 next = W->inst[c].next_sibling;
        if (is_tool(c)) ub_world_destroy(W, c);
        c = next;
    }
    for (int s = 0; s < MAX_SLOTS; ++s) g_slot[s] = -1;
    i32 sp = -1;
    for (i32 i = 0; i < W->count && sp < 0; ++i)
        if (W->inst[i].parent < 0 && is_class(i, "StarterPack")) sp = i;
    if (sp < 0) return;
    i32 kids[64];
    int nk = 0;
    for (i32 c = W->inst[sp].first_child; c >= 0 && nk < 64; c = W->inst[c].next_sibling)
        if (ub_world_alive(W, c)) kids[nk++] = c;
    for (int k = 0; k < nk; ++k) {
        i32 copy = ub_world_clone(W, kids[k]);
        if (copy < 0) break;
        W->inst[copy].flags |= UB_INST_RUNTIME;
        set_parent_l(L, copy, bp);
        start_scripts_in(copy);
    }
}

/* keeps the hotbar's slots: tools stay in their slot; new ones take the first free */
static void update_slots(void)
{
    i32 bp = S.g->backpack;
    for (int s = 0; s < MAX_SLOTS; ++s) {
        i32 t = g_slot[s];
        bool here = is_tool(t) && (W->inst[t].parent == bp || W->inst[t].parent == S.g->char_model);
        if (!here) g_slot[s] = -1;
    }
    for (i32 c = bp >= 0 ? W->inst[bp].first_child : -1; c >= 0; c = W->inst[c].next_sibling) {
        if (!is_tool(c)) continue;
        bool has = false;
        for (int s = 0; s < MAX_SLOTS; ++s) has |= g_slot[s] == c;
        for (int s = 0; s < MAX_SLOTS && !has; ++s)
            if (g_slot[s] < 0) {
                g_slot[s] = c;
                has = true;
            }
    }
    /* a tool a script put in the character is held */
    if (g_equipped >= 0 && (!is_tool(g_equipped) || W->inst[g_equipped].parent != S.g->char_model)) g_equipped = -1;
    for (i32 c = S.g->char_model >= 0 ? W->inst[S.g->char_model].first_child : -1; c >= 0 && g_equipped < 0;
         c = W->inst[c].next_sibling)
        if (is_tool(c)) g_equipped = c;
}

static i32 right_hand(void)
{
    if (S.g->char_model < 0) return -1;
    i32 h = find_child_named(S.g->char_model, S.g->r15 ? "RightHand" : "Right Arm");
    return h >= 0 ? h : S.g->char_root;
}

static void grip(lua_State *L, i32 tool)
{
    i32 handle = find_child_named(tool, "Handle");
    i32 hand = right_hand();
    if (handle < 0 || W->inst[handle].part < 0 || hand < 0) return;
    g_grip = ub_world_add(W, hand, UB_CLASS_OTHER, "Weld", "RightGrip");
    if (g_grip < 0) return;
    W->inst[g_grip].flags |= UB_INST_RUNTIME;
    ub_value v;
    memset(&v, 0, sizeof v);
    v.type = UB_PV_REF;
    v.v.ref = hand;
    ub_world_set_prop_named(W, g_grip, "Part0", &v);
    v.v.ref = handle;
    ub_world_set_prop_named(W, g_grip, "Part1", &v);
    /* C0: the hand's grip point, the tool pointing forward; C1: the tool's Grip */
    mat3 r = mat3_from_euler(0, -UB_HALF_PI, 0);
    push_cf(L, v3(0, S.g->r15 ? -0.15f : -1.0f, 0), &r);
    set_extra_value(L, g_grip, "C0", lua_gettop(L));
    lua_pop(L, 1);
    get_extra_value(L, tool, "Grip");
    if (is_cf(L, -1)) set_extra_value(L, g_grip, "C1", lua_gettop(L));
    lua_pop(L, 1);
    joint_changed(L, g_grip);
}

static void unequip(lua_State *L)
{
    i32 t = g_equipped;
    if (t < 0) return;
    g_equipped = -1;
    if (g_grip >= 0 && ub_world_alive(W, g_grip)) {
        ub_game_uncarry(S.g, -1, g_grip);
        ub_world_destroy(W, g_grip);
    }
    g_grip = -1;
    if (!ub_world_alive(W, t)) return;
    if (g_activated) {
        g_activated = false;
        fire_signal(L, t, EV_DEACTIVATED, NULL, 0);
    }
    if (S.g->backpack >= 0) set_parent_l(L, t, S.g->backpack);
    fire_signal(L, t, EV_UNEQUIPPED, NULL, 0);
}

static void equip(lua_State *L, i32 t)
{
    if (!is_tool(t) || S.g->dead || S.g->char_model < 0) return;
    if (g_equipped == t) return;
    unequip(L);
    g_equipped = t;
    set_parent_l(L, t, S.g->char_model);
    grip(L, t);
    push_inst(L, VIRT_MOUSE);
    fire_signal(L, t, EV_EQUIPPED, NULL, 1);
}

static f64 tool_num(i32 t, const char *k, f64 dflt) { return num_prop(t, k, dflt); }

static void activate(lua_State *L, i32 t)
{
    if (t < 0 || !ub_world_alive(W, t) || tool_num(t, "Enabled", 1) == 0) return;
    g_activated = true;
    fire_signal(L, t, EV_ACTIVATED, NULL, 0);
}

/* the hotbar: a row of slots at the bottom of the screen */
static void draw_hotbar(void)
{
    u32 sig = (u32)(g_equipped + 7);
    int used = 0;
    for (int s = 0; s < MAX_SLOTS; ++s) {
        sig = sig * 31u + (u32)(g_slot[s] + 3);
        if (g_slot[s] >= 0) used = s + 1;
    }
    sig = sig * 31u + (g_backpack_hidden ? 1u : 0u) + W->epoch * 0u;
    if (sig == g_hotbar_sig && g_hotbar >= 0 && ub_world_alive(W, g_hotbar)) return;
    g_hotbar_sig = sig;
    g_core = S.g->core_gui;
    if (g_core < 0 || !ub_world_alive(W, g_core)) return;
    if (g_hotbar >= 0 && ub_world_alive(W, g_hotbar)) ub_world_destroy(W, g_hotbar);
    g_hotbar = -1;
    if (!used || g_backpack_hidden) return;
    g_hotbar = make("ScreenGui", g_core, "Hotbar");
    if (g_hotbar < 0) return;
    for (int s = 0; s < used; ++s) {
        i32 f = make("Frame", g_hotbar, "Slot");
        g_hotbar_frames[s] = f;
        if (f < 0) continue;
        bool held = g_slot[s] >= 0 && g_slot[s] == g_equipped;
        f32 x0 = -(f32)used * 33.0f;
        setv(f, "Position", vudim2(0.5f, (i32)(x0 + (f32)s * 66.0f), 1, -72));
        setv(f, "Size", vudim2(0, 60, 0, 60));
        frame_look(f, held ? 0.20f : 0.09f, held ? 0.42f : 0.10f, held ? 0.62f : 0.13f, 0.25f, 8);
        char num[4] = { (char)('1' + s), 0 };
        i32 n = label(f, "Number", 13, true, 0.75f, 0.78f, 0.84f, 0);
        setv(n, "Position", vudim2(0, 5, 0, 2));
        setv(n, "Size", vudim2(1, -10, 0, 14));
        ub_value nv = ub_vstr(num, 1);
        setv(n, "Text", nv);
        i32 t = label(f, "Name", 13, false, 1, 1, 1, 2);
        g_hotbar_labels[s] = t;
        setv(t, "Position", vudim2(0, 3, 0, 18));
        setv(t, "Size", vudim2(1, -6, 1, -20));
        setv(t, "TextWrapped", ub_vbool(true));
        if (g_slot[s] >= 0) {
            const char *name = ub_world_str(W, W->inst[g_slot[s]].name);
            ub_value tv = ub_vstr(name, (u32)strlen(name));
            setv(t, "Text", tv);
        } else {
            ub_value tv = ub_vstr("", 0);
            setv(t, "Text", tv);
        }
    }
}

/* a client: what the server equipped (the tool came into the character, or left it) */
static void client_held(lua_State *L)
{
    if (g_equipped == g_client_held) return;
    i32 old = g_client_held;
    g_client_held = g_equipped;
    if (old >= 0 && ub_world_alive(W, old)) {
        if (g_activated) {
            g_activated = false;
            fire_signal(L, old, EV_DEACTIVATED, NULL, 0);
        }
        fire_signal(L, old, EV_UNEQUIPPED, NULL, 0);
    }
    if (g_equipped >= 0) {
        push_inst(L, VIRT_MOUSE);
        fire_signal(L, g_equipped, EV_EQUIPPED, NULL, 1);
    }
}

static void tools_pump(lua_State *L)
{
    bool client = S.g->role == UB_ROLE_CLIENT;
    if (S.g->spawn_count != g_seen_spawn) {
        g_seen_spawn = S.g->spawn_count;
        if (!client) refill_backpack(L); /* (a client's comes from the server) */
    }
    if (S.g->dead && g_equipped >= 0 && !client) unequip(L);
    update_slots();
    if (client) client_held(L);
    const ub_game_input *in = IN();
    if (!in->gui_keys && !S.g->dead && !g_backpack_hidden)
        for (int s = 0; s < MAX_SLOTS; ++s)
            if (in->key_pressed['1' + s]) {
                if (client) { /* the server equips it */
                    if (g_slot[s] >= 0) net_tool(g_slot[s] == g_equipped ? NET_TOOL_UNEQUIP : NET_TOOL_EQUIP, g_slot[s]);
                } else if (g_slot[s] >= 0 && g_slot[s] == g_equipped) {
                    unequip(L);
                } else if (g_slot[s] >= 0) {
                    equip(L, g_slot[s]);
                }
            }
    if (g_equipped >= 0 && is_tool(g_equipped)) {
        bool manual = tool_num(g_equipped, "ManualActivationOnly", 0) != 0;
        if (in->pressed[0] && !in->gui_mouse && !manual) {
            activate(L, g_equipped);
            if (client) net_tool(NET_TOOL_ACTIVATE, g_equipped);
        }
        if (in->released[0] && g_activated) {
            g_activated = false;
            fire_signal(L, g_equipped, EV_DEACTIVATED, NULL, 0);
            if (client) net_tool(NET_TOOL_DEACTIVATE, g_equipped);
        }
    }
    if (S.g->role != UB_ROLE_SERVER) draw_hotbar();
}

/* server: the client asked */
void interact_remote_tool(lua_State *L, int action, i32 tool)
{
    update_slots();
    if (action == NET_TOOL_EQUIP && is_tool(tool) && W->inst[tool].parent == S.g->backpack) equip(L, tool);
    else if (action == NET_TOOL_UNEQUIP) unequip(L);
    else if (action == NET_TOOL_ACTIVATE && tool == g_equipped && tool >= 0) activate(L, tool);
    else if (action == NET_TOOL_DEACTIVATE && tool == g_equipped && tool >= 0 && g_activated) {
        g_activated = false;
        fire_signal(L, tool, EV_DEACTIVATED, NULL, 0);
    }
    update_slots();
}

/* Tool:Activate(), Tool:Deactivate(); Humanoid:EquipTool(tool), :UnequipTools() */
static int m_tool_activate(lua_State *L)
{
    i32 t = check_inst(L, 1);
    if (t == g_equipped) activate(L, t);
    return 0;
}

static int m_tool_deactivate(lua_State *L)
{
    i32 t = check_inst(L, 1);
    if (t == g_equipped && g_activated) {
        g_activated = false;
        fire_signal(L, t, EV_DEACTIVATED, NULL, 0);
    }
    return 0;
}

static int m_equiptool(lua_State *L)
{
    i32 t = check_inst(L, 2);
    if (S.g->role == UB_ROLE_CLIENT) {
        net_tool(NET_TOOL_EQUIP, t);
        return 0;
    }
    update_slots();
    equip(L, t);
    update_slots();
    return 0;
}

static int m_unequiptools(lua_State *L)
{
    if (S.g->role == UB_ROLE_CLIENT) {
        net_tool(NET_TOOL_UNEQUIP, -1);
        return 0;
    }
    unequip(L);
    return 0;
}

int tools_event(i32 idx, const char *k)
{
    if (!is_tool(idx)) return -1;
    if (streq(k, "Equipped")) return EV_EQUIPPED;
    if (streq(k, "Unequipped")) return EV_UNEQUIPPED;
    if (streq(k, "Activated")) return EV_ACTIVATED;
    if (streq(k, "Deactivated")) return EV_DEACTIVATED;
    return -1;
}

lua_CFunction tools_method(i32 idx, const char *k)
{
    if (is_tool(idx)) {
        if (streq(k, "Activate")) return m_tool_activate;
        if (streq(k, "Deactivate")) return m_tool_deactivate;
    }
    if (idx >= 0 && idx == S.g->humanoid) {
        if (streq(k, "EquipTool")) return m_equiptool;
        if (streq(k, "UnequipTools")) return m_unequiptools;
    }
    return NULL;
}

void tools_reset(void)
{
    for (int s = 0; s < MAX_SLOTS; ++s) g_slot[s] = -1;
    g_equipped = g_grip = g_hotbar = g_client_held = -1;
    g_seen_spawn = 0;
    g_activated = false;
    g_hotbar_sig = 0;
}

/* ---------------------------------------------------------------- teams
 * Player.Team, TeamColor and Neutral (the game picks spawns from them),
 * Team:GetPlayers(), Teams:GetTeams(), and spawns that change your team
 * when you step on them (AllowTeamChangeOnTouch). */

static void set_team(lua_State *L, i32 team)
{
    i32 pl = S.g->player;
    if (pl < 0) return;
    ub_value v;
    memset(&v, 0, sizeof v);
    v.type = UB_PV_REF;
    v.v.ref = team;
    ub_world_set_prop_named(W, pl, "Team", &v);
    ub_value nv = ub_vbool(team < 0);
    ub_world_set_prop_named(W, pl, "Neutral", &nv);
    if (team >= 0) {
        const ub_value *tc = ub_world_prop_named(W, team, "TeamColor");
        ub_value c;
        memset(&c, 0, sizeof c);
        c.type = UB_PV_BRICKCOLOR;
        c.v.i = tc ? tc->v.i : 194;
        ub_world_set_prop_named(W, pl, "TeamColor", &c);
    }
    prop_changed(L, pl, "Team");
    prop_changed(L, pl, "TeamColor");
    prop_changed(L, pl, "Neutral");
}

static i32 player_team(void)
{
    const ub_value *v = S.g->player >= 0 ? ub_world_prop_named(W, S.g->player, "Team") : NULL;
    return v && v->type == UB_PV_REF && v->v.ref >= 0 && ub_world_alive(W, v->v.ref) ? v->v.ref : -1;
}

bool teams_get(lua_State *L, i32 idx, const char *k)
{
    if (idx < 0 || idx != S.g->player) return false;
    if (streq(k, "Team")) {
        push_inst(L, player_team());
        return true;
    }
    if (streq(k, "TeamColor")) {
        const ub_value *v = ub_world_prop_named(W, idx, "TeamColor");
        push_brickcolor(L, brickcolor_number_rgb(v ? (u32)v->v.i : 194));
        return true;
    }
    if (streq(k, "Neutral")) {
        const ub_value *v = ub_world_prop_named(W, idx, "Neutral");
        lua_pushboolean(L, !v || v->v.b);
        return true;
    }
    return false;
}

bool teams_set(lua_State *L, i32 idx, const char *k, int v)
{
    if (idx < 0 || idx != S.g->player) return false;
    if (streq(k, "Team")) {
        i32 t = opt_inst(L, v);
        if (t >= 0 && !is_class(t, "Team")) return sapi_error(L, "Team must be a Team"), true;
        set_team(L, t);
        return true;
    }
    if (streq(k, "TeamColor")) {
        u32 rgb = brickcolor_rgb(L, v), num = brickcolor_rgb_number(rgb);
        i32 teams = -1, found = -1;
        for (i32 i = 0; i < W->count && teams < 0; ++i)
            if (W->inst[i].parent < 0 && is_class(i, "Teams")) teams = i;
        for (i32 c = teams >= 0 ? W->inst[teams].first_child : -1; c >= 0 && found < 0; c = W->inst[c].next_sibling) {
            const ub_value *tc = is_class(c, "Team") ? ub_world_prop_named(W, c, "TeamColor") : NULL;
            if (tc && (u32)tc->v.i == num) found = c;
        }
        set_team(L, found);
        if (found < 0) { /* a colour no team has: keeps it, neutral as Roblox does */
            ub_value c;
            memset(&c, 0, sizeof c);
            c.type = UB_PV_BRICKCOLOR;
            c.v.i = (i32)num;
            ub_world_set_prop_named(W, idx, "TeamColor", &c);
        }
        return true;
    }
    if (streq(k, "Neutral")) {
        if (lua_toboolean(L, v)) set_team(L, -1);
        else {
            ub_value nv = ub_vbool(false);
            ub_world_set_prop_named(W, idx, "Neutral", &nv);
            prop_changed(L, idx, "Neutral");
        }
        return true;
    }
    return false;
}

static int m_team_players(lua_State *L)
{
    i32 t = check_inst(L, 1);
    lua_newtable(L);
    if (player_team() == t) {
        push_inst(L, S.g->player);
        lua_rawseti(L, -2, 1);
    }
    return 1;
}

static int m_get_teams(lua_State *L)
{
    i32 t = check_inst(L, 1);
    lua_newtable(L);
    int n = 0;
    for (i32 c = W->inst[t].first_child; c >= 0; c = W->inst[c].next_sibling)
        if (is_class(c, "Team")) {
            push_inst(L, c);
            lua_rawseti(L, -2, ++n);
        }
    return 1;
}

lua_CFunction teams_method(i32 idx, const char *k)
{
    if (is_class(idx, "Team") && streq(k, "GetPlayers")) return m_team_players;
    if (is_class(idx, "Teams") && streq(k, "GetTeams")) return m_get_teams;
    return NULL;
}

static i32 g_last_spawn_touch = -1;

void teams_pump(lua_State *L)
{
    i32 gp = S.g->chr.ground_part, inst = gp >= 0 ? ub_world_part_inst(W, gp) : -1;
    if (inst == g_last_spawn_touch) return;
    g_last_spawn_touch = inst;
    if (inst < 0 || W->inst[inst].cls != UB_CLASS_SPAWN) return;
    const ub_value *allow = ub_world_prop_named(W, inst, "AllowTeamChangeOnTouch");
    const ub_value *neutral = ub_world_prop_named(W, inst, "Neutral");
    if (!allow || !allow->v.b || !neutral || neutral->v.b) return;
    const ub_value *tc = ub_world_prop_named(W, inst, "TeamColor");
    if (!tc) return;
    i32 teams = -1;
    for (i32 i = 0; i < W->count && teams < 0; ++i)
        if (W->inst[i].parent < 0 && is_class(i, "Teams")) teams = i;
    for (i32 c = teams >= 0 ? W->inst[teams].first_child : -1; c >= 0; c = W->inst[c].next_sibling) {
        const ub_value *t = is_class(c, "Team") ? ub_world_prop_named(W, c, "TeamColor") : NULL;
        if (t && t->v.i == tc->v.i && player_team() != c) {
            set_team(L, c);
            return;
        }
    }
}

/* this file's state, one copy per script runtime (ub_script.c) */
void interact_vars(ub_svar_fn f)
{
    f(&g_hover, sizeof g_hover);
    f(&g_down, sizeof g_down);
    f(&g_rdown, sizeof g_rdown);
    f(g_p, sizeof g_p);
    f(&g_np, sizeof g_np);
    f(&g_scan_t, sizeof g_scan_t);
    f(&g_scan_epoch, sizeof g_scan_epoch);
    f(&g_core, sizeof g_core);
    f(&g_state, sizeof g_state);
    f(&g_last_speed, sizeof g_last_speed);
    f(g_slot, sizeof g_slot);
    f(&g_equipped, sizeof g_equipped);
    f(&g_grip, sizeof g_grip);
    f(&g_seen_spawn, sizeof g_seen_spawn);
    f(&g_activated, sizeof g_activated);
    f(&g_client_held, sizeof g_client_held);
    f(&g_hotbar, sizeof g_hotbar);
    f(g_hotbar_frames, sizeof g_hotbar_frames);
    f(g_hotbar_labels, sizeof g_hotbar_labels);
    f(&g_hotbar_sig, sizeof g_hotbar_sig);
    f(&g_last_spawn_touch, sizeof g_last_spawn_touch);
}
