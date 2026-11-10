/* Uniblox - OpenGL renderer.
 *
 * Two paths, chosen at start-up from what the driver offers:
 *   shader path (GL 2.0 + framebuffer objects):
 *     - per-pixel lighting: sun (Lambert) + sky/ground hemisphere ambient
 *       + Blinn-Phong specular per material + a soft Fresnel rim
 *     - sun shadows: 2048x2048 shadow map around the camera's focus,
 *       3x3 PCF filtering for soft edges
 *     - per-pixel sky: zenith-to-horizon gradient, sun disc and glow;
 *       distance fog fades into the horizon colour
 *     - 4x multisample anti-aliasing (render to an MSAA buffer, resolve)
 *   fixed-function path (plain GL 1.1): per-vertex lighting with specular,
 *     fog, gradient sky; no shadows
 * Both use mipmapped 256x256 textures with anisotropic filtering when
 * available, and 32-sided cylinders / 16x32 balls with smooth normals.
 *
 * Geometry is rebuilt on the CPU every frame into one vertex array per
 * material (a few thousand vertices for the demo scene), then drawn once
 * for the shadow map and once for the screen. */
#include "render/ub_render_internal.h"
#include "render/ub_gl.h"
#include "core/ub_log.h"
#include "core/ub_string.h"
#include "core/ub_utf.h"
#include "crt/ub_crt.h"

ub_gl_api ubgl;

#define TEX_SIZE 256
#define SHADOW_SIZE 2048
#define SHADOW_HALF 70.0f     /* shadow map covers 140 x 140 units */
#define SHADOW_DEPTH 200.0f
#define NEAR_Z 0.2f
#define FAR_Z 1500.0f

/* ------------------------------------------------------------ loading */

static void *load_proc(const char *name, const char *alt)
{
    void *p = plat_gl_proc(name);
    if (!p && alt) p = plat_gl_proc(alt);
    return p;
}

bool ub_gl_load(void)
{
    memset(&ubgl, 0, sizeof ubgl);
    bool ok = true;
#define LOAD(ret, name, args, alt) \
    ubgl.name = (ret (UBGL_API *) args)load_proc(#name, alt); \
    if (!ubgl.name) ok = false;
    UBGL_CORE(LOAD)
    ubgl.has_core = ok;
    ok = true;
    UBGL_SHADER(LOAD)
    ubgl.has_shaders = ok;
    ok = true;
    UBGL_FBO(LOAD)
    ubgl.has_fbo = ok;
    ok = true;
    UBGL_MSAA(LOAD)
    ubgl.has_msaa = ok && ubgl.has_fbo;
    ok = true;
    UBGL_VBO(LOAD)
    ubgl.has_vbo = ok;
#undef LOAD
    return ubgl.has_core;
}

/* -------------------------------------------------------------- types */

typedef struct {
    f32 m[16]; /* column-major, as OpenGL expects */
} mat4;

typedef struct {
    f32 x, y, z;
    f32 nx, ny, nz;
    f32 u, v;
    u8 r, g, b, a;
} gvert;

typedef struct {
    gvert *v;
    int count, cap;
} batch;

static struct {
    bool ready;
    bool shaders;              /* shader path active */
    bool shadows_ok, msaa_ok;  /* supported by the driver */
    ub_quality quality;
    f32 max_aniso;
    int max_samples;

    GLuint tex[UB_MAT_COUNT];  /* [UB_MAT_PLASTIC] is plain white */
    GLuint font_tex;
    ub_font_atlas font;
    int font_px;

    GLuint prog_lit, prog_sky;
    struct {
        GLint viewproj, shadowmat, tex, shadow, sun_dir, sun_col, sky_col, ground_col, cam,
            fog_col, fog_density, spec, shine, texmix, shadow_on, shadow_texel;
    } lit;
    struct {
        GLint right, up, fwd, tan, sun_dir, zenith, horizon, ground, sun_col;
    } sky;

    GLuint shadow_fbo, shadow_tex;
    int shadow_size;
    GLuint msaa_fbo, msaa_color, msaa_depth;
    int msaa_w, msaa_h, msaa_samples;

    batch batches[UB_MAT_COUNT];          /* dynamic parts, rebuilt every frame */
    batch statics[UB_MAT_COUNT];          /* static parts (CPU copy when there are no VBOs) */
    GLuint static_vbo[UB_MAT_COUNT];
    int static_verts[UB_MAT_COUNT];
    ub_part_draw *see_statics;            /* see-through static parts: drawn with the dynamic ones */
    int see_static_count;
    GLubyte stipple[16][128];             /* screen-door patterns per see-through level */
    int width, height;
} G;

/* ------------------------------------------------------------ helpers */

static void color3(u32 c, f32 *out)
{
    out[0] = (f32)((c >> 16) & 255) / 255.0f;
    out[1] = (f32)((c >> 8) & 255) / 255.0f;
    out[2] = (f32)(c & 255) / 255.0f;
}

static mat4 mat4_mul(const mat4 *a, const mat4 *b)
{
    mat4 r;
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            f32 s = 0;
            for (int k = 0; k < 4; ++k) s += a->m[k * 4 + row] * b->m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}

/* world -> eye space for a viewer at `eye` with basis right/up/forward
 * (OpenGL eye space looks down -Z, so forward is negated) */
static mat4 mat4_view(vec3 eye, vec3 r, vec3 u, vec3 f)
{
    mat4 m;
    memset(&m, 0, sizeof m);
    m.m[0] = r.x; m.m[4] = r.y; m.m[8] = r.z;  m.m[12] = -v3_dot(r, eye);
    m.m[1] = u.x; m.m[5] = u.y; m.m[9] = u.z;  m.m[13] = -v3_dot(u, eye);
    m.m[2] = -f.x; m.m[6] = -f.y; m.m[10] = -f.z; m.m[14] = v3_dot(f, eye);
    m.m[15] = 1;
    return m;
}

static mat4 mat4_perspective(f32 fov_y, f32 aspect, f32 n, f32 f)
{
    mat4 m;
    memset(&m, 0, sizeof m);
    f32 t = ub_cosf(fov_y * 0.5f) / ub_sinf(fov_y * 0.5f);
    m.m[0] = t / aspect;
    m.m[5] = t;
    m.m[10] = (f + n) / (n - f);
    m.m[11] = -1;
    m.m[14] = 2 * f * n / (n - f);
    return m;
}

static mat4 mat4_ortho(f32 l, f32 r, f32 b, f32 t, f32 n, f32 f)
{
    mat4 m;
    memset(&m, 0, sizeof m);
    m.m[0] = 2 / (r - l);
    m.m[5] = 2 / (t - b);
    m.m[10] = -2 / (f - n);
    m.m[12] = -(r + l) / (r - l);
    m.m[13] = -(t + b) / (t - b);
    m.m[14] = -(f + n) / (f - n);
    m.m[15] = 1;
    return m;
}

static bool has_ext(const char *name)
{
    const char *ext = (const char *)ubgl.glGetString(GL_EXTENSIONS);
    if (!ext) return false;
    size_t n = ub_strlen(name);
    for (const char *p = ext; *p;) {
        const char *e = p;
        while (*e && *e != ' ') ++e;
        if ((size_t)(e - p) == n && memcmp(p, name, n) == 0) return true;
        p = *e ? e + 1 : e;
    }
    return false;
}

/* --------------------------------------------------------------- shaders */

static const char *k_lit_vs =
    "#version 110\n"
    "uniform mat4 u_viewproj;\n"
    "uniform mat4 u_shadowmat;\n"
    "varying vec3 v_pos;\n"
    "varying vec3 v_nrm;\n"
    "varying vec2 v_uv;\n"
    "varying vec4 v_col;\n"
    "varying vec4 v_shadow;\n"
    "void main() {\n"
    "  v_pos = gl_Vertex.xyz;\n"
    "  v_nrm = gl_Normal;\n"
    "  v_uv = gl_MultiTexCoord0.xy;\n"
    "  v_col = gl_Color;\n"
    /* normal offset: look up the shadow a little off the surface (no acne) */
    "  v_shadow = u_shadowmat * vec4(gl_Vertex.xyz + gl_Normal * 0.06, 1.0);\n"
    "  gl_Position = u_viewproj * gl_Vertex;\n"
    "}\n";

static const char *k_lit_fs =
    "#version 110\n"
    "uniform sampler2D u_tex;\n"
    "uniform sampler2D u_shadow;\n"
    "uniform vec3 u_sun_dir, u_sun_col, u_sky_col, u_ground_col, u_cam, u_fog_col;\n"
    "uniform float u_fog_density, u_spec, u_shine, u_texmix, u_shadow_on, u_shadow_texel;\n"
    "varying vec3 v_pos;\n"
    "varying vec3 v_nrm;\n"
    "varying vec2 v_uv;\n"
    "varying vec4 v_col;\n"
    "varying vec4 v_shadow;\n"
    "float shadow_at(vec4 sc) {\n"
    "  vec3 p = sc.xyz / sc.w;\n"
    "  if (p.x <= 0.0 || p.x >= 1.0 || p.y <= 0.0 || p.y >= 1.0 || p.z >= 1.0) return 1.0;\n"
    "  float lit = 0.0;\n"
    "  for (int i = -1; i <= 1; i++)\n"
    "    for (int j = -1; j <= 1; j++) {\n"
    "      float d = texture2D(u_shadow, p.xy + vec2(float(i), float(j)) * u_shadow_texel * 1.25).r;\n"
    "      lit += (p.z - 0.0008 <= d) ? 1.0 : 0.0;\n"
    "    }\n"
    "  return lit / 9.0;\n"
    "}\n"
    "void main() {\n"
    "  vec3 n = normalize(v_nrm);\n"
    "  float t = texture2D(u_tex, v_uv).r;\n"
    "  vec3 albedo = v_col.rgb * mix(1.0, t * 1.15, u_texmix);\n"
    "  float ndl = max(dot(n, u_sun_dir), 0.0);\n"
    "  float sh = 1.0;\n"
    "  if (u_shadow_on > 0.5 && ndl > 0.0) sh = shadow_at(v_shadow);\n"
    "  vec3 amb = mix(u_ground_col, u_sky_col, n.y * 0.5 + 0.5);\n"
    "  vec3 v = normalize(u_cam - v_pos);\n"
    "  vec3 h = normalize(u_sun_dir + v);\n"
    "  float spec = u_spec * pow(max(dot(n, h), 0.0), u_shine) * ndl * sh;\n"
    "  float rim = pow(1.0 - max(dot(n, v), 0.0), 4.0) * 0.25 * u_spec;\n"
    "  vec3 col = albedo * (amb + u_sun_col * ndl * sh) + u_sun_col * spec + u_sky_col * rim;\n"
    "  if (v_col.a < 0.5) col = albedo * 1.25;\n" /* alpha 0 marks glowing (neon) parts */
    "  float dist = length(u_cam - v_pos);\n"
    "  float fog = 1.0 - exp(-u_fog_density * dist);\n"
    "  col = mix(col, u_fog_col, clamp(fog, 0.0, 1.0));\n"
    "  gl_FragColor = vec4(col, 1.0);\n"
    "}\n";

static const char *k_sky_vs =
    "#version 110\n"
    "uniform vec3 u_right, u_up, u_fwd;\n"
    "uniform vec2 u_tan;\n"
    "varying vec3 v_dir;\n"
    "void main() {\n"
    "  v_dir = u_fwd + u_right * gl_Vertex.x * u_tan.x + u_up * gl_Vertex.y * u_tan.y;\n"
    "  gl_Position = vec4(gl_Vertex.xy, 0.0, 1.0);\n"
    "}\n";

static const char *k_sky_fs =
    "#version 110\n"
    "uniform vec3 u_sun_dir, u_zenith, u_horizon, u_ground, u_sun_col;\n"
    "varying vec3 v_dir;\n"
    "void main() {\n"
    "  vec3 d = normalize(v_dir);\n"
    "  vec3 col;\n"
    "  if (d.y >= 0.0) col = mix(u_horizon, u_zenith, pow(d.y, 0.55));\n"
    "  else col = mix(u_horizon, u_ground, min(1.0, -d.y * 5.0));\n"
    "  float s = max(dot(d, u_sun_dir), 0.0);\n"
    "  col += u_sun_col * (pow(s, 900.0) * 3.0 + pow(s, 24.0) * 0.18 + pow(s, 4.0) * 0.05);\n"
    "  gl_FragColor = vec4(col, 1.0);\n"
    "}\n";

static GLuint compile(GLenum type, const char *src, const char *what)
{
    GLuint sh = ubgl.glCreateShader(type);
    ubgl.glShaderSource(sh, 1, &src, NULL);
    ubgl.glCompileShader(sh);
    GLint ok = 0;
    ubgl.glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        log[0] = 0;
        ubgl.glGetShaderInfoLog(sh, sizeof log, NULL, log);
        ub_error("gl: %s shader failed to compile: %s", what, log);
        ubgl.glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint link(const char *vs_src, const char *fs_src, const char *what)
{
    GLuint vs = compile(GL_VERTEX_SHADER, vs_src, what);
    GLuint fs = compile(GL_FRAGMENT_SHADER, fs_src, what);
    if (!vs || !fs) return 0;
    GLuint p = ubgl.glCreateProgram();
    ubgl.glAttachShader(p, vs);
    ubgl.glAttachShader(p, fs);
    ubgl.glLinkProgram(p);
    ubgl.glDeleteShader(vs);
    ubgl.glDeleteShader(fs);
    GLint ok = 0;
    ubgl.glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        log[0] = 0;
        ubgl.glGetProgramInfoLog(p, sizeof log, NULL, log);
        ub_error("gl: %s program failed to link: %s", what, log);
        ubgl.glDeleteProgram(p);
        return 0;
    }
    return p;
}

static bool init_shaders(void)
{
    G.prog_lit = link(k_lit_vs, k_lit_fs, "lit");
    G.prog_sky = link(k_sky_vs, k_sky_fs, "sky");
    if (!G.prog_lit || !G.prog_sky) return false;
#define U(prog, field, name) G.field = ubgl.glGetUniformLocation(G.prog, name)
    U(prog_lit, lit.viewproj, "u_viewproj");
    U(prog_lit, lit.shadowmat, "u_shadowmat");
    U(prog_lit, lit.tex, "u_tex");
    U(prog_lit, lit.shadow, "u_shadow");
    U(prog_lit, lit.sun_dir, "u_sun_dir");
    U(prog_lit, lit.sun_col, "u_sun_col");
    U(prog_lit, lit.sky_col, "u_sky_col");
    U(prog_lit, lit.ground_col, "u_ground_col");
    U(prog_lit, lit.cam, "u_cam");
    U(prog_lit, lit.fog_col, "u_fog_col");
    U(prog_lit, lit.fog_density, "u_fog_density");
    U(prog_lit, lit.spec, "u_spec");
    U(prog_lit, lit.shine, "u_shine");
    U(prog_lit, lit.texmix, "u_texmix");
    U(prog_lit, lit.shadow_on, "u_shadow_on");
    U(prog_lit, lit.shadow_texel, "u_shadow_texel");
    U(prog_sky, sky.right, "u_right");
    U(prog_sky, sky.up, "u_up");
    U(prog_sky, sky.fwd, "u_fwd");
    U(prog_sky, sky.tan, "u_tan");
    U(prog_sky, sky.sun_dir, "u_sun_dir");
    U(prog_sky, sky.zenith, "u_zenith");
    U(prog_sky, sky.horizon, "u_horizon");
    U(prog_sky, sky.ground, "u_ground");
    U(prog_sky, sky.sun_col, "u_sun_col");
#undef U
    return true;
}

/* ------------------------------------------------------------- textures */

/* Upload a square 8-bit texture with a full box-filtered mip chain. */
static void upload_mipmapped(GLenum format, const u8 *src, int size)
{
    u8 *a = (u8 *)plat_alloc((size_t)size * size);
    if (!a) return;
    memcpy(a, src, (size_t)size * size);
    int level = 0;
    for (int s = size;; s /= 2, ++level) {
        ubgl.glTexImage2D(GL_TEXTURE_2D, level, (GLint)format, s, s, 0, format, GL_UNSIGNED_BYTE, a);
        if (s == 1) break;
        int h = s / 2;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < h; ++x) {
                int sum = a[(2 * y) * s + 2 * x] + a[(2 * y) * s + 2 * x + 1] +
                          a[(2 * y + 1) * s + 2 * x] + a[(2 * y + 1) * s + 2 * x + 1];
                a[y * h + x] = (u8)((sum + 2) / 4); /* in place: reads stay ahead of writes */
            }
    }
    plat_free(a);
}

static void init_textures(void)
{
    ubgl.glGenTextures(UB_MAT_COUNT, G.tex);
    ubgl.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    u8 white[4 * 4];
    memset(white, 255, sizeof white);
    for (int m = 0; m < UB_MAT_COUNT; ++m) {
        ubgl.glBindTexture(GL_TEXTURE_2D, G.tex[m]);
        ubgl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        ubgl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        ubgl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        ubgl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        if (G.max_aniso > 1.0f)
            ubgl.glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, G.max_aniso);
        u8 *t = ub_texture_hd((ub_material)m, TEX_SIZE);
        if (t) {
            /* Store brightness as a multiplier 0.52..1.0 (texel gain
             * 0.6..1.15 divided by 1.15), so plain GL 1.1 can use it with
             * GL_MODULATE; the shader multiplies by 1.15 again. */
            for (int i = 0; i < TEX_SIZE * TEX_SIZE; ++i)
                t[i] = (u8)((154 + t[i] * 140 / 255) * 255 / 294);
            upload_mipmapped(GL_LUMINANCE, t, TEX_SIZE);
            plat_free(t);
        } else {
            upload_mipmapped(GL_LUMINANCE, white, 4);
        }
    }
}

static void free_font(void)
{
    if (G.font_tex) ubgl.glDeleteTextures(1, &G.font_tex);
    G.font_tex = 0;
    plat_free(G.font.pixels);
    G.font.pixels = NULL;
}

static void ensure_font(int px)
{
    if (G.font_tex && px == G.font_px) return;
    free_font();
    G.font_px = px;
    if (!plat_font_atlas(px, &G.font)) {
        ub_warn("gl: could not build a %d px font", px);
        return;
    }
    ubgl.glGenTextures(1, &G.font_tex);
    ubgl.glBindTexture(GL_TEXTURE_2D, G.font_tex);
    ubgl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    ubgl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    ubgl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    ubgl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    ubgl.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    ubgl.glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, G.font.width, G.font.height, 0, GL_ALPHA,
                      GL_UNSIGNED_BYTE, G.font.pixels);
}

/* -------------------------------------------------------- framebuffers */

static bool init_shadow_map(void)
{
    GLint max_tex = 0;
    ubgl.glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex);
    G.shadow_size = max_tex >= SHADOW_SIZE ? SHADOW_SIZE : 1024;
    ubgl.glGenTextures(1, &G.shadow_tex);
    ubgl.glBindTexture(GL_TEXTURE_2D, G.shadow_tex);
    ubgl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    ubgl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    ubgl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    ubgl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    ubgl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    ubgl.glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, G.shadow_size, G.shadow_size, 0,
                      GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
    ubgl.glGenFramebuffers(1, &G.shadow_fbo);
    ubgl.glBindFramebuffer(GL_FRAMEBUFFER, G.shadow_fbo);
    ubgl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, G.shadow_tex, 0);
    ubgl.glDrawBuffer(GL_NONE);
    ubgl.glReadBuffer(GL_NONE);
    bool ok = ubgl.glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    ubgl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ubgl.glDrawBuffer(GL_BACK);
    ubgl.glReadBuffer(GL_BACK);
    if (!ok) ub_warn("gl: shadow framebuffer incomplete; shadows off");
    return ok;
}

static void free_msaa(void)
{
    if (G.msaa_fbo) ubgl.glDeleteFramebuffers(1, &G.msaa_fbo);
    if (G.msaa_color) ubgl.glDeleteRenderbuffers(1, &G.msaa_color);
    if (G.msaa_depth) ubgl.glDeleteRenderbuffers(1, &G.msaa_depth);
    G.msaa_fbo = G.msaa_color = G.msaa_depth = 0;
    G.msaa_w = G.msaa_h = 0;
}

/* (Re)create the multisampled render target for the window size. */
static bool ensure_msaa(int w, int h, int samples)
{
    if (G.msaa_fbo && G.msaa_w == w && G.msaa_h == h && G.msaa_samples == samples) return true;
    free_msaa();
    ubgl.glGenRenderbuffers(1, &G.msaa_color);
    ubgl.glBindRenderbuffer(GL_RENDERBUFFER, G.msaa_color);
    ubgl.glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, w, h);
    ubgl.glGenRenderbuffers(1, &G.msaa_depth);
    ubgl.glBindRenderbuffer(GL_RENDERBUFFER, G.msaa_depth);
    ubgl.glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH_COMPONENT24, w, h);
    ubgl.glGenFramebuffers(1, &G.msaa_fbo);
    ubgl.glBindFramebuffer(GL_FRAMEBUFFER, G.msaa_fbo);
    ubgl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, G.msaa_color);
    ubgl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, G.msaa_depth);
    bool ok = ubgl.glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    ubgl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!ok) {
        ub_warn("gl: %dx MSAA framebuffer incomplete; anti-aliasing off", samples);
        free_msaa();
        G.msaa_ok = false;
        return false;
    }
    G.msaa_w = w;
    G.msaa_h = h;
    G.msaa_samples = samples;
    return true;
}

static void free_statics(void);

/* ---------------------------------------------------------------- init */

static void make_stipples(void)
{
    for (int level = 0; level < 16; ++level)
        for (int y = 0; y < 32; ++y)
            for (int b = 0; b < 4; ++b) {
                GLubyte bits = 0;
                for (int i = 0; i < 8; ++i) { /* most significant bit = leftmost pixel */
                    int x = b * 8 + i;
                    if (ub_see_through_pattern[y & 3][x & 3] >= level) bits |= (GLubyte)(0x80 >> i);
                }
                G.stipple[level][y * 4 + b] = bits;
            }
}

bool gl_init(char *desc, int desc_len, bool fixed_function)
{
    memset(&G, 0, sizeof G);
    make_stipples();
    if (!ub_gl_load()) {
        ub_fmt(desc, desc_len, "OpenGL 1.1 functions missing");
        return false;
    }
    ub_meshes_init();
    const char *ver = (const char *)ubgl.glGetString(GL_VERSION);
    const char *ren = (const char *)ubgl.glGetString(GL_RENDERER);
    int major = ver ? ver[0] - '0' : 1;

    if (has_ext("GL_EXT_texture_filter_anisotropic")) {
        ubgl.glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &G.max_aniso);
        if (G.max_aniso > 16.0f) G.max_aniso = 16.0f;
    }
    G.shaders = !fixed_function && major >= 2 && ubgl.has_shaders && init_shaders();
    G.shadows_ok = G.shaders && ubgl.has_fbo && init_shadow_map();
    if (ubgl.has_msaa) {
        ubgl.glGetIntegerv(GL_MAX_SAMPLES, &G.max_samples);
        G.msaa_ok = G.max_samples >= 2;
    }
    init_textures();

    ubgl.glEnable(GL_DEPTH_TEST);
    ubgl.glDepthFunc(GL_LEQUAL);
    ubgl.glFrontFace(GL_CCW);
    ubgl.glCullFace(GL_BACK);
    ubgl.glEnable(GL_CULL_FACE);
    ubgl.glShadeModel(GL_SMOOTH);
    ubgl.glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST);
    GLenum err = ubgl.glGetError();
    if (err) ub_warn("gl: error 0x%x during setup", err);

    ub_fmt(desc, desc_len, "OpenGL %s on %s: %s%s%s, %ux aniso", ver ? ver : "?", ren ? ren : "?",
           G.shaders ? "shaders" : "fixed function", G.shadows_ok ? ", shadows" : "",
           G.msaa_ok ? ", MSAA" : "", (u32)(G.max_aniso > 1 ? G.max_aniso : 1));
    G.ready = true;
    return true;
}

void gl_set_quality(ub_quality q)
{
    G.quality = q;
}

void gl_shutdown(void)
{
    if (!G.ready) return;
    free_font();
    free_msaa();
    ubgl.glDeleteTextures(UB_MAT_COUNT, G.tex);
    if (G.shadow_tex) ubgl.glDeleteTextures(1, &G.shadow_tex);
    if (G.shadow_fbo) ubgl.glDeleteFramebuffers(1, &G.shadow_fbo);
    if (G.prog_lit) ubgl.glDeleteProgram(G.prog_lit);
    if (G.prog_sky) ubgl.glDeleteProgram(G.prog_sky);
    for (int m = 0; m < UB_MAT_COUNT; ++m) plat_free(G.batches[m].v);
    free_statics();
    memset(&G, 0, sizeof G);
}

/* ------------------------------------------------------------ geometry */

static gvert *batch_push(batch *b, int n)
{
    if (b->count + n > b->cap) {
        int cap = b->cap ? b->cap * 2 : 4096;
        while (cap < b->count + n) cap *= 2;
        gvert *nv = (gvert *)plat_alloc(sizeof(gvert) * (size_t)cap);
        if (!nv) return NULL;
        if (b->v) {
            memcpy(nv, b->v, sizeof(gvert) * (size_t)b->count);
            plat_free(b->v);
        }
        b->v = nv;
        b->cap = cap;
    }
    gvert *r = b->v + b->count;
    b->count += n;
    return r;
}

/* Texture coordinate around a curved surface: arc length, made continuous
 * across the seam at angle +-pi. */
static f32 arc_u(vec3 l, f32 radius)
{
    return ub_atan2f(l.z, l.x) * radius;
}

static void emit_part(const ub_part_draw *p, batch *set)
{
    const ub_mesh *mesh = ub_mesh_get((ub_shape)p->shape, UB_DETAIL_HIGH);
    ub_material mat = (ub_material)(p->material < UB_MAT_COUNT ? p->material : UB_MAT_PLASTIC);
    batch *b = &set[mat];
    u8 ca = (p->flags & UB_DRAW_GLOW) ? 0 : 255;
    vec3 size = v3(UB_MAX(p->size.x, 0.01f), UB_MAX(p->size.y, 0.01f), UB_MAX(p->size.z, 0.01f));
    f32 repeat = 1.0f / ub_materials[mat].world_size;
    f32 radius = 0.25f * (size.x + size.z); /* for wrapping textures round curves */
    u8 cr = (u8)(p->color >> 16), cg = (u8)(p->color >> 8), cb = (u8)p->color;

    for (int fi = 0; fi < mesh->face_count; ++fi) {
        const ub_face *f = &mesh->faces[fi];
        int n = f->count;
        vec3 wp[UB_FACE_MAX_VERTS], wn[UB_FACE_MAX_VERTS];
        f32 tu[UB_FACE_MAX_VERTS], tv[UB_FACE_MAX_VERTS];

        vec3 ns = v3_normalize(v3(f->normal.x / size.x, f->normal.y / size.y, f->normal.z / size.z));
        vec3 face_n = mat3_apply(&p->rot, ns);
        vec3 s0 = v3_mul(mesh->verts[f->v[0]], size);
        vec3 au = v3_normalize(v3_sub(v3_mul(mesh->verts[f->v[1]], size), s0));
        vec3 av = v3_cross(ns, au);
        bool curved_side = f->smooth;
        for (int i = 0; i < n; ++i) {
            vec3 l = mesh->verts[f->v[i]];
            vec3 s = v3_mul(l, size);
            wp[i] = v3_add(p->pos, mat3_apply(&p->rot, s));
            if (f->smooth) {
                vec3 vn = mesh->vnormals[f->v[i]];
                wn[i] = v3_normalize(mat3_apply(&p->rot, v3(vn.x / size.x, vn.y / size.y, vn.z / size.z)));
            } else {
                wn[i] = face_n;
            }
            if (curved_side) {
                tu[i] = arc_u(l, radius) * repeat;
                tv[i] = s.y * repeat;
            } else {
                vec3 d = v3_sub(s, s0);
                tu[i] = v3_dot(d, au) * repeat;
                tv[i] = v3_dot(d, av) * repeat;
            }
        }
        if (curved_side) { /* keep u continuous across the +-pi seam */
            f32 wrap = UB_TWO_PI * radius * repeat;
            for (int i = 1; i < n; ++i) {
                if (tu[i] - tu[0] > wrap * 0.5f) tu[i] -= wrap;
                else if (tu[0] - tu[i] > wrap * 0.5f) tu[i] += wrap;
            }
        }

        /* Triangles are ordered counter-clockwise as seen from outside
         * (in this left-handed world the plain cross product then points
         * inward). */
        vec3 c = v3_cross(v3_sub(wp[1], wp[0]), v3_sub(wp[2], wp[0]));
        bool flip = v3_dot(c, face_n) > 0;

        /* Fixed-function GL lights and fogs per vertex, which goes wrong
         * across huge faces like the ground; split big quads into a grid
         * of ~6-unit cells. (The shader path works per pixel.) */
        if (!G.shaders && n == 4) {
            int nu = UB_CLAMP(ub_ceilf(v3_length(v3_sub(wp[1], wp[0])) / 6.0f), 1, 24);
            int nv = UB_CLAMP(ub_ceilf(v3_length(v3_sub(wp[3], wp[0])) / 6.0f), 1, 24);
            if (nu * nv > 1) {
                gvert *out = batch_push(b, nu * nv * 6);
                if (!out) return;
                for (int j = 0; j < nv; ++j)
                    for (int i = 0; i < nu; ++i) {
                        static const int corner[6][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 0 }, { 1, 1 }, { 0, 1 } };
                        for (int k = 0; k < 6; ++k) {
                            int kk = flip ? (k % 3 == 1 ? k + 1 : k % 3 == 2 ? k - 1 : k) : k;
                            f32 su = (f32)(i + corner[kk][0]) / (f32)nu, sv = (f32)(j + corner[kk][1]) / (f32)nv;
                            /* bilinear between corners 0-1 (u) and 0-3 (v) */
                            f32 w0 = (1 - su) * (1 - sv), w1 = su * (1 - sv), w2 = su * sv, w3 = (1 - su) * sv;
                            vec3 pp = v3_add(v3_add(v3_scale(wp[0], w0), v3_scale(wp[1], w1)),
                                             v3_add(v3_scale(wp[2], w2), v3_scale(wp[3], w3)));
                            vec3 nn = v3_normalize(v3_add(v3_add(v3_scale(wn[0], w0), v3_scale(wn[1], w1)),
                                                          v3_add(v3_scale(wn[2], w2), v3_scale(wn[3], w3))));
                            gvert *o = out++;
                            o->x = pp.x; o->y = pp.y; o->z = pp.z;
                            o->nx = nn.x; o->ny = nn.y; o->nz = nn.z;
                            o->u = tu[0] * w0 + tu[1] * w1 + tu[2] * w2 + tu[3] * w3;
                            o->v = tv[0] * w0 + tv[1] * w1 + tv[2] * w2 + tv[3] * w3;
                            o->r = cr; o->g = cg; o->b = cb; o->a = ca;
                        }
                    }
                continue;
            }
        }

        gvert *out = batch_push(b, (n - 2) * 3);
        if (!out) return;
        for (int t = 1; t + 1 < n; ++t) {
            int idx[3] = { 0, flip ? t + 1 : t, flip ? t : t + 1 };
            for (int k = 0; k < 3; ++k) {
                int i = idx[k];
                gvert *o = out++;
                o->x = wp[i].x; o->y = wp[i].y; o->z = wp[i].z;
                o->nx = wn[i].x; o->ny = wn[i].y; o->nz = wn[i].z;
                o->u = tu[i]; o->v = tv[i];
                o->r = cr; o->g = cg; o->b = cb; o->a = ca;
            }
        }
    }
}

/* Point the vertex arrays at `base`: client memory, or an offset into
 * the bound vertex buffer when base is NULL-relative. */
static void bind_pointers(const gvert *base, bool full)
{
    ubgl.glVertexPointer(3, GL_FLOAT, sizeof(gvert), &base->x);
    if (full) {
        ubgl.glNormalPointer(GL_FLOAT, sizeof(gvert), &base->nx);
        ubgl.glTexCoordPointer(2, GL_FLOAT, sizeof(gvert), &base->u);
        ubgl.glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(gvert), &base->r);
    }
}

static void free_statics(void)
{
    plat_free(G.see_statics);
    G.see_statics = NULL;
    G.see_static_count = 0;
    for (int m = 0; m < UB_MAT_COUNT; ++m) {
        if (G.static_vbo[m]) ubgl.glDeleteBuffers(1, &G.static_vbo[m]);
        G.static_vbo[m] = 0;
        plat_free(G.statics[m].v);
        G.statics[m].v = NULL;
        G.statics[m].count = G.statics[m].cap = 0;
        G.static_verts[m] = 0;
    }
}

void gl_set_static(const ub_part_draw *parts, int count)
{
    if (!G.ready) return;
    free_statics();
    int see = 0;
    for (int i = 0; i < count; ++i) see += ub_draw_see_level(parts[i].flags) != 0;
    if (see) G.see_statics = (ub_part_draw *)plat_alloc(sizeof(ub_part_draw) * (size_t)see);
    for (int i = 0; i < count; ++i) {
        if (!ub_draw_see_level(parts[i].flags)) emit_part(&parts[i], G.statics);
        else if (G.see_statics) G.see_statics[G.see_static_count++] = parts[i];
    }
    u32 bytes = 0;
    for (int m = 0; m < UB_MAT_COUNT; ++m) {
        G.static_verts[m] = G.statics[m].count;
        bytes += (u32)G.statics[m].count * (u32)sizeof(gvert);
        if (ubgl.has_vbo && G.statics[m].count) { /* move it to GPU memory */
            ubgl.glGenBuffers(1, &G.static_vbo[m]);
            ubgl.glBindBuffer(GL_ARRAY_BUFFER, G.static_vbo[m]);
            ubgl.glBufferData(GL_ARRAY_BUFFER, (ptrdiff_t)(G.statics[m].count * (int)sizeof(gvert)),
                              G.statics[m].v, GL_STATIC_DRAW);
            plat_free(G.statics[m].v);
            G.statics[m].v = NULL;
            G.statics[m].cap = 0;
        }
    }
    if (ubgl.has_vbo) ubgl.glBindBuffer(GL_ARRAY_BUFFER, 0);
    ub_info("gl: %d static parts, %u KB of geometry%s", count, bytes / 1024,
            ubgl.has_vbo ? " in vertex buffers" : " in system memory");
}

/* Draw one material: static geometry (with_statics), then this frame's
 * dynamic parts. */
static void draw_material_ex(int m, bool full, bool with_statics)
{
    if (with_statics && G.static_verts[m]) {
        if (G.static_vbo[m]) {
            ubgl.glBindBuffer(GL_ARRAY_BUFFER, G.static_vbo[m]);
            bind_pointers((const gvert *)0, full);
        } else {
            bind_pointers(G.statics[m].v, full);
        }
        ubgl.glDrawArrays(GL_TRIANGLES, 0, G.static_verts[m]);
        if (G.static_vbo[m]) ubgl.glBindBuffer(GL_ARRAY_BUFFER, 0);
    }
    if (G.batches[m].count) {
        bind_pointers(G.batches[m].v, full);
        ubgl.glDrawArrays(GL_TRIANGLES, 0, G.batches[m].count);
    }
}

static void draw_material(int m, bool full) { draw_material_ex(m, full, true); }

/* Every material with its surface settings (the lit pass). */
static void draw_materials(bool with_statics)
{
    for (int m = 0; m < UB_MAT_COUNT; ++m) {
        if (!G.batches[m].count && !(with_statics && G.static_verts[m])) continue;
        const ub_material_info *mi = &ub_materials[m];
        ubgl.glBindTexture(GL_TEXTURE_2D, G.tex[m]);
        if (G.shaders) {
            ubgl.glUniform1f(G.lit.spec, mi->specular);
            ubgl.glUniform1f(G.lit.shine, mi->shininess);
            ubgl.glUniform1f(G.lit.texmix, m == UB_MAT_PLASTIC ? 0.0f : 1.0f);
        } else {
            f32 s[4] = { mi->specular, mi->specular, mi->specular, 1 };
            ubgl.glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, s);
            ubgl.glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, mi->shininess > 128 ? 128 : mi->shininess);
        }
        draw_material_ex(m, true, with_statics);
    }
}

static void set_client_state(bool full)
{
    ubgl.glEnableClientState(GL_VERTEX_ARRAY);
    if (full) {
        ubgl.glEnableClientState(GL_NORMAL_ARRAY);
        ubgl.glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        ubgl.glEnableClientState(GL_COLOR_ARRAY);
    } else {
        ubgl.glDisableClientState(GL_NORMAL_ARRAY);
        ubgl.glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        ubgl.glDisableClientState(GL_COLOR_ARRAY);
    }
}

/* ------------------------------------------------------------- frame */

static void draw_sky_shader(vec3 r, vec3 u, vec3 f, f32 tx, f32 ty, const ub_render_env *env)
{
    f32 c[3];
    ubgl.glUseProgram(G.prog_sky);
    ubgl.glUniform3f(G.sky.right, r.x, r.y, r.z);
    ubgl.glUniform3f(G.sky.up, u.x, u.y, u.z);
    ubgl.glUniform3f(G.sky.fwd, f.x, f.y, f.z);
    ubgl.glUniform2f(G.sky.tan, tx, ty);
    ubgl.glUniform3f(G.sky.sun_dir, env->sun_dir.x, env->sun_dir.y, env->sun_dir.z);
    color3(env->sky_top, c);      ubgl.glUniform3f(G.sky.zenith, c[0], c[1], c[2]);
    color3(env->sky_horizon, c);  ubgl.glUniform3f(G.sky.horizon, c[0], c[1], c[2]);
    color3(env->ground_color, c); ubgl.glUniform3f(G.sky.ground, c[0], c[1], c[2]);
    color3(env->sun_color, c);    ubgl.glUniform3f(G.sky.sun_col, c[0], c[1], c[2]);
    ubgl.glDisable(GL_DEPTH_TEST);
    ubgl.glDepthMask(GL_FALSE);
    ubgl.glDisable(GL_CULL_FACE);
    ubgl.glBegin(GL_QUADS);
    ubgl.glVertex2f(-1, -1);
    ubgl.glVertex2f(1, -1);
    ubgl.glVertex2f(1, 1);
    ubgl.glVertex2f(-1, 1);
    ubgl.glEnd();
    ubgl.glDepthMask(GL_TRUE);
    ubgl.glEnable(GL_DEPTH_TEST);
    ubgl.glEnable(GL_CULL_FACE);
}

/* Fixed-function sky: horizontal strips coloured by elevation. */
static void draw_sky_strips(vec3 u, vec3 f, f32 ty, const ub_render_env *env)
{
    ubgl.glMatrixMode(GL_PROJECTION);
    ubgl.glLoadIdentity();
    ubgl.glMatrixMode(GL_MODELVIEW);
    ubgl.glLoadIdentity();
    ubgl.glDisable(GL_DEPTH_TEST);
    ubgl.glDepthMask(GL_FALSE);
    ubgl.glDisable(GL_CULL_FACE);
    ubgl.glBegin(GL_QUADS);
    const int strips = 24;
    for (int i = 0; i < strips; ++i) {
        f32 y0 = -1.0f + 2.0f * (f32)i / strips, y1 = -1.0f + 2.0f * (f32)(i + 1) / strips;
        f32 ys[2] = { y0, y1 };
        f32 col[2][3];
        for (int k = 0; k < 2; ++k) {
            vec3 d = v3_normalize(v3_add(f, v3_scale(u, ys[k] * ty)));
            f32 a[3], b[3];
            f32 t;
            if (d.y >= 0) { color3(env->sky_horizon, a); color3(env->sky_top, b); t = d.y * 1.4f; }
            else { color3(env->sky_horizon, a); color3(env->ground_color, b); t = -d.y * 5.0f; }
            t = ub_clampf(t, 0, 1);
            for (int j = 0; j < 3; ++j) col[k][j] = a[j] + (b[j] - a[j]) * t;
        }
        ubgl.glColor4f(col[0][0], col[0][1], col[0][2], 1);
        ubgl.glVertex2f(-1, y0);
        ubgl.glVertex2f(1, y0);
        ubgl.glColor4f(col[1][0], col[1][1], col[1][2], 1);
        ubgl.glVertex2f(1, y1);
        ubgl.glVertex2f(-1, y1);
    }
    ubgl.glEnd();
    ubgl.glDepthMask(GL_TRUE);
    ubgl.glEnable(GL_DEPTH_TEST);
    ubgl.glEnable(GL_CULL_FACE);
}

static mat4 shadow_pass(const ub_camera *cam, const ub_render_env *env)
{
    /* light space basis: looking along the sunlight */
    vec3 ld = v3_scale(env->sun_dir, -1.0f);
    vec3 up = ub_absf(ld.y) > 0.99f ? v3(0, 0, 1) : v3(0, 1, 0);
    vec3 lr = v3_normalize(v3_cross(up, ld));
    vec3 lu = v3_cross(ld, lr);
    /* centre on the focus, snapped to whole shadow texels so shadow edges
     * do not shimmer as the camera moves */
    f32 texel = 2.0f * SHADOW_HALF / (f32)G.shadow_size;
    vec3 c = cam->focus;
    f32 cx = (f32)ub_roundf(v3_dot(c, lr) / texel) * texel;
    f32 cy = (f32)ub_roundf(v3_dot(c, lu) / texel) * texel;
    f32 cz = v3_dot(c, ld);
    vec3 center = v3_add(v3_add(v3_scale(lr, cx), v3_scale(lu, cy)), v3_scale(ld, cz));
    mat4 lv = mat4_view(center, lr, lu, ld);
    mat4 lp = mat4_ortho(-SHADOW_HALF, SHADOW_HALF, -SHADOW_HALF, SHADOW_HALF, -SHADOW_DEPTH, SHADOW_DEPTH);
    mat4 lvp = mat4_mul(&lp, &lv);

    ubgl.glBindFramebuffer(GL_FRAMEBUFFER, G.shadow_fbo);
    ubgl.glViewport(0, 0, G.shadow_size, G.shadow_size);
    ubgl.glClear(GL_DEPTH_BUFFER_BIT);
    ubgl.glUseProgram(0);
    ubgl.glDisable(GL_LIGHTING);
    ubgl.glDisable(GL_TEXTURE_2D);
    ubgl.glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    ubgl.glEnable(GL_POLYGON_OFFSET_FILL);
    ubgl.glPolygonOffset(1.5f, 4.0f);
    ubgl.glMatrixMode(GL_PROJECTION);
    ubgl.glLoadMatrixf(lp.m);
    ubgl.glMatrixMode(GL_MODELVIEW);
    ubgl.glLoadMatrixf(lv.m);
    set_client_state(false);
    for (int m = 0; m < UB_MAT_COUNT; ++m) draw_material(m, false);
    ubgl.glDisable(GL_POLYGON_OFFSET_FILL);
    ubgl.glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    ubgl.glBindFramebuffer(GL_FRAMEBUFFER, 0);

    static const mat4 bias = { { 0.5f, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0, 0.5f, 0, 0.5f, 0.5f, 0.5f, 1 } };
    return mat4_mul(&bias, &lvp);
}

void gl_draw_scene(int width, int height, const ub_camera *cam, const ub_render_env *env,
                   const ub_part_draw *parts, int count, ub_render_stats *stats)
{
    if (!G.ready || width <= 0 || height <= 0) return;
    G.width = width;
    G.height = height;

    /* camera */
    f32 sy = ub_sinf(cam->yaw), cy = ub_cosf(cam->yaw);
    f32 sp = ub_sinf(cam->pitch), cp = ub_cosf(cam->pitch);
    vec3 fwd = v3(sy * cp, sp, cy * cp);
    vec3 right = v3(cy, 0, -sy);
    vec3 up = v3_cross(fwd, right);
    f32 aspect = (f32)width / (f32)height;
    f32 fov = ub_clampf(cam->fov_y, 0.2f, 2.8f);
    f32 ty = ub_sinf(fov * 0.5f) / ub_cosf(fov * 0.5f);
    mat4 view = mat4_view(cam->pos, right, up, fwd);
    mat4 proj = mat4_perspective(fov, aspect, NEAR_Z, FAR_Z);
    mat4 vp = mat4_mul(&proj, &view);

    /* The 2D pass at the end of the previous frame changed state; put
     * back everything the 3D passes rely on. (Depth writes only happen
     * while the depth test is enabled.) */
    ubgl.glDisable(GL_BLEND);
    ubgl.glDisable(GL_SCISSOR_TEST);
    ubgl.glEnable(GL_DEPTH_TEST);
    ubgl.glDepthMask(GL_TRUE);
    ubgl.glDepthFunc(GL_LEQUAL);
    ubgl.glEnable(GL_CULL_FACE);
    ubgl.glCullFace(GL_BACK);

    /* geometry (see-through parts come last, below) */
    for (int m = 0; m < UB_MAT_COUNT; ++m) G.batches[m].count = 0;
    for (int i = 0; i < count; ++i)
        if (!ub_draw_see_level(parts[i].flags)) emit_part(&parts[i], G.batches);
    u32 tris = 0;
    for (int m = 0; m < UB_MAT_COUNT; ++m) tris += (u32)(G.batches[m].count + G.static_verts[m]) / 3;
    stats->parts = (u32)count;
    stats->triangles = tris;
    stats->polys_drawn = tris;

    bool shadows = G.shadows_ok && G.quality >= UB_QUALITY_MEDIUM;
    bool msaa = G.msaa_ok && G.quality >= UB_QUALITY_HIGH;
    mat4 shadow_mat;
    memset(&shadow_mat, 0, sizeof shadow_mat);
    if (shadows) shadow_mat = shadow_pass(cam, env);

    if (msaa) {
        int samples = G.max_samples >= 4 ? 4 : G.max_samples;
        if (!ensure_msaa(width, height, samples)) msaa = false;
    }
    ubgl.glBindFramebuffer(GL_FRAMEBUFFER, msaa ? G.msaa_fbo : 0);
    ubgl.glViewport(0, 0, width, height);
    ubgl.glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);

    f32 sun[3], sky[3], ground[3], fogc[3];
    color3(env->sun_color, sun);
    color3(env->sky_light, sky);
    color3(env->ground_light, ground);
    color3(env->sky_horizon, fogc);

    if (G.shaders) {
        draw_sky_shader(right, up, fwd, ty * aspect, ty, env);
        ubgl.glUseProgram(G.prog_lit);
        ubgl.glUniformMatrix4fv(G.lit.viewproj, 1, GL_FALSE, vp.m);
        ubgl.glUniformMatrix4fv(G.lit.shadowmat, 1, GL_FALSE, shadow_mat.m);
        ubgl.glUniform1i(G.lit.tex, 0);
        ubgl.glUniform1i(G.lit.shadow, 1);
        ubgl.glUniform3f(G.lit.sun_dir, env->sun_dir.x, env->sun_dir.y, env->sun_dir.z);
        ubgl.glUniform3f(G.lit.sun_col, sun[0], sun[1], sun[2]);
        ubgl.glUniform3f(G.lit.sky_col, sky[0], sky[1], sky[2]);
        ubgl.glUniform3f(G.lit.ground_col, ground[0], ground[1], ground[2]);
        ubgl.glUniform3f(G.lit.cam, cam->pos.x, cam->pos.y, cam->pos.z);
        ubgl.glUniform3f(G.lit.fog_col, fogc[0], fogc[1], fogc[2]);
        ubgl.glUniform1f(G.lit.fog_density, env->fog_density);
        ubgl.glUniform1f(G.lit.shadow_on, shadows ? 1.0f : 0.0f);
        ubgl.glUniform1f(G.lit.shadow_texel, 1.0f / (f32)(G.shadow_size ? G.shadow_size : 1));
        ubgl.glActiveTexture(GL_TEXTURE1);
        ubgl.glBindTexture(GL_TEXTURE_2D, shadows ? G.shadow_tex : G.tex[UB_MAT_PLASTIC]);
        ubgl.glActiveTexture(GL_TEXTURE0);
    } else {
        draw_sky_strips(up, fwd, ty, env);
        ubgl.glMatrixMode(GL_PROJECTION);
        ubgl.glLoadMatrixf(proj.m);
        ubgl.glMatrixMode(GL_MODELVIEW);
        ubgl.glLoadMatrixf(view.m);
        f32 pos[4] = { env->sun_dir.x, env->sun_dir.y, env->sun_dir.z, 0 };
        f32 dif[4] = { sun[0], sun[1], sun[2], 1 };
        f32 amb[4] = { (sky[0] + ground[0]) * 0.5f, (sky[1] + ground[1]) * 0.5f, (sky[2] + ground[2]) * 0.5f, 1 };
        f32 zero[4] = { 0, 0, 0, 1 };
        ubgl.glLightfv(GL_LIGHT0, GL_POSITION, pos);  /* after the view matrix: world space */
        ubgl.glLightfv(GL_LIGHT0, GL_DIFFUSE, dif);
        ubgl.glLightfv(GL_LIGHT0, GL_SPECULAR, dif);
        ubgl.glLightfv(GL_LIGHT0, GL_AMBIENT, zero);
        ubgl.glLightModelfv(GL_LIGHT_MODEL_AMBIENT, amb);
        ubgl.glEnable(GL_LIGHTING);
        ubgl.glEnable(GL_LIGHT0);
        ubgl.glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
        ubgl.glEnable(GL_COLOR_MATERIAL);
        ubgl.glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        ubgl.glEnable(GL_TEXTURE_2D);
        if (env->fog_density > 0) {
            f32 fc[4] = { fogc[0], fogc[1], fogc[2], 1 };
            ubgl.glFogi(GL_FOG_MODE, GL_EXP);
            ubgl.glFogf(GL_FOG_DENSITY, env->fog_density);
            ubgl.glFogfv(GL_FOG_COLOR, fc);
            /* per-pixel fog: per-vertex fog smears the far corners of big
             * faces (the ground) across the whole face */
            ubgl.glHint(GL_FOG_HINT, GL_NICEST);
            ubgl.glEnable(GL_FOG);
        }
    }

    set_client_state(true);
    draw_materials(true);
    /* see-through parts: one pass per level, with that level's screen-door
     * stipple (the same pattern as the software renderer) */
    bool any_see = G.see_static_count > 0;
    for (int i = 0; i < count && !any_see; ++i) any_see = ub_draw_see_level(parts[i].flags) != 0;
    for (int level = 1; level < 16 && any_see; ++level) {
        bool here = false;
        for (int m = 0; m < UB_MAT_COUNT; ++m) G.batches[m].count = 0;
        for (int i = 0; i < count; ++i)
            if (ub_draw_see_level(parts[i].flags) == level) {
                emit_part(&parts[i], G.batches);
                here = true;
            }
        for (int i = 0; i < G.see_static_count; ++i)
            if (ub_draw_see_level(G.see_statics[i].flags) == level) {
                emit_part(&G.see_statics[i], G.batches);
                here = true;
            }
        if (!here) continue;
        ubgl.glEnable(GL_POLYGON_STIPPLE);
        ubgl.glPolygonStipple(G.stipple[level]);
        draw_materials(false);
        for (int m = 0; m < UB_MAT_COUNT; ++m) stats->triangles += (u32)G.batches[m].count / 3;
    }
    if (any_see) ubgl.glDisable(GL_POLYGON_STIPPLE);
    ubgl.glDisableClientState(GL_COLOR_ARRAY);
    ubgl.glDisableClientState(GL_NORMAL_ARRAY);
    ubgl.glDisableClientState(GL_TEXTURE_COORD_ARRAY);

    if (G.shaders) {
        ubgl.glUseProgram(0);
    } else {
        ubgl.glDisable(GL_LIGHTING);
        ubgl.glDisable(GL_COLOR_MATERIAL);
        ubgl.glDisable(GL_FOG);
    }

    if (msaa) { /* resolve the anti-aliased image into the window */
        ubgl.glBindFramebuffer(GL_READ_FRAMEBUFFER, G.msaa_fbo);
        ubgl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        ubgl.glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        ubgl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
}

/* ------------------------------------------------------------------ 2D */

static int g_text_px;

void gl_set_text_px(int px) { g_text_px = px; }

int gl_line_height(void)
{
    if (g_text_px > 0) return g_text_px + 4;
    int h = G.height > 0 ? G.height : 480;
    return UB_CLAMP(h / 34, 15, 40);
}

void gl_rect(int x, int y, int w, int h, u32 color)
{
    if (!G.ready || w <= 0 || h <= 0) return;
    f32 c[3];
    color3(color, c);
    f32 sx = 2.0f / (f32)G.width, sy = 2.0f / (f32)G.height;
    f32 x0 = (f32)x * sx - 1.0f, x1 = (f32)(x + w) * sx - 1.0f;
    f32 y0 = 1.0f - (f32)y * sy, y1 = 1.0f - (f32)(y + h) * sy;
    ubgl.glDisable(GL_TEXTURE_2D);
    ubgl.glColor4f(c[0], c[1], c[2], 1);
    ubgl.glBegin(GL_QUADS);
    ubgl.glVertex2f(x0, y0);
    ubgl.glVertex2f(x1, y0);
    ubgl.glVertex2f(x1, y1);
    ubgl.glVertex2f(x0, y1);
    ubgl.glEnd();
    ubgl.glEnable(GL_TEXTURE_2D);
}

void gl_clip(int x, int y, int w, int h)
{
    if (!G.ready) return;
    if (w <= 0 || h <= 0) {
        ubgl.glDisable(GL_SCISSOR_TEST);
        return;
    }
    ubgl.glScissor(x, G.height - (y + h), w, h); /* GL counts rows from the bottom */
    ubgl.glEnable(GL_SCISSOR_TEST);
}

void gl_begin_2d(int width, int height)
{
    if (!G.ready) return;
    G.width = width;
    G.height = height;
    ensure_font(gl_line_height() - 2);
    ubgl.glViewport(0, 0, width, height);
    ubgl.glMatrixMode(GL_PROJECTION);
    ubgl.glLoadIdentity();
    ubgl.glMatrixMode(GL_MODELVIEW);
    ubgl.glLoadIdentity();
    ubgl.glDisable(GL_DEPTH_TEST);
    ubgl.glDisable(GL_CULL_FACE);
    ubgl.glDisable(GL_LIGHTING);
    ubgl.glDisable(GL_FOG);
    ubgl.glEnable(GL_BLEND);
    ubgl.glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    ubgl.glEnable(GL_TEXTURE_2D);
    ubgl.glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
}

static void draw_string(f32 x, f32 y, const char *s, f32 r, f32 g, f32 b)
{
    const ub_font_atlas *fa = &G.font;
    f32 sx = 2.0f / (f32)G.width, sy = 2.0f / (f32)G.height;
    ubgl.glColor4f(r, g, b, 1);
    ubgl.glBegin(GL_QUADS);
    for (const char *p = s; *p;) {
        u32 c = ub_utf8_decode(&p);
        if (c < 32 || c > 127) c = '?';
        int i = (int)c - 32;
        /* just the glyph's own box: its advance plus a pixel each side */
        int gw = UB_MIN(fa->advance[i] + 2, fa->cell_w - 1), gh = fa->glyph_h + 1;
        f32 u0 = (f32)((i % 16) * fa->cell_w) / (f32)fa->width;
        f32 v0 = (f32)((i / 16) * fa->cell_h) / (f32)fa->height;
        f32 u1 = u0 + (f32)gw / (f32)fa->width;
        f32 v1 = v0 + (f32)gh / (f32)fa->height;
        f32 x0 = (x - 1.0f) * sx - 1.0f, x1 = (x - 1.0f + (f32)gw) * sx - 1.0f;
        f32 y0 = 1.0f - (y - 1.0f) * sy, y1 = 1.0f - (y - 1.0f + (f32)gh) * sy;
        ubgl.glTexCoord2f(u0, v0); ubgl.glVertex2f(x0, y0);
        ubgl.glTexCoord2f(u1, v0); ubgl.glVertex2f(x1, y0);
        ubgl.glTexCoord2f(u1, v1); ubgl.glVertex2f(x1, y1);
        ubgl.glTexCoord2f(u0, v1); ubgl.glVertex2f(x0, y1);
        x += (f32)fa->advance[i];
    }
    ubgl.glEnd();
}

void gl_text(int x, int y, const char *utf8, u32 color)
{
    if (!G.ready || !G.font_tex) return;
    ubgl.glBindTexture(GL_TEXTURE_2D, G.font_tex);
    f32 c[3];
    color3(color, c);
    if (!(color & UB_TEXT_FLAT)) draw_string((f32)x + 1, (f32)y + 1, utf8, 0, 0, 0);  /* shadow */
    draw_string((f32)x, (f32)y, utf8, c[0], c[1], c[2]);
}
