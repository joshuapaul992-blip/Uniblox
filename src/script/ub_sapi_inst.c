/* Uniblox - instances for scripts: properties, methods, events, services,
 * Instance.new, tweens, Debris and raycasts, in Roblox's terms.
 *
 * Properties of classes Uniblox knows (parts, Humanoid, Lighting, value
 * objects...) map onto the world; any other property a script sets is
 * kept in a per-instance table, so scripts written for things Uniblox
 * does not simulate yet still run. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "script/ub_sapi.h"
#include "core/ub_log.h"
#include "world/ub_classes.h"
#include "physics/ub_pmath.h"
#include "platform/platform.h"
#include "render/ub_texture.h"

#define W (&S.g->world)

static const char *cls_of(i32 i)
{
    return i >= 0 ? ub_world_str(W, W->inst[i].class_name) : "DataModel";
}

static bool streq(const char *a, const char *b) { return strcmp(a, b) == 0; }

static const char *name_of(i32 i) { return i >= 0 ? ub_world_str(W, W->inst[i].name) : "Game"; }

/* ------------------------------------------------------------ virtual */

static const struct {
    i32 idx;
    const char *cls;
} k_virtual[] = {
    { VIRT_GAME, "DataModel" }, { VIRT_RUNSERVICE, "RunService" }, { VIRT_DEBRIS, "Debris" },
    { VIRT_TWEENSERVICE, "TweenService" }, { VIRT_HTTP, "HttpService" }, { VIRT_USERINPUT, "UserInputService" },
    { VIRT_MARKETPLACE, "MarketplaceService" }, { VIRT_DATASTORE, "DataStoreService" }, { VIRT_CHAT, "Chat" },
    { VIRT_TELEPORT, "TeleportService" }, { VIRT_SOUNDSERVICE, "SoundService" },
    { VIRT_PHYSICSSERVICE, "PhysicsService" }, { VIRT_COLLECTION, "CollectionService" },
    { VIRT_TEXTSERVICE, "TextService" }, { VIRT_GUISERVICE, "GuiService" },
    { VIRT_CONTENTPROVIDER, "ContentProvider" }, { VIRT_MOUSE, "Mouse" }, { VIRT_CAMERA, "Camera" },
    { VIRT_PROMPTSERVICE, "ProximityPromptService" }, { VIRT_POLICY, "PolicyService" },
    { VIRT_BADGE, "BadgeService" }, { VIRT_LOCALIZATION, "LocalizationService" },
    { VIRT_PATHFINDING, "PathfindingService" }, { VIRT_GROUP, "GroupService" }, { VIRT_SOCIAL, "SocialService" },
    { VIRT_ANALYTICS, "AnalyticsService" },
};

static const char *virt_class(i32 idx)
{
    for (int i = 0; i < (int)(sizeof k_virtual / sizeof k_virtual[0]); ++i)
        if (k_virtual[i].idx == idx) return k_virtual[i].cls;
    return NULL;
}

/* ---------------------------------------------------------- the userdata */

typedef struct {
    i32 idx;
} sinst;

void push_inst(lua_State *L, i32 idx)
{
    if (idx == -1 || (idx >= 0 && idx >= W->count)) {
        lua_pushnil(L);
        return;
    }
    lua_pushlightuserdata(L, &REG_INSTANCES);
    lua_rawget(L, LUA_REGISTRYINDEX);
    lua_rawgeti(L, -1, idx);
    if (lua_isnil(L, -1)) { /* one userdata per instance, so == works */
        lua_pop(L, 1);
        sinst *p = (sinst *)lua_newuserdata(L, sizeof(sinst));
        p->idx = idx;
        luaL_getmetatable(L, "Instance");
        lua_setmetatable(L, -2);
        lua_pushvalue(L, -1);
        lua_rawseti(L, -3, idx);
    }
    lua_remove(L, -2);
}

bool is_inst(lua_State *L, int i)
{
    if (!lua_touserdata(L, i) || !lua_getmetatable(L, i)) return false;
    luaL_getmetatable(L, "Instance");
    bool same = lua_rawequal(L, -1, -2);
    lua_pop(L, 2);
    return same;
}

i32 check_inst(lua_State *L, int i)
{
    if (!is_inst(L, i)) luaL_typerror(L, i, "Instance");
    return ((sinst *)lua_touserdata(L, i))->idx;
}

i32 opt_inst(lua_State *L, int i)
{
    if (lua_isnoneornil(L, i)) return -1;
    return check_inst(L, i);
}

/* ------------------------------------------------------------- classes */

static const char *const k_parents[][2] = {
    { "Part", "FormFactorPart" }, { "FormFactorPart", "BasePart" }, { "WedgePart", "FormFactorPart" },
    { "CornerWedgePart", "BasePart" }, { "TrussPart", "BasePart" }, { "SpawnLocation", "Part" }, { "Seat", "Part" },
    { "VehicleSeat", "BasePart" }, { "MeshPart", "BasePart" }, { "UnionOperation", "PartOperation" },
    { "NegateOperation", "PartOperation" }, { "IntersectOperation", "PartOperation" },
    { "PartOperation", "BasePart" }, { "BasePart", "PVInstance" }, { "Model", "PVInstance" },
    { "Workspace", "Model" }, { "Script", "BaseScript" }, { "LocalScript", "Script" },
    { "BaseScript", "LuaSourceContainer" }, { "ModuleScript", "LuaSourceContainer" }, { "IntValue", "ValueBase" },
    { "NumberValue", "ValueBase" }, { "StringValue", "ValueBase" }, { "BoolValue", "ValueBase" },
    { "ObjectValue", "ValueBase" }, { "Vector3Value", "ValueBase" }, { "Color3Value", "ValueBase" },
    { "CFrameValue", "ValueBase" }, { "BrickColorValue", "ValueBase" }, { "Folder", "Instance" },
};

static bool is_a(const char *cls, const char *want)
{
    if (streq(want, "Instance")) return true;
    if (ub_cdb_is_a(cls, want)) return true;
    for (int guard = 0; guard < 8; ++guard) {
        if (streq(cls, want)) return true;
        const char *up = NULL;
        for (int i = 0; i < (int)(sizeof k_parents / sizeof k_parents[0]); ++i)
            if (streq(k_parents[i][0], cls)) up = k_parents[i][1];
        if (!up) return false;
        cls = up;
    }
    return false;
}

static bool is_part(i32 i) { return i >= 0 && W->inst[i].part >= 0; }
static ub_part *part_of(i32 i) { return &W->parts[W->inst[i].part]; }
static bool is_value_class(const char *cls) { return is_a(cls, "ValueBase"); }

static i32 find_root_class(const char *cls)
{
    for (i32 i = 0; i < W->count; ++i)
        if (W->inst[i].parent < 0 && ub_world_alive(W, i) && !(W->inst[i].flags & UB_INST_DETACHED) &&
            streq(ub_world_str(W, W->inst[i].class_name), cls))
            return i;
    return -1;
}

static bool is_service(i32 i)
{
    return i < 0 || (W->inst[i].parent < 0 && !(W->inst[i].flags & UB_INST_DETACHED));
}

/* GetService: a real root of that class, a virtual service, or a new
 * (run-time) root for storage services the place did not have. */
static i32 get_service(const char *cls, bool create)
{
    if (streq(cls, "Workspace")) return ub_world_workspace(W);
    i32 r = find_root_class(cls);
    if (r >= 0) return r;
    for (int i = 0; i < (int)(sizeof k_virtual / sizeof k_virtual[0]); ++i)
        if (streq(k_virtual[i].cls, cls)) return k_virtual[i].idx;
    static const char *const storage[] = { "Lighting", "ReplicatedStorage", "ReplicatedFirst", "ServerStorage",
                                           "ServerScriptService", "StarterGui", "StarterPack", "StarterPlayer",
                                           "Teams", "Players", NULL };
    for (int i = 0; storage[i] && create; ++i)
        if (streq(storage[i], cls)) {
            i32 n = ub_world_add(W, -1, streq(cls, "Lighting") ? UB_CLASS_LIGHTING : UB_CLASS_FOLDER, cls, cls);
            if (n >= 0) W->inst[n].flags |= UB_INST_RUNTIME;
            return n;
        }
    return -1;
}

/* ------------------------------------------------------- extra properties */

static void push_props(lua_State *L, i32 idx, bool create)
{
    lua_pushlightuserdata(L, &REG_PROPS);
    lua_rawget(L, LUA_REGISTRYINDEX);
    lua_rawgeti(L, -1, idx);
    if (lua_isnil(L, -1) && create) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_rawseti(L, -3, idx);
    }
    lua_remove(L, -2);
}

/* pushes the stored value (nil if none) */
static void get_extra(lua_State *L, i32 idx, const char *key)
{
    push_props(L, idx, false);
    if (lua_isnil(L, -1)) return;
    lua_getfield(L, -1, key);
    lua_remove(L, -2);
}

static void set_extra(lua_State *L, i32 idx, const char *key, int vi)
{
    vi = vi < 0 ? lua_gettop(L) + vi + 1 : vi;
    push_props(L, idx, true);
    lua_pushvalue(L, vi);
    lua_setfield(L, -2, key);
    lua_pop(L, 1);
}

/* ------------------------------------------------------------ helpers */

void get_extra_value(lua_State *L, i32 idx, const char *key) { get_extra(L, idx, key); }
void set_extra_value(lua_State *L, i32 idx, const char *key, int v) { set_extra(L, idx, key, v); }

void prop_changed(lua_State *L, i32 idx, const char *prop)
{
    /* Changed (value objects pass the new value, others the name), and
     * GetPropertyChangedSignal(prop) */
    if (idx >= 0 && is_value_class(cls_of(idx)) && streq(prop, "Value")) {
        if (!gui_get(L, idx, "Value")) get_extra(L, idx, "Value");
    } else {
        lua_pushstring(L, prop);
    }
    fire_signal(L, idx, EV_CHANGED, NULL, 1);
    fire_signal(L, idx, EV_PROPCHANGED, prop, 0);
}

static void push_children(lua_State *L, i32 idx, bool all)
{
    lua_newtable(L);
    int n = 0;
    if (idx == VIRT_GAME) {
        for (i32 i = 0; i < W->count; ++i)
            if (W->inst[i].parent < 0 && ub_world_alive(W, i) && !(W->inst[i].flags & UB_INST_DETACHED)) {
                push_inst(L, i);
                lua_rawseti(L, -2, ++n);
            }
        return;
    }
    if (idx < 0) return;
    /* depth first, in order */
    i32 stack[256];
    int sp = 0;
    for (i32 c = W->inst[idx].last_child; c >= 0; ) { /* push in reverse: find previous by walking */
        (void)c;
        break;
    }
    i32 kids[1024];
    int nk = 0;
    for (i32 c = W->inst[idx].first_child; c >= 0 && nk < 1024; c = W->inst[c].next_sibling) kids[nk++] = c;
    if (!all) {
        for (int k = 0; k < nk; ++k) {
            push_inst(L, kids[k]);
            lua_rawseti(L, -2, ++n);
        }
        return;
    }
    for (int k = nk - 1; k >= 0 && sp < 256; --k) stack[sp++] = kids[k];
    while (sp > 0) {
        i32 i = stack[--sp];
        push_inst(L, i);
        lua_rawseti(L, -2, ++n);
        nk = 0;
        for (i32 c = W->inst[i].first_child; c >= 0 && nk < 1024; c = W->inst[c].next_sibling) kids[nk++] = c;
        for (int k = nk - 1; k >= 0 && sp < 256; --k) stack[sp++] = kids[k];
    }
}

static i32 find_child(i32 idx, const char *name, bool recursive)
{
    if (idx == VIRT_GAME) {
        for (i32 i = 0; i < W->count; ++i)
            if (W->inst[i].parent < 0 && ub_world_alive(W, i) && !(W->inst[i].flags & UB_INST_DETACHED) &&
                streq(name_of(i), name))
                return i;
        return -1;
    }
    if (idx < 0) return -1;
    for (i32 c = W->inst[idx].first_child; c >= 0; c = W->inst[c].next_sibling)
        if (streq(name_of(c), name)) return c;
    if (recursive)
        for (i32 c = W->inst[idx].first_child; c >= 0; c = W->inst[c].next_sibling) {
            i32 r = find_child(c, name, true);
            if (r >= 0) return r;
        }
    return -1;
}

static void full_name(i32 idx, char *buf, int cap)
{
    i32 chain[64];
    int n = 0;
    for (i32 i = idx; i >= 0 && n < 64; i = W->inst[i].parent) chain[n++] = i;
    int len = 0;
    buf[0] = 0;
    for (int k = n - 1; k >= 0 && len < cap - 2; --k)
        len += snprintf(buf + len, (size_t)(cap - len), "%s%s", k == n - 1 ? "" : ".", name_of(chain[k]));
}


static void part_moved(i32 idx) { ub_game_part_changed(S.g, W->inst[idx].part, UB_CHANGE_PHYS | UB_CHANGE_LOOK); }

/* Moves a part to a Roblox CFrame. */
static void move_part(i32 idx, vec3 pos, const mat3 *rot)
{
    part_set_cframe(part_of(idx), pos, rot);
    part_moved(idx);
}

/* All parts under `root` (or root itself). */
static int collect_parts(i32 root, i32 *out, int max)
{
    int n = 0;
    i32 stack[512];
    int sp = 0;
    stack[sp++] = root;
    while (sp > 0 && n < max) {
        i32 i = stack[--sp];
        if (W->inst[i].part >= 0) out[n++] = i;
        for (i32 c = W->inst[i].first_child; c >= 0 && sp < 512; c = W->inst[c].next_sibling) stack[sp++] = c;
    }
    return n;
}

static i32 model_primary(lua_State *L, i32 model)
{
    get_extra(L, model, "PrimaryPart");
    i32 p = is_inst(L, -1) ? ((sinst *)lua_touserdata(L, -1))->idx : -1;
    lua_pop(L, 1);
    if (p >= 0 && ub_world_alive(W, p) && ub_world_is_descendant(W, p, model)) return p;
    return -1;
}

/* A model's pivot: its PrimaryPart's CFrame, else the middle of its parts. */
static void model_pivot(lua_State *L, i32 model, vec3 *pos, mat3 *rot)
{
    i32 pp = model_primary(L, model);
    if (pp >= 0) {
        part_get_cframe(part_of(pp), pos, rot);
        return;
    }
    i32 parts[4096];
    int n = collect_parts(model, parts, 4096);
    vec3 lo = v3(1e30f, 1e30f, 1e30f), hi = v3(-1e30f, -1e30f, -1e30f);
    for (int i = 0; i < n; ++i) {
        vec3 p = rbx_vec(part_of(parts[i])->pos);
        lo = v3_min(lo, p);
        hi = v3_max(hi, p);
    }
    *pos = n ? v3_scale(v3_add(lo, hi), 0.5f) : v3(0, 0, 0);
    *rot = mat3_identity();
}

/* Moves a model so its pivot lands on (pos, rot), keeping its shape. */
static void model_pivot_to(lua_State *L, i32 model, vec3 pos, const mat3 *rot)
{
    vec3 op;
    mat3 orot;
    model_pivot(L, model, &op, &orot);
    i32 parts[4096];
    int n = collect_parts(model, parts, 4096);
    mat3 inv = mat3_transpose(&orot);
    for (int i = 0; i < n; ++i) {
        vec3 pp;
        mat3 pr;
        part_get_cframe(part_of(parts[i]), &pp, &pr);
        vec3 local = mat3_apply(&inv, v3_sub(pp, op));
        mat3 lr = mat3_mul(&inv, &pr);
        mat3 nr = mat3_mul(rot, &lr);
        move_part(parts[i], v3_add(pos, mat3_apply(rot, local)), &nr);
    }
}

/* -------------------------------------------------------- property get */

static f32 g_grav(void) { return S.g->world.gravity > 1 ? S.g->world.gravity : 1; }

static bool get_part_prop(lua_State *L, i32 idx, const char *k)
{
    ub_part *p = part_of(idx);
    if (streq(k, "Position")) {
        push_v3(L, rbx_vec(p->pos));
    } else if (streq(k, "CFrame")) {
        vec3 pos;
        mat3 r;
        part_get_cframe(p, &pos, &r);
        push_cf(L, pos, &r);
    } else if (streq(k, "Size")) {
        push_v3(L, part_get_size(p));
    } else if (streq(k, "Orientation") || streq(k, "Rotation")) {
        vec3 pos;
        mat3 r;
        part_get_cframe(p, &pos, &r);
        push_v3(L, ub_rbx_orientation(&r));
    } else if (streq(k, "Color")) {
        push_c3_rgb(L, p->color);
    } else if (streq(k, "BrickColor")) {
        push_brickcolor(L, p->color);
    } else if (streq(k, "Transparency")) {
        lua_pushnumber(L, p->transparency / 255.0);
    } else if (streq(k, "Anchored")) {
        lua_pushboolean(L, (p->flags & UB_PART_ANCHORED) != 0);
    } else if (streq(k, "CanCollide")) {
        lua_pushboolean(L, (p->flags & UB_PART_CAN_COLLIDE) != 0);
    } else if (streq(k, "Material")) {
        push_enum(L, EN_MATERIAL, (p->flags & UB_PART_GLOW) ? 288 : material_from_ub(p->material));
    } else if (streq(k, "Shape") && streq(cls_of(idx), "Part")) {
        push_enum(L, EN_PARTTYPE, p->shape == UB_SHAPE_BALL ? 0 : p->shape == UB_SHAPE_CYLINDER ? 2 : 1);
    } else if (streq(k, "Velocity") || streq(k, "AssemblyLinearVelocity") || streq(k, "RotVelocity") ||
               streq(k, "AssemblyAngularVelocity")) {
        i32 b = S.g->phys_ok ? ub_phys_part_body(&S.g->phys, W->inst[idx].part) : -1;
        vec3 v = v3(0, 0, 0);
        bool ang = k[0] == 'R' || streq(k, "AssemblyAngularVelocity");
        if (b > 0) v = ang ? S.g->phys.bodies[b].w : S.g->phys.bodies[b].v;
        if (idx == S.g->char_root && S.g->phys_ok && !ang) v = ub_char_velocity(&S.g->chr, &S.g->phys);
        push_v3(L, ang ? v3(-v.x, -v.y, v.z) : rbx_vec(v)); /* angular velocity is a pseudovector */
    } else if (streq(k, "Mass") || streq(k, "AssemblyMass")) {
        i32 b = S.g->phys_ok ? ub_phys_part_body(&S.g->phys, W->inst[idx].part) : -1;
        lua_pushnumber(L, b > 0 ? S.g->phys.bodies[b].mass : p->size.x * p->size.y * p->size.z * 0.7);
    } else if (streq(k, "Locked") || streq(k, "Massless") || streq(k, "CanTouch") || streq(k, "CanQuery") ||
               streq(k, "CastShadow")) {
        get_extra(L, idx, k);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_pushboolean(L, !(streq(k, "Locked") || streq(k, "Massless")));
        }
    } else if (streq(k, "Reflectance")) {
        get_extra(L, idx, k);
        if (lua_isnil(L, -1)) { lua_pop(L, 1); lua_pushnumber(L, 0); }
    } else {
        return false;
    }
    return true;
}

static bool get_class_prop(lua_State *L, i32 idx, const char *k)
{
    const char *cls = cls_of(idx);
    ub_game *g = S.g;
    if (idx == g->humanoid) {
        if (streq(k, "Health")) lua_pushnumber(L, g->health);
        else if (streq(k, "MaxHealth")) lua_pushnumber(L, g->max_health);
        else if (streq(k, "WalkSpeed")) lua_pushnumber(L, g->walk_speed);
        else if (streq(k, "JumpHeight")) lua_pushnumber(L, g->jump_height);
        else if (streq(k, "JumpPower")) lua_pushnumber(L, sqrt(2.0 * g_grav() * g->jump_height));
        else if (streq(k, "RootPart")) push_inst(L, g->char_root);
        else if (streq(k, "MoveDirection")) {
            vec3 v = g->phys_ok ? ub_char_velocity(&g->chr, &g->phys) : v3(0, 0, 0);
            v.y = 0;
            push_v3(L, rbx_vec(v3_length(v) > 0.5f ? v3_normalize(v) : v3(0, 0, 0)));
        } else if (streq(k, "FloorMaterial")) {
            i32 gp = g->phys_ok ? g->chr.ground_part : -1;
            push_enum(L, EN_MATERIAL, gp >= 0 ? material_from_ub(W->parts[gp].material) : 1792);
        } else if (streq(k, "Sit") || streq(k, "PlatformStand")) lua_pushboolean(L, 0);
        else if (streq(k, "RigType")) push_enum(L, enum_type_named("HumanoidRigType"), g->r15 ? 1 : 0);
        else if (streq(k, "UseJumpPower")) lua_pushboolean(L, 0);
        else if (streq(k, "HipHeight")) lua_pushnumber(L, g->r15 ? 2.0 : 0.0);
        else return false;
        return true;
    }
    if (idx == g->player) {
        if (streq(k, "RespawnLocation")) push_inst(L, g->respawn_location);
        else if (streq(k, "Character")) push_inst(L, g->char_model);
        else if (streq(k, "UserId")) lua_pushinteger(L, 1);
        else if (streq(k, "DisplayName")) lua_pushstring(L, name_of(idx));
        else if (streq(k, "AccountAge")) lua_pushinteger(L, 365);
        else if (streq(k, "Neutral")) lua_pushboolean(L, 1);
        else return false;
        return true;
    }
    if (idx == g->players && streq(k, "LocalPlayer")) {
        push_inst(L, g->player);
        return true;
    }
    if (idx == g->players && streq(k, "MaxPlayers")) {
        lua_pushinteger(L, 1);
        return true;
    }
    if (idx == W->workspace) {
        if (streq(k, "Gravity")) lua_pushnumber(L, W->gravity);
        else if (streq(k, "FallenPartsDestroyHeight")) lua_pushnumber(L, W->fallen_height);
        else if (streq(k, "DistributedGameTime")) lua_pushnumber(L, script_time());
        else if (streq(k, "CurrentCamera")) push_inst(L, VIRT_CAMERA);
        else return false;
        return true;
    }
    if (streq(cls, "Lighting")) {
        ub_lighting *l = &W->lighting;
        if (streq(k, "ClockTime")) lua_pushnumber(L, l->clock_time);
        else if (streq(k, "TimeOfDay")) {
            int t = (int)(l->clock_time * 3600 + 0.5);
            lua_pushfstring(L, "%d%d:%d%d:%d%d", t / 36000, t / 3600 % 10, t / 600 % 6, t / 60 % 10, t % 60 / 10, t % 10);
        } else if (streq(k, "Brightness")) lua_pushnumber(L, l->brightness);
        else if (streq(k, "FogEnd")) lua_pushnumber(L, l->fog_end > 0 ? l->fog_end : 100000);
        else if (streq(k, "GeographicLatitude")) lua_pushnumber(L, l->latitude);
        else if (streq(k, "Ambient")) push_c3_rgb(L, l->ambient);
        else if (streq(k, "OutdoorAmbient")) push_c3_rgb(L, l->outdoor_ambient);
        else if (streq(k, "FogColor")) push_c3_rgb(L, l->fog_color);
        else return false;
        return true;
    }
    if (is_a(cls, "BaseScript")) {
        if (streq(k, "Disabled")) lua_pushboolean(L, (W->inst[idx].flags & UB_INST_DISABLED) != 0);
        else if (streq(k, "Enabled")) lua_pushboolean(L, !(W->inst[idx].flags & UB_INST_DISABLED));
        else return false;
        return true;
    }
    if (is_value_class(cls) && streq(k, "Value")) {
        if (ub_world_prop_named(W, idx, "Value") && gui_get(L, idx, "Value")) return true;
        get_extra(L, idx, "Value");
        if (lua_isnil(L, -1) && !streq(cls, "ObjectValue")) { /* defaults */
            lua_pop(L, 1);
            if (streq(cls, "StringValue")) lua_pushliteral(L, "");
            else if (streq(cls, "BoolValue")) lua_pushboolean(L, 0);
            else if (streq(cls, "Vector3Value")) push_v3(L, v3(0, 0, 0));
            else if (streq(cls, "Color3Value")) push_c3(L, 0, 0, 0);
            else if (streq(cls, "CFrameValue")) { mat3 id = mat3_identity(); push_cf(L, v3(0, 0, 0), &id); }
            else if (streq(cls, "BrickColorValue")) push_brickcolor(L, 0xA3A2A5);
            else lua_pushnumber(L, 0);
        }
        return true;
    }
    if (is_a(cls, "Model") && streq(k, "PrimaryPart")) {
        push_inst(L, model_primary(L, idx));
        return true;
    }
    return false;
}

/* ------------------------------------------------------------- methods */

static int m_findfirstchild(lua_State *L)
{
    i32 i = check_inst(L, 1);
    push_inst(L, find_child(i, luaL_checkstring(L, 2), lua_toboolean(L, 3)));
    return 1;
}

static i32 child_by_class(i32 idx, const char *cls, bool isa, bool recursive)
{
    if (idx < 0) return -1;
    for (i32 c = W->inst[idx].first_child; c >= 0; c = W->inst[c].next_sibling)
        if (isa ? is_a(cls_of(c), cls) : streq(cls_of(c), cls)) return c;
    if (recursive)
        for (i32 c = W->inst[idx].first_child; c >= 0; c = W->inst[c].next_sibling) {
            i32 r = child_by_class(c, cls, isa, true);
            if (r >= 0) return r;
        }
    return -1;
}

static int m_ffcofclass(lua_State *L)
{
    push_inst(L, child_by_class(check_inst(L, 1), luaL_checkstring(L, 2), false, lua_toboolean(L, 3)));
    return 1;
}

static int m_ffcwhichisa(lua_State *L)
{
    push_inst(L, child_by_class(check_inst(L, 1), luaL_checkstring(L, 2), true, lua_toboolean(L, 3)));
    return 1;
}

static int m_ffancestor(lua_State *L)
{
    i32 i = check_inst(L, 1);
    const char *n = luaL_checkstring(L, 2);
    for (i = i >= 0 ? W->inst[i].parent : -1; i >= 0; i = W->inst[i].parent)
        if (streq(name_of(i), n)) break;
    push_inst(L, i);
    return 1;
}

static int m_ffancestorofclass(lua_State *L)
{
    i32 i = check_inst(L, 1);
    const char *n = luaL_checkstring(L, 2);
    for (i = i >= 0 ? W->inst[i].parent : -1; i >= 0; i = W->inst[i].parent)
        if (streq(cls_of(i), n) || (lua_toboolean(L, lua_upvalueindex(1)) && is_a(cls_of(i), n))) break;
    push_inst(L, i);
    return 1;
}

static int m_waitforchild_resume(lua_State *L);

static int m_waitforchild(lua_State *L)
{
    i32 i = check_inst(L, 1);
    const char *n = luaL_checkstring(L, 2);
    i32 c = find_child(i, n, false);
    if (c >= 0) {
        push_inst(L, c);
        return 1;
    }
    /* poll each frame until it appears (or the timeout passes) */
    f64 timeout = luaL_optnumber(L, 3, -1);
    lua_settop(L, 2);
    lua_pushnumber(L, script_time() + (timeout >= 0 ? timeout : 1e30));
    /* re-check after a frame via a Lua loop: simpler and yields safely */
    (void)m_waitforchild_resume;
    return yield_for(L, 0); /* the Lua wrapper loops; see k_waitforchild */
}

static int m_waitforchild_resume(lua_State *L) { return 0; }

static int m_getchildren(lua_State *L)
{
    push_children(L, check_inst(L, 1), false);
    return 1;
}

static int m_getdescendants(lua_State *L)
{
    push_children(L, check_inst(L, 1), true);
    return 1;
}

static int m_isa(lua_State *L)
{
    i32 i = check_inst(L, 1);
    const char *c = i < 0 ? virt_class(i) : cls_of(i);
    lua_pushboolean(L, is_a(c ? c : "Instance", luaL_checkstring(L, 2)));
    return 1;
}

static int m_isdescendantof(lua_State *L)
{
    i32 a = check_inst(L, 1), b = check_inst(L, 2);
    lua_pushboolean(L, a >= 0 && b == VIRT_GAME ? !(W->inst[a].flags & UB_INST_DETACHED) || W->inst[a].parent >= 0
                                                : (a >= 0 && b >= 0 && a != b && ub_world_is_descendant(W, a, b)));
    return 1;
}

static int m_isancestorof(lua_State *L)
{
    i32 a = check_inst(L, 1), b = check_inst(L, 2);
    lua_pushboolean(L, a >= 0 && b >= 0 && a != b && ub_world_is_descendant(W, b, a));
    return 1;
}

/* ------------------------------------------- reusing instances' slots */

/* An instance and everything below it (destroyed ones keep their links),
 * in a plat_alloc'd array; NULL when out of memory. */
i32 *sapi_subtree(i32 root, i32 *count)
{
    i32 cap = 64, n = 0, done = 0;
    i32 *list = (i32 *)plat_alloc(sizeof(i32) * (size_t)cap);
    if (!list) return NULL;
    list[n++] = root;
    while (done < n) {
        i32 i = list[done++];
        for (i32 c = W->inst[i].first_child; c >= 0; c = W->inst[c].next_sibling) {
            if (n == cap) {
                i32 *nl = (i32 *)plat_alloc(sizeof(i32) * (size_t)cap * 2);
                if (!nl) {
                    plat_free(list);
                    return NULL;
                }
                memcpy(nl, list, sizeof(i32) * (size_t)n);
                plat_free(list);
                list = nl;
                cap *= 2;
            }
            list[n++] = c;
        }
    }
    *count = n;
    return list;
}

/* candidates: instances whose userdata went (or that were destroyed) */
static i32 *g_dead;
static i32 g_dead_n, g_dead_cap;

static void queue_dead(i32 idx)
{
    if (g_dead_n == g_dead_cap) {
        i32 nc = g_dead_cap ? g_dead_cap * 2 : 256;
        i32 *n = (i32 *)plat_alloc(sizeof(i32) * (size_t)nc);
        if (!n) return; /* it just stays used */
        if (g_dead) {
            memcpy(n, g_dead, sizeof(i32) * (size_t)g_dead_n);
            plat_free(g_dead);
        }
        g_dead = n;
        g_dead_cap = nc;
    }
    g_dead[g_dead_n++] = idx;
}

static int inst_gc(lua_State *L)
{
    /* runs inside the collector: only note it */
    sinst *p = (sinst *)lua_touserdata(L, 1);
    if (p && p->idx >= 0) queue_dead(p->idx);
    return 0;
}

static bool has_userdata(lua_State *L, int instances, i32 idx)
{
    lua_rawgeti(L, instances, idx);
    bool has = !lua_isnil(L, -1);
    lua_pop(L, 1);
    return has;
}

static bool protected_inst(i32 i)
{
    return i == S.g->char_model || i == S.g->char_root || i == S.g->humanoid || i == S.g->player ||
           i == S.g->players || i == W->workspace;
}

static void drop_c_refs(const u8 *marked, i32 n);

void reclaim_instances(lua_State *L)
{
    if (!g_dead_n) return;
    lua_pushlightuserdata(L, &REG_INSTANCES);
    lua_rawget(L, LUA_REGISTRYINDEX);
    int instances = lua_gettop(L);
    u8 *marked = (u8 *)plat_alloc((size_t)W->count + 1);
    i32 *slots = (i32 *)plat_alloc(sizeof(i32) * ((size_t)W->count + 1));
    if (!marked || !slots) {
        plat_free(marked);
        plat_free(slots);
        lua_pop(L, 1);
        return;
    }
    memset(marked, 0, (size_t)W->count + 1);
    i32 ns = 0;
    for (i32 k = 0; k < g_dead_n; ++k) {
        i32 i = g_dead[k];
        if (i < 0 || i >= W->count || marked[i] || (W->inst[i].flags & UB_INST_FREE) || protected_inst(i)) continue;
        if (has_userdata(L, instances, i)) continue; /* a script can still reach it */
        u16 f = W->inst[i].flags;
        if (f & UB_INST_DESTROYED) {
            marked[i] = 1;
            slots[ns++] = i;
        } else if (W->inst[i].parent < 0 && (f & UB_INST_DETACHED)) {
            /* never parented (or taken out) and forgotten: gone, unless a
             * script still holds something inside it */
            i32 n;
            i32 *sub = sapi_subtree(i, &n);
            if (!sub) continue;
            bool held = false;
            for (i32 j = 1; j < n && !held; ++j) held = has_userdata(L, instances, sub[j]) || protected_inst(sub[j]);
            if (!held) {
                ub_world_destroy(W, i);
                for (i32 j = 0; j < n; ++j)
                    if (!marked[sub[j]]) {
                        marked[sub[j]] = 1;
                        slots[ns++] = sub[j];
                    }
            }
            plat_free(sub);
        }
    }
    g_dead_n = 0;
    if (ns) {
        forget_instances(L, marked, W->count);
        drop_c_refs(marked, W->count);
        for (i32 k = 0; k < ns; ++k) ub_world_reclaim(W, slots[k]);
    }
    plat_free(marked);
    plat_free(slots);
    lua_pop(L, 1);
}

void free_instances(void)
{
    plat_free(g_dead);
    g_dead = NULL;
    g_dead_n = g_dead_cap = 0;
}

/* May scripts make `n` more instances? Collects garbage first if needed. */
static void check_room(lua_State *L, i32 n)
{
    if (ub_world_live_count(W) + n <= S.base_live + (i32)UB_SCRIPT_MAX_INSTANCES) return;
    lua_gc(L, LUA_GCCOLLECT, 0);
    reclaim_instances(L);
    if (ub_world_live_count(W) + n > S.base_live + (i32)UB_SCRIPT_MAX_INSTANCES)
        luaL_error(L, "too many instances made by scripts (at most %d)", (int)UB_SCRIPT_MAX_INSTANCES);
}

static void do_destroy(lua_State *L, i32 i)
{
    if (i < 0 || is_service(i) || protected_inst(i)) return;
    if (!ub_world_alive(W, i)) return;
    i32 n;
    i32 *sub = sapi_subtree(i, &n);
    if (!sub) return;
    for (i32 j = 0; j < n; ++j)
        if (protected_inst(sub[j])) { /* the character is never destroyed with something else */
            plat_free(sub);
            return;
        }
    fire_signal(L, i, EV_DESTROYING, NULL, 0);
    if (!ub_world_alive(W, i)) { /* a Destroying handler destroyed it already */
        plat_free(sub);
        return;
    }
    tags_moved(L, i, sapi_in_game(i), false);
    for (i32 jj = 0; jj < n; ++jj) { /* joints in it let go; parts in it stop being carried */
        ub_game_uncarry(S.g, sub[jj], sub[jj]);
        ub_game_uncarry(S.g, sub[jj], -1);
    }
    if (!ub_world_alive(W, i)) { /* a handler destroyed it already */
        plat_free(sub);
        return;
    }
    i32 parent = W->inst[i].parent;
    for (i32 j = 0; j < n; ++j)
        if (W->inst[sub[j]].source != UB_NO_STRING) stop_script(sub[j], NULL);
    ub_world_destroy(W, i);
    ub_game_subtree_changed(S.g, i);
    if (parent >= 0) {
        push_inst(L, i);
        fire_signal(L, parent, EV_CHILDREMOVED, NULL, 1);
    }
    /* its connections go (as in Roblox), so handlers that refer to it do
     * not keep it alive; then its slots can be reused */
    u8 *marked = (u8 *)plat_alloc((size_t)W->count + 1);
    if (marked) {
        memset(marked, 0, (size_t)W->count + 1);
        for (i32 j = 0; j < n; ++j) marked[sub[j]] = 1;
        forget_instances(L, marked, W->count);
        plat_free(marked);
    }
    for (i32 j = 0; j < n; ++j) queue_dead(sub[j]);
    plat_free(sub);
}

static int m_destroy(lua_State *L)
{
    do_destroy(L, check_inst(L, 1));
    return 0;
}

static int m_clearallchildren(lua_State *L)
{
    i32 i = check_inst(L, 1);
    while (i >= 0 && W->inst[i].first_child >= 0) {
        i32 c = W->inst[i].first_child;
        if (c == S.g->char_model) { /* never the player's character */
            ub_world_set_parent(W, c, i);
            if (W->inst[i].first_child == c) break;
            continue;
        }
        do_destroy(L, c);
    }
    return 0;
}

static int m_clone(lua_State *L)
{
    i32 i = check_inst(L, 1);
    if (i < 0 || is_service(i)) {
        lua_pushnil(L);
        return 1;
    }
    i32 n;
    i32 *sub = sapi_subtree(i, &n);
    if (!sub) return luaL_error(L, "out of memory");
    plat_free(sub);
    check_room(L, n);
    i32 c = ub_world_clone(W, i);
    if (c >= 0) { /* copy the extra properties */
        push_props(L, i, false);
        if (lua_istable(L, -1)) {
            push_props(L, c, true);
            lua_pushnil(L);
            while (lua_next(L, -3)) {
                lua_pushvalue(L, -2);
                lua_insert(L, -2);
                lua_rawset(L, -4);
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        W->inst[c].flags &= (u16)~UB_INST_RUNTIME;
    }
    push_inst(L, c);
    return 1;
}

static int m_getfullname(lua_State *L)
{
    char b[512];
    i32 i = check_inst(L, 1);
    if (i < 0) snprintf(b, sizeof b, "%s", i == VIRT_GAME ? "Game" : virt_class(i));
    else full_name(i, b, sizeof b);
    lua_pushstring(L, b);
    return 1;
}

static int m_propsignal(lua_State *L)
{
    push_signal(L, check_inst(L, 1), EV_PROPCHANGED, luaL_checkstring(L, 2));
    return 1;
}

static int m_noop(lua_State *L)
{
    (void)L;
    return 0;
}

static int m_true(lua_State *L)
{
    lua_pushboolean(L, 1);
    return 1;
}

static int m_false(lua_State *L)
{
    lua_pushboolean(L, 0);
    return 1;
}

static int m_zero(lua_State *L)
{
    lua_pushinteger(L, 0);
    return 1;
}

static int m_emptytable(lua_State *L)
{
    lua_newtable(L);
    return 1;
}

/* parts and models */
static int m_getpivot(lua_State *L)
{
    i32 i = check_inst(L, 1);
    vec3 p;
    mat3 r;
    if (is_part(i)) part_get_cframe(part_of(i), &p, &r);
    else model_pivot(L, i, &p, &r);
    push_cf(L, p, &r);
    return 1;
}

static int m_pivotto(lua_State *L)
{
    i32 i = check_inst(L, 1);
    scf *c = check_cf(L, 2);
    if (is_part(i)) move_part(i, c->p, &c->r);
    else if (i >= 0) model_pivot_to(L, i, c->p, &c->r);
    return 0;
}

static int m_moveto(lua_State *L)
{
    i32 i = check_inst(L, 1);
    vec3 target = check_v3(L, 2), p;
    mat3 r;
    if (i == S.g->humanoid) return 0; /* Humanoid:MoveTo: walking to a point is not done yet */
    model_pivot(L, i, &p, &r);
    model_pivot_to(L, i, target, &r);
    return 0;
}

static int m_getmass(lua_State *L)
{
    i32 i = check_inst(L, 1);
    if (!is_part(i)) return luaL_error(L, "GetMass needs a part");
    lua_getfield(L, 1, "Mass");
    return 1;
}

static int m_applyimpulse(lua_State *L)
{
    i32 i = check_inst(L, 1);
    vec3 imp = rbx_vec(check_v3(L, 2));
    if (!is_part(i) || !S.g->phys_ok) return 0;
    i32 b = ub_phys_part_body(&S.g->phys, W->inst[i].part);
    if (b > 0) ub_phys_apply_impulse(&S.g->phys, b, imp, S.g->phys.bodies[b].pos);
    return 0;
}

static int m_getboundingbox(lua_State *L)
{
    i32 i = check_inst(L, 1);
    i32 parts[4096];
    int n = i >= 0 ? collect_parts(i, parts, 4096) : 0;
    vec3 lo = v3(1e30f, 1e30f, 1e30f), hi = v3(-1e30f, -1e30f, -1e30f);
    for (int k = 0; k < n; ++k) {
        const ub_part *p = part_of(parts[k]);
        vec3 h = v3_scale(p->size, 0.5f);
        vec3 e = v3((f32)fabs(p->rot.x.x) * h.x + (f32)fabs(p->rot.y.x) * h.y + (f32)fabs(p->rot.z.x) * h.z,
                    (f32)fabs(p->rot.x.y) * h.x + (f32)fabs(p->rot.y.y) * h.y + (f32)fabs(p->rot.z.y) * h.z,
                    (f32)fabs(p->rot.x.z) * h.x + (f32)fabs(p->rot.y.z) * h.y + (f32)fabs(p->rot.z.z) * h.z);
        lo = v3_min(lo, rbx_vec(v3_sub(p->pos, e)));
        hi = v3_max(hi, rbx_vec(v3_add(p->pos, e)));
        lo = v3_min(lo, rbx_vec(v3_add(p->pos, e)));
        hi = v3_max(hi, rbx_vec(v3_sub(p->pos, e)));
    }
    if (!n) lo = hi = v3(0, 0, 0);
    mat3 id = mat3_identity();
    push_cf(L, v3_scale(v3_add(lo, hi), 0.5f), &id);
    push_v3(L, v3_sub(hi, lo));
    return 2;
}

static int m_getextentssize(lua_State *L)
{
    m_getboundingbox(L);
    return 1;
}

static int m_setprimarypartcframe(lua_State *L)
{
    i32 i = check_inst(L, 1);
    scf *c = check_cf(L, 2);
    if (model_primary(L, i) < 0) return luaL_error(L, "Model:SetPrimaryPartCFrame() failed because no PrimaryPart has been set");
    model_pivot_to(L, i, c->p, &c->r);
    return 0;
}

static int m_getprimarypartcframe(lua_State *L)
{
    i32 i = check_inst(L, 1), pp = model_primary(L, i);
    vec3 p;
    mat3 r;
    if (pp < 0) {
        lua_pushnil(L);
        return 1;
    }
    part_get_cframe(part_of(pp), &p, &r);
    push_cf(L, p, &r);
    return 1;
}

/* humanoid, players */
static int m_takedamage(lua_State *L)
{
    check_inst(L, 1);
    f64 d = luaL_checknumber(L, 2);
    ub_game_set_health(S.g, S.g->health - (f32)d);
    lua_pushnumber(L, S.g->health);
    fire_signal(L, S.g->humanoid, EV_HEALTHCHANGED, NULL, 1);
    return 0;
}

static int m_getstate(lua_State *L)
{
    push_enum(L, EN_HUMANOIDSTATE, S.g->dead ? 15 : humanoid_state());
    return 1;
}

static int anim_track_index(lua_State *L)
{
    const char *k = luaL_checkstring(L, 2);
    if (streq(k, "Stopped") || streq(k, "Ended") || streq(k, "KeyframeReached")) {
        push_signal(L, -2000, EV_COMPLETED, NULL);
        return 1;
    }
    if (streq(k, "IsPlaying")) {
        lua_pushboolean(L, 0);
        return 1;
    }
    if (streq(k, "Length") || streq(k, "Speed") || streq(k, "TimePosition")) {
        lua_pushnumber(L, 0);
        return 1;
    }
    lua_pushcfunction(L, m_noop); /* Play, Stop, AdjustSpeed, ... */
    return 1;
}

static int m_loadanimation(lua_State *L)
{
    /* animations are not played yet: a track that does nothing */
    lua_newtable(L);
    lua_newtable(L);
    lua_pushcfunction(L, anim_track_index);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    return 1;
}

static int m_getplayers(lua_State *L)
{
    lua_newtable(L);
    push_inst(L, S.g->player);
    lua_rawseti(L, -2, 1);
    return 1;
}

static int m_playerfromcharacter(lua_State *L)
{
    i32 c = opt_inst(L, 2);
    push_inst(L, c >= 0 && c == S.g->char_model ? S.g->player : -1);
    return 1;
}

static int m_playerbyuserid(lua_State *L)
{
    push_inst(L, luaL_checkinteger(L, 2) == 1 ? S.g->player : -1);
    return 1;
}

static int m_getmouse(lua_State *L)
{
    push_inst(L, VIRT_MOUSE);
    return 1;
}

static int m_thumbnail(lua_State *L)
{
    /* nothing is downloaded: no picture, but the call succeeds */
    lua_pushliteral(L, "");
    lua_pushboolean(L, 1);
    return 2;
}

static int m_playername(lua_State *L)
{
    lua_pushstring(L, ub_world_str(W, W->inst[S.g->player].name));
    return 1;
}

static int m_one(lua_State *L)
{
    lua_pushinteger(L, 1);
    return 1;
}

static int m_loadcharacter(lua_State *L)
{
    (void)L;
    ub_game_set_health(S.g, 0);
    S.g->respawn_timer = 0;
    return 0;
}

static int m_kick(lua_State *L)
{
    char b[300];
    snprintf(b, sizeof b, "the player was kicked: %s", luaL_optstring(L, 2, ""));
    script_output(b, true);
    return 0;
}

static int m_distancefromcharacter(lua_State *L)
{
    vec3 p = rbx_vec(check_v3(L, 2));
    lua_pushnumber(L, v3_length(v3_sub(p, ub_game_root_position(S.g))));
    return 1;
}

/* lighting */
static int m_setminutes(lua_State *L)
{
    f64 m = luaL_checknumber(L, 2);
    m = fmod(m, 1440.0);
    if (m < 0) m += 1440;
    W->lighting.clock_time = (f32)(m / 60.0);
    return 0;
}

static int m_getminutes(lua_State *L)
{
    lua_pushnumber(L, W->lighting.clock_time * 60.0);
    return 1;
}

/* game */
static int m_getservice(lua_State *L)
{
    const char *cls = luaL_checkstring(L, 2);
    i32 s = get_service(cls, lua_toboolean(L, lua_upvalueindex(1)) == 0);
    if (s == -1 && lua_toboolean(L, lua_upvalueindex(1)) == 0)
        return sapi_error(L, "'%s' is not a valid Service name", cls);
    push_inst(L, s);
    return 1;
}

/* workspace */
static bool filtered(lua_State *L, int params, i32 inst)
{
    if (params == 0) return false;
    lua_getfield(L, params, "FilterType");
    int type = lua_isnil(L, -1) ? 0 : check_enum(L, -1, EN_RAYCASTFILTER);
    lua_pop(L, 1);
    lua_getfield(L, params, "FilterDescendantsInstances");
    bool in_list = false;
    if (lua_istable(L, -1)) {
        int n = (int)lua_objlen(L, -1);
        for (int k = 1; k <= n && !in_list; ++k) {
            lua_rawgeti(L, -1, k);
            if (is_inst(L, -1)) {
                i32 f = ((sinst *)lua_touserdata(L, -1))->idx;
                in_list = f >= 0 && ub_world_is_descendant(W, inst, f);
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    return type == 1 ? !in_list : in_list;
}

/* Casts a ray in Roblox space; skips filtered instances. */
static bool cast(lua_State *L, vec3 origin, vec3 dir, int params, i32 ignore, i32 *inst, vec3 *pos, vec3 *normal)
{
    if (!S.g->phys_ok) return false;
    vec3 o = rbx_vec(origin), d = rbx_vec(dir);
    f32 len = v3_length(d);
    if (len < 1e-6f) return false;
    d = v3_scale(d, 1.0f / len);
    f32 done = 0;
    for (int tries = 0; tries < 32; ++tries) {
        ub_phys_hit h;
        if (!ub_phys_raycast(&S.g->phys, o, d, len - done, -1, &h)) return false;
        i32 hit = h.part >= 0 ? ub_world_part_inst(W, h.part) : S.g->char_root;
        bool skip = hit < 0 || (ignore >= 0 && ub_world_is_descendant(W, hit, ignore)) || filtered(L, params, hit);
        if (!skip) {
            *inst = hit;
            *pos = rbx_vec(h.point);
            *normal = rbx_vec(h.normal);
            return true;
        }
        f32 step = h.t + 0.01f;
        done += step;
        o = v3_madd(o, d, step);
        if (done >= len) return false;
    }
    return false;
}

static int m_raycast(lua_State *L)
{
    vec3 o = check_v3(L, 2), d = check_v3(L, 3);
    int params = lua_istable(L, 4) ? 4 : 0;
    i32 inst;
    vec3 pos, n;
    if (!cast(L, o, d, params, -1, &inst, &pos, &n)) {
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    push_inst(L, inst);
    lua_setfield(L, -2, "Instance");
    push_v3(L, pos);
    lua_setfield(L, -2, "Position");
    push_v3(L, n);
    lua_setfield(L, -2, "Normal");
    lua_pushnumber(L, v3_length(v3_sub(pos, o)));
    lua_setfield(L, -2, "Distance");
    push_enum(L, EN_MATERIAL, is_part(inst) ? material_from_ub(part_of(inst)->material) : 256);
    lua_setfield(L, -2, "Material");
    return 1;
}

static int m_findpartonray(lua_State *L)
{
    lua_getfield(L, 2, "Origin");
    lua_getfield(L, 2, "Direction");
    vec3 o = check_v3(L, -2), d = check_v3(L, -1);
    i32 ignore = -1;
    int params = 0;
    if (is_inst(L, 3)) ignore = check_inst(L, 3);
    else if (lua_istable(L, 3)) { /* WithIgnoreList */
        lua_newtable(L);
        lua_pushvalue(L, 3);
        lua_setfield(L, -2, "FilterDescendantsInstances");
        params = lua_gettop(L);
    }
    i32 inst;
    vec3 pos, n;
    if (!cast(L, o, d, params, ignore, &inst, &pos, &n)) {
        lua_pushnil(L);
        push_v3(L, v3_add(o, d));
        push_v3(L, v3(0, 0, 0));
        return 3;
    }
    push_inst(L, inst);
    push_v3(L, pos);
    push_v3(L, n);
    push_enum(L, EN_MATERIAL, is_part(inst) ? material_from_ub(part_of(inst)->material) : 256);
    return 4;
}

/* ------------------------------------------------ overlap queries
 * GetPartBoundsInBox/InRadius compare part bounding boxes, GetPartsInPart
 * and the Region3 finders the parts' boxes, all in Uniblox space. */

typedef struct { vec3 c; mat3 r; vec3 h; } sobb;

static f32 absf(f32 x) { return x < 0 ? -x : x; }

static bool obb_overlap(const sobb *a, const sobb *b)
{
    const vec3 *ax = &a->r.x, *bx = &b->r.x;
    vec3 d = v3_sub(b->c, a->c);
    vec3 axes[15];
    int n = 0;
    for (int i = 0; i < 3; ++i) axes[n++] = ax[i];
    for (int i = 0; i < 3; ++i) axes[n++] = bx[i];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) axes[n++] = v3_cross(ax[i], bx[j]);
    for (int k = 0; k < n; ++k) {
        vec3 l = axes[k];
        if (v3_dot(l, l) < 1e-10f) continue;
        f32 ra = 0, rb = 0;
        const f32 *ha = &a->h.x, *hb = &b->h.x;
        for (int i = 0; i < 3; ++i) {
            ra += ha[i] * absf(v3_dot(l, ax[i]));
            rb += hb[i] * absf(v3_dot(l, bx[i]));
        }
        if (absf(v3_dot(l, d)) > ra + rb) return false;
    }
    return true;
}

static sobb part_box(const ub_part *p, bool bounds)
{
    sobb o;
    o.c = p->pos;
    o.h = v3_scale(p->size, 0.5f);
    o.r = p->rot;
    if (bounds) { /* the axis-aligned box around it */
        vec3 h = o.h;
        o.h = v3(absf(p->rot.x.x) * h.x + absf(p->rot.y.x) * h.y + absf(p->rot.z.x) * h.z,
                 absf(p->rot.x.y) * h.x + absf(p->rot.y.y) * h.y + absf(p->rot.z.y) * h.z,
                 absf(p->rot.x.z) * h.x + absf(p->rot.y.z) * h.y + absf(p->rot.z.z) * h.z);
        o.r = mat3_identity();
    }
    return o;
}

enum { Q_BOX, Q_SPHERE };

/* Pushes a list of the parts in the Workspace overlapping the query. */
static int overlap(lua_State *L, int kind, const sobb *q, vec3 sc, f32 sr, bool bounds, int params, i32 ignore,
                   bool white, int list, int max, i32 self)
{
    bool respect = false;
    if (params) {
        lua_getfield(L, params, "MaxParts");
        if (lua_isnumber(L, -1) && lua_tointeger(L, -1) > 0) max = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, params, "RespectCanCollide");
        respect = lua_toboolean(L, -1);
        lua_pop(L, 1);
    }
    i32 ws = ub_world_workspace(W);
    lua_newtable(L);
    int n = 0;
    for (i32 pi = 0; pi < W->part_count && (max <= 0 || n < max); ++pi) {
        const ub_part *p = &W->parts[pi];
        if (p->flags & UB_PART_REMOVED) continue;
        if (respect && !(p->flags & UB_PART_CAN_COLLIDE)) continue;
        i32 inst = ub_world_part_inst(W, pi);
        if (inst < 0 || inst == self || !ub_world_alive(W, inst) || !ub_world_is_descendant(W, inst, ws)) continue;
        if (ignore >= 0 && ub_world_is_descendant(W, inst, ignore)) continue;
        if (list) { /* an ignore list or a white list of instances */
            bool in = false;
            for (int k = 1, m = (int)lua_objlen(L, list); k <= m && !in; ++k) {
                lua_rawgeti(L, list, k);
                if (is_inst(L, -1)) {
                    i32 f = ((sinst *)lua_touserdata(L, -1))->idx;
                    in = f >= 0 && ub_world_is_descendant(W, inst, f);
                }
                lua_pop(L, 1);
            }
            if (in != white) continue;
        }
        if (filtered(L, params, inst)) continue;
        sobb b = part_box(p, bounds);
        bool hit;
        if (kind == Q_SPHERE) { /* closest point of the box to the centre */
            vec3 d = v3_sub(sc, b.c);
            f32 dist2 = 0;
            const vec3 *ax = &b.r.x;
            const f32 *h = &b.h.x;
            for (int i = 0; i < 3; ++i) {
                f32 t = v3_dot(d, ax[i]), e = absf(t) - h[i];
                if (e > 0) dist2 += e * e;
            }
            hit = dist2 <= sr * sr;
        } else hit = obb_overlap(q, &b);
        if (!hit) continue;
        push_inst(L, inst);
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

static sobb query_box(lua_State *L, int cfi, int sizei)
{
    scf *c = check_cf(L, cfi);
    vec3 size = check_v3(L, sizei);
    sobb q;
    q.c = rbx_vec(c->p);
    q.r = rbx_rot(&c->r);
    q.h = v3_scale(v3(absf(size.x), absf(size.y), absf(size.z)), 0.5f);
    return q;
}

static int opt_params(lua_State *L, int i) { return lua_istable(L, i) ? i : 0; }

static int m_boundsinbox(lua_State *L)
{
    sobb q = query_box(L, 2, 3);
    return overlap(L, Q_BOX, &q, v3(0, 0, 0), 0, true, opt_params(L, 4), -1, false, 0, 0, -1);
}

static int m_boundsinradius(lua_State *L)
{
    vec3 c = rbx_vec(check_v3(L, 2));
    f32 r = (f32)luaL_checknumber(L, 3);
    return overlap(L, Q_SPHERE, NULL, c, r, true, opt_params(L, 4), -1, false, 0, 0, -1);
}

static int m_partsinpart(lua_State *L)
{
    i32 i = check_inst(L, 2);
    if (!is_part(i)) return sapi_error(L, "GetPartsInPart: expected a BasePart");
    sobb q = part_box(part_of(i), false);
    return overlap(L, Q_BOX, &q, v3(0, 0, 0), 0, false, opt_params(L, 3), -1, false, 0, 0, i);
}

/* FindPartsInRegion3(region, ignore, max), ...WithIgnoreList(region, list,
 * max), ...WithWhiteList(region, list, max): at most 20 by default */
static int find_in_region(lua_State *L, int mode) /* 0 one ignore, 1 ignore list, 2 white list */
{
    vec3 lo, hi;
    if (!get_region3(L, 2, &lo, &hi)) return luaL_typerror(L, 2, "Region3");
    sobb q;
    vec3 a = rbx_vec(lo), b = rbx_vec(hi);
    q.c = v3_scale(v3_add(a, b), 0.5f);
    q.h = v3_scale(v3(absf(b.x - a.x), absf(b.y - a.y), absf(b.z - a.z)), 0.5f);
    q.r = mat3_identity();
    i32 ignore = mode == 0 ? opt_inst(L, 3) : -1;
    int list = mode != 0 && lua_istable(L, 3) ? 3 : 0;
    int max = (int)luaL_optinteger(L, 4, 20);
    return overlap(L, Q_BOX, &q, v3(0, 0, 0), 0, false, 0, ignore, mode == 2, list, max, -1);
}

static int m_findinregion(lua_State *L) { return find_in_region(L, 0); }
static int m_findinregion_ignore(lua_State *L) { return find_in_region(L, 1); }
static int m_findinregion_white(lua_State *L) { return find_in_region(L, 2); }

static int m_servertime(lua_State *L)
{
    lua_pushnumber(L, script_time());
    return 1;
}

/* run service */
static int m_bindtorenderstep(lua_State *L)
{
    luaL_checktype(L, 4, LUA_TFUNCTION);
    push_signal(L, VIRT_RUNSERVICE, EV_RENDERSTEPPED, NULL);
    lua_getfield(L, -1, "Connect");
    lua_insert(L, -2);
    lua_pushvalue(L, 4);
    lua_call(L, 2, 0);
    return 0;
}

/* http (offline) */
static int m_guid(lua_State *L)
{
    static u32 seed = 0x1234567u;
    char b[40];
    u32 r[4];
    for (int i = 0; i < 4; ++i) {
        seed = seed * 1103515245u + 12345u + (u32)plat_time_us();
        r[i] = seed;
    }
    snprintf(b, sizeof b, "%s%08X-%04X-%04X-%04X-%04X%08X%s", lua_toboolean(L, 2) ? "{" : "", r[0], r[1] >> 16,
             r[1] & 0xFFFF, r[2] >> 16, r[2] & 0xFFFF, r[3], lua_toboolean(L, 2) ? "}" : "");
    lua_pushstring(L, b);
    return 1;
}

static int m_offline(lua_State *L)
{
    return luaL_error(L, "%s is not available offline", lua_tostring(L, lua_upvalueindex(1)));
}

/* datastores: a store that only lives while the game runs */
static int ds_get(lua_State *L)
{
    lua_getfield(L, 1, "_data");
    lua_pushvalue(L, 2);
    lua_gettable(L, -2);
    return 1;
}

static int ds_set(lua_State *L)
{
    lua_getfield(L, 1, "_data");
    lua_pushvalue(L, 2);
    lua_pushvalue(L, 3);
    lua_settable(L, -3);
    return 0;
}

static int ds_update(lua_State *L)
{
    lua_getfield(L, 1, "_data");
    lua_pushvalue(L, 3);
    lua_pushvalue(L, 2);
    lua_gettable(L, -3);
    lua_call(L, 1, 1);
    lua_pushvalue(L, 2);
    lua_pushvalue(L, -2);
    lua_settable(L, -4);
    return 1;
}

static int m_getdatastore(lua_State *L)
{
    static char REG_STORES;
    const char *name = luaL_checkstring(L, 2);
    lua_pushlightuserdata(L, &REG_STORES);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushlightuserdata(L, &REG_STORES);
        lua_pushvalue(L, -2);
        lua_rawset(L, LUA_REGISTRYINDEX);
    }
    lua_getfield(L, -1, name);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_newtable(L);
        lua_setfield(L, -2, "_data");
        lua_pushcfunction(L, ds_get);
        lua_setfield(L, -2, "GetAsync");
        lua_pushcfunction(L, ds_set);
        lua_setfield(L, -2, "SetAsync");
        lua_pushcfunction(L, ds_update);
        lua_setfield(L, -2, "UpdateAsync");
        lua_pushcfunction(L, ds_set);
        lua_setfield(L, -2, "RemoveAsync");
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, name);
    }
    return 1;
}

typedef struct {
    const char *name;
    lua_CFunction fn;
} method;

static const method k_inst_methods[] = {
    { "FindFirstChild", m_findfirstchild }, { "findFirstChild", m_findfirstchild },
    { "FindFirstChildOfClass", m_ffcofclass }, { "FindFirstChildWhichIsA", m_ffcwhichisa },
    { "FindFirstAncestor", m_ffancestor }, { "WaitForChild", m_waitforchild }, { "GetChildren", m_getchildren },
    { "getChildren", m_getchildren }, { "children", m_getchildren }, { "GetDescendants", m_getdescendants },
    { "IsA", m_isa }, { "isA", m_isa }, { "IsDescendantOf", m_isdescendantof }, { "IsAncestorOf", m_isancestorof },
    { "Destroy", m_destroy }, { "destroy", m_destroy }, { "Remove", m_destroy }, { "remove", m_destroy },
    { "ClearAllChildren", m_clearallchildren }, { "Clone", m_clone }, { "clone", m_clone },
    { "GetFullName", m_getfullname }, { "GetAttribute", attr_get }, { "SetAttribute", attr_set },
    { "GetAttributes", attr_getall }, { "GetAttributeChangedSignal", attr_signal },
    { "GetPropertyChangedSignal", m_propsignal }, { "GetPivot", m_getpivot }, { "PivotTo", m_pivotto },
    { "BreakJoints", m_noop }, { "MakeJoints", m_noop }, { "GetTags", tag_list_of }, { "HasTag", tag_has },
    { "AddTag", tag_add }, { "RemoveTag", tag_remove }, { NULL, NULL },
};

static const method k_part_methods[] = {
    { "GetMass", m_getmass }, { "ApplyImpulse", m_applyimpulse }, { "ApplyAngularImpulse", m_noop },
    { "GetTouchingParts", m_emptytable }, { "CanCollideWith", m_true }, { "SetNetworkOwner", m_noop },
    { "GetNetworkOwner", m_noop }, { "SetNetworkOwnershipAuto", m_noop }, { "GetConnectedParts", m_emptytable },
    { "IsGrounded", m_true }, { NULL, NULL },
};

static const method k_model_methods[] = {
    { "MoveTo", m_moveto }, { "SetPrimaryPartCFrame", m_setprimarypartcframe },
    { "GetPrimaryPartCFrame", m_getprimarypartcframe }, { "GetBoundingBox", m_getboundingbox },
    { "GetExtentsSize", m_getextentssize }, { "GetModelCFrame", m_getpivot }, { NULL, NULL },
};

static const method k_humanoid_methods[] = {
    { "TakeDamage", m_takedamage }, { "MoveTo", m_moveto }, { "ChangeState", m_noop }, { "GetState", m_getstate },
    { "SetStateEnabled", m_noop }, { "UnequipTools", m_noop }, { "EquipTool", m_noop },
    { "LoadAnimation", m_loadanimation }, { "GetPlayingAnimationTracks", m_emptytable }, { NULL, NULL },
};

static const method k_player_methods[] = {
    { "LoadCharacter", m_loadcharacter }, { "Kick", m_kick }, { "IsInGroup", m_false }, { "GetRankInGroup", m_zero },
    { "GetRoleInGroup", m_noop }, { "HasAppearanceLoaded", m_true },
    { "DistanceFromCharacter", m_distancefromcharacter }, { "GetMouse", m_getmouse }, { NULL, NULL },
};

static const method k_players_methods[] = {
    { "GetPlayers", m_getplayers }, { "getPlayers", m_getplayers }, { "GetPlayerFromCharacter", m_playerfromcharacter },
    { "getPlayerFromCharacter", m_playerfromcharacter }, { "GetPlayerByUserId", m_playerbyuserid },
    { "GetUserThumbnailAsync", m_thumbnail }, { "GetNameFromUserIdAsync", m_playername },
    { "GetUserIdFromNameAsync", m_one }, { NULL, NULL },
};

static const method k_lighting_methods[] = {
    { "SetMinutesAfterMidnight", m_setminutes }, { "GetMinutesAfterMidnight", m_getminutes }, { NULL, NULL },
};

static const method k_workspace_methods[] = {
    { "Raycast", m_raycast }, { "FindPartOnRay", m_findpartonray }, { "GetPartBoundsInBox", m_boundsinbox },
    { "GetPartBoundsInRadius", m_boundsinradius }, { "GetPartsInPart", m_partsinpart },
    { "FindPartsInRegion3", m_findinregion }, { "FindPartsInRegion3WithIgnoreList", m_findinregion_ignore },
    { "FindPartsInRegion3WithWhiteList", m_findinregion_white },
    { "FindPartOnRayWithIgnoreList", m_findpartonray }, { "FindPartOnRayWithWhitelist", m_findpartonray },
    { "GetServerTimeNow", m_servertime }, { NULL, NULL },
};

static lua_CFunction find_method(const method *list, const char *k)
{
    for (int i = 0; list[i].name; ++i)
        if (streq(list[i].name, k)) return list[i].fn;
    return NULL;
}

/* --------------------------------------------------------------- events */

static int find_event(i32 idx, const char *k)
{
    const char *cls = idx >= 0 ? cls_of(idx) : virt_class(idx);
    if (idx >= 0) {
        if (streq(k, "Changed")) return EV_CHANGED;
        if (streq(k, "ChildAdded")) return EV_CHILDADDED;
        if (streq(k, "ChildRemoved")) return EV_CHILDREMOVED;
        if (streq(k, "DescendantAdded")) return EV_DESCENDANTADDED;
        if (streq(k, "DescendantRemoving")) return EV_DESCENDANTREMOVING;
        if (streq(k, "Destroying")) return EV_DESTROYING;
        if (streq(k, "AncestryChanged")) return EV_ANCESTRYCHANGED;
        if (streq(k, "AttributeChanged")) return EV_ATTRCHANGED;
    }
    if (is_part(idx) && (streq(k, "Touched") || streq(k, "touched"))) return EV_TOUCHED;
    if (is_part(idx) && streq(k, "TouchEnded")) return EV_TOUCHENDED;
    int iev = interact_event(idx, k);
    if (iev >= 0) return iev;
    if (idx == S.g->humanoid) {
        if (streq(k, "Died")) return EV_DIED;
        if (streq(k, "HealthChanged")) return EV_HEALTHCHANGED;
        if (streq(k, "Touched")) return EV_TOUCHED;
        if (streq(k, "Running") || streq(k, "Jumping") || streq(k, "StateChanged") || streq(k, "FreeFalling") ||
            streq(k, "Seated") || streq(k, "MoveToFinished"))
            return EV_TOUCHENDED; /* never fires yet */
    }
    if (idx == S.g->players && streq(k, "PlayerAdded")) return EV_PLAYERADDED;
    if (idx == S.g->players && streq(k, "PlayerRemoving")) return EV_PLAYERREMOVING;
    if (idx == S.g->player) {
        if (streq(k, "CharacterAdded")) return EV_CHARACTERADDED;
        if (streq(k, "CharacterRemoving")) return EV_CHARACTERREMOVING;
        if (streq(k, "CharacterAppearanceLoaded")) return EV_CHARACTERADDED;
        if (streq(k, "Chatted")) return EV_CHATTED;
    }
    if (idx == VIRT_RUNSERVICE) {
        if (streq(k, "Heartbeat")) return EV_HEARTBEAT;
        if (streq(k, "Stepped")) return EV_STEPPED;
        if (streq(k, "RenderStepped") || streq(k, "PreRender") || streq(k, "PostSimulation") || streq(k, "PreSimulation"))
            return streq(k, "PostSimulation") ? EV_HEARTBEAT : streq(k, "PreSimulation") ? EV_STEPPED : EV_RENDERSTEPPED;
    }
    (void)cls;
    return gui_event(idx, k);
}

/* ------------------------------------------------------------- __index */

static int inst_index(lua_State *L)
{
    i32 idx = check_inst(L, 1);
    const char *k = luaL_checkstring(L, 2);
    /* virtual services and game */
    if (idx < 0) {
        const char *vc = virt_class(idx);
        if (streq(k, "ClassName")) { lua_pushstring(L, vc); return 1; }
        if (streq(k, "Name")) { lua_pushstring(L, idx == VIRT_GAME ? "Game" : vc); return 1; }
        if (streq(k, "Parent")) { push_inst(L, idx == VIRT_GAME ? -1 : VIRT_GAME); return 1; }
        if (idx == VIRT_GAME) {
            if (streq(k, "GetService") || streq(k, "getService") || streq(k, "service")) {
                lua_pushboolean(L, 0);
                lua_pushcclosure(L, m_getservice, 1);
                return 1;
            }
            if (streq(k, "FindService")) {
                lua_pushboolean(L, 1);
                lua_pushcclosure(L, m_getservice, 1);
                return 1;
            }
            if (streq(k, "IsLoaded")) { lua_pushcfunction(L, m_true); return 1; }
            if (streq(k, "BindToClose")) { lua_pushcfunction(L, m_noop); return 1; }
            if (streq(k, "PlaceId") || streq(k, "GameId") || streq(k, "CreatorId")) { lua_pushinteger(L, 0); return 1; }
            if (streq(k, "JobId")) { lua_pushliteral(L, ""); return 1; }
            if (streq(k, "Workspace") || streq(k, "workspace")) { push_inst(L, ub_world_workspace(W)); return 1; }
            if (streq(k, "Loaded")) { push_signal(L, VIRT_GAME, EV_COMPLETED, NULL); return 1; }
            i32 s = get_service(k, true); /* game.Players, game.Lighting... */
            if (s != -1) { push_inst(L, s); return 1; }
        }
        if (idx == VIRT_RUNSERVICE) {
            if (streq(k, "IsServer") || streq(k, "IsClient") || streq(k, "IsRunning") || streq(k, "IsRunMode")) {
                lua_pushcfunction(L, m_true);
                return 1;
            }
            if (streq(k, "IsStudio")) { lua_pushcfunction(L, m_false); return 1; }
            if (streq(k, "BindToRenderStep")) { lua_pushcfunction(L, m_bindtorenderstep); return 1; }
            if (streq(k, "UnbindFromRenderStep")) { lua_pushcfunction(L, m_noop); return 1; }
        }
        if (idx == VIRT_HTTP) {
            if (streq(k, "GenerateGUID")) { lua_pushcfunction(L, m_guid); return 1; }
            if (streq(k, "JSONEncode")) { lua_pushcfunction(L, json_encode); return 1; }
            if (streq(k, "JSONDecode")) { lua_pushcfunction(L, json_decode); return 1; }
            if (streq(k, "UrlEncode")) { lua_pushcfunction(L, url_encode); return 1; }
            if (streq(k, "HttpEnabled")) { lua_pushboolean(L, 0); return 1; }
        }
        if (idx == VIRT_COLLECTION && collection_index(L, k)) return 1;
        if (offline_index(L, idx, k)) return 1;
        if (idx == VIRT_DATASTORE && (streq(k, "GetDataStore") || streq(k, "GetGlobalDataStore") ||
                                      streq(k, "GetOrderedDataStore"))) {
            lua_pushcfunction(L, m_getdatastore);
            return 1;
        }
        if (idx == VIRT_TWEENSERVICE || idx == VIRT_DEBRIS) {
            lua_getfield(L, LUA_REGISTRYINDEX, idx == VIRT_TWEENSERVICE ? "ub.TweenService" : "ub.Debris");
            lua_getfield(L, -1, k);
            if (!lua_isnil(L, -1)) return 1;
            lua_pop(L, 2);
        }
        if (gui_virtual_index(L, idx, k)) return 1;
        if (streq(k, "GetPropertyChangedSignal")) { lua_pushcfunction(L, m_propsignal); return 1; }
        if (streq(k, "IsA")) { lua_pushcfunction(L, m_isa); return 1; }
        if (streq(k, "GetChildren")) { lua_pushcfunction(L, m_getchildren); return 1; }
        if (streq(k, "FindFirstChild")) { lua_pushcfunction(L, m_findfirstchild); return 1; }
        if (streq(k, "GetFullName")) { lua_pushcfunction(L, m_getfullname); return 1; }
        int ev = find_event(idx, k);
        if (ev >= 0) { push_signal(L, idx, ev, NULL); return 1; }
        /* other services' methods: do nothing, or fail when they need the internet */
        lua_pushstring(L, k);
        lua_pushcclosure(L, idx == VIRT_HTTP || idx == VIRT_MARKETPLACE || idx == VIRT_TELEPORT ? m_offline : m_noop, 1);
        return 1;
    }
    if (streq(k, "Name")) { lua_pushstring(L, name_of(idx)); return 1; }
    if (streq(k, "ClassName") || streq(k, "className")) { lua_pushstring(L, cls_of(idx)); return 1; }
    if (streq(k, "Parent")) {
        i32 p = W->inst[idx].parent;
        if (p < 0 && is_service(idx) && ub_world_alive(W, idx)) p = VIRT_GAME;
        push_inst(L, p);
        return 1;
    }
    if (streq(k, "Archivable")) { lua_pushboolean(L, 1); return 1; }
    if (is_part(idx) && get_part_prop(L, idx, k)) return 1;
    if (joint_get(L, idx, k)) return 1;
    if (attach_get(L, idx, k)) return 1;
    if (teams_get(L, idx, k)) return 1;
    if (sound_get(L, idx, k)) return 1;
    if (get_class_prop(L, idx, k)) return 1;
    if (gui_get(L, idx, k)) return 1;
    lua_CFunction m = find_method(k_inst_methods, k);
    if (!m) m = gui_method(idx, k);
    if (!m) m = interact_method(idx, k);
    if (!m && is_part(idx)) m = find_method(k_part_methods, k);
    if (!m && is_a(cls_of(idx), "Model")) m = find_method(k_model_methods, k);
    if (!m && idx == S.g->humanoid) m = find_method(k_humanoid_methods, k);
    if (!m && idx == S.g->player) m = find_method(k_player_methods, k);
    if (!m && idx == S.g->players) m = find_method(k_players_methods, k);
    if (!m && streq(cls_of(idx), "Lighting")) m = find_method(k_lighting_methods, k);
    if (!m && idx == W->workspace) m = find_method(k_workspace_methods, k);
    if (m == m_ffancestorofclass) {}
    if (!m && (streq(k, "FindFirstAncestorOfClass") || streq(k, "FindFirstAncestorWhichIsA"))) {
        lua_pushboolean(L, streq(k, "FindFirstAncestorWhichIsA"));
        lua_pushcclosure(L, m_ffancestorofclass, 1);
        return 1;
    }
    if (m == m_waitforchild) { /* WaitForChild polls in Lua, so it can yield */
        lua_getfield(L, LUA_REGISTRYINDEX, "ub.WaitForChild");
        return 1;
    }
    if (m) {
        lua_pushcfunction(L, m);
        return 1;
    }
    int ev = find_event(idx, k);
    if (ev >= 0) {
        push_signal(L, idx, ev, NULL);
        return 1;
    }
    get_extra(L, idx, k);
    if (!lua_isnil(L, -1)) return 1;
    lua_pop(L, 1);
    i32 c = find_child(idx, k, false);
    if (c >= 0) {
        push_inst(L, c);
        return 1;
    }
    return sapi_error(L, "%s is not a valid member of %s \"%s\"", k, cls_of(idx), name_of(idx));
}

/* ----------------------------------------------------------- __newindex */

static f32 numf(lua_State *L, int i) { return (f32)luaL_checknumber(L, i); }

static bool set_part_prop(lua_State *L, i32 idx, const char *k, int v)
{
    ub_part *p = part_of(idx);
    u32 what = UB_CHANGE_LOOK;
    if (streq(k, "Position")) {
        vec3 pos;
        mat3 r;
        part_get_cframe(p, &pos, &r);
        part_set_cframe(p, check_v3(L, v), &r);
        what = UB_CHANGE_PHYS;
    } else if (streq(k, "CFrame")) {
        scf *c = check_cf(L, v);
        part_set_cframe(p, c->p, &c->r);
        what = UB_CHANGE_PHYS;
    } else if (streq(k, "Size")) {
        part_set_size(p, check_v3(L, v));
        what = UB_CHANGE_PHYS | UB_CHANGE_LOOK;
    } else if (streq(k, "Orientation") || streq(k, "Rotation")) {
        vec3 pos;
        mat3 r;
        part_get_cframe(p, &pos, &r);
        r = ub_rbx_from_orientation(check_v3(L, v));
        part_set_cframe(p, pos, &r);
        what = UB_CHANGE_PHYS;
    } else if (streq(k, "Color")) {
        p->color = check_c3_rgb(L, v);
    } else if (streq(k, "BrickColor")) {
        p->color = brickcolor_rgb(L, v);
    } else if (streq(k, "Transparency")) {
        f32 t = numf(L, v);
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        p->transparency = (u8)(t * 255.0f + 0.5f);
    } else if (streq(k, "Anchored")) {
        if (lua_toboolean(L, v)) p->flags |= UB_PART_ANCHORED;
        else p->flags &= (u8)~UB_PART_ANCHORED;
        what = UB_CHANGE_PHYS;
    } else if (streq(k, "CanCollide")) {
        if (lua_toboolean(L, v)) p->flags |= UB_PART_CAN_COLLIDE;
        else p->flags &= (u8)~UB_PART_CAN_COLLIDE;
        what = UB_CHANGE_PHYS;
    } else if (streq(k, "Material")) {
        int m = check_enum(L, v, EN_MATERIAL);
        p->material = material_to_ub(m);
        if (m == 288) p->flags |= UB_PART_GLOW;
        else p->flags &= (u8)~UB_PART_GLOW;
        what = UB_CHANGE_LOOK | UB_CHANGE_PHYS;
    } else if (streq(k, "Shape") && streq(cls_of(idx), "Part")) {
        int s = check_enum(L, v, EN_PARTTYPE);
        vec3 pos, size = part_get_size(p);
        mat3 r;
        part_get_cframe(p, &pos, &r);
        p->shape = (u8)(s == 0 ? UB_SHAPE_BALL : s == 2 ? UB_SHAPE_CYLINDER : s == 3 ? UB_SHAPE_WEDGE
                        : s == 4 ? UB_SHAPE_CORNER_WEDGE : UB_SHAPE_BOX);
        part_set_size(p, size);
        part_set_cframe(p, pos, &r);
        what = UB_CHANGE_LOOK | UB_CHANGE_PHYS;
    } else if (streq(k, "Velocity") || streq(k, "AssemblyLinearVelocity") || streq(k, "RotVelocity") ||
               streq(k, "AssemblyAngularVelocity")) {
        vec3 val = check_v3(L, v);
        bool ang = k[0] == 'R' || streq(k, "AssemblyAngularVelocity");
        if (idx == S.g->char_root) { /* launching the character */
            if (!ang) ub_game_set_velocity(S.g, rbx_vec(val));
            return true;
        }
        i32 b = S.g->phys_ok ? ub_phys_part_body(&S.g->phys, W->inst[idx].part) : -1;
        if (b > 0 && S.g->phys.bodies[b].type == UB_BODY_DYNAMIC) {
            ub_phys_wake(&S.g->phys, b);
            if (ang) S.g->phys.bodies[b].w = v3(-val.x, -val.y, val.z);
            else S.g->phys.bodies[b].v = rbx_vec(val);
        }
        return true;
    } else {
        return false;
    }
    ub_game_part_changed(S.g, W->inst[idx].part, what);
    return true;
}

static bool set_class_prop(lua_State *L, i32 idx, const char *k, int v)
{
    ub_game *g = S.g;
    const char *cls = cls_of(idx);
    if (idx == g->humanoid) {
        if (streq(k, "Health")) {
            ub_game_set_health(g, numf(L, v));
            lua_pushnumber(L, g->health);
            fire_signal(L, idx, EV_HEALTHCHANGED, NULL, 1);
        } else if (streq(k, "MaxHealth")) {
            g->max_health = numf(L, v) > 0 ? numf(L, v) : 1;
            if (g->health > g->max_health) g->health = g->max_health;
        } else if (streq(k, "WalkSpeed")) ub_game_set_walk_speed(g, numf(L, v));
        else if (streq(k, "JumpHeight")) ub_game_set_jump_height(g, numf(L, v));
        else if (streq(k, "JumpPower")) {
            f32 jp = numf(L, v);
            ub_game_set_jump_height(g, jp * jp / (2.0f * g_grav()));
        } else return false;
        return true;
    }
    if (idx == W->workspace && streq(k, "Gravity")) {
        ub_game_set_gravity(g, numf(L, v));
        return true;
    }
    if (idx == W->workspace && streq(k, "FallenPartsDestroyHeight")) {
        W->fallen_height = numf(L, v);
        if (g->phys_ok) g->phys.fallen_height = W->fallen_height;
        return true;
    }
    if (streq(cls, "Lighting")) {
        ub_lighting *l = &W->lighting;
        if (streq(k, "ClockTime")) {
            f64 t = fmod(luaL_checknumber(L, v), 24.0);
            l->clock_time = (f32)(t < 0 ? t + 24 : t);
        } else if (streq(k, "TimeOfDay")) {
            int part[3] = { 0, 0, 0 }, n = 0;
            for (const char *c = luaL_checkstring(L, v); *c && n < 3; ++c) {
                if (*c == ':') ++n;
                else if (*c >= '0' && *c <= '9') part[n] = part[n] * 10 + (*c - '0');
            }
            l->clock_time = (f32)fmod(part[0] + part[1] / 60.0 + part[2] / 3600.0, 24.0);
        } else if (streq(k, "Brightness")) l->brightness = numf(L, v);
        else if (streq(k, "FogEnd")) l->fog_end = numf(L, v) >= 100000 ? 0 : numf(L, v);
        else if (streq(k, "GeographicLatitude")) l->latitude = numf(L, v);
        else if (streq(k, "Ambient")) l->ambient = check_c3_rgb(L, v);
        else if (streq(k, "OutdoorAmbient")) l->outdoor_ambient = check_c3_rgb(L, v);
        else if (streq(k, "FogColor")) l->fog_color = check_c3_rgb(L, v);
        else return false;
        return true;
    }
    if (is_a(cls, "BaseScript") && (streq(k, "Disabled") || streq(k, "Enabled"))) {
        bool off = lua_toboolean(L, v) == (k[0] == 'D');
        if (off) {
            W->inst[idx].flags |= UB_INST_DISABLED;
            stop_script(idx, NULL);
        } else {
            W->inst[idx].flags &= (u16)~UB_INST_DISABLED;
            start_scripts_in(idx);
        }
        return true;
    }
    if (is_value_class(cls) && streq(k, "Value")) {
        if (ub_world_prop_named(W, idx, "Value") && gui_set(L, idx, k, v)) return true;
        if (streq(cls, "IntValue")) {
            f64 n = luaL_checknumber(L, v);
            lua_pushnumber(L, n < 0 ? ceil(n - 0.5) : floor(n + 0.5));
            set_extra(L, idx, "Value", -1);
            lua_pop(L, 1);
        } else if (streq(cls, "NumberValue")) {
            lua_pushnumber(L, luaL_checknumber(L, v));
            set_extra(L, idx, "Value", -1);
            lua_pop(L, 1);
        } else if (streq(cls, "StringValue")) {
            luaL_checkstring(L, v);
            lua_pushstring(L, lua_tostring(L, v));
            set_extra(L, idx, "Value", -1);
            lua_pop(L, 1);
        } else if (streq(cls, "BoolValue")) {
            lua_pushboolean(L, lua_toboolean(L, v));
            set_extra(L, idx, "Value", -1);
            lua_pop(L, 1);
        } else {
            set_extra(L, idx, "Value", v);
        }
        return true;
    }
    if (is_a(cls, "Model") && streq(k, "PrimaryPart")) {
        if (!lua_isnil(L, v)) check_inst(L, v);
        set_extra(L, idx, "PrimaryPart", v);
        return true;
    }
    if (idx == g->player && streq(k, "Character")) return true; /* stays the player's own */
    if (idx == g->player && streq(k, "RespawnLocation")) {
        i32 p = opt_inst(L, v);
        if (p >= 0 && !is_part(p)) return sapi_error(L, "RespawnLocation must be a part");
        ub_game_set_respawn_location(g, p);
        return true;
    }
    return false;
}

static int set_parent(lua_State *L, i32 idx, int v)
{
    i32 np = opt_inst(L, v);
    if (np == VIRT_GAME) np = -1;
    if (np < -1) return sapi_error(L, "cannot put an instance in %s", virt_class(np));
    if (is_service(idx) || idx == S.g->char_model || idx == S.g->char_root || idx == S.g->humanoid ||
        idx == S.g->player)
        return sapi_error(L, "The Parent property of %s is locked", name_of(idx));
    if (!ub_world_alive(W, idx)) return sapi_error(L, "The Parent property of %s is locked, current parent: NULL", name_of(idx));
    i32 old = W->inst[idx].parent;
    if (old == np) return 0;
    bool was_in = sapi_in_game(idx);
    if (!ub_world_set_parent(W, idx, np)) return sapi_error(L, "Attempt to set parent of %s to %s would result in circular reference", name_of(idx), np >= 0 ? name_of(np) : "nil");
    ub_game_subtree_changed(S.g, idx);
    if (old >= 0) {
        push_inst(L, idx);
        fire_signal(L, old, EV_CHILDREMOVED, NULL, 1);
    }
    if (np >= 0) {
        push_inst(L, idx);
        fire_signal(L, np, EV_CHILDADDED, NULL, 1);
        for (i32 a = np; a >= 0; a = W->inst[a].parent) {
            push_inst(L, idx);
            fire_signal(L, a, EV_DESCENDANTADDED, NULL, 1);
        }
        start_scripts_in(idx);
    }
    tags_moved(L, idx, was_in, sapi_in_game(idx));
    joints_moved(L, idx);
    fire_signal(L, idx, EV_ANCESTRYCHANGED, NULL, 0);
    return 0;
}

int set_prop(lua_State *L, i32 idx, const char *k, int v)
{
    if (idx < 0) {
        if (gui_virtual_newindex(L, idx, k, v)) return 0;
        return sapi_error(L, "%s cannot be assigned to", k);
    }
    if (streq(k, "Parent")) {
        set_parent(L, idx, v);
    } else if (streq(k, "Name")) {
        const char *n = luaL_checkstring(L, v);
        ub_world_set_name(W, idx, n);
    } else if (streq(k, "ClassName")) {
        return sapi_error(L, "ClassName cannot be assigned to");
    } else if (is_part(idx) && set_part_prop(L, idx, k, v)) {
    } else if (joint_set(L, idx, k, v)) {
    } else if (attach_set(L, idx, k, v)) {
    } else if (teams_set(L, idx, k, v)) {
    } else if (sound_set(L, idx, k, v)) {
    } else if (set_class_prop(L, idx, k, v)) {
    } else if (gui_set(L, idx, k, v)) {
        if (ub_constraint_class(cls_of(idx))) S.g->constraints_dirty = true;
    } else {
        set_extra(L, idx, k, v); /* kept, so scripts for things Uniblox lacks still run */
    }
    prop_changed(L, idx, k);
    return 0;
}

static int inst_newindex(lua_State *L)
{
    i32 idx = check_inst(L, 1);
    return set_prop(L, idx, luaL_checkstring(L, 2), 3);
}

static int inst_tostring(lua_State *L)
{
    i32 idx = check_inst(L, 1);
    lua_pushstring(L, idx < 0 ? (idx == VIRT_GAME ? "Game" : virt_class(idx)) : name_of(idx));
    return 1;
}

/* --------------------------------------------------------- Instance.new */

static int instance_new(lua_State *L)
{
    const char *cls = luaL_checkstring(L, 1);
    i32 parent = opt_inst(L, 2);
    check_room(L, 1);
    static const struct {
        const char *cls;
        u8 shape;
        f32 sx, sy, sz;
    } parts[] = {
        { "Part", UB_SHAPE_BOX, 4, 1, 2 }, { "WedgePart", UB_SHAPE_WEDGE, 4, 1, 2 },
        { "CornerWedgePart", UB_SHAPE_CORNER_WEDGE, 2, 2, 2 }, { "TrussPart", UB_SHAPE_BOX, 2, 2, 2 },
        { "SpawnLocation", UB_SHAPE_BOX, 12, 1, 12 }, { "Seat", UB_SHAPE_BOX, 2, 1, 2 },
        { "VehicleSeat", UB_SHAPE_BOX, 4, 1, 2 }, { "MeshPart", UB_SHAPE_BOX, 2, 2, 2 },
        { "UnionOperation", UB_SHAPE_BOX, 2, 2, 2 },
    };
    i32 idx = -1;
    for (int i = 0; i < (int)(sizeof parts / sizeof parts[0]); ++i)
        if (streq(parts[i].cls, cls)) {
            ub_part p;
            memset(&p, 0, sizeof p);
            p.rot = mat3_identity();
            p.size = v3(parts[i].sx, parts[i].sy, parts[i].sz);
            p.shape = parts[i].shape;
            p.color = 0xA3A2A5;
            p.material = UB_MAT_PLASTIC;
            p.flags = UB_PART_CAN_COLLIDE;
            idx = ub_world_add_part(W, -1, streq(cls, "SpawnLocation") ? UB_CLASS_SPAWN : UB_CLASS_PART, cls, cls, &p);
            break;
        }
    if (idx < 0) idx = gui_instance_new(cls); /* GUI objects, values, remotes: with Roblox's defaults */
    if (idx < 0) {
        ub_class c = streq(cls, "Model") ? UB_CLASS_MODEL : streq(cls, "Folder") ? UB_CLASS_FOLDER : UB_CLASS_OTHER;
        if (streq(cls, "Workspace") || streq(cls, "DataModel") || streq(cls, "Players") || streq(cls, "Lighting"))
            return sapi_error(L, "Unable to create an Instance of type \"%s\"", cls);
        idx = ub_world_add(W, -1, c, cls, cls);
        if (idx >= 0 && (streq(cls, "Script") || streq(cls, "LocalScript") || streq(cls, "ModuleScript")))
            ub_world_set_source(W, idx, "");
    }
    if (idx < 0) return luaL_error(L, "out of memory");
    W->inst[idx].flags |= UB_INST_DETACHED;
    push_inst(L, idx);
    if (parent >= 0 || parent == VIRT_GAME) set_parent(L, idx, 2);
    return 1;
}

/* --------------------------------------------------------------- tweens */

#define MAX_TWEENS 512
#define TWEEN_PROPS 8

typedef struct {
    bool used;
    i32 inst;
    stweeninfo info;
    int state;             /* Enum.PlaybackState */
    f64 start;
    int nprops;
    struct {
        char name[32];
        int kind;          /* 0 number, 1 Vector3, 2 Color3, 3 CFrame */
        f32 from[12], to[12];
    } p[TWEEN_PROPS];
    f32 to_cached[TWEEN_PROPS][12];
} stween;

static stween g_tweens[MAX_TWEENS];

static int value_kind(lua_State *L, int i, f32 *out)
{
    if (i < 0) i = lua_gettop(L) + i + 1; /* it pushes below */
    if (lua_type(L, i) == LUA_TNUMBER) {
        out[0] = (f32)lua_tonumber(L, i);
        return 0;
    }
    if (is_v3(L, i)) {
        vec3 v = check_v3(L, i);
        out[0] = v.x;
        out[1] = v.y;
        out[2] = v.z;
        return 1;
    }
    if (is_c3(L, i)) {
        u32 c = check_c3_rgb(L, i);
        lua_getfield(L, i, "R");
        lua_getfield(L, i, "G");
        lua_getfield(L, i, "B");
        out[0] = (f32)lua_tonumber(L, -3);
        out[1] = (f32)lua_tonumber(L, -2);
        out[2] = (f32)lua_tonumber(L, -1);
        lua_pop(L, 3);
        (void)c;
        return 2;
    }
    if (is_udim2(L, i)) {
        get_udim2(L, i, out);
        return 4;
    }
    if (is_vector2(L, i)) {
        get_vector2(L, i, out);
        return 5;
    }
    if (is_udim(L, i)) {
        get_udim(L, i, out);
        return 6;
    }
    if (is_cf(L, i)) {
        scf *c = check_cf(L, i);
        memcpy(out, &c->p, sizeof(vec3));
        quat q = quat_from_mat3(&c->r);
        out[3] = q.x;
        out[4] = q.y;
        out[5] = q.z;
        out[6] = q.w;
        return 3;
    }
    return -1;
}

static void push_value(lua_State *L, int kind, const f32 *v)
{
    if (kind == 0) lua_pushnumber(L, v[0]);
    else if (kind == 1) push_v3(L, v3(v[0], v[1], v[2]));
    else if (kind == 2) push_c3(L, v[0], v[1], v[2]);
    else if (kind == 4) push_udim2(L, v[0], v[1], v[2], v[3]);
    else if (kind == 5) push_vector2(L, v[0], v[1]);
    else if (kind == 6) push_udim(L, v[0], v[1]);
    else {
        quat q = { v[3], v[4], v[5], v[6] };
        mat3 r = quat_to_mat3(quat_normalize(q));
        push_cf(L, v3(v[0], v[1], v[2]), &r);
    }
}

typedef struct {
    i32 id;
} stweenud;

static int tween_play(lua_State *L)
{
    stween *t = &g_tweens[((stweenud *)luaL_checkudata(L, 1, "Tween"))->id];
    if (!t->used || t->inst < 0 || !ub_world_alive(W, t->inst)) return 0;
    /* starting values are read when it starts */
    push_inst(L, t->inst);
    for (int i = 0; i < t->nprops; ++i) {
        lua_getfield(L, -1, t->p[i].name);
        value_kind(L, -1, t->p[i].from);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    t->start = script_time();
    t->state = t->info.delay > 0 ? 1 : 2;
    return 0;
}

static int tween_cancel(lua_State *L)
{
    i32 id = ((stweenud *)luaL_checkudata(L, 1, "Tween"))->id;
    stween *t = &g_tweens[id];
    if (t->state == 2 || t->state == 1) {
        t->state = 5;
        push_enum(L, EN_PLAYBACKSTATE, 5);
        fire_signal(L, -1000 - id, EV_COMPLETED, NULL, 1);
    }
    return 0;
}

static int tween_pause(lua_State *L)
{
    stween *t = &g_tweens[((stweenud *)luaL_checkudata(L, 1, "Tween"))->id];
    if (t->state == 2) t->state = 3;
    return 0;
}

static int tween_index(lua_State *L)
{
    i32 id = ((stweenud *)luaL_checkudata(L, 1, "Tween"))->id;
    const char *k = luaL_checkstring(L, 2);
    if (streq(k, "Play")) lua_pushcfunction(L, tween_play);
    else if (streq(k, "Cancel")) lua_pushcfunction(L, tween_cancel);
    else if (streq(k, "Pause")) lua_pushcfunction(L, tween_pause);
    else if (streq(k, "Destroy")) lua_pushcfunction(L, m_noop);
    else if (streq(k, "Completed")) push_signal(L, -1000 - id, EV_COMPLETED, NULL);
    else if (streq(k, "PlaybackState")) push_enum(L, EN_PLAYBACKSTATE, g_tweens[id].state);
    else if (streq(k, "Instance")) push_inst(L, g_tweens[id].inst);
    else if (streq(k, "ClassName")) lua_pushliteral(L, "Tween");
    else return sapi_error(L, "%s is not a valid member of Tween", k);
    return 1;
}

static int tween_create(lua_State *L)
{
    /* TweenService:Create(instance, tweenInfo, {Property = goal, ...}) */
    i32 inst = check_inst(L, 2);
    stweeninfo *info = check_tweeninfo(L, 3);
    luaL_checktype(L, 4, LUA_TTABLE);
    int id = -1;
    for (int i = 0; i < MAX_TWEENS; ++i)
        if (!g_tweens[i].used || g_tweens[i].state == 4 || g_tweens[i].state == 5 || g_tweens[i].state == 0) {
            if (!g_tweens[i].used || g_tweens[i].state != 0) {
                id = i;
                break;
            }
        }
    if (id < 0) return luaL_error(L, "too many tweens at once");
    stween *t = &g_tweens[id];
    memset(t, 0, sizeof *t);
    t->used = true;
    t->inst = inst;
    t->info = *info;
    t->state = 0;
    lua_pushnil(L);
    while (lua_next(L, 4) && t->nprops < TWEEN_PROPS) {
        if (lua_type(L, -2) == LUA_TSTRING) {
            int kind = value_kind(L, -1, t->p[t->nprops].to);
            if (kind < 0) return luaL_error(L, "TweenService: %s cannot be tweened", lua_tostring(L, -2));
            snprintf(t->p[t->nprops].name, sizeof t->p[t->nprops].name, "%s", lua_tostring(L, -2));
            t->p[t->nprops].kind = kind;
            t->nprops++;
        }
        lua_pop(L, 1);
    }
    stweenud *u = (stweenud *)lua_newuserdata(L, sizeof(stweenud));
    u->id = id;
    luaL_getmetatable(L, "Tween");
    lua_setmetatable(L, -2);
    return 1;
}

static int tween_getvalue(lua_State *L)
{
    lua_pushnumber(L, ease(check_enum(L, 3, EN_EASINGSTYLE), check_enum(L, 4, EN_EASINGDIRECTION), (f32)luaL_checknumber(L, 2)));
    return 1;
}

void update_tweens(lua_State *L, f64 now)
{
    for (int id = 0; id < MAX_TWEENS; ++id) {
        stween *t = &g_tweens[id];
        if (!t->used || (t->state != 1 && t->state != 2)) continue;
        if (!ub_world_alive(W, t->inst)) {
            t->state = 5;
            continue;
        }
        f64 el = now - t->start - t->info.delay;
        if (el < 0) continue;
        t->state = 2;
        f64 dur = t->info.time > 0 ? t->info.time : 1e-6;
        f64 cycle = t->info.reverses ? dur * 2 : dur;
        int reps = t->info.repeat < 0 ? 1 << 30 : t->info.repeat + 1;
        bool done = el >= cycle * reps;
        f64 c = done ? cycle : fmod(el, cycle);
        f64 x = c / dur;
        if (t->info.reverses && x > 1) x = 2 - x;
        if (done) x = t->info.reverses ? 0 : 1;
        f32 a = ease(t->info.style, t->info.dir, (f32)x);
        push_inst(L, t->inst);
        int inst_i = lua_gettop(L);
        for (int i = 0; i < t->nprops; ++i) {
            f32 v[12];
            static const int floats[7] = { 1, 3, 3, 7, 4, 2, 2 };
            int n = floats[t->p[i].kind >= 0 && t->p[i].kind < 7 ? t->p[i].kind : 0];
            for (int k = 0; k < n; ++k) v[k] = t->p[i].from[k] + (t->p[i].to[k] - t->p[i].from[k]) * a;
            if (t->p[i].kind == 3) { /* rotations: blend the shorter way */
                quat qa = { t->p[i].from[3], t->p[i].from[4], t->p[i].from[5], t->p[i].from[6] };
                quat qb = { t->p[i].to[3], t->p[i].to[4], t->p[i].to[5], t->p[i].to[6] };
                quat q = quat_nlerp(qa, qb, a);
                v[3] = q.x;
                v[4] = q.y;
                v[5] = q.z;
                v[6] = q.w;
            }
            push_value(L, t->p[i].kind, v);
            set_prop(L, ((sinst *)lua_touserdata(L, inst_i))->idx, t->p[i].name, lua_gettop(L));
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        if (done) {
            t->state = 4;
            push_enum(L, EN_PLAYBACKSTATE, 4);
            fire_signal(L, -1000 - id, EV_COMPLETED, NULL, 1);
        }
    }
}

/* --------------------------------------------------------------- Debris */

#define MAX_DEBRIS 4096
static struct {
    i32 inst;
    f64 when;
} g_debris[MAX_DEBRIS];
static int g_debris_n;

static void drop_c_refs(const u8 *marked, i32 n)
{
    for (int id = 0; id < MAX_TWEENS; ++id)
        if (g_tweens[id].used && g_tweens[id].inst >= 0 && g_tweens[id].inst < n && marked[g_tweens[id].inst]) {
            g_tweens[id].inst = -1;
            g_tweens[id].state = 5;
        }
    int keep = 0;
    for (int i = 0; i < g_debris_n; ++i)
        if (!(g_debris[i].inst >= 0 && g_debris[i].inst < n && marked[g_debris[i].inst])) g_debris[keep++] = g_debris[i];
    g_debris_n = keep;
}

static int debris_add(lua_State *L)
{
    i32 inst = check_inst(L, 2);
    f64 t = luaL_optnumber(L, 3, 10);
    if (g_debris_n < MAX_DEBRIS) {
        g_debris[g_debris_n].inst = inst;
        g_debris[g_debris_n].when = script_time() + t;
        g_debris_n++;
    }
    return 0;
}

void update_debris(lua_State *L, f64 now)
{
    int keep = 0;
    for (int i = 0; i < g_debris_n; ++i) {
        if (g_debris[i].when <= now) do_destroy(L, g_debris[i].inst);
        else g_debris[keep++] = g_debris[i];
    }
    g_debris_n = keep;
}

/* ---------------------------------------------------------------- setup */

static const char k_waitforchild[] =
    "local find, wait, time, warn, tostring = ...\n"
    "return function(self, name, timeout)\n"
    "  local c = find(self, name)\n"
    "  local limit = timeout and (time() + timeout)\n"
    "  local warned = false\n"
    "  local start = time()\n"
    "  while not c do\n"
    "    if limit and time() >= limit then return nil end\n"
    "    if not timeout and not warned and time() - start > 5 then\n"
    "      warned = true\n"
    "      warn('Infinite yield possible on \\'' .. self:GetFullName() .. ':WaitForChild(\"' .. tostring(name) .. '\")\\'')\n"
    "    end\n"
    "    wait()\n"
    "    c = find(self, name)\n"
    "  end\n"
    "  return c\n"
    "end\n";

void open_instances(lua_State *L)
{
    memset(g_tweens, 0, sizeof g_tweens);
    g_debris_n = 0;
    free_instances();
    S.base_live = ub_world_live_count(W);
    luaL_newmetatable(L, "Instance");
    lua_pushcfunction(L, inst_index);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, inst_newindex);
    lua_setfield(L, -2, "__newindex");
    lua_pushcfunction(L, inst_tostring);
    lua_setfield(L, -2, "__tostring");
    lua_pushcfunction(L, inst_gc);
    lua_setfield(L, -2, "__gc");
    lua_pushliteral(L, "The metatable is locked");
    lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);
    luaL_newmetatable(L, "Tween");
    lua_pushcfunction(L, tween_index);
    lua_setfield(L, -2, "__index");
    lua_pushliteral(L, "The metatable is locked");
    lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);

    /* service method tables */
    lua_newtable(L);
    lua_pushcfunction(L, tween_create);
    lua_setfield(L, -2, "Create");
    lua_pushcfunction(L, tween_getvalue);
    lua_setfield(L, -2, "GetValue");
    lua_setfield(L, LUA_REGISTRYINDEX, "ub.TweenService");
    lua_newtable(L);
    lua_pushcfunction(L, debris_add);
    lua_setfield(L, -2, "AddItem");
    lua_pushcfunction(L, debris_add);
    lua_setfield(L, -2, "addItem");
    lua_setfield(L, LUA_REGISTRYINDEX, "ub.Debris");

    /* globals */
    lua_pushlightuserdata(L, &REG_BASE);
    lua_rawget(L, LUA_REGISTRYINDEX);
    int base = lua_gettop(L);
    push_inst(L, VIRT_GAME);
    lua_pushvalue(L, -1);
    lua_setfield(L, base, "game");
    lua_setfield(L, base, "Game");
    push_inst(L, ub_world_workspace(W));
    lua_pushvalue(L, -1);
    lua_setfield(L, base, "workspace");
    lua_setfield(L, base, "Workspace");
    lua_newtable(L); /* Instance, read-only */
    lua_newtable(L);
    lua_newtable(L);
    lua_pushcfunction(L, instance_new);
    lua_setfield(L, -2, "new");
    lua_setfield(L, -2, "__index");
    lua_pushliteral(L, "The metatable is locked");
    lua_setfield(L, -2, "__metatable");
    lua_setmetatable(L, -2);
    lua_setfield(L, base, "Instance");

    /* WaitForChild in Lua, so it can wait */
    if (luaL_loadbuffer(L, k_waitforchild, sizeof k_waitforchild - 1, "=WaitForChild") == 0) {
        lua_pushcfunction(L, m_findfirstchild);
        lua_getfield(L, base, "wait");
        lua_getfield(L, base, "time");
        lua_getfield(L, base, "warn");
        lua_getfield(L, base, "tostring");
        lua_call(L, 5, 1);
        lua_setfield(L, LUA_REGISTRYINDEX, "ub.WaitForChild");
    } else {
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}
