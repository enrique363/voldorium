/* Window, input and drawing of the TA display lists (SDL2 + OpenGL 3.3).
 *
 * Each STARTRENDER draws one TA context: background colour, then the opaque list,
 * punch-through (alpha test against PT_ALPHA_REF) and translucent lists in that order.
 * Vertices are in screen space (640x480) with z = 1/w. Like the PowerVR they are rasterized
 * in screen space (gl_Position.w = 1): 1/w and attribute/w are interpolated linearly and
 * divided per pixel, and depth = 1/(1 + 1/w) keeps "larger 1/w is nearer", so PowerVR depth
 * compare modes map to the inverse GL ones. This matters for vertices behind the camera,
 * which the game's projector (0x8C03A9CC) sends as (X * 1e30, Y * 1e30, 1/w = 1e30): the
 * PowerVR draws those polygons out to the screen edge, homogeneous GL clipping would not.
 * ISP word: 31-29 depth compare, 26 Z write disable. TSP word: 31-29 / 28-26 source and
 * destination blend, 20 use vertex alpha, 19 ignore texture alpha, 18/17 flip U/V,
 * 16/15 clamp U/V, 14-13 filter, 7-6 texture instruction (decal, modulate, decal alpha,
 * modulate alpha). Modifier volumes and fog are not drawn yet; translucent polygons are
 * drawn in submission order (no per-pixel sort).
 *
 * M1_HEADLESS=1: no window. M1_SHOTS="n,n,...": save render n as build/m1/shot_n.ppm.
 * Keys: arrows = D-pad, Z = A, X = B, A = X, S = Y, Enter = Start; a game controller works too.
 */
#include "m1.h"
#include "ta.h"
#include <SDL2/SDL.h>
#ifdef _WIN32
/* Windows' opengl32 exports GL 1.1 only: the newer functions are loaded after the context is
   created (gl_load) and reached through these pointers */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>
#define GL_FUNCS(X) \
	X(PFNGLACTIVETEXTUREPROC, glActiveTexture) \
	X(PFNGLATTACHSHADERPROC, glAttachShader) \
	X(PFNGLBINDBUFFERPROC, glBindBuffer) \
	X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) \
	X(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer) \
	X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) \
	X(PFNGLBUFFERDATAPROC, glBufferData) \
	X(PFNGLCOMPILESHADERPROC, glCompileShader) \
	X(PFNGLCREATEPROGRAMPROC, glCreateProgram) \
	X(PFNGLCREATESHADERPROC, glCreateShader) \
	X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) \
	X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer) \
	X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D) \
	X(PFNGLGENBUFFERSPROC, glGenBuffers) \
	X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers) \
	X(PFNGLGENERATEMIPMAPPROC, glGenerateMipmap) \
	X(PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC, glRenderbufferStorageMultisample) \
	X(PFNGLBLITFRAMEBUFFERPROC, glBlitFramebuffer) \
	X(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers) \
	X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) \
	X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) \
	X(PFNGLGETSHADERIVPROC, glGetShaderiv) \
	X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) \
	X(PFNGLLINKPROGRAMPROC, glLinkProgram) \
	X(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage) \
	X(PFNGLSHADERSOURCEPROC, glShaderSource) \
	X(PFNGLUNIFORM1FPROC, glUniform1f) \
	X(PFNGLUNIFORM1IPROC, glUniform1i) \
	X(PFNGLUNIFORM2IPROC, glUniform2i) \
	X(PFNGLUNIFORM2FPROC, glUniform2f) \
	X(PFNGLUNIFORM3FPROC, glUniform3f) \
	X(PFNGLBUFFERSUBDATAPROC, glBufferSubData) \
	X(PFNGLUNIFORM4FPROC, glUniform4f) \
	X(PFNGLUSEPROGRAMPROC, glUseProgram) \
	X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer)
#define X(T, n) static T p_##n;
GL_FUNCS(X)
#undef X
#define glActiveTexture p_glActiveTexture
#define glAttachShader p_glAttachShader
#define glBindBuffer p_glBindBuffer
#define glBindFramebuffer p_glBindFramebuffer
#define glBindRenderbuffer p_glBindRenderbuffer
#define glBindVertexArray p_glBindVertexArray
#define glBufferData p_glBufferData
#define glCompileShader p_glCompileShader
#define glCreateProgram p_glCreateProgram
#define glCreateShader p_glCreateShader
#define glEnableVertexAttribArray p_glEnableVertexAttribArray
#define glFramebufferRenderbuffer p_glFramebufferRenderbuffer
#define glFramebufferTexture2D p_glFramebufferTexture2D
#define glGenBuffers p_glGenBuffers
#define glGenFramebuffers p_glGenFramebuffers
#define glGenRenderbuffers p_glGenRenderbuffers
#define glGenVertexArrays p_glGenVertexArrays
#define glGetShaderInfoLog p_glGetShaderInfoLog
#define glGetShaderiv p_glGetShaderiv
#define glGetUniformLocation p_glGetUniformLocation
#define glLinkProgram p_glLinkProgram
#define glRenderbufferStorage p_glRenderbufferStorage
#define glShaderSource p_glShaderSource
#define glUniform1f p_glUniform1f
#define glUniform1i p_glUniform1i
#define glUniform2i p_glUniform2i
#define glUniform2f p_glUniform2f
#define glUniform3f p_glUniform3f
#define glUniform4f p_glUniform4f
#define glBufferSubData p_glBufferSubData
#define glUseProgram p_glUseProgram
#define glVertexAttribPointer p_glVertexAttribPointer
#define glGenerateMipmap p_glGenerateMipmap
#define glRenderbufferStorageMultisample p_glRenderbufferStorageMultisample
#define glBlitFramebuffer p_glBlitFramebuffer
static int gl_load(void)
{
#define X(T, n) if (!(p_##n = (T)SDL_GL_GetProcAddress(#n))) { fprintf(stderr, "GL: no %s\n", #n); return 0; }
	GL_FUNCS(X)
#undef X
	return 1;
}
#else
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
static int gl_load(void) { return 1; }
#endif
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <spawn.h>
extern char **environ;
#endif

u32 hw_reg(u32 addr);
unsigned tex_get(u32 tcw, u32 tsp);
unsigned tex_get_rep(u32 tcw, u32 tsp);
unsigned tex_get_hi(u32 tcw, u32 tsp, int *scale, int *edge, int text, int pictures);
void tex_hi_reset(void);

static SDL_Window *win;
static SDL_GLContext glc;
static SDL_GameController *pads[2];	/* player 1 (with the keyboard), player 2 */
static int enabled = -1;
static GLuint prog, vao, vbo;
static GLint u_tex, u_mode, u_flags, u_alpha_ref, u_target, u_xmap, u_umap, u_xoff, u_fog, u_fog_col, u_ymap, u_xzoom;
static GLint u_fog_col_ram, u_fog_density, u_fog_tab;
static GLuint fog_tex;
/* the picture's shape (width / height, 4:3 to 16:9) and its zoom: the game is simulated and drawn for
   16:9 (Hor+, the same for every player, also online); a narrower picture shows its middle */
static float view_aspect = 16.0f / 9, view_zoom = 1;
static int player_shot;	/* F12 pressed: a screenshot after the next frame (player_screenshot) */
/* viewport of the current target, for user tile clip scissors */
static int vp_x, vp_y, vp_w, vp_h, tgt_w = 640, tgt_h = 480;
static u32 buttons, buttons_b;	/* window input: player 1 (keyboard, first controller), player 2 */
static u64 rendered;
static u32 shots[64];
static int nshots;

static const char *vs_src =
	"#version 330 core\n"
	"layout(location = 0) in vec3 pos;\n"
	"layout(location = 1) in vec2 uv;\n"
	"layout(location = 2) in vec4 col;\n"
	"layout(location = 3) in vec4 ofs;\n"
	"noperspective out vec3 v_uvw; noperspective out vec4 v_col; noperspective out vec4 v_ofs;\n"
	"noperspective out float v_bad; // > 0 inside a triangle with a non-finite vertex\n"
	"uniform vec2 target;  // render target size: 640x480 screen, or a texture\n"
	"uniform vec2 xmap;    // widescreen: x' = xmap.x + (x - xmap.x) * xmap.y\n"
	"uniform vec2 umap;    // texture u' = umap.x + (u - umap.x) * umap.y\n"
	"uniform float xoff;   // added after the x mapping (copies of tiled layers)\n"
	"uniform vec2 ymap;	// y' = ymap.x + (y - ymap.x) * ymap.y (blur fill only)\n"
	"uniform float xzoom;  // the picture's shape: 16:9 is 1, narrower shapes show less of the sides\n"
	"void main() {\n"
	"  v_bad = any(isinf(pos)) || any(isnan(pos)) ? 1.0 : 0.0;\n"
	"  float iw = clamp(pos.z, 1e-7, 1e30);\n"
	"  vec2 p = clamp(pos.xy, vec2(-1e30), vec2(1e30));\n"
	"  float x = xmap.x + (p.x - xmap.x) * xmap.y + xoff;\n"
	"  x = target.x * 0.5 + (x - target.x * 0.5) * xzoom;\n"
	"  float y = ymap.x + (p.y - ymap.x) * ymap.y;\n"
	"  gl_Position = vec4(x * 2.0 / target.x - 1.0, 1.0 - y * 2.0 / target.y, 0.0, 1.0);\n"
	"  v_uvw = vec3(vec2(umap.x + (uv.x - umap.x) * umap.y, uv.y) * iw, iw);\n"
	"  v_col = col * iw; v_ofs = ofs * iw;\n"
	"}\n";

static const char *fs_src =
	"#version 330 core\n"
	"noperspective in vec3 v_uvw; noperspective in vec4 v_col; noperspective in vec4 v_ofs;\n"
	"noperspective in float v_bad;\n"
	"uniform sampler2D tex;\n"
	"uniform int mode;      // texture instruction, -1 = no texture\n"
	"uniform int flags;     // 1 use alpha, 2 ignore texture alpha, 4 offset colour, 8 cut-out edge,\n"
	"                       // 16 the edge as coverage (punch-through with MSAA)\n"
	"uniform float alpha_ref; // PT_ALPHA_REF (0-255), < 0: no alpha test\n"
	"uniform int fog;       // TSP fog mode: 1 = per vertex (offset alpha)\n"
	"uniform vec3 fog_col;  // FOG_COL_VERT\n"
	"uniform vec3 fog_col_ram; // FOG_COL_RAM (table fog)\n"
	"uniform float fog_density; // FOG_DENSITY as a number\n"
	"uniform sampler2D fog_tab; // FOG_TABLE: 128 x 2 (row 0 the low bytes, row 1 the high bytes)\n"
	"// table fog (as the PowerVR, and Flycast): the index is a log scale of 1/w * FOG_DENSITY,\n"
	"// 16 entries per power of two, interpolated between an entry's high and low byte\n"
	"float fog_table(float iw) {\n"
	"  float z = clamp(iw * fog_density, 1.0, 255.9999);\n"
	"  float e = floor(log2(z));\n"
	"  float m = z * 16.0 / exp2(e) - 16.0;\n"
	"  float i = floor(m) + e * 16.0 + 0.5;\n"
	"  return texture(fog_tab, vec2(i / 128.0, 0.75 - (m - floor(m)) / 2.0)).r;\n"
	"}\n"
	"out vec4 frag;\n"
	"void main() {\n"
	"  if (v_bad > 0.0) discard;  // the PowerVR cannot set such a triangle up and drops it\n"
	"  float w = 1.0 / v_uvw.z;\n"
	"  vec2 v_uv = v_uvw.xy * w;\n"
	"  vec4 ofs = v_ofs * w;\n"
	"  gl_FragDepth = 1.0 / (1.0 + v_uvw.z);\n"
	"  vec4 c = v_col * w;\n"
	"  if ((flags & 1) == 0) c.a = 1.0;\n"
	"  float edge = 1.0;\n"
	"  if (mode >= 0) {\n"
	"    vec4 t = texture(tex, v_uv);\n"
	"    if ((flags & 2) != 0) t.a = 1.0;\n"
	"    // an enlarged cut-out (tex.c hi_edge): the edge where its smooth alpha crosses one half,\n"
	"    // one screen pixel wide; punch-through tests the rest and keeps the edge apart\n"
	"    if ((flags & 8) != 0) {\n"
	"      edge = clamp((t.a - 0.5) / max(fwidth(t.a), 1.0 / 255.0) + 0.5, 0.0, 1.0);\n"
	"      t.a = alpha_ref >= 0.0 ? 1.0 : edge;\n"
	"    }\n"
	"    if (mode == 0) c = vec4(t.rgb, t.a);\n"
	"    else if (mode == 1) c = vec4(t.rgb * c.rgb, t.a);\n"
	"    else if (mode == 2) c = vec4(mix(c.rgb, t.rgb, t.a), c.a);\n"
	"    else c = t * c;\n"
	"    if ((flags & 4) != 0) c.rgb += ofs.rgb;\n"
	"  }\n"
	"  if (fog == 1) c.rgb = mix(c.rgb, fog_col, ofs.a);\n"
	"  else if (fog == 0) c.rgb = mix(c.rgb, fog_col_ram, fog_table(v_uvw.z));\n"
	"  else if (fog == 3) c = vec4(fog_col_ram, fog_table(v_uvw.z));\n"
	"  // the PowerVR compares 8-bit alphas: alpha 1.0 interpolated across a polygon comes out a\n"
	"  // hair below 1.0 in floats and would fail PT_ALPHA_REF 255\n"
	"  if (alpha_ref >= 0.0 && floor(c.a * 255.0 + 0.5) < alpha_ref) discard;\n"
	"  if (alpha_ref >= 0.0 && (flags & 8) != 0) {\n"
	"    if ((flags & 16) != 0) { if (edge <= 0.0) discard; c.a = edge; }  // alpha to coverage\n"
	"    else if (edge < 0.5) discard;\n"
	"  }\n"
	"  frag = clamp(c, 0.0, 1.0);\n"
	"}\n";

static GLuint shader(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, 0);
	glCompileShader(s);
	GLint ok;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok)
	{
		char log[1024];
		glGetShaderInfoLog(s, sizeof(log), 0, log);
		m1_fatal("The graphics driver rejected the game's shader:\n%s", log);
	}
	return s;
}

static void parse_shots(void)
{
	const char *s = getenv("M1_SHOTS");
	while (s && *s && nshots < 64)
	{
		char *end;
		shots[nshots++] = strtoul(s, &end, 10);
		s = *end ? end + 1 : end;
	}
}

/* ---- frame buffer, resolution, anti-aliasing, full screen --------------------------------
 * Each frame is drawn into frame_fbo at the chosen resolution (res_w x res_h, always 16:9; 0 =
 * native: the window's 16:9 area) with msaa samples (0 = off), resolved into resolve_fbo (the
 * screenshots read there) and scaled into the window. Settings: M1_RESOLUTION ("native" or
 * WxH), M1_MSAA, M1_FULLSCREEN, M1_WINDOW (window size), from [video] in voldorium.ini; the in-game
 * GRAPHICS menu (online.c) changes them through video_toggle_fullscreen, video_next_resolution
 * and video_next_msaa, which also save them. */
static GLuint frame_fbo, frame_color, frame_depth, resolve_fbo, resolve_tex;
static int frame_w, frame_h, frame_samples = -1;
static int res_w, res_h, msaa, max_samples = 8;
static int res_pct;	/* resolution = 77% ... 50%: that share of native, upscaled with FSR (0: native or WxH) */
static const int fsr_pcts[] = { 77, 67, 59, 50 };	/* FSR 1's ultra quality, quality, balanced, performance */

void config_set(const char *section, const char *key, const char *value);
void config_save(void);
int video_text_sharp(void);
int video_text_mode(void);
int video_portraits(void);
static int a4k_ready(void);	/* Anime4K picture enlargement (below) */
static void auto_quality(void);	/* first start: HI-RES PICTURES and TEXT for the graphics card (below) */

static void video_settings_load(void)
{
	const char *r = getenv("M1_RESOLUTION");
	if (r && sscanf(r, "%dx%d", &res_w, &res_h) != 2)
		res_w = res_h = 0;
	res_pct = r && strchr(r, '%') ? atoi(r) : 0;
	if (res_pct < 25 || res_pct >= 100)
		res_pct = 0;
	msaa = getenv("M1_MSAA") ? atoi(getenv("M1_MSAA")) : 0;
}

static void frame_target(int w, int h)
{
	const int samples = msaa > max_samples ? max_samples : msaa < 2 ? 0 : msaa;
	if (w == frame_w && h == frame_h && samples == frame_samples)
		return;
	frame_w = w, frame_h = h, frame_samples = samples;
	if (!frame_fbo)
	{
		glGenFramebuffers(1, &frame_fbo);
		glGenRenderbuffers(1, &frame_color);
		glGenRenderbuffers(1, &frame_depth);
		glGenFramebuffers(1, &resolve_fbo);
		glGenTextures(1, &resolve_tex);
	}
	glBindRenderbuffer(GL_RENDERBUFFER, frame_color);
	glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, w, h);
	glBindRenderbuffer(GL_RENDERBUFFER, frame_depth);
	glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, w, h);
	glBindFramebuffer(GL_FRAMEBUFFER, frame_fbo);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, frame_color);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, frame_depth);
	glBindTexture(GL_TEXTURE_2D, resolve_tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, resolve_fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, resolve_tex, 0);
	printf("video: drawing at %dx%d, %s\n", w, h, samples ? (samples == 2 ? "2x MSAA" : samples == 4 ? "4x MSAA" : "8x MSAA") : "no MSAA");
}

/* ---- FSR 1 upscaling ---------------------------------------------------------------------
 * A picture drawn smaller than its area in the window (resolution 77% to 50% of native, or a
 * fixed size below it) is upscaled with AMD FidelityFX Super Resolution 1: EASU (edge-adaptive
 * upscale, 12 texels per pixel) into fsr_tex at the window's size, then RCAS (contrast-adaptive
 * sharpening) into the window. Ported to GLSL 330 from AMD's ffx_fsr1.h (FSR 1.0, MIT licence:
 * Copyright (c) 2021 Advanced Micro Devices, Inc.), the 32-bit float paths, without RCAS's
 * optional noise removal. [video] upscale = linear (M1_UPSCALE) keeps plain bilinear scaling.
 *
 * FSR 1 licence: Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 * and associated documentation files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions: The above copyright
 * notice and this permission notice shall be included in all copies or substantial portions of
 * the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
 * PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE. */
static const char *fsr_vs =
	"#version 330 core\n"
	"layout(location = 0) in vec2 p;\n"
	"void main() { gl_Position = vec4(p, 0.0, 1.0); }\n";
static const char *easu_fs =
	"#version 330 core\n"
	"out vec4 o;\n"
	"uniform sampler2D src; uniform vec2 origin, scale; uniform ivec2 isize;\n"
	"vec3 T(ivec2 p) { return texelFetch(src, clamp(p, ivec2(0), isize - 1), 0).rgb; }\n"
	"float L(vec3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }\n"
	"void Set(inout vec2 dir, inout float len, float w, float a, float b, float c, float d, float e) {\n"
	"  float dc = d - c, cb = c - b;\n"
	"  float lx = 1.0 / max(max(abs(dc), abs(cb)), 1e-8);\n"
	"  float dx = d - b; dir.x += dx * w; lx = clamp(abs(dx) * lx, 0.0, 1.0); len += lx * lx * w;\n"
	"  float ec = e - c, ca = c - a;\n"
	"  float ly = 1.0 / max(max(abs(ec), abs(ca)), 1e-8);\n"
	"  float dy = e - a; dir.y += dy * w; ly = clamp(abs(dy) * ly, 0.0, 1.0); len += ly * ly * w;\n"
	"}\n"
	"void Tap(inout vec3 ac, inout float aw, vec2 off, vec2 dir, vec2 len2, float lob, float clp, vec3 c) {\n"
	"  vec2 v = vec2(off.x * dir.x + off.y * dir.y, off.x * -dir.y + off.y * dir.x) * len2;\n"
	"  float d2 = min(dot(v, v), clp);\n"
	"  float wB = 0.4 * d2 - 1.0, wA = lob * d2 - 1.0;\n"
	"  wB *= wB; wA *= wA; wB = 1.5625 * wB - 0.5625;\n"
	"  float w = wB * wA; ac += c * w; aw += w;\n"
	"}\n"
	"void main() {\n"
	"  vec2 pp = (gl_FragCoord.xy - origin) * scale - 0.5;\n"
	"  vec2 fp = floor(pp); pp -= fp; ivec2 q = ivec2(fp);\n"
	"  vec3 b = T(q + ivec2(0, -1)), c = T(q + ivec2(1, -1));\n"
	"  vec3 e = T(q + ivec2(-1, 0)), f = T(q), g = T(q + ivec2(1, 0)), h = T(q + ivec2(2, 0));\n"
	"  vec3 i = T(q + ivec2(-1, 1)), j = T(q + ivec2(0, 1)), k = T(q + ivec2(1, 1)), l = T(q + ivec2(2, 1));\n"
	"  vec3 n = T(q + ivec2(0, 2)), m = T(q + ivec2(1, 2));\n"
	"  float bL = L(b), cL = L(c), eL = L(e), fL = L(f), gL = L(g), hL = L(h);\n"
	"  float iL = L(i), jL = L(j), kL = L(k), lL = L(l), nL = L(n), mL = L(m);\n"
	"  vec2 dir = vec2(0.0); float len = 0.0;\n"
	"  Set(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);\n"
	"  Set(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);\n"
	"  Set(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);\n"
	"  Set(dir, len, pp.x * pp.y, gL, jL, kL, lL, mL);\n"
	"  float dirR = dot(dir, dir); bool zro = dirR < 1.0 / 32768.0;\n"
	"  dirR = zro ? 1.0 : inversesqrt(dirR); dir.x = zro ? 1.0 : dir.x; dir *= dirR;\n"
	"  len = len * 0.5; len *= len;\n"
	"  float stretch = dot(dir, dir) / max(abs(dir.x), abs(dir.y));\n"
	"  vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);\n"
	"  float lob = 0.5 + (0.25 - 0.04 - 0.5) * len, clp = 1.0 / lob;\n"
	"  vec3 mn = min(min(f, g), min(j, k)), mx = max(max(f, g), max(j, k));\n"
	"  vec3 ac = vec3(0.0); float aw = 0.0;\n"
	"  Tap(ac, aw, vec2(0.0, -1.0) - pp, dir, len2, lob, clp, b);\n"
	"  Tap(ac, aw, vec2(1.0, -1.0) - pp, dir, len2, lob, clp, c);\n"
	"  Tap(ac, aw, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, i);\n"
	"  Tap(ac, aw, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, j);\n"
	"  Tap(ac, aw, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, f);\n"
	"  Tap(ac, aw, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, e);\n"
	"  Tap(ac, aw, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, k);\n"
	"  Tap(ac, aw, vec2(2.0, 1.0) - pp, dir, len2, lob, clp, l);\n"
	"  Tap(ac, aw, vec2(2.0, 0.0) - pp, dir, len2, lob, clp, h);\n"
	"  Tap(ac, aw, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, g);\n"
	"  Tap(ac, aw, vec2(1.0, 2.0) - pp, dir, len2, lob, clp, m);\n"
	"  Tap(ac, aw, vec2(0.0, 2.0) - pp, dir, len2, lob, clp, n);\n"
	"  o = vec4(min(mx, max(mn, ac / aw)), 1.0);\n"
	"}\n";
static const char *rcas_fs =
	"#version 330 core\n"
	"out vec4 o;\n"
	"uniform sampler2D src; uniform vec2 origin; uniform ivec2 isize; uniform float sharp;\n"
	"vec3 T(ivec2 p) { return texelFetch(src, clamp(p, ivec2(0), isize - 1), 0).rgb; }\n"
	"void main() {\n"
	"  ivec2 q = ivec2(gl_FragCoord.xy - origin);\n"
	"  vec3 b = T(q + ivec2(0, -1)), d = T(q + ivec2(-1, 0)), e = T(q), f = T(q + ivec2(1, 0)), h = T(q + ivec2(0, 1));\n"
	"  vec3 mn4 = min(min(b, d), min(f, h)), mx4 = max(max(b, d), max(f, h));\n"
	"  vec3 hitMin = min(mn4, e) / max(4.0 * mx4, vec3(1e-5));\n"
	"  vec3 hitMax = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, vec3(-1e-5));\n"
	"  vec3 lobeC = max(-hitMin, hitMax);\n"
	"  float lobe = max(-0.1875, min(max(lobeC.r, max(lobeC.g, lobeC.b)), 0.0)) * sharp;\n"
	"  o = vec4(clamp((lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0), 0.0, 1.0), 1.0);\n"
	"}\n";

static GLuint fsr_easu, fsr_rcas, fsr_vao, fsr_vbo, fsr_fbo, fsr_tex;
static GLint u_e_src, u_e_origin, u_e_scale, u_e_isize, u_r_src, u_r_origin, u_r_isize, u_r_sharp;
static int fsr_w, fsr_h, fsr_state;	/* fsr_state: 0 not tried, 1 ready, -1 off (linear, or a shader failed) */

static GLuint fsr_shader(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, 0);
	glCompileShader(s);
	GLint ok;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok)
	{
		char log[1024];
		glGetShaderInfoLog(s, sizeof(log), 0, log);
		printf("video: FSR shader rejected, upscaling bilinear instead:\n%s\n", log);
		return 0;
	}
	return s;
}

static int fsr_ready(void)
{
	if (fsr_state)
		return fsr_state > 0;
	fsr_state = -1;
	const char *u = getenv("M1_UPSCALE");
	if (u && !strcmp(u, "linear"))
		return 0;
	const GLuint vs = fsr_shader(GL_VERTEX_SHADER, fsr_vs), es = fsr_shader(GL_FRAGMENT_SHADER, easu_fs),
				 rs = fsr_shader(GL_FRAGMENT_SHADER, rcas_fs);
	if (!vs || !es || !rs)
		return 0;
	fsr_easu = glCreateProgram();
	glAttachShader(fsr_easu, vs);
	glAttachShader(fsr_easu, es);
	glLinkProgram(fsr_easu);
	fsr_rcas = glCreateProgram();
	glAttachShader(fsr_rcas, vs);
	glAttachShader(fsr_rcas, rs);
	glLinkProgram(fsr_rcas);
	u_e_src = glGetUniformLocation(fsr_easu, "src");
	u_e_origin = glGetUniformLocation(fsr_easu, "origin");
	u_e_scale = glGetUniformLocation(fsr_easu, "scale");
	u_e_isize = glGetUniformLocation(fsr_easu, "isize");
	u_r_src = glGetUniformLocation(fsr_rcas, "src");
	u_r_origin = glGetUniformLocation(fsr_rcas, "origin");
	u_r_isize = glGetUniformLocation(fsr_rcas, "isize");
	u_r_sharp = glGetUniformLocation(fsr_rcas, "sharp");
	static const float tri[6] = { -1, -1, 3, -1, -1, 3 };	/* one triangle covers the viewport */
	glGenVertexArrays(1, &fsr_vao);
	glBindVertexArray(fsr_vao);
	glGenBuffers(1, &fsr_vbo);
	glBindBuffer(GL_ARRAY_BUFFER, fsr_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(tri), tri, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, (void *)0);
	glEnableVertexAttribArray(0);
	glGenFramebuffers(1, &fsr_fbo);
	glGenTextures(1, &fsr_tex);
	fsr_state = 1;
	printf("video: FSR 1 upscaling ready\n");
	return 1;
}

/* resolve_tex (rw x rh) into the window's area x0, y0, vw x vh; 0: not done (bilinear instead) */
static int fsr_present(int rw, int rh, int x0, int y0, int vw, int vh)
{
	if (!fsr_ready())
		return 0;
	GLint active, tex0;
	glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
	glActiveTexture(GL_TEXTURE0);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);
	const GLboolean depth = glIsEnabled(GL_DEPTH_TEST), blend = glIsEnabled(GL_BLEND), stencil = glIsEnabled(GL_STENCIL_TEST),
					cull = glIsEnabled(GL_CULL_FACE), scissor = glIsEnabled(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_SCISSOR_TEST);
	if (vw != fsr_w || vh != fsr_h)
	{
		fsr_w = vw, fsr_h = vh;
		glBindTexture(GL_TEXTURE_2D, fsr_tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, vw, vh, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glBindFramebuffer(GL_FRAMEBUFFER, fsr_fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fsr_tex, 0);
	}
	glBindVertexArray(fsr_vao);
	/* EASU: the drawn picture, enlarged into fsr_tex */
	glBindFramebuffer(GL_FRAMEBUFFER, fsr_fbo);
	glViewport(0, 0, vw, vh);
	glUseProgram(fsr_easu);
	glBindTexture(GL_TEXTURE_2D, resolve_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);	/* no mipmaps: complete for texelFetch */
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glUniform1i(u_e_src, 0);
	glUniform2f(u_e_origin, 0, 0);
	glUniform2f(u_e_scale, (float)rw / (float)vw, (float)rh / (float)vh);
	glUniform2i(u_e_isize, rw, rh);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	/* RCAS: sharpened into the window (sharpness 0.2 stops, AMD's default: 2^-0.2) */
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(x0, y0, vw, vh);
	glUseProgram(fsr_rcas);
	glBindTexture(GL_TEXTURE_2D, fsr_tex);
	glUniform1i(u_r_src, 0);
	glUniform2f(u_r_origin, (float)x0, (float)y0);
	glUniform2i(u_r_isize, vw, vh);
	glUniform1f(u_r_sharp, 0.87055f);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	/* the frame's own state again */
	glUseProgram(prog);
	glBindVertexArray(vao);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBindTexture(GL_TEXTURE_2D, (GLuint)tex0);
	glActiveTexture((GLenum)active);
	if (depth) glEnable(GL_DEPTH_TEST);
	if (blend) glEnable(GL_BLEND);
	if (stencil) glEnable(GL_STENCIL_TEST);
	if (cull) glEnable(GL_CULL_FACE);
	if (scissor) glEnable(GL_SCISSOR_TEST);
	return 1;
}

/* the monitor's size (the desktop mode of the window's display) */
static void display_size(int *w, int *h)
{
	SDL_DisplayMode m;
	if (!win || SDL_GetDesktopDisplayMode(SDL_GetWindowDisplayIndex(win), &m))
		*w = 1920, *h = 1080;
	else
		*w = m.w, *h = m.h;
}

/* a picture's shape (width / height) for a w x h area: 4:3 to 16:9 (wider screens get bars at
   the sides, narrower ones at the top and bottom) */
static float picture_aspect(int w, int h)
{
	const float a = h > 0 ? (float)w / (float)h : 16.0f / 9;
	return a > 16.0f / 9 ? 16.0f / 9 : a < 4.0f / 3 ? 4.0f / 3 : a;
}

/* a window: the chosen resolution's size (native: M1_WINDOW or 1280x720), shrunk to fit the
   screen's usable area */
static void window_fit(void)
{
	int w = 1280, h = 720;
	if (res_w)
		w = res_w, h = res_h;
	else if (getenv("M1_WINDOW"))
		sscanf(getenv("M1_WINDOW"), "%dx%d", &w, &h);
	SDL_Rect u;
	if (!SDL_GetDisplayUsableBounds(SDL_GetWindowDisplayIndex(win), &u) && (w > u.w * 9 / 10 || h > u.h * 9 / 10))
	{
		const float a = picture_aspect(w, h);
		h = u.h * 9 / 10;
		w = (int)(h * a + 0.5f);
		if (w > u.w * 9 / 10)
			w = u.w * 9 / 10, h = (int)(w / a + 0.5f);
	}
	SDL_SetWindowSize(win, w, h);
	SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}

int video_is_fullscreen(void) { return win && (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0; }

/* GRAPHICS' UPDATE line (online.c): the release page in the browser; a full screen game is
   minimized first, so the browser is not hidden behind it */
void video_open_url(const char *url)
{
	if (win && video_is_fullscreen())
		SDL_MinimizeWindow(win);
#ifdef _WIN32
	const int ok = SDL_OpenURL(url) == 0;
#else
	/* xdg-open, not SDL_OpenURL: the player's SDL2 may be older than 2.0.14 */
	pid_t pid;
	char *argv[] = { "xdg-open", (char *)url, 0 };
	const int ok = posix_spawnp(&pid, "xdg-open", 0, 0, argv, environ) == 0;
#endif
	printf("open %s: %s\n", url, ok ? "ok" : "failed");
}

/* a GRAPHICS change also goes into this process's environment: a restart into or out of an
   online match (online.c relaunch) inherits it, and the settings file is only read where the
   environment has nothing */
static void vsync_apply(void);
static void setting_env(const char *name, const char *value)
{
#ifdef _WIN32
	_putenv_s(name, value);
#else
	setenv(name, value, 1);
#endif
}

void video_toggle_fullscreen(void)
{
	if (!win)
		return;
	const int full = !video_is_fullscreen();
	SDL_SetWindowFullscreen(win, full ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
	if (!full)
		window_fit();
	vsync_apply();	/* the display's refresh rate may differ in the other mode */
	config_set("video", "fullscreen", full ? "1" : "0");
	config_save();
	setting_env("M1_FULLSCREEN", full ? "1" : "0");
}

/* the resolutions offered: native (the screen's shape, 4:3 to 16:9), then the usual 16:9, 16:10
   and 4:3 sizes up to the monitor's, by height */
static const int sizes[][2] = { { 1280, 720 }, { 1024, 768 }, { 1280, 800 }, { 1600, 900 }, { 1680, 1050 }, { 1920, 1080 },
								{ 1440, 1080 }, { 1920, 1200 }, { 1600, 1200 }, { 2560, 1440 }, { 1920, 1440 }, { 2560, 1600 },
								{ 3200, 1800 }, { 3840, 2160 }, { 2880, 2160 }, { 3840, 2400 }, { 5120, 2880 }, { 7680, 4320 } };

void video_resolution_label(char *buf, size_t n)
{
	int w, h;
	display_size(&w, &h);
	if (win && !video_is_fullscreen())
		SDL_GL_GetDrawableSize(win, &w, &h);	/* in a window, native is the window's size */
	if (res_w)
		snprintf(buf, n, "%d X %d", res_w, res_h);
	else
	{
		/* the screen's (or window's) size in the picture's shape */
		const float a = picture_aspect(w, h);
		const int pw = (int)(h * a + 0.5f) > w ? w : (int)(h * a + 0.5f), ph = (int)(pw / a + 0.5f) > h ? h : (int)(pw / a + 0.5f);
		if (res_pct)
			snprintf(buf, n, "FSR %s", res_pct >= 77 ? "ULTRA QUALITY" : res_pct >= 67 ? "QUALITY" : res_pct >= 59 ? "BALANCED" : "PERFORMANCE");
		else
			snprintf(buf, n, "NATIVE %d X %d", pw, ph);
	}
}

/* the resolution line's help: with FSR, the size the game is drawn at */
void video_resolution_help(char *buf, size_t n)
{
	if (res_w || !res_pct)
	{
		snprintf(buf, n, "Resolution the game draws at. Select to change.");
		return;
	}
	int w, h;
	display_size(&w, &h);
	if (win && !video_is_fullscreen())
		SDL_GL_GetDrawableSize(win, &w, &h);
	const float a = picture_aspect(w, h);
	const int pw = (int)(h * a + 0.5f) > w ? w : (int)(h * a + 0.5f), ph = (int)(pw / a + 0.5f) > h ? h : (int)(pw / a + 0.5f);
	snprintf(buf, n, "Drawn at %d x %d and sharpened up to your screen.", pw * res_pct / 100, ph * res_pct / 100);
}

/* native, FSR 77%, 67%, 59%, 50% (of native), then the sizes up to the monitor's, then native */
void video_next_resolution(void)
{
	int dw, dh;
	display_size(&dw, &dh);
	const int n = (int)(sizeof(sizes) / sizeof(sizes[0])), np = (int)(sizeof(fsr_pcts) / sizeof(fsr_pcts[0]));
	char v[32] = "native";
	if (!res_w)
	{
		int p = -1;
		for (int i = 0; i < np; i++)
			if (fsr_pcts[i] == res_pct)
				p = i;
		if (p + 1 < np)
		{
			res_pct = fsr_pcts[p + 1];
			snprintf(v, sizeof(v), "%d%%", res_pct);
			if (win && !video_is_fullscreen())
				window_fit();
			config_set("video", "resolution", v);
			config_save();
			setting_env("M1_RESOLUTION", v);
			return;
		}
		res_pct = 0;	/* after 50%: the first fixed size */
	}
	int cur = -1;
	for (int i = 0; i < n; i++)
		if (sizes[i][0] == res_w && sizes[i][1] == res_h)
			cur = i;
	int next = -1;
	for (int i = cur + 1; i < n; i++)
		if (sizes[i][0] <= dw && sizes[i][1] <= dh)
		{
			next = i;
			break;
		}
	if (next < 0)
		res_w = res_h = 0;	/* after the largest: back to native */
	else
	{
		res_w = sizes[next][0], res_h = sizes[next][1];
		snprintf(v, sizeof(v), "%dx%d", res_w, res_h);
	}
	if (win && !video_is_fullscreen())
		window_fit();
	config_set("video", "resolution", v);
	config_save();
	setting_env("M1_RESOLUTION", v);
}

int video_msaa(void) { return msaa < 2 ? 0 : msaa > max_samples ? max_samples : msaa; }

/* VSync ([video] vsync, M1_VSYNC; on by default). Frames are paced by the 60 Hz clock (hw.c
   pace_frame, netplay.c wait_next_frame); with VSync each swap also waits for the display's
   refresh, so a frame is never shown half drawn. On a display of about 60 Hz the two clocks
   would drift apart and drop a frame every few seconds, so there the clock runs a little fast
   (video_pace_hz 62) and the display's refresh sets the pace; on faster displays (120 Hz and
   up) the clock keeps 60 and the swap waits at most one short refresh, and G-Sync / FreeSync in
   full screen show each frame when it is ready. Unpaced runs (M1_FAST) never wait. */
static int vsync_want = -1, vsync_on, vsync_adaptive, display_hz;
static void vsync_apply(void)
{
	if (vsync_want < 0)
		vsync_want = !getenv("M1_VSYNC") || atoi(getenv("M1_VSYNC"));
	const int fast = getenv("M1_FAST") != 0;
	/* adaptive where the driver has it (-1, EXT_swap_control_tear): a frame that is late shows at
	   once instead of waiting a whole refresh, which in a rollback match (re-simulated frames)
	   would slow the game down; plain VSync otherwise */
	vsync_adaptive = 0;
	vsync_on = 0;
	char why[160] = "";
	if (vsync_want && !fast)
	{
		vsync_adaptive = SDL_GL_SetSwapInterval(-1) == 0;
		vsync_on = vsync_adaptive || SDL_GL_SetSwapInterval(1) == 0;
		if (!vsync_on)
			snprintf(why, sizeof(why), ", the driver refused it: %s", SDL_GetError());
	}
	else
		snprintf(why, sizeof(why), fast ? ", unpaced run" : ", VSYNC OFF in the settings");
	if (!vsync_on)
		SDL_GL_SetSwapInterval(0);
	SDL_DisplayMode m;
	display_hz = win && SDL_GetWindowDisplayMode(win, &m) == 0 ? m.refresh_rate : 0;
	if (win && SDL_GetCurrentDisplayMode(SDL_GetWindowDisplayIndex(win), &m) == 0 && m.refresh_rate)
		display_hz = m.refresh_rate;
	printf("video: vsync %s (display %d Hz%s)\n", vsync_on ? (vsync_adaptive ? "adaptive" : "on") : "off", display_hz, why);
}
int video_vsync(void) { return vsync_want < 0 ? 1 : vsync_want; }
/* the rate the frame clock runs at: 62 when a VSync'd display of about 60 Hz sets the pace */
double video_pace_hz(void)
{
	return vsync_on && display_hz >= 58 && display_hz <= 62 ? 62.0 : 60.0;
}
void video_next_vsync(void)
{
	vsync_want = !video_vsync();
	if (win)
		vsync_apply();
	config_set("video", "vsync", vsync_want ? "1" : "0");
	config_save();
	setting_env("M1_VSYNC", vsync_want ? "1" : "0");
}

void video_next_msaa(void)
{
	msaa = video_msaa() == 0 ? 2 : video_msaa() * 2;
	if (msaa > max_samples || msaa > 8)
		msaa = 0;
	char v[8];
	snprintf(v, sizeof(v), "%d", msaa);
	config_set("video", "msaa", v);
	config_save();
	setting_env("M1_MSAA", v);
}

/* No window: development runs go on headless; the player form says why and stops (running on
   invisibly looks like the game never started) */
static int video_fail(const char *what)
{
	if (!m1_verbose)
		m1_fatal("This PC could not start the game's graphics (%s: %s).\n\n"
				 "The port needs OpenGL 3.3: update the graphics driver. Remote Desktop sessions and some "
				 "virtual machines do not provide it.", what, SDL_GetError());
	fprintf(stderr, "%s: %s (running headless)\n", what, SDL_GetError());
	return enabled = 0;
}

/* game controllers: the first one plays with the keyboard as player 1, the second is player 2
   (port B, present while it is connected) */
static void pad_added(int index)
{
	if (!SDL_IsGameController(index))
		return;
	const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(index);
	for (int k = 0; k < 2; k++)
		if (pads[k] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[k])) == id)
			return;	/* already open */
	for (int k = 0; k < 2; k++)
		if (!pads[k])
		{
			pads[k] = SDL_GameControllerOpen(index);
			return;
		}
}
static void pad_removed(SDL_JoystickID id)
{
	for (int k = 0; k < 2; k++)
		if (pads[k] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[k])) == id)
		{
			SDL_GameControllerClose(pads[k]);
			pads[k] = 0;
		}
}
/* what the shoulder buttons and triggers press ([pad] lb rb lt rt in voldorium.ini): Dreamcast
   buttons joined with +, e.g. "a+b" (the game's Button Config then maps them to moves). The
   default is the Dreamcast's L (lb, lt) and R (rb, rt). A combination is one input to netplay,
   like any button. */
const char *config_get(const char *section, const char *key);
static u32 pad_combo_bits(const char *s, u32 def)
{
	if (!s || !*s)
		return def;
	static const struct { const char *name; u32 bit; } names[] = {
		{ "a", 0x0004 }, { "b", 0x0002 }, { "x", 0x0400 }, { "y", 0x0200 }, { "l", 0x1000 }, { "r", 0x2000 },
		{ "start", 0x0008 },
	};
	u32 b = 0;
	char buf[64];
	snprintf(buf, sizeof(buf), "%s", s);
	for (char *t = strtok(buf, "+ \t"); t; t = strtok(0, "+ \t"))
	{
		u32 bit = 0;
		for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
			if (!SDL_strcasecmp(t, names[i].name))
				bit = names[i].bit;
		if (!bit)
		{
			printf("voldorium.ini [pad]: unknown button \"%s\" in \"%s\" (a b x y l r start)\n", t, s);
			return def;
		}
		b |= bit;
	}
	return b;
}
static u32 pad_combo[4];	/* lb, rb, lt, rt */
static void pad_combos_load(void)
{
	static const char *const keys[4] = { "lb", "rb", "lt", "rt" };
	static const u32 defs[4] = { 0x1000, 0x2000, 0x1000, 0x2000 };
	for (int i = 0; i < 4; i++)
	{
		char env[16];
		snprintf(env, sizeof(env), "M1_PAD_%c%c", keys[i][0] - 32, keys[i][1] - 32);	/* M1_PAD_LT (tests) */
		const char *v = getenv(env);
		pad_combo[i] = pad_combo_bits(v ? v : config_get("pad", keys[i]), defs[i]);
	}
	printf("pad: lb %04x rb %04x lt %04x rt %04x (Dreamcast button bits)\n", pad_combo[0], pad_combo[1], pad_combo[2], pad_combo[3]);
}

static u32 pad_bits(SDL_GameController *pad)
{
	if (!pad)
		return 0;
	if (!pad_combo[0])
		pad_combos_load();
	u32 b = 0;
	/* left stick as the D-pad */
	const int dz = 16000;
	const int ax = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX), ay = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
	if (ax < -dz) b |= 0x0040;
	if (ax > dz) b |= 0x0080;
	if (ay < -dz) b |= 0x0010;
	if (ay > dz) b |= 0x0020;
	/* triggers and shoulder buttons: [pad] lt rt lb rb */
	if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > dz) b |= pad_combo[2];
	if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > dz) b |= pad_combo[3];
	if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) b |= pad_combo[0];
	if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) b |= pad_combo[1];
	static const struct { int btn; u32 bit; } map[] = {
		{ SDL_CONTROLLER_BUTTON_A, 0x0004 }, { SDL_CONTROLLER_BUTTON_B, 0x0002 },
		{ SDL_CONTROLLER_BUTTON_X, 0x0400 }, { SDL_CONTROLLER_BUTTON_Y, 0x0200 },
		{ SDL_CONTROLLER_BUTTON_START, 0x0008 }, { SDL_CONTROLLER_BUTTON_DPAD_UP, 0x0010 },
		{ SDL_CONTROLLER_BUTTON_DPAD_DOWN, 0x0020 }, { SDL_CONTROLLER_BUTTON_DPAD_LEFT, 0x0040 },
		{ SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 0x0080 },
	};
	for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		if (SDL_GameControllerGetButton(pad, map[i].btn))
			b |= map[i].bit;
	return b;
}

int video_enabled(void)
{
	if (enabled >= 0)
		return enabled;
	enabled = getenv("M1_HEADLESS") == 0;
	parse_shots();
	if (!enabled)
		return 0;
	SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");	/* the window shows the executable's icon (m1/voldorium.rc) */
	SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
	/* the screen's real pixels: without this, Windows display scaling (125%, 150%...) shrinks
	   the desktop the game sees (1920x1080 at 150% looks like 1280x720) and stretches the
	   smaller picture back up */
	SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0)
		return video_fail("SDL");
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);	/* modifier volumes (shadows) */
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	/* the picture is drawn into a frame buffer of its own (resolution and anti-aliasing,
	   see "frame buffer" below); the window only shows it */
	video_settings_load();
	pad_combos_load();
	int win_w = 1280, win_h = 720;
	if (getenv("M1_WINDOW"))
		sscanf(getenv("M1_WINDOW"), "%dx%d", &win_w, &win_h);
	const int full = getenv("M1_FULLSCREEN") && atoi(getenv("M1_FULLSCREEN"));
	const Uint32 win_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | (full ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
	win = SDL_CreateWindow("Voldorium ver. " VOLDORIUM_VERSION, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, win_w, win_h, win_flags);
	if (win)
	{
		if (!full)
			window_fit();
		glc = SDL_GL_CreateContext(win);
	}
	if (!win)
		return video_fail("window");
	if (!glc)
		return video_fail("OpenGL 3.3 context");
	if (!gl_load())
		return video_fail("OpenGL functions");
	vsync_apply();	/* frames are paced by the emulated V Blank; VSync only times the swap */
	printf("video: %s / %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
	{
		int dw, dh;
		display_size(&dw, &dh);
#ifdef _WIN32
		printf("video: display %dx%d, %s\n", dw, dh, IsProcessDPIAware() ? "DPI aware" : "scaled by Windows");
#else
		printf("video: display %dx%d\n", dw, dh);
#endif
	}
	glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
	prog = glCreateProgram();
	glAttachShader(prog, shader(GL_VERTEX_SHADER, vs_src));
	glAttachShader(prog, shader(GL_FRAGMENT_SHADER, fs_src));
	glLinkProgram(prog);
	glUseProgram(prog);
	u_tex = glGetUniformLocation(prog, "tex");
	u_mode = glGetUniformLocation(prog, "mode");
	u_flags = glGetUniformLocation(prog, "flags");
	u_alpha_ref = glGetUniformLocation(prog, "alpha_ref");
	u_fog = glGetUniformLocation(prog, "fog");
	u_fog_col = glGetUniformLocation(prog, "fog_col");
	u_fog_col_ram = glGetUniformLocation(prog, "fog_col_ram");
	u_fog_density = glGetUniformLocation(prog, "fog_density");
	u_fog_tab = glGetUniformLocation(prog, "fog_tab");
	glUniform1i(u_fog_tab, 1);	/* texture unit 1 */
	u_target = glGetUniformLocation(prog, "target");
	u_ymap = glGetUniformLocation(prog, "ymap");
	glUniform2f(u_ymap, 0, 1);
	u_xmap = glGetUniformLocation(prog, "xmap");
	u_umap = glGetUniformLocation(prog, "umap");
	u_xoff = glGetUniformLocation(prog, "xoff");
	u_xzoom = glGetUniformLocation(prog, "xzoom");
	glUniform1f(u_xzoom, 1);
	glUniform1i(u_tex, 0);
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(TaVert), (void *)0);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(TaVert), (void *)12);
	/* colours are ARGB words (TaVert): bytes B, G, R, A, so the BGRA layout gives the shader RGBA */
	glVertexAttribPointer(2, GL_BGRA, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(TaVert), (void *)20);
	glVertexAttribPointer(3, GL_BGRA, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(TaVert), (void *)24);
	for (int i = 0; i < 4; i++)
		glEnableVertexAttribArray(i);
	auto_quality();	/* first start: HI-RES PICTURES and TEXT for this graphics card */
	/* Anime4K's shaders are compiled now, not at the first picture (half a second) */
	if ((video_portraits() || video_text_mode() == 0) && a4k_ready())
	{
		glUseProgram(prog);
		glBindVertexArray(vao);
		glBindBuffer(GL_ARRAY_BUFFER, vbo);
	}
	for (int i = 0; i < SDL_NumJoysticks(); i++)
		pad_added(i);
	return enabled;
}

/* a texture's pixels; returns its texture name. A texture re-uploaded (animated textures, palette
   cycling: five a frame in Inferno's defeat, issue #11) gets a new texture and the old one is
   deleted, which the driver frees once the GPU is done with it. Writing into the old one
   (glTexSubImage2D) made Mesa wait for the GPU to finish every draw already sent with it: five
   waits a frame, 43 to 62 ms of drawing per frame on a Steam Deck (issue #19). Polygons waiting
   in the draw batch are drawn first (state_invalidate): one of them may use the old contents, and
   a new texture can get the old one's name back. */
static void state_invalidate(void);
static u64 gp_upload[2];	/* M1_GPUPROF=2: CPU time in texture uploads (other phases, translucent) */
static u32 gp_uploads[2];
static int gp_level(void);
static u32 gp_phase;
unsigned gl_upload_texture(unsigned id, u32 w, u32 h, const u32 *rgba)
{
	const u64 t0 = gp_level() >= 2 ? SDL_GetPerformanceCounter() : 0;
	state_invalidate();
	if (id)
		glDeleteTextures(1, &id);
	glGenTextures(1, &id);
	glBindTexture(GL_TEXTURE_2D, id);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	if (t0)
		gp_upload[gp_phase == 3] += SDL_GetPerformanceCounter() - t0, gp_uploads[gp_phase == 3]++;
	return id;
}

/* ---- Anime4K: 2D pictures enlarged by a small neural network ----------------------------------
 * Opaque 2D pictures drawn above 640x480 (the character select portraits) are enlarged when they
 * are loaded (tex.c tex_get_hi) by Anime4K's Upscale CNN x2 (VL), an mpv user shader (MIT
 * licence, a4k_shaders.h made by tools/a4k_shaders.py), run on the GPU once per picture: each
 * run doubles the size, two runs give the 4 times tex.c asks for. The passes' GLSL is kept as is;
 * this adds what mpv gives a shader around it (NAME_tex, _pos, _pt, _size, _texOff for each bound
 * texture) and runs the passes into half-float textures. The colour comes from the network, the
 * picture is opaque. [video] portraits = 0 (M1_A4K=0, GRAPHICS menu HI-RES PICTURES OFF): off,
 * the cubic enlargement instead (or the original pixels with [video] text = sharp). */
#include "a4k_shaders.h"
#define A4K_PASSES (int)(sizeof(a4k_passes) / sizeof(a4k_passes[0]))
static GLuint a4k_prog[A4K_PASSES], a4k_out_prog, a4k_vao, a4k_vbo, a4k_fbo;
static int a4k_state;	/* 0 not tried, 1 ready, -1 off (M1_A4K=0, or a shader failed) */

static GLuint a4k_shader(GLenum type, const char *src, const char *what)
{
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, 0);
	glCompileShader(s);
	GLint ok;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok)
	{
		char log[1024];
		glGetShaderInfoLog(s, sizeof(log), 0, log);
		printf("video: Anime4K pass %s rejected, pictures enlarged plainly instead:\n%s\n", what, log);
		return 0;
	}
	return s;
}

static GLuint a4k_link(GLuint vs, GLuint fs, int nbind, const char *const *bind)
{
	const GLuint p = glCreateProgram();
	glAttachShader(p, vs);
	glAttachShader(p, fs);
	glLinkProgram(p);
	glUseProgram(p);
	for (int i = 0; i < nbind; i++)
	{
		char name[64];
		snprintf(name, sizeof(name), "%s_raw", bind[i]);
		glUniform1i(glGetUniformLocation(p, name), i);	/* texture unit i */
	}
	return p;
}

static int a4k_ready(void)
{
	if (a4k_state)
		return a4k_state > 0;
	a4k_state = -1;	/* [video] portraits = 0 (M1_A4K=0) never calls it (video_portraits) */
	const clock_t t0 = clock();
	static const char *vs_src =
		"#version 330 core\n"
		"layout(location = 0) in vec2 p;\n"
		"out vec2 v_pos;\n"
		"void main() { v_pos = p * 0.5 + 0.5; gl_Position = vec4(p, 0.0, 1.0); }\n";
	const GLuint vs = a4k_shader(GL_VERTEX_SHADER, vs_src, "vertex");
	if (!vs)
		return 0;
	static char src[64 * 1024];
	for (int i = 0; i < A4K_PASSES; i++)
	{
		const A4kPass *ps = &a4k_passes[i];
		int n = snprintf(src, sizeof(src), "#version 330 core\nin vec2 v_pos;\nout vec4 o_col;\n");
		for (int b = 0; b < ps->nbind; b++)
		{
			const char *t = ps->bind[b];
			n += snprintf(src + n, sizeof(src) - n,
						  "uniform sampler2D %s_raw;\n#define %s_pos v_pos\n#define %s_size vec2(textureSize(%s_raw, 0))\n"
						  "#define %s_pt (1.0 / %s_size)\n#define %s_tex(p) texture(%s_raw, p)\n"
						  "#define %s_texOff(off) %s_tex(%s_pos + %s_pt * (off))\n",
						  t, t, t, t, t, t, t, t, t, t, t, t);
		}
		n += snprintf(src + n, sizeof(src) - n, "%s\nvoid main() { o_col = hook(); }\n", ps->body);
		if (n >= (int)sizeof(src))
			return 0;
		const GLuint fs = a4k_shader(GL_FRAGMENT_SHADER, src, ps->desc);
		if (!fs)
			return 0;
		a4k_prog[i] = a4k_link(vs, fs, ps->nbind, ps->bind);
	}
	/* the colour clamped, opaque; a cut-out (tex.c): the alpha from ALPHA (smoothed), and where
	   it falls to nothing, the colour of the original texel (ORIG): blending by colour (dst *
	   (1 - src colour), the menus' dark titles) must see the clear texels as they were */
	static const char *out_src =
		"#version 330 core\nin vec2 v_pos;\nout vec4 o_col;\nuniform sampler2D MAIN_raw, ALPHA_raw, ORIG_raw;\nuniform int has_alpha;\n"
		"void main() {\n"
		"  vec3 c = clamp(texture(MAIN_raw, v_pos).rgb, 0.0, 1.0);\n"
		"  if (has_alpha == 0) { o_col = vec4(c, 1.0); return; }\n"
		"  float a = texture(ALPHA_raw, v_pos).r;\n"
		"  o_col = vec4(mix(texture(ORIG_raw, v_pos).rgb, c, clamp(a * 2.0, 0.0, 1.0)), a);\n"
		"}\n";
	const GLuint ofs = a4k_shader(GL_FRAGMENT_SHADER, out_src, "output");
	if (!ofs)
		return 0;
	static const char *const main_name[3] = { "MAIN", "ALPHA", "ORIG" };
	a4k_out_prog = a4k_link(vs, ofs, 3, main_name);
	static const float tri[6] = { -1, -1, 3, -1, -1, 3 };	/* one triangle covers the viewport */
	glGenVertexArrays(1, &a4k_vao);
	glBindVertexArray(a4k_vao);
	glGenBuffers(1, &a4k_vbo);
	glBindBuffer(GL_ARRAY_BUFFER, a4k_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(tri), tri, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, (void *)0);
	glEnableVertexAttribArray(0);
	glGenFramebuffers(1, &a4k_fbo);
	a4k_state = 1;
	printf("video: Anime4K picture enlargement ready (%d passes, %.0f ms)\n", A4K_PASSES,
		   (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC);
	return 1;
}

static GLuint a4k_texture(int w, int h, GLenum fmt, GLint filter, const void *rgba)
{
	GLuint t;
	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glTexImage2D(GL_TEXTURE_2D, 0, (GLint)fmt, w, h, 0, GL_RGBA, fmt == GL_RGBA16F ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE, rgba);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	return t;
}

/* the picture rgba (w x h, its colour; alpha not used) enlarged 2^runs times into texture id
   (replaced; 0: a new one), RGBA8; its alpha from alpha (the enlarged size, one byte a texel) or
   opaque when 0; with alpha, orig (w x h) gives the colour where alpha is nothing. Returns 0
   when Anime4K failed (id is then left alone) */
unsigned gl_a4k_upscale(unsigned id, u32 w, u32 h, const u32 *rgba, int runs, const u8 *alpha, const u32 *orig)
{
	if (runs < 1 || !a4k_ready())
		return 0;
	state_invalidate();
	/* the frame's state, put back at the end */
	GLint draw_fb, read_fb, viewport[4], active, unit_tex[16];
	GLboolean mask[4];
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fb);
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
	glGetIntegerv(GL_VIEWPORT, viewport);
	glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
	glGetBooleanv(GL_COLOR_WRITEMASK, mask);
	for (int i = 0; i < 16; i++)
	{
		glActiveTexture(GL_TEXTURE0 + i);
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &unit_tex[i]);
	}
	const GLboolean depth = glIsEnabled(GL_DEPTH_TEST), blend = glIsEnabled(GL_BLEND), stencil = glIsEnabled(GL_STENCIL_TEST),
					cull = glIsEnabled(GL_CULL_FACE), scissor = glIsEnabled(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_SCISSOR_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glBindVertexArray(a4k_vao);
	glBindFramebuffer(GL_FRAMEBUFFER, a4k_fbo);

	/* the textures by name: MAIN and what the passes save */
	enum { MAXTEX = 40 };
	const char *name[MAXTEX];
	GLuint tex[MAXTEX];
	int ntex = 1, mw = (int)w, mh = (int)h;
	name[0] = "MAIN";
	tex[0] = a4k_texture(mw, mh, GL_RGBA8, GL_LINEAR, rgba);
	for (int r = 0; r < runs; r++)
		for (int i = 0; i < A4K_PASSES; i++)
		{
			const A4kPass *ps = &a4k_passes[i];
			const int ow = ps->x2 ? mw * 2 : mw, oh = ps->x2 ? mh * 2 : mh;
			const GLuint out = a4k_texture(ow, oh, GL_RGBA16F, ps->x2 ? GL_LINEAR : GL_NEAREST, 0);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, out, 0);
			glViewport(0, 0, ow, oh);
			glUseProgram(a4k_prog[i]);
			for (int b = 0; b < ps->nbind; b++)
			{
				GLuint t = 0;
				for (int k = 0; k < ntex; k++)
					if (!strcmp(name[k], ps->bind[b]))
						t = tex[k];
				glActiveTexture(GL_TEXTURE0 + b);
				glBindTexture(GL_TEXTURE_2D, t);
			}
			glDrawArrays(GL_TRIANGLES, 0, 3);
			int k = 0;	/* saved under its name, in place of the earlier one */
			while (k < ntex && strcmp(name[k], ps->save))
				k++;
			if (k < ntex)
				glDeleteTextures(1, &tex[k]);
			else if (ntex < MAXTEX)
				ntex++;
			name[k] = ps->save;
			tex[k] = out;
			if (ps->x2)
				mw = ow, mh = oh;
		}
	/* MAIN into the picture's texture: colour clamped, opaque */
	const GLuint old = id;
	if (id)
		glDeleteTextures(1, &id);
	id = a4k_texture(mw, mh, GL_RGBA8, GL_LINEAR, 0);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, id, 0);
	glViewport(0, 0, mw, mh);
	glUseProgram(a4k_out_prog);
	GLuint atex = 0, otex = 0;
	if (alpha)
	{
		otex = a4k_texture((int)w, (int)h, GL_RGBA8, GL_NEAREST, orig);
		glActiveTexture(GL_TEXTURE2);
		glBindTexture(GL_TEXTURE_2D, otex);
		glGenTextures(1, &atex);
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, atex);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, mw, mh, 0, GL_RED, GL_UNSIGNED_BYTE, alpha);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	}
	glUniform1i(glGetUniformLocation(a4k_out_prog, "has_alpha"), alpha != 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, tex[0]);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	if (atex)
		glDeleteTextures(1, &atex);
	if (otex)
		glDeleteTextures(1, &otex);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
	for (int k = 0; k < ntex; k++)
		glDeleteTextures(1, &tex[k]);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw_fb);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read_fb);
	glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
	glColorMask(mask[0], mask[1], mask[2], mask[3]);
	for (int i = 0; i < 16; i++)
	{
		glActiveTexture(GL_TEXTURE0 + i);
		glBindTexture(GL_TEXTURE_2D, old && (GLuint)unit_tex[i] == old ? 0 : (GLuint)unit_tex[i]);	/* not a deleted name */
	}
	glActiveTexture((GLenum)active);
	glUseProgram(prog);
	glBindVertexArray(vao);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	if (depth) glEnable(GL_DEPTH_TEST);
	if (blend) glEnable(GL_BLEND);
	if (stencil) glEnable(GL_STENCIL_TEST);
	if (cull) glEnable(GL_CULL_FACE);
	if (scissor) glEnable(GL_SCISSOR_TEST);
	return id;
}

/* ---- input ------------------------------------------------------------------------------ */
/* keyboard map: Dreamcast button bits and their keys; M1_KEYS="up=Up,a=Z,..." (SDL key names,
   from [keys] in voldorium.ini) replaces any of the defaults */
static struct { const char *name; u32 bit; SDL_Scancode key; } keymap[] = {
	{ "up", 0x0010, SDL_SCANCODE_UP }, { "down", 0x0020, SDL_SCANCODE_DOWN },
	{ "left", 0x0040, SDL_SCANCODE_LEFT }, { "right", 0x0080, SDL_SCANCODE_RIGHT },
	{ "a", 0x0004, SDL_SCANCODE_Z }, { "b", 0x0002, SDL_SCANCODE_X },
	{ "x", 0x0400, SDL_SCANCODE_A }, { "y", 0x0200, SDL_SCANCODE_S },
	{ "start", 0x0008, SDL_SCANCODE_RETURN },
	{ "l", 0x1000, SDL_SCANCODE_Q }, { "r", 0x2000, SDL_SCANCODE_W },	/* triggers (maple.c) */
};

static void load_keymap(void)
{
	const char *e = getenv("M1_KEYS");
	while (e && *e)
	{
		char name[16], key[48];
		const char *comma = strchr(e, ',');
		const size_t len = comma ? (size_t)(comma - e) : strlen(e);
		char item[80];
		snprintf(item, sizeof(item), "%.*s", (int)(len < 79 ? len : 79), e);
		if (sscanf(item, "%15[^=]=%47[^,]", name, key) == 2)
			for (unsigned i = 0; i < sizeof(keymap) / sizeof(keymap[0]); i++)
				if (!strcmp(keymap[i].name, name))
				{
					const SDL_Scancode sc = SDL_GetScancodeFromName(key);
					if (sc != SDL_SCANCODE_UNKNOWN)
						keymap[i].key = sc;
					else
						printf("keys: unknown key name \"%s\" for %s\n", key, name);
				}
		e = comma ? comma + 1 : 0;
	}
}

void config_set(const char *section, const char *key, const char *value);
void config_save(void);
int video_text_sharp(void);

static int esc_confirmed(void);

void video_poll(void)
{
	if (!video_enabled())
		return;
	static int keys_loaded;
	if (!keys_loaded)
	{
		load_keymap();
		keys_loaded = 1;
	}
	/* the mouse cursor hides after 2 s without moving, in a window and full screen (issue #21);
	   the game has no use for it */
	static Uint32 moved;
	static int shown = 1;
	SDL_Event e;
	while (SDL_PollEvent(&e))
	{
		if (e.type == SDL_QUIT)
			m1_stop("window closed");
		if (e.type == SDL_MOUSEMOTION)
		{
			moved = SDL_GetTicks();
			if (!shown)
				SDL_ShowCursor(SDL_ENABLE), shown = 1;
		}
		if (e.type == SDL_KEYDOWN && !e.key.repeat)
		{
			const SDL_Keycode k = e.key.keysym.sym;
			if (k == SDLK_F11 || (k == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT)))
			{
				video_toggle_fullscreen();	/* remembered in voldorium.ini */
			}
			else if (k == SDLK_F12)
				player_shot = 1;	/* saved after the next frame (player_screenshot) */
			else if (k == SDLK_ESCAPE && !esc_confirmed())
				;	/* the first press only shows "Press Esc again to quit" (issue #16) */
			else if (k == SDLK_ESCAPE)
			{
				if (getenv("M1_NET_PRELUDE"))
				{
					void online_return(const char *why);
					online_return("");	/* a lobby match: Esc leaves it, back to the game */
				}
				m1_stop("quit (Esc)");
			}
		}
		if (e.type == SDL_CONTROLLERDEVICEADDED)
			pad_added(e.cdevice.which);
		if (e.type == SDL_CONTROLLERDEVICEREMOVED)
			pad_removed(e.cdevice.which);
	}
	if (shown && SDL_GetTicks() - moved > 2000)
		SDL_ShowCursor(SDL_DISABLE), shown = 0;
	const u8 *k = SDL_GetKeyboardState(0);
	u32 b = 0;
	for (unsigned i = 0; i < sizeof(keymap) / sizeof(keymap[0]); i++)
		if (k[keymap[i].key] && !(keymap[i].bit == 0x0008 && (SDL_GetModState() & KMOD_ALT)))
			b |= keymap[i].bit;
	b |= pad_bits(pads[0]);
	buttons_b = pad_bits(pads[1]);
	buttons = b;
}

u32 video_buttons(void) { return buttons; }
u32 video_buttons_b(void) { return buttons_b; }
int video_port_b(void) { static int forced = -1; if (forced < 0) forced = getenv("M1_PORT_B") != 0; return pads[1] != 0 || forced; }	/* M1_PORT_B: tests, an idle player 2 */

/* netplay status in the window title (netplay.c); also pumps events while waiting */
void video_title(const char *status)
{
	if (!video_enabled())
		return;
	char t[256];
	snprintf(t, sizeof(t), "Voldorium ver. " VOLDORIUM_VERSION "%s%s", status && status[0] ? " - " : "", status ? status : "");
	SDL_SetWindowTitle(win, t);
	SDL_PumpEvents();
}

/* ---- drawing ---------------------------------------------------------------------------- */
static const GLenum blend_src[8] = { GL_ZERO, GL_ONE, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR,
									 GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA };
static const GLenum blend_dst[8] = { GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR,
									 GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA };
/* PowerVR compares 1/w (greater = nearer); GL depth here grows with distance */
static const GLenum depth_func[8] = { GL_NEVER, GL_GREATER, GL_EQUAL, GL_GEQUAL, GL_LESS, GL_NOTEQUAL, GL_LEQUAL, GL_ALWAYS };

/* a vertex the PowerVR cannot set a triangle up from: not finite, or the game's "behind the
   camera" value 1e30 (the character select's light streaks have both). Such triangles reach
   far off screen, and how the GPU clips them depends on its precision: some drew them as black
   streaks across the picture. The PowerVR draws nothing for them */
static int vert_bad(const TaVert *t)
{
	return !(fabsf(t->x) < 1e20f && fabsf(t->y) < 1e20f && fabsf(t->z) < 1e20f);
}

/* a polygon's strip, without the triangles that have a bad vertex */
static void draw_strip(const TaCtx *c, const TaPoly *p)
{
	int bad = 0;
	for (u32 k = 0; k < p->count && !bad; k++)
		bad = vert_bad(&c->v[p->first + k]);
	if (!bad)
	{
		glDrawArrays(GL_TRIANGLE_STRIP, (GLint)p->first, (GLsizei)p->count);
		return;
	}
	for (u32 k = 0; k + 2 < p->count; k++)
	{
		const TaVert *t = &c->v[p->first + k];
		if (vert_bad(t) || vert_bad(t + 1) || vert_bad(t + 2))
			continue;
		glFrontFace(k & 1 ? GL_CW : GL_CCW);	/* every other triangle of a strip is wound the other way */
		glDrawArrays(GL_TRIANGLE_STRIP, (GLint)(p->first + k), 3);
	}
	glFrontFace(GL_CCW);
}

/* Drawing state, as a value: each polygon's state is worked out first (state_for), then only
   what differs from the state in force is sent to OpenGL (state_apply). The same draws in the
   same order, without the ~20 state calls per polygon that cost the Steam Deck about 4 ms per
   fight frame (issue #19: frames over 17.5 ms went from 34% to 3%). Draws wait in a batch until
   the state changes; one glMultiDrawArrays per batch made no difference on the Deck and was
   slower on NVIDIA's Windows driver, so a batch is separate glDrawArrays. */
typedef struct
{
	GLenum depth_func;
	int depth_mask;
	int cull;				/* 0 off, else GL_FRONT / GL_BACK */
	int blend;
	GLenum bsrc, bdst;
	int flags, fog, mode;	/* mode -1: untextured */
	float alpha_ref, fog_col[3];
	int stencil, stencil_ref;
	unsigned tex;			/* textured: the texture and its sampling */
	GLint filt, wrap_s, wrap_t;
	float xmap[2], umap[2];
	int scissor, sc[4];
} PolyState;
static PolyState st_cur;
static int st_valid;		/* st_cur is what OpenGL has (0 after anything else touched it) */
static GLint *b_first;
static GLsizei *b_count;
static u32 b_n, b_cap;

static u32 gp_flush[6];
/* M1_GPUPROF=2: each batch timed on the GPU (gp_batch_begin/end, below the profiler) */
static void gp_batch_begin(void);
static void gp_batch_end(void);
static void batch_flush(void)
{
	gp_flush[0]++;
	if (b_n)
		gp_batch_begin();
	for (u32 i = 0; i < b_n; i++)
		glDrawArrays(GL_TRIANGLE_STRIP, b_first[i], b_count[i]);
	if (b_n)
		gp_batch_end();
	b_n = 0;
}
static u32 gp_batch_tcw, gp_batch_tsp, gp_batch_isp, gp_batch_list;	/* the first polygon's, for M1_GPUPROF=2 */
static void batch_add(u32 first, u32 count)
{
	if (b_n == b_cap)
	{
		b_cap = b_cap ? b_cap * 2 : 1024;
		b_first = realloc(b_first, b_cap * sizeof(*b_first));
		b_count = realloc(b_count, b_cap * sizeof(*b_count));
	}
	b_first[b_n] = (GLint)first;
	b_count[b_n++] = (GLsizei)count;
}
/* drawn polygons wait in the batch: anything that changes OpenGL state or a texture's
   contents sends them first and makes the state cache start over */
static void state_invalidate(void)
{
	batch_flush();
	glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);	/* only the polygons' own state turns it on */
	st_valid = 0;
}

/* this frame's hi-res background pieces (hibg_update): the texture for polygon i of hb_base */
u64 tex_content_id(u32 tcw, u32 tsp);
u64 render_count(void);
static const TaPoly *hb_base;
static u32 hb_n;
static GLuint *hb_poly;
#define HB_WAIT (~(GLuint)0)	/* hb_poly: a piece whose picture is not made yet */
#define HB_PIC (~(GLuint)1)		/* hb_poly: a flat picture drawn by code with no 2D class, in front of
								   the layer (the VS loading screen's portraits): enlarged like 2D */
static u8 *bg_layer;	/* this frame's 4:3 background layer (side_fill) */

static void state_for(const TaPoly *p, int list, PolyState *s)
{
	memset(s, 0, sizeof(*s));
	const u32 isp = p->isp, tsp = p->tsp;
	const int textured = (isp >> 25) & 1;
	s->depth_func = depth_func[(isp >> 29) & 7];
	s->depth_mask = ((isp >> 26) & 1) ? 0 : 1;
	/* ISP culling mode (28-27): 2 and 3 cull one winding (the game relies on it for polygons
	   with vertices behind the camera); 1 (cull if small) is not modelled */
	const int cull = (isp >> 27) & 3;
	if (cull >= 2 && !p->sprite)
		s->cull = cull == 2 ? GL_FRONT : GL_BACK;
	if (list == 2)
	{
		s->blend = 1;
		s->bsrc = blend_src[(tsp >> 29) & 7];
		s->bdst = blend_dst[(tsp >> 26) & 7];
	}
	s->flags = ((tsp >> 20) & 1) | (((tsp >> 19) & 1) << 1) | (((isp >> 24) & 1) << 2);
	s->alpha_ref = list == 4 ? (float)(hw_reg(0x005f811c) & 255) : -1.0f;
	s->fog = (int)((tsp >> 22) & 3);
	{
		const u32 fc = hw_reg(0x005f80b4);	/* FOG_COL_VERT */
		s->fog_col[0] = ((fc >> 16) & 255) / 255.0f;
		s->fog_col[1] = ((fc >> 8) & 255) / 255.0f;
		s->fog_col[2] = (fc & 255) / 255.0f;
	}
	/* opaque and punch-through polygons mark whether modifier volumes shade them (stencil
	   bit 7, from PCW bit 7) */
	if (list != 2)
	{
		s->stencil = 1;
		s->stencil_ref = p->shadow ? 0x80 : 0;
	}
	if (!textured)
	{
		s->mode = -1;
		return;
	}
	s->mode = (tsp >> 6) & 3;
	GLint filt = ((tsp >> 13) & 3) ? GL_LINEAR : GL_NEAREST;
	/* text and other point-sampled 2D drawn above 640x480 would show each texel as a block: it
	   gets the texture upscaled by Scale2x (tex.c tex_get_hi) and bilinear filtering, so the
	   shapes stay the same size with smooth edges; punch-through then keeps texels at least half
	   opaque, as the filtered edges fall between the two. M1_SMOOTH2D=0 ([video] text = sharp):
	   off. */
	const int tm = video_text_mode(), smooth = tm < 2 ? 2 - tm : 0, pictures = video_portraits();	/* smooth: 2 HI-RES, 1 SMOOTH; pictures: [video] portraits (tex.c) */
	int hi = 1, hb = 0;
	unsigned rep;
	if ((rep = tex_get_rep(p->tcw, tsp)))
	{
		s->tex = rep;	/* replacement art (tex.c) */
		filt = GL_LINEAR;
	}
	else if (hb_n && p >= hb_base && p < hb_base + hb_n && hb_poly[p - hb_base] == HB_WAIT)
		s->tex = tex_get(p->tcw, tsp);
	else if (hb_n && p >= hb_base && p < hb_base + hb_n && hb_poly[p - hb_base] && hb_poly[p - hb_base] != HB_PIC)
	{
		s->tex = hb_poly[p - hb_base];	/* a piece of the hi-res background (hibg_update) */
		filt = GL_LINEAR;
		hb = 1;
	}
	else if ((smooth || pictures) && tgt_w == 640 && vp_h > 600 &&
			 ((p->cls >= M1_2D_HUD && p->cls <= M1_2D_SCREEN) ||
			  (hb_n && p >= hb_base && p < hb_base + hb_n && (bg_layer[p - hb_base] || hb_poly[p - hb_base] == HB_PIC))))
		/* 2D, or a picture of a 4:3 background layer drawn by other code (the loading screens'
		   portraits) */
	{
		int edge = 0;
		s->tex = tex_get_hi(p->tcw, tsp, &hi, &edge, smooth, pictures);
		if (hi > 1)
		{
			filt = GL_LINEAR;
			/* an enlarged cut-out (text): its edge drawn one screen pixel wide, as coverage
			   under MSAA (decal mode uses the texture's alpha as colour, so not there) */
			if (edge && s->mode != 2 && !(s->flags & 2))
				s->flags |= list == 4 && frame_samples > 0 ? 8 | 16 : 8;
			if (list == 4 && (hw_reg(0x005f811c) & 255) > 128)
				s->alpha_ref = 128.0f;
		}
	}
	else
		s->tex = tex_get(p->tcw, tsp);
	s->filt = filt;
	s->wrap_s = ((tsp >> 16) & 1) ? GL_CLAMP_TO_EDGE : ((tsp >> 18) & 1) ? GL_MIRRORED_REPEAT : GL_REPEAT;
	s->wrap_t = ((tsp >> 15) & 1) ? GL_CLAMP_TO_EDGE : ((tsp >> 17) & 1) ? GL_MIRRORED_REPEAT : GL_REPEAT;
	if (hb)
		s->wrap_s = s->wrap_t = GL_CLAMP_TO_EDGE;	/* its edges continue in the next piece, not around */
}

/* make OpenGL's state s, sending only what differs from st_cur (everything when !st_valid);
   the batch must be empty; view: also the x / u mapping and the scissor box */
static void state_apply(const PolyState *s, int view)
{
	const PolyState *c = &st_cur;
	const int all = !st_valid;
	if (all || s->depth_func != c->depth_func)
		glDepthFunc(s->depth_func);
	if (all || s->depth_mask != c->depth_mask)
		glDepthMask(s->depth_mask ? GL_TRUE : GL_FALSE);
	if (all || s->cull != c->cull)
	{
		if (s->cull)
		{
			glEnable(GL_CULL_FACE);
			glCullFace((GLenum)s->cull);
		}
		else
			glDisable(GL_CULL_FACE);
	}
	if (all || s->blend != c->blend || s->bsrc != c->bsrc || s->bdst != c->bdst)
	{
		if (s->blend)
		{
			glEnable(GL_BLEND);
			glBlendFunc(s->bsrc, s->bdst);
		}
		else
			glDisable(GL_BLEND);
	}
	if (all || s->flags != c->flags)
		glUniform1i(u_flags, s->flags);
	if (all || (s->flags & 16) != (c->flags & 16))
	{
		if (s->flags & 16)
			glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
		else
			glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
	}
	if (all || s->alpha_ref != c->alpha_ref)
		glUniform1f(u_alpha_ref, s->alpha_ref);
	if (all || s->fog != c->fog)
		glUniform1i(u_fog, s->fog);
	if (all || memcmp(s->fog_col, c->fog_col, sizeof(s->fog_col)))
		glUniform3f(u_fog_col, s->fog_col[0], s->fog_col[1], s->fog_col[2]);
	if (all || s->stencil != c->stencil || s->stencil_ref != c->stencil_ref)
	{
		if (s->stencil)
		{
			glEnable(GL_STENCIL_TEST);
			glStencilMask(0x80);
			glStencilFunc(GL_ALWAYS, s->stencil_ref, 0xff);
			glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
		}
		else
			glDisable(GL_STENCIL_TEST);
	}
	if (all || s->mode != c->mode)
		glUniform1i(u_mode, s->mode);
	/* a texture keeps its own sampling settings: all of them when the texture changes */
	if (s->tex && (all || s->tex != c->tex))
	{
		glBindTexture(GL_TEXTURE_2D, s->tex);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, s->filt);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, s->filt);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, s->wrap_s);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, s->wrap_t);
	}
	else if (s->tex)
	{
		if (s->filt != c->filt)
		{
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, s->filt);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, s->filt);
		}
		if (s->wrap_s != c->wrap_s)
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, s->wrap_s);
		if (s->wrap_t != c->wrap_t)
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, s->wrap_t);
	}
	if (view && (all || memcmp(s->xmap, c->xmap, sizeof(s->xmap))))
		glUniform2f(u_xmap, s->xmap[0], s->xmap[1]);
	if (view && (all || memcmp(s->umap, c->umap, sizeof(s->umap))))
		glUniform2f(u_umap, s->umap[0], s->umap[1]);
	if (view && (all || s->scissor != c->scissor || memcmp(s->sc, c->sc, sizeof(s->sc))))
	{
		if (s->scissor)
		{
			glEnable(GL_SCISSOR_TEST);
			glScissor(s->sc[0], s->sc[1], s->sc[2], s->sc[3]);
		}
		else
			glDisable(GL_SCISSOR_TEST);
	}
	/* an untextured polygon leaves the bound texture and its settings as they were */
	const PolyState prev = st_cur;
	st_cur = *s;
	if (!s->tex && !all)
		st_cur.tex = prev.tex, st_cur.filt = prev.filt, st_cur.wrap_s = prev.wrap_s, st_cur.wrap_t = prev.wrap_t;
	st_valid = 1;
}

/* one polygon's state, all of it, outside the batched drawing (side_fill) */
static void set_state(const TaPoly *p, int list)
{
	PolyState s;
	state_for(p, list, &s);
	st_valid = 0;
	state_apply(&s, 0);
	st_valid = 0;
}

/* background plane colour: ISP_BACKGND_T (005F808C) points into the parameter buffer at
   PARAM_BASE: ISP, TSP, TCW, then 3 vertices of 3 + skip words; colour of the first vertex */
static u32 background_colour(void)
{
	const u32 bg = hw_reg(0x005f808c), base = hw_reg(0x005f8020) & 0x7fffff;
	const u32 addr = base + ((bg >> 3) & 0x1fffff) * 4;
	const u32 isp = vram_rd32_map32(addr);
	const u32 colword = 3 + 3 + (((isp >> 25) & 1) ? 2 : 0);
	return vram_rd32_map32(addr + colword * 4);
}

/* F12: the next finished picture (at the drawing resolution, without the window's bars) as
   screenshots/Voldorium <date> <time>.png next to the executable. Xbox Game Bar captures nothing
   from this OpenGL window, so the game takes its own (issue #10). */
static void player_screenshot(int w, int h)
{
	u8 *px = malloc((size_t)w * h * 3), *top = malloc((size_t)w * h * 3);
	if (!px || !top)
	{
		free(px), free(top);
		return;
	}
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px);
	for (int y = 0; y < h; y++)
		memcpy(top + (size_t)y * w * 3, px + (size_t)(h - 1 - y) * w * 3, (size_t)w * 3);
	const char *config_exe_dir(void);
	int png_write_rgb(const char *path, const u8 *rgb, u32 w, u32 h);
	char dir[1100], path[1200];
	snprintf(dir, sizeof(dir), "%s/screenshots", config_exe_dir());
#ifdef _WIN32
	CreateDirectoryA(dir, 0);
#else
	mkdir(dir, 0755);
#endif
	const time_t now = time(0);
	char stamp[32];
	strftime(stamp, sizeof(stamp), "%Y-%m-%d %H-%M-%S", localtime(&now));
	snprintf(path, sizeof(path), "%s/Voldorium %s.png", dir, stamp);
	for (int k = 2; k < 100; k++)
	{
		FILE *f = fopen(path, "rb");	/* two in one second: "... (2).png" */
		if (!f)
			break;
		fclose(f);
		snprintf(path, sizeof(path), "%s/Voldorium %s (%d).png", dir, stamp, k);
	}
	const int ok = png_write_rgb(path, top, (u32)w, (u32)h);
	printf("screenshot %s (%dx%d): %s\n", path, w, h, ok ? "saved" : "could not be written");
	free(px), free(top);
}

static void screenshot(u64 n, int w, int h)
{
	static u8 *px;
	px = realloc(px, (size_t)w * h * 3);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px);
	char name[64];
	snprintf(name, sizeof(name), "shot_%llu.ppm", (unsigned long long)n);
	FILE *f = fopen(name, "wb");
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	for (int y = h - 1; y >= 0; y--)
		fwrite(px + (size_t)y * w * 3, 1, (size_t)w * 3, f);
	fclose(f);
	printf("saved %s (%dx%d)\n", name, w, h);
}

/* ---- widescreen (sc/docs/phase3-widescreen.md; 2D is never stretched) --------------------
 * The port is 16:9 only: the 640x480 virtual screen is shown across a 16:9 picture. 3D gets its
 * proportions from the game's g_aspect_scale_x = 0.75 (hw.c), so it is drawn full width
 * (Hor+). Flat (constant-depth) polygons drawn by known 2D code are compressed by 0.75 about
 * an anchor: in a fight the HUD and battle text by their position (centre x below 280 -> 0,
 * above 360 -> 640, else centre 320; [video] hud = middle / 4:3 move the sides in, side_anchor;
 * text by the extent of its whole run of glyphs, so one string
 * is never split between anchors; pieces inside a window, such as the practice menus, take the
 * window's anchor); on all other screens about the centre (pillarbox). Untextured full-width
 * fills stretch. Tiled layers (menu_bg_scroll_draw: the menu wallpaper, which wraps every 1024
 * virtual pixels, and the options menu's logos, which repeat every 256) are compressed like
 * other 2D and repeated one or more periods to either side, so they fill the width without
 * distortion. */
/* text runs of the frame being drawn: consecutive 2D primitives of one group on one line with
   no gap wider than a few characters (one call can draw a whole HUD line, e.g. "STAGE 1",
   the elapsed time and "INSERT COIN") */
static u32 *run_of, run_cap, nruns;
static float *run_x0, *run_x1;

static void text_runs(const TaCtx *c)
{
	if (c->np > run_cap)
	{
		run_cap = c->np;
		run_of = realloc(run_of, run_cap * sizeof(*run_of));
		run_x0 = realloc(run_x0, run_cap * sizeof(*run_x0));
		run_x1 = realloc(run_x1, run_cap * sizeof(*run_x1));
	}
	nruns = 0;
	u16 grp = 0;
	float cy = 0;
	for (u32 i = 0; i < c->np; i++)
	{
		const TaPoly *p = &c->p[i];
		run_of[i] = ~0u;
		if (p->cls != M1_2D_PRIM || p->count == 0)
			continue;
		float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
		for (u32 k = 0; k < p->count; k++)
		{
			const TaVert *t = &c->v[p->first + k];
			x0 = t->x < x0 ? t->x : x0;
			x1 = t->x > x1 ? t->x : x1;
			y0 = t->y < y0 ? t->y : y0;
			y1 = t->y > y1 ? t->y : y1;
		}
		const float y = (y0 + y1) / 2;
		const u32 r = nruns - 1;
		if (nruns && grp == p->grp && fabsf(y - cy) < 8 && x0 < run_x1[r] + 24 && x1 > run_x0[r] - 24)
		{
			run_x0[r] = x0 < run_x0[r] ? x0 : run_x0[r];
			run_x1[r] = x1 > run_x1[r] ? x1 : run_x1[r];
		}
		else
		{
			run_x0[nruns] = x0, run_x1[nruns] = x1, nruns++;
			grp = p->grp, cy = y;
		}
		run_of[i] = nruns - 1;
	}
}

/* fight overlays: flat polygons at a 2D depth. 2D is drawn at 1/w in the thousands (the HUD,
   menus, the practice panels); flat 3D sprites (particles) stay below 1. Some overlay pieces come
   from code that is not a known 2D function (class 0: the practice pause menu's window, the
   corners of the attack data panel), so the depth says what is 2D */
#define OVERLAY_Z 100.0f
static int is_overlay(float z0, float z1)
{
	return isfinite(z1) && z1 - z0 <= 1e-6f * (z1 > 1 ? z1 : 1) && z0 >= OVERLAY_Z;
}

/* fight windows (the practice pause menu, command list, attack data): overlay panels at least
   96x40 and not full width. Everything drawn over one takes that panel's anchor, so a window is
   placed as a whole instead of its pieces going to different anchors */
static u32 npanels, panel_cap;
static float (*panel)[4];	/* x0 x1 y0 y1 */

static void fight_panels(const TaCtx *c)
{
	npanels = 0;
	if (tgt_w != 640 || rd32(0x8c24a584) != 1)
		return;
	for (u32 i = 0; i < c->np; i++)
	{
		const TaPoly *p = &c->p[i];
		if (p->count == 0)
			continue;
		float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f, z0 = 1e30f, z1 = -1e30f;
		for (u32 k = 0; k < p->count; k++)
		{
			const TaVert *t = &c->v[p->first + k];
			x0 = t->x < x0 ? t->x : x0;
			x1 = t->x > x1 ? t->x : x1;
			y0 = t->y < y0 ? t->y : y0;
			y1 = t->y > y1 ? t->y : y1;
			z0 = t->z < z0 ? t->z : z0;
			z1 = t->z > z1 ? t->z : z1;
		}
		/* a window across the centre counts from 24 high: the mission battle's objective box is
		   52 high for two lines but about 30 for one, and below 40 its pieces went to the centre
		   and both sides in EDGES and MIDDLE (issue #21) */
		const int centred = x0 < 280 && x1 > 360;
		if (!is_overlay(z0, z1) || x1 - x0 < 96 || y1 - y0 < (centred ? 24 : 40) || (x0 <= 1 && x1 >= 639))
			continue;
		if (npanels == panel_cap)
		{
			panel_cap = panel_cap ? panel_cap * 2 : 16;
			panel = realloc(panel, panel_cap * sizeof(*panel));
		}
		panel[npanels][0] = x0, panel[npanels][1] = x1, panel[npanels][2] = y0, panel[npanels][3] = y1;
		npanels++;
	}
}

/* where fight HUD pieces left or right of centre go ([video] hud, M1_HUD, GRAPHICS menu): 4:3
   (where the original 4:3 picture has them, about the centre; the default, also for a
   voldorium.ini without the setting), middle (halfway) or edges (to the screen's sides).
   Drawing only: each player chooses their own, online too */
static int hud_mode = -1;	/* 0 edges, 1 middle, 2 4:3 */
int video_hud(void)
{
	if (hud_mode < 0)
	{
		const char *e = getenv("M1_HUD");
		hud_mode = e && !strcmp(e, "middle") ? 1 : e && !strcmp(e, "edges") ? 0 : 2;	/* default 4:3 */
	}
	return hud_mode;
}
void video_next_hud(void)
{
	static const char *const names[3] = { "edges", "middle", "4:3" };
	hud_mode = (video_hud() + 1) % 3;
	config_set("video", "hud", names[hud_mode]);
	config_save();
	setting_env("M1_HUD", names[hud_mode]);
}
/* [video] backgrounds (M1_BGMODE): 0 blur, 1 replace, 2 stretch (default), 3 black; see side_fill */
static int bg_mode = -1;
int video_backgrounds(void)
{
	if (bg_mode < 0)
	{
		const char *m = getenv("M1_BGMODE");
		bg_mode = m && !strcmp(m, "replace") ? 1 : m && !strcmp(m, "blur") ? 0 : m && !strcmp(m, "black") ? 3 : 2;
	}
	return bg_mode;
}

void video_next_backgrounds(void)
{
	static const int next[4] = { 3, 2, 0, 1 };	/* stretch, blur, black, replace, stretch... */
	static const char *const names[4] = { "blur", "replace", "stretch", "black" };
	bg_mode = next[video_backgrounds()];
	config_set("video", "backgrounds", names[bg_mode]);
	config_save();
	setting_env("M1_BGMODE", names[bg_mode]);
}

/* 2D text, the GRAPHICS menu's TEXT line: 0 HI-RES (enlarged with curved outlines and Anime4K
   colour, tex.c cut-outs), 1 ORIGINAL (the smooth text before 1.2.2, the cubic enlargement
   alone: lighter, for slower graphics), 2 SHARP (the original pixels). [video] text = smooth
   with fonts = 1 (default) or 0, or text = sharp; M1_SMOOTH2D=0: sharp, M1_HIFONTS=0: smooth */
static int text_mode = -1;
int video_text_mode(void)
{
	if (text_mode < 0)
		text_mode = getenv("M1_SMOOTH2D") && !atoi(getenv("M1_SMOOTH2D")) ? 2
				  : getenv("M1_HIFONTS") && !atoi(getenv("M1_HIFONTS")) ? 1 : 0;
	return text_mode;
}
int video_text_sharp(void) { return video_text_mode() == 2; }

void video_next_text(void)
{
	text_mode = (video_text_mode() + 1) % 3;
	config_set("video", "text", text_mode == 2 ? "sharp" : "smooth");
	if (text_mode < 2)
		config_set("video", "fonts", text_mode ? "0" : "1");
	config_save();
	setting_env("M1_SMOOTH2D", text_mode == 2 ? "0" : "1");
	setting_env("M1_HIFONTS", text_mode == 1 ? "0" : "1");
	tex_hi_reset();
}

/* [video] portraits (M1_A4K=0: off): opaque 2D pictures, the character select portraits,
   enlarged by Anime4K (a4k_upscale) instead of the cubic enlargement or the original pixels */
static int portraits = -1;
int video_portraits(void)
{
	if (portraits < 0)
		portraits = !getenv("M1_A4K") || atoi(getenv("M1_A4K")) != 0;
	return portraits;
}

void video_next_portraits(void)
{
	portraits = !video_portraits();
	config_set("video", "portraits", portraits ? "1" : "0");
	config_save();
	setting_env("M1_A4K", portraits ? "1" : "0");
	tex_hi_reset();
}

/* ---- first start: HI-RES PICTURES and TEXT HI-RES for the graphics card ----------------------
 * Both are made for a dedicated graphics card: each picture or font is enlarged once, when it
 * appears, which on integrated graphics (laptops, the Steam Deck) means a pause at each menu or
 * loading screen. When voldorium.ini does not say ([video] portraits, fonts; also not set in the
 * environment), they are chosen here once and written there, so the GRAPHICS menu shows them
 * and the player's own choice is never changed:
 *   off on the Steam Deck (its GPU, VANGOGH; Valve's board on Linux), integrated graphics by
 *   name (Intel apart from Arc A and B cards, AMD's "Radeon Graphics" and Vega APUs, software
 *   drivers), and when Anime4K takes over 40 ms on a 256x256 picture enlarged 4 times (10 ms
 *   on an RTX 4060: a card a quarter as fast still pauses only about 0.2 s per background);
 *   else on. M1_AUTOHW=deck|igpu|slow|fast (tests): that result. */
static int has(const char *s, const char *what) { return s && strstr(s, what) != 0; }

static const char *weak_by_name(const char *r)
{
	if (getenv("SteamDeck") && atoi(getenv("SteamDeck")))
		return "Steam Deck";
	if (has(r, "VANGOGH") || has(r, "Custom GPU 0405") || has(r, "Custom GPU 0932"))
		return "Steam Deck";
#ifndef _WIN32
	{
		char vendor[64] = "";
		FILE *f = fopen("/sys/devices/virtual/dmi/id/board_vendor", "r");
		if (f)
		{
			if (!fgets(vendor, sizeof(vendor), f))
				vendor[0] = 0;
			fclose(f);
		}
		if (!strncmp(vendor, "Valve", 5))
			return "Steam Deck";
	}
#endif
	if (has(r, "llvmpipe") || has(r, "softpipe") || has(r, "SwiftShader") || has(r, "Basic Render") || has(r, "GDI Generic"))
		return "software drawing";
	if ((has(r, "Intel") || has(r, "INTEL")) && !has(r, "Arc(TM) A") && !has(r, "Arc(TM) B") && !has(r, "Arc A") && !has(r, "Arc B"))
		return "integrated graphics";
	static const char *const apu[] = { "Radeon(TM) Graphics", "Radeon Graphics", "Vega", "RENOIR", "CEZANNE", "LUCIENNE",
									   "BARCELO", "REMBRANDT", "PHOENIX", "RAVEN", "PICASSO", "MENDOCINO", "RAPHAEL",
									   "STONEY", "CARRIZO", "GFX1103", "GFX1150", "780M", "760M", "740M", "680M", "660M",
									   "880M", "890M", "860M", "610M" };
	for (unsigned i = 0; i < sizeof(apu) / sizeof(apu[0]); i++)
		if (has(r, apu[i]) && !has(r, " RX "))
			return "integrated graphics";
	return 0;
}

/* Anime4K's time on a 256x256 picture enlarged 4 times, in ms (the second of two runs: the
   first can include the driver's own preparation) */
static double a4k_bench(void)
{
	unsigned gl_a4k_upscale(unsigned id, u32 w, u32 h, const u32 *rgba, int runs, const u8 *alpha, const u32 *orig);
	if (!a4k_ready())
		return 1e9;
	static u32 pic[256 * 256];
	for (u32 y = 0; y < 256; y++)
		for (u32 x = 0; x < 256; x++)
			pic[y * 256 + x] = 0xff000000u | ((x ^ y) & 255) | (((x * 3 + y) & 255) << 8) | (((y * 5) & 255) << 16);
	double ms = 1e9;
	for (int run = 0; run < 2; run++)
	{
		glFinish();
		const Uint64 t0 = SDL_GetPerformanceCounter();
		const unsigned id = gl_a4k_upscale(0, 256, 256, pic, 2, 0, 0);
		glFinish();
		ms = (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
		if (!id)
			return 1e9;
		GLuint t = id;
		glDeleteTextures(1, &t);
	}
	return ms;
}

static void auto_quality(void)
{
	const int pick_pictures = !config_get("video", "portraits") && !getenv("M1_A4K");
	const int pick_fonts = !config_get("video", "fonts") && !getenv("M1_HIFONTS") && video_text_mode() != 2;
	if (!pick_pictures && !pick_fonts)
		return;
	const char *r = (const char *)glGetString(GL_RENDERER);
	const char *test = getenv("M1_AUTOHW");
	const char *why = test ? (!strcmp(test, "deck") ? "Steam Deck" : !strcmp(test, "igpu") ? "integrated graphics" : 0) : weak_by_name(r);
	char because[96];
	if (why)
		snprintf(because, sizeof(because), "%s", why);
	else
	{
		const double ms = test && !strcmp(test, "slow") ? 99 : test && !strcmp(test, "fast") ? 1 : a4k_bench();
		snprintf(because, sizeof(because), "Anime4K %.0f ms on a test picture", ms);
		if (ms > 40)
			why = because;
	}
	const int on = !why;
	if (pick_pictures)
	{
		portraits = on;
		config_set("video", "portraits", on ? "1" : "0");
		setting_env("M1_A4K", on ? "1" : "0");
	}
	if (pick_fonts)
	{
		text_mode = on ? 0 : 1;
		config_set("video", "fonts", on ? "1" : "0");
		setting_env("M1_HIFONTS", on ? "1" : "0");
	}
	config_save();
	printf("video: first start on %s (%s): %s%s%s, change them in GRAPHICS, MORE SETTINGS\n", r ? r : "?", because,
		   pick_pictures ? (on ? "HI-RES PICTURES ON" : "HI-RES PICTURES OFF") : "", pick_pictures && pick_fonts ? ", " : "",
		   pick_fonts ? (on ? "TEXT HI-RES" : "TEXT ORIGINAL") : "");
}

static float side_anchor(float cx)
{
	/* the 4:3 screen's left edge (x 0, squeezed by 0.75 about the anchor) lands on the picture's
	   left edge (EDGES), halfway between it and where 4:3 shows it (MIDDLE), or there (4:3, x 80);
	   the right edge likewise. A picture narrower than 16:9 has its edge at 320 - 320 / view_zoom.
	   An anchor a puts x 0 at a * (1 - 0.75), so a = 4 * that place: 0, 160, 320 in 16:9 */
	const float edge = 320 - 320 / view_zoom;
	const float place[3] = { edge, (edge + 80) / 2, 80 };
	const float a = 4 * place[video_hud()];
	return cx < 280 ? a : cx > 360 ? 640 - a : 320;
}

/* the anchor of the largest panel around (x, y) (a margin takes in the panels' corner pieces),
   or -1 */
static float panel_anchor(float x, float y)
{
	float best = 0, a = -1;
	for (u32 i = 0; i < npanels; i++)
	{
		const float *r = panel[i];
		if (x < r[0] - 12 || x > r[1] + 12 || y < r[2] - 12 || y > r[3] + 12)
			continue;
		const float area = (r[1] - r[0]) * (r[3] - r[2]);
		if (area > best)
		{
			const float cx = (r[0] + r[1]) / 2;
			best = area, a = side_anchor(cx);
		}
	}
	return a;
}

static void wide_map(const TaCtx *c, const TaPoly *p, float *xa, float *xk, float *ua, float *uk)
{
	*xa = 0, *xk = 1, *ua = 0, *uk = 1;
	if (tgt_w != 640)
		return;	/* render targets: no mapping */
	float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f, z0 = 1e30f, z1 = -1e30f, us = 0;
	for (u32 k = 0; k < p->count; k++)
	{
		const TaVert *t = &c->v[p->first + k];
		x0 = t->x < x0 ? t->x : x0;
		x1 = t->x > x1 ? t->x : x1;
		y0 = t->y < y0 ? t->y : y0;
		y1 = t->y > y1 ? t->y : y1;
		z0 = t->z < z0 ? t->z : z0;
		z1 = t->z > z1 ? t->z : z1;
		us += t->u;
	}
	/* a vertex behind the camera can be infinite, and inf - x <= inf would pass for flat: such a
	   3D strip was squeezed like 2D while its neighbours were not, opening lines between them
	   (the character select's light streaks) */
	const int flat = isfinite(z0) && isfinite(z1) && z1 - z0 <= 1e-6f * (z1 > 1 ? z1 : 1);
	const int battle = rd32(0x8c24a584) == 1;	/* game mode 1 = fight */
	/* the menus' 3D pieces (0x8c0ef6c0: the stage select's stage tiles and the chosen stage's
	   plate, issue #11) are laid out in the 4:3 screen without the game's widescreen projection:
	   like 2D, they are compressed about the centre with the panels around them */
	const int menu_3d = !battle && p->caller == 0x8c0ef6c0u && isfinite(z0) && isfinite(z1);
	if (!flat && !menu_3d)
		return;	/* 3D */
	const int textured = (p->isp >> 25) & 1;
	(void)us;
	if (!battle && textured && !(p->cls == 0 && z1 < 0.01f && x0 <= 1 && x1 >= 639))
	{
		void art_note(const TaPoly *p, float x0, float x1, float z);
		art_note(p, x0, x1, z1);	/* M1_ARTDUMP: 4:3 artwork that leaves bars (artdump.c) */
	}
	if (p->cls == M1_2D_TILED)
	{
		/* undistorted like other 2D; the caller adds copies one wrap period either side */
		*xa = 320;
		*xk = 0.75f;
		return;
	}
	if (p->cls == 0 && battle && !is_overlay(z0, z1))
		return;	/* 3D sprites (particles, effects) */
	if (p->cls == 0 && z1 < 0.01f && x0 <= 1 && x1 >= 639)
		return;	/* far full-width backdrops of 3D scenes (the intro's skies, 1/w about 0.001)
				   span 16:9; centred pictures at that depth (the intro's framed scenes) and
				   menu pieces not drawn by known 2D code (1/w 0.6 and up) stay undistorted */
	if (x0 <= 1 && x1 >= 639 && !textured)
		return;	/* full-width flat fill */
	*xk = 0.75f;
	*xa = 320;
	if (battle)
	{
		float cx = (x0 + x1) / 2;
		const u32 i = (u32)(p - c->p);
		if (p->cls == M1_2D_PRIM && i < run_cap && run_of[i] != ~0u)
			cx = (run_x0[run_of[i]] + run_x1[run_of[i]]) / 2;
		*xa = side_anchor(cx);
		const float pa = panel_anchor(cx, (y0 + y1) / 2);
		if (pa >= 0)
			*xa = pa;	/* part of a window: the window's anchor */
	}
}

/* M1_DUMPLIST=n: print render n's polygons (list, ISP, TSP, TCW, first vertex) to list_dump.txt;
   M1_DUMPLIST=n,n,...: several renders, list_dump_<n>.txt */
static void dump_list(TaCtx *c)
{
	static long want[16];
	static int nwant = -1, several;
	if (nwant < 0)
	{
		nwant = 0;
		const char *s = getenv("M1_DUMPLIST");
		several = s && strchr(s, ',');
		while (s && *s && nwant < 16)
		{
			want[nwant++] = atol(s);
			s = strchr(s, ',');
			s = s ? s + 1 : 0;
		}
	}
	int hit = 0;
	for (int k = 0; k < nwant; k++)
		hit |= (long)rendered == want[k];
	if (!hit || !c)
		return;
	char name[64] = "list_dump.txt";
	if (several)
		snprintf(name, sizeof(name), "list_dump_%lu.txt", (unsigned long)rendered);
	FILE *f = fopen(name, "w");
	for (u32 i = 0; i < c->np; i++)
	{
		const TaPoly *p = &c->p[i];
		const TaVert *v = &c->v[p->first];
		float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f, z0 = 1e30f, z1 = -1e30f;
		for (u32 k = 0; k < p->count; k++)
		{
			const TaVert *t = &c->v[p->first + k];
			if (t->x < x0) x0 = t->x;
			if (t->x > x1) x1 = t->x;
			if (t->y < y0) y0 = t->y;
			if (t->y > y1) y1 = t->y;
			if (t->z < z0) z0 = t->z;
			if (t->z > z1) z1 = t->z;
		}
		fprintf(f, "%s %08x c%u ", p->via_sq ? "SQ " : "DMA", p->caller, p->cls);
		fprintf(f, "%4u list %u%s isp %08x tsp %08x tcw %08x n %3u  v0 %7.1f %7.1f z %.4g uv %.3f %.3f col %08x ofs %08x  x %.1f..%.1f y %.1f..%.1f z %.4g..%.4g\n",
				i, p->list, p->sprite ? " spr" : "", p->isp, p->tsp, p->tcw, p->count, v->x, v->y, v->z, v->u, v->v, v->col, v->ofs,
				x0, x1, y0, y1, z0, z1);
		if (p->clip)
			fprintf(f, "      clip %u rect %u %u %u %u%c", p->clip, p->clip_rect[0], p->clip_rect[1], p->clip_rect[2], p->clip_rect[3], 10);
		if (p->sprite || getenv("M1_DUMPVERTS"))	/* M1_DUMPVERTS=1: every polygon's vertices */
			for (u32 k = 0; k < p->count; k++)
			{
				const TaVert *t = &c->v[p->first + k];
				fprintf(f, "      v%u %7.2f %7.2f z %.4g uv %.4f %.4f col %08x%c", k, t->x, t->y, t->z, t->u, t->v, t->col, 10);
			}
	}
	{
		u32 shadowed = 0;
		for (u32 i = 0; i < c->np; i++)
			shadowed += c->p[i].shadow;
		fprintf(f, "modifier volume triangles %u (ends with mode 1: %u, 2: %u), polygons with the shadow bit %u, FPU_SHAD_SCALE %08x%c",
				c->nmv, 0u, 0u, shadowed, hw_reg(0x005f8074), 10);
		u32 m1 = 0, m2 = 0;
		for (u32 i = 0; i < c->nmv; i++)
			m1 += c->mvmode[i] == 1, m2 += c->mvmode[i] == 2;
		fprintf(f, "  volume ends: %u inside, %u outside%c", m1, m2, 10);
		for (u32 i = 0; i < c->nmv && i < 40; i++)
		{
			const float *t = &c->mv[i * 9];
			fprintf(f, "  mv %3u flags %02x  (%.1f %.1f %.5f) (%.1f %.1f %.5f) (%.1f %.1f %.5f)%c", i, c->mvmode[i], t[0], t[1], t[2], t[3],
					t[4], t[5], t[6], t[7], t[8], 10);
		}
	}
	if (getenv("M1_TEXDUMP"))
	{
		/* M1_TEXDUMP=tcw: that texture as decoded now (tex.c), tex_dump.ppm;
		   M1_TEXDUMP=all: every texture of the render, tex_<tcw>.ppm and tex_<tcw>_alpha.pgm */
		const int all = !strcmp(getenv("M1_TEXDUMP"), "all");
		const u32 want_tcw = (u32)strtoul(getenv("M1_TEXDUMP"), 0, 16);
		for (u32 i = 0; i < c->np; i++)
			if (all ? c->p[i].tcw != 0 : c->p[i].tcw == want_tcw)
			{
				u32 tw, th;
				static u32 px[1024 * 1024];
				u32 tex_decode_rgba(u32 tcw, u32 tsp, u32 *out, u32 *w, u32 *h);
				tex_decode_rgba(c->p[i].tcw, c->p[i].tsp, px, &tw, &th);
				char tn[64], an[64];
				snprintf(tn, sizeof(tn), all ? "tex_%08x.ppm" : "tex_dump.ppm", c->p[i].tcw);
				snprintf(an, sizeof(an), all ? "tex_%08x_alpha.pgm" : "tex_alpha.pgm", c->p[i].tcw);
				FILE *tf = fopen(tn, "wb");
				fprintf(tf, "P6\n%u %u\n255\n", tw, th);
				for (u32 k = 0; k < tw * th; k++)
				{
					const u32 a = px[k] >> 24;
					const u8 rgb[3] = { (u8)((px[k] & 255) * a / 255), (u8)((px[k] >> 8 & 255) * a / 255), (u8)((px[k] >> 16 & 255) * a / 255) };
					fwrite(rgb, 1, 3, tf);
				}
				fclose(tf);
				tf = fopen(an, "wb");
				fprintf(tf, "P5\n%u %u\n255\n", tw, th);
				for (u32 k = 0; k < tw * th; k++)
					fputc((int)(px[k] >> 24), tf);
				fclose(tf);
				printf("texdump: PT_ALPHA_REF %08x PAL_RAM_CTRL %08x\n", hw_reg(0x005f811c), hw_reg(0x005f8108));
				if (!all)
					break;
			}
	}
	{
		/* the palette RAM (1024 words) and PAL_RAM_CTRL */
		FILE *pf = fopen("pal_dump.txt", "w");
		fprintf(pf, "PAL_RAM_CTRL %08x%c", hw_reg(0x005f8108), 10);
		for (u32 i = 0; i < 1024; i++)
			fprintf(pf, "%08x%c", hw_reg(0x005f9000u + 4 * i), i % 16 == 15 ? 10 : ' ');
		fclose(pf);
	}
	fclose(f);
	f = fopen("vram_dump.bin", "wb");
	fwrite(hw_vram(), 1, VRAM_SIZE, f);
	fclose(f);
	printf("dumped render %ld display list (%u polygons) to list_dump.txt, VRAM to vram_dump.bin\n", want, c->np);
}


/* ---- modifier volumes ("cheap shadow", FPU_SHAD_SCALE bit 8) --------------------------------
 * The opaque list's modifier volumes are closed meshes; a pixel is inside a volume when an odd
 * number of its triangles lie in front of the visible surface. Opaque and punch-through
 * polygons with PCW bit 7 set (stencil bit 7) are darkened inside, by FPU_SHAD_SCALE / 256.
 * Per volume: its triangles toggle stencil bit 0 where they pass the depth test; at the
 * triangle that ends the volume (ISP 31-29 = 1 inside, 2 outside) the volume's footprint sets
 * bit 1 where bit 0 says inside (or outside) and clears bit 0. A full-screen quad then darkens
 * pixels with bits 7 and 1. The mode with two parameter sets (bit 8 clear) is not modelled. */
static u32 mv_base;	/* first vertex of the volume triangles in the buffer */

static void upload_volumes(TaCtx *c)
{
	static TaVert *tmp;
	static u32 cap;
	const u32 need = 3 * c->nmv + 4;
	if (need > cap)
	{
		cap = need;
		tmp = realloc(tmp, cap * sizeof(TaVert));
	}
	for (u32 i = 0; i < 3 * c->nmv; i++)
	{
		memset(&tmp[i], 0, sizeof(TaVert));
		tmp[i].x = c->mv[i * 3];
		tmp[i].y = c->mv[i * 3 + 1];
		tmp[i].z = c->mv[i * 3 + 2];
		tmp[i].col = 0xff0000ffu;
	}
	const u32 sh = hw_reg(0x005f8074) & 255, g = sh;	/* FPU_SHAD_SCALE: shade factor / 256 */
	static const float qx[4] = { -10000, 10000, -10000, 10000 }, qy[4] = { -10000, -10000, 10000, 10000 };
	for (int i = 0; i < 4; i++)
	{
		TaVert *t = &tmp[3 * c->nmv + (u32)i];
		memset(t, 0, sizeof(*t));
		t->x = qx[i];
		t->y = qy[i];
		t->z = 1.0f;
		t->col = 0xff000000u | g << 16 | g << 8 | g;
	}
	mv_base = c->nv;
	glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)(c->nv * sizeof(TaVert)), (GLsizeiptr)(need * sizeof(TaVert)), tmp);
}

static void shadow_pass(TaCtx *c)
{
	static int off = -1;	/* M1_NOSHADOW=1: no modifier volumes */
	if (off < 0)
		off = getenv("M1_NOSHADOW") != 0;
	if (off || !c->nmv || !(hw_reg(0x005f8074) & 0x100))
		return;
	glUniform1i(u_mode, -1);
	glUniform1i(u_flags, 1);
	glUniform1f(u_alpha_ref, -1.0f);
	glUniform1i(u_fog, 2);
	glUniform2f(u_xmap, 0, 1);
	glUniform2f(u_umap, 0, 1);
	glUniform1f(u_xoff, 0);
	glDisable(GL_CULL_FACE);
	glDisable(GL_BLEND);
	glDisable(GL_SCISSOR_TEST);
	glEnable(GL_STENCIL_TEST);
	if (getenv("M1_SHADOWDEBUG") && atoi(getenv("M1_SHADOWDEBUG")) == 3)
	{
		/* debug: the volume triangles in solid colour */
		glDisable(GL_STENCIL_TEST);
		glDisable(GL_DEPTH_TEST);
		glDrawArrays(GL_TRIANGLES, (GLint)mv_base, (GLsizei)(3 * c->nmv));
		glEnable(GL_DEPTH_TEST);
		return;
	}
	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
	glDepthMask(GL_FALSE);
	/* triangles come in groups, one per global parameter (bit 7 starts one); a group whose mode
	   is 1 or 2 ends a volume that began after the previous such group (as Flycast draws them) */
	u32 vol_start = 0;
	for (u32 s = 0; s < c->nmv;)
	{
		u32 e = s + 1;
		while (e < c->nmv && !(c->mvmode[e] & 0x80))
			e++;
		const u32 mode = c->mvmode[s] & 7, last = (c->mvmode[s] >> 3) & 1;
		/* faces in front of the scene: XOR the parity bit (closed volume), or set it for an
		   open volume (mode set, not the last) */
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(getenv("M1_MVNODEPTH") ? GL_ALWAYS : getenv("M1_MVSTRICT") ? GL_LESS : GL_LEQUAL);
		glStencilMask(0x01);
		if (!last && mode > 0)
		{
			glStencilFunc(GL_ALWAYS, 0x01, 0x01);
			glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
		}
		else
		{
			glStencilFunc(GL_ALWAYS, 0, 0);
			glStencilOp(GL_KEEP, GL_KEEP, GL_INVERT);
		}
		glDrawArrays(GL_TRIANGLES, (GLint)(mv_base + 3 * s), (GLsizei)(3 * (e - s)));
		if (getenv("M1_SHADOWDEBUG") && atoi(getenv("M1_SHADOWDEBUG")) == 5 && s == 0)
		{
			/* debug: stencil histogram after the first group's faces */
			static u8 st[2048 * 2048];
			glReadPixels(vp_x, vp_y, vp_w, vp_h, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, st);
			u32 n0 = 0, n1 = 0, n7 = 0;
			for (int i = 0; i < vp_w * vp_h; i++)
				n0 += st[i] & 1, n1 += (st[i] >> 1) & 1, n7 += st[i] >> 7;
			printf("stencil after group 0 (%u triangles, mode %u, last %u): bit0 %u, bit1 %u, bit7 %u pixels%c", e - s, mode, last, n0, n1, n7, 10);
		}
		if (mode == 1 || mode == 2)
		{
			/* resolve over the volume's footprint: bit 1 where inside (or outside), then
			   clear bit 0 */
			glDisable(GL_DEPTH_TEST);
			glStencilMask(0x02);
			glStencilFunc(GL_EQUAL, mode == 2 ? 0x02 : 0x03, 0x01);
			glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
			glDrawArrays(GL_TRIANGLES, (GLint)(mv_base + 3 * vol_start), (GLsizei)(3 * (e - vol_start)));
			if (getenv("M1_SHADOWDEBUG") && atoi(getenv("M1_SHADOWDEBUG")) == 5 && s == 0)
			{
				static u8 st[2048 * 2048];
				glReadPixels(vp_x, vp_y, vp_w, vp_h, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, st);
				u32 n0 = 0, n1 = 0;
				for (int i = 0; i < vp_w * vp_h; i++)
					n0 += st[i] & 1, n1 += (st[i] >> 1) & 1;
				printf("stencil after the resolve: bit0 %u, bit1 %u%c", n0, n1, 10);
			}
			glStencilMask(0x01);
			glStencilFunc(GL_ALWAYS, 0, 0);
			glStencilOp(GL_KEEP, GL_KEEP, GL_ZERO);
			glDrawArrays(GL_TRIANGLES, (GLint)(mv_base + 3 * vol_start), (GLsizei)(3 * (e - vol_start)));
			vol_start = e;
		}
		s = e;
	}
	/* darken shadow-enabled pixels inside a volume */
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glStencilMask(0);
	static int dbg = -1;	/* M1_SHADOWDEBUG=1: darken all shadow-flagged pixels, 2: all pixels inside volumes */
	if (dbg < 0)
		dbg = getenv("M1_SHADOWDEBUG") ? atoi(getenv("M1_SHADOWDEBUG")) : 0;
	const int m = dbg == 1 ? 0x80 : dbg == 2 ? 0x02 : 0x82;
	glStencilFunc(GL_EQUAL, m, m);
	glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ZERO, GL_SRC_COLOR);
	glDrawArrays(GL_TRIANGLE_STRIP, (GLint)(mv_base + 3 * c->nmv), 4);
	glDisable(GL_BLEND);
	glDisable(GL_STENCIL_TEST);
	glDepthMask(GL_TRUE);
	glEnable(GL_DEPTH_TEST);
}

/* ---- blur fill (16:9 sides of 4:3 pictures) ---------------------------------------------
 * A screen whose 2D background is a 4:3 picture (it reaches the left or right edge of the
 * 640x480 screen) would have bars beside it. Its background layer (the same rule as
 * artdump.c: flat textured pieces outside fights at the depth of the edge pieces or farther
 * back; not scrolling wallpapers, which already span 16:9) is drawn again, enlarged evenly to
 * cover 16:9 (cropping top and bottom), into a small texture, blurred and darkened, and laid
 * under the frame. The picture itself stays undistorted in the middle. M1_BLURFILL=0: off. */
#define BLUR_W 192
#define BLUR_H 108
static GLuint blur_prog, blur_vao, blur_vbo, blur_fbo[2], blur_tex[2];
static GLint u_b_src, u_b_dir, u_b_gain;

static const char *blur_vs =
	"#version 330 core\n"
	"layout(location = 0) in vec2 p;\n"
	"out vec2 uv;\n"
	"void main() { uv = p * 0.5 + 0.5; gl_Position = vec4(p, 0.0, 1.0); }\n";
static const char *blur_fs =
	"#version 330 core\n"
	"in vec2 uv; out vec4 o;\n"
	"uniform sampler2D src; uniform vec2 dir; uniform float gain;\n"
	"void main() {\n"
	"  vec3 s = texture(src, uv).rgb * 0.227027;\n"
	"  s += (texture(src, uv + dir * 1.384615).rgb + texture(src, uv - dir * 1.384615).rgb) * 0.316216;\n"
	"  s += (texture(src, uv + dir * 3.230769).rgb + texture(src, uv - dir * 3.230769).rgb) * 0.070270;\n"
	"  o = vec4(s * gain, 1.0);\n"
	"}\n";

static void blur_init(void)
{
	blur_prog = glCreateProgram();
	glAttachShader(blur_prog, shader(GL_VERTEX_SHADER, blur_vs));
	glAttachShader(blur_prog, shader(GL_FRAGMENT_SHADER, blur_fs));
	glLinkProgram(blur_prog);
	u_b_src = glGetUniformLocation(blur_prog, "src");
	u_b_dir = glGetUniformLocation(blur_prog, "dir");
	u_b_gain = glGetUniformLocation(blur_prog, "gain");
	static const float tri[6] = { -1, -1, 3, -1, -1, 3 };	/* one triangle covers the target */
	glGenVertexArrays(1, &blur_vao);
	glBindVertexArray(blur_vao);
	glGenBuffers(1, &blur_vbo);
	glBindBuffer(GL_ARRAY_BUFFER, blur_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(tri), tri, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, (void *)0);
	glEnableVertexAttribArray(0);
	glGenFramebuffers(2, blur_fbo);
	glGenTextures(2, blur_tex);
	for (int i = 0; i < 2; i++)
	{
		glBindTexture(GL_TEXTURE_2D, blur_tex[i]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, BLUR_W, BLUR_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glBindFramebuffer(GL_FRAMEBUFFER, blur_fbo[i]);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, blur_tex[i], 0);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, frame_fbo);
}

/* tiled layers (menu_bg_scroll_draw): the wallpaper spans -192..832 and repeats every 1024. Others
   cover just the 4:3 screen, like the options menu's grid of logos (-29..611, two 64-wide halves
   per logo, repeating every 256): their period is the smallest x step between two tiles with the
   same texture on the same row. tiled_x0/x1: the layer's extent; tiled_row: each row's own (the
   rows are staggered, so where one ends another may not). Cached per render */
static float tiled_period, tiled_x0, tiled_x1;
static struct { float y0, x0, x1; } tiled_row[64];
static u32 tiled_rows;

static int tiled_row_of(float y0)
{
	for (u32 r = 0; r < tiled_rows; r++)
		if (fabsf(tiled_row[r].y0 - y0) < 0.5f)
			return (int)r;
	return -1;
}

static void tiled_layer(const TaCtx *c)
{
	static u64 done = ~0ull;
	if (done == rendered)
		return;
	done = rendered;
	tiled_period = 0, tiled_x0 = 1e30f, tiled_x1 = -1e30f, tiled_rows = 0;
	static struct { float x0, y0; u32 tcw; } *t;
	static u32 cap;
	u32 n = 0;
	for (u32 i = 0; i < c->np; i++)
	{
		const TaPoly *p = &c->p[i];
		if (p->cls != M1_2D_TILED || p->count == 0)
			continue;
		float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f;
		for (u32 k = 0; k < p->count; k++)
		{
			const TaVert *v = &c->v[p->first + k];
			x0 = fminf(x0, v->x), x1 = fmaxf(x1, v->x), y0 = fminf(y0, v->y);
		}
		if (!isfinite(x0) || !isfinite(x1))
			continue;
		tiled_x0 = fminf(tiled_x0, x0), tiled_x1 = fmaxf(tiled_x1, x1);
		int r = tiled_row_of(y0);
		if (r < 0 && tiled_rows < 64)
		{
			r = (int)tiled_rows++;
			tiled_row[r].y0 = y0, tiled_row[r].x0 = x0, tiled_row[r].x1 = x1;
		}
		if (r >= 0)
			tiled_row[r].x0 = fminf(tiled_row[r].x0, x0), tiled_row[r].x1 = fmaxf(tiled_row[r].x1, x1);
		if (n == cap)
			t = realloc(t, (cap = cap ? cap * 2 : 256) * sizeof(*t));
		t[n].x0 = x0, t[n].y0 = y0, t[n].tcw = p->tcw, n++;
	}
	if (!n)
		return;
	if (tiled_x1 - tiled_x0 >= 1000)
	{
		tiled_period = 1024;	/* the wallpaper */
		return;
	}
	float best = 1e30f;
	for (u32 i = 0; i < n; i++)
		for (u32 j = i + 1; j < n; j++)
		{
			const float d = fabsf(t[i].x0 - t[j].x0);
			if (t[i].tcw == t[j].tcw && fabsf(t[i].y0 - t[j].y0) < 0.5f && d > 8 && d < best)
				best = d;
		}
	tiled_period = best < 1e30f ? best : 0;
}

static int bg_tiled_picture;	/* the background is the mission battle map (bg_mask) */
static int bg_gallery_picture;	/* the Art Gallery's picture viewer is showing (bg_mask) */

/* marks the polygons of the frame's 4:3 background layer; returns how many */
static u32 bg_mask(const TaCtx *c, u8 *mask)
{
	const u32 mode = rd32(0x8c24a584);
	const int battle = mode == 1 || mode == 23 || mode == 45;	/* fight, demo fight, demo replay */
	memset(mask, 0, c->np);
	bg_tiled_picture = 0;
	/* the Art Gallery (game mode 61): its menus' tiled background is darkened and point sampled,
	   the picture viewer's tiles are filtered (TSP filter mode, bits 13-14), with or without a
	   caption over them and through the fades between pictures */
	bg_gallery_picture = 0;
	if (mode == 61)
		for (u32 i = 0; i < c->np && !bg_gallery_picture; i++)
			bg_gallery_picture = c->p[i].cls == M1_2D_TILED && ((c->p[i].tsp >> 13) & 3);
	if (battle)
		return 0;
	float zmax = -1, tx0 = 1e30f, tx1 = -1e30f;
	static float *pz, *px0, *px1;
	static u32 cap;
	if (c->np > cap)
	{
		cap = c->np;
		pz = realloc(pz, cap * sizeof(float));
		px0 = realloc(px0, cap * sizeof(float));
		px1 = realloc(px1, cap * sizeof(float));
	}
	for (u32 i = 0; i < c->np; i++)
	{
		const TaPoly *p = &c->p[i];
		pz[i] = -1;
		if (!((p->isp >> 25) & 1) || p->count < 3)
			continue;	/* untextured */
		float x0 = 1e30f, x1 = -1e30f, z0 = 1e30f, z1 = -1e30f;
		for (u32 k = 0; k < p->count; k++)
		{
			const TaVert *t = &c->v[p->first + k];
			x0 = fminf(x0, t->x), x1 = fmaxf(x1, t->x), z0 = fminf(z0, t->z), z1 = fmaxf(z1, t->z);
		}
		if (!isfinite(x0) || !isfinite(x1) || !isfinite(z1) || z1 - z0 > 1e-6f * (z1 > 1 ? z1 : 1))
			continue;	/* 3D */
		if (p->cls == 0 && z1 < 0.01f && x0 <= 1 && x1 >= 639)
			continue;	/* far backdrop of a 3D scene: already 16:9 */
		if (p->cls == M1_2D_SCREEN)
			continue;	/* character select: its backdrop is the 3D light streaks; the two big
						   portraits of a two-player pick reach both screen edges and would
						   otherwise be taken for a 4:3 picture and stretched */
		pz[i] = z1, px0[i] = x0, px1[i] = x1;
		if (p->cls == M1_2D_TILED)
			tx0 = fminf(tx0, x0), tx1 = fmaxf(tx1, x1);
	}
	/* a repeating tiled layer is no part of a 4:3 picture: its pieces at the screen edges would
	   otherwise make the whole layer switch between background and not as it scrolls */
	tiled_layer(c);
	/* the mission battle map once it can scroll (a save with more than one region) draws a margin
	   of up to a tile (64) beyond the 4:3 screen on either side: still the map, not a wallpaper
	   (issue #17); the draw loop keeps that margin out of the sides */
	const int map_margin = (mode == 62 || mode == 63) && tx0 <= 1 && tx1 >= 639 && tx0 >= -130 && tx1 <= 770;
	const int wallpaper = !map_margin && (tx0 < -32 || tx1 > 672 || tiled_period > 0);
	/* the mission battle map (game modes 62/63, a tiled picture of exactly the screen, scrolled
	   through its texture) is the background by itself: other pieces at the edges (the map's
	   flying comet) would move the depth limit, and the map's markers with it, between frames.
	   Other menus and loading screens are tiled pictures of the screen too, with nothing placed
	   on them: they keep the usual rule (and the stretch) */
	bg_tiled_picture = (mode == 62 || mode == 63) && !wallpaper && tx0 <= 1 && tx1 >= 639;
	for (u32 i = 0; i < c->np; i++)
		if (pz[i] >= 0 && (px0[i] <= 1 || px1[i] >= 639) && !(wallpaper && c->p[i].cls == M1_2D_TILED) &&
			(!bg_tiled_picture || c->p[i].cls == M1_2D_TILED))
			zmax = fmaxf(zmax, pz[i]);
	if (zmax < 0)
		return 0;
	u32 n = 0;
	for (u32 i = 0; i < c->np; i++)
		if (pz[i] >= 0 && pz[i] <= zmax * 1.0001f + 1e-7f && !(wallpaper && c->p[i].cls == M1_2D_TILED))
			mask[i] = 1, n++;
	return n;
}

/* a 16:9 replacement picture (artswap.c) over the whole picture area, behind everything */
static void draw_replacement(int id, const u8 *rgba, u32 w, u32 h)
{
	static GLuint tex[256];
	if (id < 0 || id >= 256)
		return;
	if (!tex[id])
	{
		glGenTextures(1, &tex[id]);
		glBindTexture(GL_TEXTURE_2D, tex[id]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
		glGenerateMipmap(GL_TEXTURE_2D);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	glUseProgram(blur_prog);
	glBindVertexArray(blur_vao);
	glDisable(GL_BLEND);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_STENCIL_TEST);
	glViewport(vp_x, vp_y, vp_w, vp_h);
	glBindTexture(GL_TEXTURE_2D, tex[id]);
	glUniform1i(u_b_src, 0);
	glUniform2f(u_b_dir, 0, 0);
	glUniform1f(u_b_gain, 1);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glUseProgram(prog);
	glBindVertexArray(vao);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glEnable(GL_DEPTH_TEST);
}

/* the 16:9 sides of a 4:3 background, by M1_BGMODE ([video] backgrounds):
	 stretch  the background layer is drawn at full width (stretched by 4/3; menus, logos and
			  text in front stay undistorted). Returns 2: see the draw loop. Not for a tiled
			  picture of the screen (the mission battle map): its markers would leave their
			  places, so it gets the blur fill; nor for the Art Gallery's pictures (blur fill)
	 replace  its 16:9 replacement picture if there is one (returns 1: the layer's own
			  polygons are then not drawn, see bg_layer), else the blur fill
	 blur	 the blur fill only
	 black	 nothing: black sides, as on a 16:9 TV (issue #15)
   M1_BLURFILL=0 turns the blur fill off. */

/* ---- hi-res backgrounds ----------------------------------------------------------------------
 * A 4:3 background (the layer side_fill finds: the main menu's is 70 tiles of 64x64) is
 * enlarged by Anime4K as one picture, so the tiles meet without seams, and each tile is then
 * drawn from its own piece of the result (state_for, hb_poly): the game still draws the tiles,
 * with its tints, fades and the 16:9 modes. Pieces are flat rectangles showing a whole opaque
 * texture; others are drawn as they are. The picture is made again when the layer changes
 * (textures, contents, places: not colours), once it has stayed the same for 3 frames, so
 * nothing is enlarged while it moves. [video] portraits (HI-RES PICTURES) turns it on. */
typedef struct { u32 tcw, tsp; float r[6]; GLuint tex; } HbTile;	/* r: x0 y0 x1 y1, the texture's u v shown */
enum { HB_MAX = 512 };
static HbTile hb_tiles[HB_MAX];
static u32 hb_ntiles, hb_stable, hb_cap;
static u64 hb_sig, hb_built_sig;
static int hb_runs;
static GLuint hb_fbo[2];

/* the screen rectangle of a flat textured quad showing its texture the right way round, from
   its top left corner to u, v (r[4], r[5]: the whole texture or part of it, as the loading
   screens' half-height bottom row) */
static int hb_rect(const TaCtx *c, const TaPoly *p, float r[6])
{
	if (!((p->isp >> 25) & 1) || p->count != 4 || (p->list != 0 && p->list != 4))
		return 0;
	const TaVert *v = &c->v[p->first];
	float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
	for (int k = 0; k < 4; k++)
		x0 = fminf(x0, v[k].x), x1 = fmaxf(x1, v[k].x), y0 = fminf(y0, v[k].y), y1 = fmaxf(y1, v[k].y);
	if (x1 - x0 < 4 || y1 - y0 < 4 || x0 < -0.5f || y0 < -0.5f || x1 > 640.5f || y1 > 480.5f)
		return 0;
	float ue = -1, ve = -1;
	for (int k = 0; k < 4; k++)
		if (fabsf(v[k].x - x1) < 0.01f && fabsf(v[k].y - y1) < 0.01f)
			ue = v[k].u, ve = v[k].v;
	if (ue < 0.05f || ue > 1.001f || ve < 0.05f || ve > 1.001f)
		return 0;
	for (int k = 0; k < 4; k++)
	{
		const int left = fabsf(v[k].x - x0) < 0.01f, right = fabsf(v[k].x - x1) < 0.01f;
		const int top = fabsf(v[k].y - y0) < 0.01f, bottom = fabsf(v[k].y - y1) < 0.01f;
		if (!(left || right) || !(top || bottom) || fabsf(v[k].u - (right ? ue : 0)) > 0.002f || fabsf(v[k].v - (bottom ? ve : 0)) > 0.002f)
			return 0;
	}
	r[0] = x0, r[1] = y0, r[2] = x1, r[3] = y1, r[4] = fminf(ue, 1), r[5] = fminf(ve, 1);
	return 1;
}

static void hibg_build(const TaCtx *c, const u8 *mask, int runs)
{
	u32 tex_decode_rgba(u32 tcw, u32 tsp, u32 *out, u32 *w, u32 *h);
	unsigned gl_a4k_upscale(unsigned id, u32 w, u32 h, const u32 *rgba, int runs, const u8 *alpha, const u32 *orig);
	static int log = -1;
	if (log < 0)
		log = getenv("M1_HILOG") != 0;
	const clock_t t0 = clock();
	state_invalidate();
	for (u32 i = 0; i < hb_ntiles; i++)
		if (hb_tiles[i].tex)
			glDeleteTextures(1, &hb_tiles[i].tex);
	hb_ntiles = 0;
	static u32 *canvas, *px;
	if (!canvas)
		canvas = malloc(640 * 480 * 4), px = malloc(1024 * 1024 * 4);
	for (u32 i = 0; i < 640 * 480; i++)
		canvas[i] = 0xff000000u;
	/* the pieces, untinted, into one 640x480 picture (bilinear: exact for 1:1 tiles) */
	for (u32 i = 0; i < c->np && hb_ntiles < HB_MAX; i++)
	{
		const TaPoly *p = &c->p[i];
		float r[6];
		if (!mask[i] || !hb_rect(c, p, r))
			continue;
		int dup = 0;
		for (u32 k = 0; k < hb_ntiles && !dup; k++)
			dup = hb_tiles[k].tcw == p->tcw && hb_tiles[k].tsp == p->tsp && !memcmp(hb_tiles[k].r, r, sizeof(r));
		if (dup)
			continue;
		u32 w, h, opaque = 1;
		tex_decode_rgba(p->tcw, p->tsp, px, &w, &h);
		for (u32 k = 0; k < w * h && opaque; k++)
			opaque = px[k] >> 24 == 255;
		if (!opaque)
			continue;
		HbTile *t = &hb_tiles[hb_ntiles++];
		t->tcw = p->tcw, t->tsp = p->tsp, t->tex = 0;
		memcpy(t->r, r, sizeof(r));
		const int ya = (int)fmaxf(0, floorf(r[1])), yb = (int)fminf(480, ceilf(r[3]));
		const int xa = (int)fmaxf(0, floorf(r[0])), xb = (int)fminf(640, ceilf(r[2]));
		for (int y = ya; y < yb; y++)
			for (int x = xa; x < xb; x++)
			{
				const float cx = x + 0.5f, cy = y + 0.5f;
				if (cx < r[0] || cx >= r[2] || cy < r[1] || cy >= r[3])
					continue;
				const float uw = r[4] * w, vh = r[5] * h;	/* the texels shown */
				float fu = (cx - r[0]) / (r[2] - r[0]) * uw - 0.5f, fv = (cy - r[1]) / (r[3] - r[1]) * vh - 0.5f;
				fu = fminf(fmaxf(fu, 0), fmaxf(uw - 1.001f, 0)), fv = fminf(fmaxf(fv, 0), fmaxf(vh - 1.001f, 0));
				const u32 u0 = (u32)fu, v0 = (u32)fv;
				const float a = fu - u0, b = fv - v0;
				const u32 *q = &px[v0 * w + u0];
				u32 out = 0xff000000u;
				for (int ch = 0; ch < 24; ch += 8)
				{
					const float top = ((q[0] >> ch) & 255) * (1 - a) + ((q[1] >> ch) & 255) * a;
					const float bot = ((q[w] >> ch) & 255) * (1 - a) + ((q[w + 1] >> ch) & 255) * a;
					out |= (u32)(top * (1 - b) + bot * b + 0.5f) << ch;
				}
				canvas[y * 640 + x] = out;
			}
	}
	const GLuint big = hb_ntiles ? gl_a4k_upscale(0, 640, 480, canvas, runs, 0, 0) : 0;
	if (big)
	{
		/* each piece cut out of the enlarged picture into its own texture */
		const float k = (float)(1 << runs);
		GLint draw_fb, read_fb;
		glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fb);
		glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
		const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
		glDisable(GL_SCISSOR_TEST);
		if (!hb_fbo[0])
			glGenFramebuffers(2, hb_fbo);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, hb_fbo[0]);
		glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, big, 0);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, hb_fbo[1]);
		for (u32 i = 0; i < hb_ntiles; i++)
		{
			HbTile *t = &hb_tiles[i];
			const int sx0 = (int)lroundf(t->r[0] * k), sy0 = (int)lroundf(t->r[1] * k);
			const int sx1 = (int)lroundf(t->r[2] * k), sy1 = (int)lroundf(t->r[3] * k);
			/* the whole texture's size; the part not shown repeats the shown part's last texels,
			   so filtering at its edge stays in the picture */
			const int cw = sx1 - sx0, ch = sy1 - sy0;
			const int tw = (int)lroundf(cw / t->r[4]), th = (int)lroundf(ch / t->r[5]);
			glGenTextures(1, &t->tex);
			glBindTexture(GL_TEXTURE_2D, t->tex);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
			glBlitFramebuffer(sx0, sy0, sx1, sy1, 0, 0, cw, ch, GL_COLOR_BUFFER_BIT, GL_NEAREST);
			if (th > ch)
				glBlitFramebuffer(sx0, sy1 - 1, sx1, sy1, 0, ch, cw, th, GL_COLOR_BUFFER_BIT, GL_NEAREST);
			if (tw > cw)
				glBlitFramebuffer(sx1 - 1, sy0, sx1, sy1, cw, 0, tw, ch, GL_COLOR_BUFFER_BIT, GL_NEAREST);
			if (tw > cw && th > ch)
				glBlitFramebuffer(sx1 - 1, sy1 - 1, sx1, sy1, cw, ch, tw, th, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		}
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
		glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw_fb);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read_fb);
		if (scissor)
			glEnable(GL_SCISSOR_TEST);
		glDeleteTextures(1, &big);
		st_valid = 0;
	}
	if (log)
		printf("hibg: render %llu, %u pieces, x%d, %.1f ms\n", (unsigned long long)render_count(), hb_ntiles, 1 << runs,
			   (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC);
}

/* the hi-res pieces for this frame's background layer (mask) */
static void hibg_update(const TaCtx *c, const u8 *mask)
{
	if (!video_portraits() || vp_h <= 600)
		return;
	{
		/* M1_HILOG=1: each new layer's pieces (list, size, texture, uv), for new kinds of layers */
		static int log = -1;
		static u64 last;
		if (log < 0)
			log = getenv("M1_HILOG") != 0;
		u64 all = 0xcbf29ce484222325ull;
		for (u32 i = 0; i < c->np; i++)
			if (mask[i])
				all = (all ^ c->p[i].tcw ^ ((u64)c->p[i].tsp << 32)) * 0x100000001b3ull;
		if (log && all != last)
		{
			last = all;
			u32 m = 0;
			for (u32 i = 0; i < c->np; i++)
				m += mask[i];
			printf("hibg: render %llu layer of %u polygons\n", (unsigned long long)render_count(), m);
			for (u32 i = 0, shown = 0; i < c->np && shown < 12; i++)
				if (mask[i])
				{
					const TaPoly *p = &c->p[i];
					const TaVert *v = &c->v[p->first];
					float r[6];
					printf("  list %u n %u tcw %08x tsp %08x v0 %.1f,%.1f uv %.3f,%.3f v%u %.1f,%.1f uv %.3f,%.3f%s\n", p->list, p->count,
						   p->tcw, p->tsp, v[0].x, v[0].y, v[0].u, v[0].v, p->count - 1, v[p->count - 1].x, v[p->count - 1].y,
						   v[p->count - 1].u, v[p->count - 1].v, hb_rect(c, p, r) ? " piece" : "");
					shown++;
				}
		}
	}
	/* every polygon of the layer may have its picture enlarged (state_for); pieces below */
	if (c->np > hb_cap)
		hb_poly = realloc(hb_poly, (hb_cap = c->np) * sizeof(GLuint));
	memset(hb_poly, 0, c->np * sizeof(GLuint));
	hb_base = c->p, hb_n = c->np;
	for (u32 i = 0; i < c->np; i++)
	{
		const TaPoly *p = &c->p[i];
		if (mask[i] || p->cls || !((p->isp >> 25) & 1) || p->list == 1 || p->list == 3 || p->count < 3)
			continue;
		const TaVert *v = &c->v[p->first];
		int flat = 1;
		for (u32 k = 1; k < p->count && flat; k++)
			flat = v[k].z == v[0].z;
		if (flat)
			hb_poly[i] = HB_PIC;
	}
	u64 sig = 0xcbf29ce484222325ull;
	u32 n = 0;
	for (u32 i = 0; i < c->np; i++)
	{
		float r[6];
		if (!mask[i] || !hb_rect(c, &c->p[i], r))
			continue;
		hb_poly[i] = HB_WAIT;	/* a piece: plain until the enlarged picture is ready */
		const u64 content = tex_content_id(c->p[i].tcw, c->p[i].tsp);
		u32 words[8] = { c->p[i].tcw, c->p[i].tsp, (u32)content, (u32)(content >> 32) };
		memcpy(&words[4], r, 4 * sizeof(float));
		for (int k = 0; k < 8; k++)
			sig = (sig ^ words[k]) * 0x100000001b3ull;
		n++;
	}
	if (!n)
		return;
	if (sig != hb_sig)
		hb_sig = sig, hb_stable = 0;
	else if (hb_stable < 1000)
		hb_stable++;
	const int runs = vp_h > 1200 ? 2 : 1;	/* 4 times the 640x480 picture above 1200 lines, else 2 */
	if (sig != hb_built_sig || runs != hb_runs)
	{
		if (hb_stable < 3)
			return;	/* drawn as it is while it changes */
		hibg_build(c, mask, runs);
		hb_built_sig = sig, hb_runs = runs;
	}
	for (u32 i = 0; i < c->np; i++)
	{
		float r[6];
		if (!mask[i] || !hb_rect(c, &c->p[i], r))
			continue;
		for (u32 k = 0; k < hb_ntiles; k++)
			if (hb_tiles[k].tcw == c->p[i].tcw && hb_tiles[k].tsp == c->p[i].tsp && !memcmp(hb_tiles[k].r, r, sizeof(r)))
			{
				hb_poly[i] = hb_tiles[k].tex;
				break;
			}
	}
}

static int side_fill(TaCtx *c, const u32 *order, u32 n)
{
	hb_n = 0;	/* no hi-res background pieces unless this frame has the layer (hibg_update) */
	static int on = -1;
	if (on < 0)
		on = !getenv("M1_BLURFILL") || atoi(getenv("M1_BLURFILL"));
	const int mode = video_backgrounds();
	static u32 cap;
	if (tgt_w != 640)
		return 0;
	if (c->np > cap)
		bg_layer = realloc(bg_layer, cap = c->np);
	if (!bg_mask(c, bg_layer))
		return 0;
	{
		/* a 4:3 picture covers the 4:3 screen: the layer must cover nearly all of a 32 x 24
		   grid of it. The ending's credits shrink the picture into a frame between full-width
		   strips; enlarged into the sides, those made dark bars there (issue #11): such a
		   layout keeps black sides, as its own surroundings */
		static u8 grid[24][32];
		memset(grid, 0, sizeof(grid));
		for (u32 i = 0; i < c->np; i++)
		{
			if (!bg_layer[i])
				continue;
			const TaPoly *p = &c->p[i];
			float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
			for (u32 k = 0; k < p->count; k++)
			{
				const TaVert *t = &c->v[p->first + k];
				x0 = fminf(x0, t->x), x1 = fmaxf(x1, t->x), y0 = fminf(y0, t->y), y1 = fmaxf(y1, t->y);
			}
			const int gx0 = (int)fmaxf(0, floorf(x0 / 20 + 0.25f)), gx1 = (int)fminf(32, ceilf(x1 / 20 - 0.25f));
			const int gy0 = (int)fmaxf(0, floorf(y0 / 20 + 0.25f)), gy1 = (int)fminf(24, ceilf(y1 / 20 - 0.25f));
			for (int gy = gy0; gy < gy1; gy++)
				for (int gx = gx0; gx < gx1; gx++)
					grid[gy][gx] = 1;
		}
		u32 covered = 0;
		for (int gy = 0; gy < 24; gy++)
			for (int gx = 0; gx < 32; gx++)
				covered += grid[gy][gx];
		if (covered < 32 * 24 * 85 / 100)
		{
			memset(bg_layer, 0, c->np);
			return 0;
		}
	}
	hibg_update(c, bg_layer);	/* hi-res pieces, whatever fills the sides */
	if (mode == 3)
		return 0;	/* black sides */
	if (mode == 2 && !bg_tiled_picture && !bg_gallery_picture)
		return 2;	/* stretch; the mission battle map gets the blur fill instead (markers are placed
					   on it), and so do the Art Gallery's pictures (artwork, never distorted) */
	if (!blur_prog)
	{
		blur_init();
		glUseProgram(prog);
		glBindVertexArray(vao);
		glBindBuffer(GL_ARRAY_BUFFER, vbo);
	}
	if (mode == 1)
	{
		const u8 *artswap_find(const TaCtx *c, const u8 *mask, u32 *w, u32 *h, int *id);
		u32 w, h;
		int id;
		const u8 *rgba = artswap_find(c, bg_layer, &w, &h, &id);
		if (rgba)
		{
			draw_replacement(id, rgba, w, h);
			return 1;
		}
	}
	if (!on)
		return 0;
	u8 *mask = bg_layer;
	/* the background layer, enlarged by 4/3 about the centre: covers 16:9 without distortion */
	glBindFramebuffer(GL_FRAMEBUFFER, blur_fbo[0]);
	glViewport(0, 0, BLUR_W, BLUR_H);
	glDisable(GL_SCISSOR_TEST);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glUniform2f(u_xmap, 320, 1);
	glUniform2f(u_umap, 0, 1);
	glUniform2f(u_ymap, 240, 4.0f / 3);
	glUniform1f(u_xoff, 0);
	for (u32 j = 0; j < n; j++)
		if (mask[order[j]])
		{
			const TaPoly *p = &c->p[order[j]];
			set_state(p, p->list);
			glDisable(GL_DEPTH_TEST);
			glDisable(GL_STENCIL_TEST);
			draw_strip(c, p);
		}
	glUniform2f(u_ymap, 0, 1);
	glDisable(GL_BLEND);
	/* blur: horizontal and vertical passes, twice */
	glUseProgram(blur_prog);
	glBindVertexArray(blur_vao);
	glUniform1i(u_b_src, 0);
	glUniform1f(u_b_gain, 1);
	for (int it = 0; it < 2; it++)
		for (int d = 0; d < 2; d++)
		{
			glBindFramebuffer(GL_FRAMEBUFFER, blur_fbo[1 - d]);
			glBindTexture(GL_TEXTURE_2D, blur_tex[d]);
			glUniform2f(u_b_dir, d ? 0 : 1.0f / BLUR_W, d ? 1.0f / BLUR_H : 0);
			glDrawArrays(GL_TRIANGLES, 0, 3);
		}
	/* under the frame, darkened so the picture in the middle stands out */
	glBindFramebuffer(GL_FRAMEBUFFER, frame_fbo);
	glViewport(vp_x, vp_y, vp_w, vp_h);
	glBindTexture(GL_TEXTURE_2D, blur_tex[0]);
	glUniform2f(u_b_dir, 0, 0);
	glUniform1f(u_b_gain, 0.6f);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glUseProgram(prog);
	glBindVertexArray(vao);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glEnable(GL_DEPTH_TEST);
	return 0;
}

/* the fog table (FOG_TABLE, 005F8200: 128 entries of 16 bits), FOG_COL_RAM and FOG_DENSITY (a
   mantissa 1.7 in bits 15-8 and a signed exponent in bits 7-0) for table fog (TSP fog modes 0 and
   3; the character select's light tunnel fades into its band with it, issue #11) */
static void fog_table_update(void)
{
	static u16 last[128];
	static int have;
	u8 tab[256];
	u16 cur[128];
	for (int i = 0; i < 128; i++)
	{
		cur[i] = (u16)hw_reg(0x005f8200u + 4u * (u32)i);
		tab[i] = (u8)(cur[i] & 255);		/* row 0: the end of the step */
		tab[128 + i] = (u8)(cur[i] >> 8);	/* row 1: its start */
	}
	glActiveTexture(GL_TEXTURE1);
	if (!fog_tex)
	{
		glGenTextures(1, &fog_tex);
		glBindTexture(GL_TEXTURE_2D, fog_tex);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	else
		glBindTexture(GL_TEXTURE_2D, fog_tex);
	if (!have || memcmp(cur, last, sizeof(cur)))
	{
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 128, 2, 0, GL_RED, GL_UNSIGNED_BYTE, tab);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
		memcpy(last, cur, sizeof(cur));
		have = 1;
	}
	glActiveTexture(GL_TEXTURE0);
	const u32 fc = hw_reg(0x005f80b0), fd = hw_reg(0x005f80b8);
	glUniform3f(u_fog_col_ram, ((fc >> 16) & 255) / 255.0f, ((fc >> 8) & 255) / 255.0f, (fc & 255) / 255.0f);
	glUniform1f(u_fog_density, (float)((fd >> 8) & 255) / 128.0f * ldexpf(1.0f, (int)(s8)(fd & 255)));
}

/* M1_GPUPROF=1 (diagnosis, issue #19): each frame drawn in over 4 ms, split into phases timed
   with glFinish (the GPU's work included): setup (vertex upload), opaque and punch-through (with
   texture decodes and uploads), shadows (modifier volumes), translucent, rest (the picture to the
   window, overlays, swap). The glFinish calls slow drawing down: for measuring only. */
static int gp_on = -1;
static double gp_ms[6];
static u64 gp_t;
static void gp_mark(u32 next)
{
	if (gp_on < 0)
		gp_on = getenv("M1_GPUPROF") != 0;
	if (!gp_on)
		return;
	glFinish();
	const u64 t = SDL_GetPerformanceCounter();
	if (gp_t)
		gp_ms[gp_phase] += (double)(t - gp_t) * 1000.0 / (double)SDL_GetPerformanceFrequency();
	gp_t = t, gp_phase = next;
}
/* M1_GPUPROF=2 (issue #19): every batch of polygons drawn with one state is also timed on the GPU
   with a timer query; a slow frame then lists its 12 costliest translucent batches: GPU time,
   polygons, screen area covered (in screens, so overdraw shows), blending, texture, shader
   flags, depth test, and the share of the translucent phase the batches account for (the rest is
   state changes, uploads and the driver) */
typedef struct
{
	u32 phase, n, tcw, tsp, isp, list;
	float area;
	PolyState s;
	GLuint q;
} GpBatch;
enum { GP_MAXB = 8192 };
static GpBatch gp_b[GP_MAXB];
static u32 gp_nb;
static GLuint gp_q[GP_MAXB];
static const TaCtx *gp_ctx;
static u64 gp_cpu[2];	/* CPU time in state_for (texture lookups, decodes, uploads): other phases, translucent */
static PFNGLGENQUERIESPROC q_GenQueries;
static PFNGLBEGINQUERYPROC q_BeginQuery;
static PFNGLENDQUERYPROC q_EndQuery;
static PFNGLGETQUERYOBJECTUI64VPROC q_GetQueryObjectui64v;
static int gp_level(void)
{
	static int lv = -1;
	if (lv < 0)
	{
		lv = getenv("M1_GPUPROF") ? atoi(getenv("M1_GPUPROF")) : 0;
		if (lv >= 2)
		{
			q_GenQueries = (PFNGLGENQUERIESPROC)SDL_GL_GetProcAddress("glGenQueries");
			q_BeginQuery = (PFNGLBEGINQUERYPROC)SDL_GL_GetProcAddress("glBeginQuery");
			q_EndQuery = (PFNGLENDQUERYPROC)SDL_GL_GetProcAddress("glEndQuery");
			q_GetQueryObjectui64v = (PFNGLGETQUERYOBJECTUI64VPROC)SDL_GL_GetProcAddress("glGetQueryObjectui64v");
			if (!q_GenQueries || !q_BeginQuery || !q_EndQuery || !q_GetQueryObjectui64v)
				lv = 1;
			else
				q_GenQueries(GP_MAXB, gp_q);
		}
	}
	return lv;
}
static void gp_batch_begin(void)
{
	if (gp_level() < 2 || gp_nb >= GP_MAXB || !gp_ctx)
		return;
	GpBatch *g = &gp_b[gp_nb];
	g->phase = gp_phase, g->n = b_n, g->tcw = gp_batch_tcw, g->tsp = gp_batch_tsp, g->isp = gp_batch_isp, g->list = gp_batch_list;
	g->s = st_cur, g->q = gp_q[gp_nb];
	float area = 0;
	for (u32 i = 0; i < b_n; i++)
		for (GLsizei k = 0; k + 2 < b_count[i]; k++)
		{
			const u32 j = (u32)b_first[i] + (u32)k;
			if (j + 2 >= gp_ctx->nv)
				break;
			const TaVert *a = &gp_ctx->v[j], *b = &gp_ctx->v[j + 1], *d = &gp_ctx->v[j + 2];
			area += fabsf((b->x - a->x) * (d->y - a->y) - (b->y - a->y) * (d->x - a->x)) * 0.5f;
		}
	g->area = area / (640.0f * 480.0f);
	q_BeginQuery(GL_TIME_ELAPSED, g->q);
}
static void gp_batch_end(void)
{
	if (gp_level() < 2 || gp_nb >= GP_MAXB || !gp_ctx)
		return;
	q_EndQuery(GL_TIME_ELAPSED);
	gp_nb++;
}
static int gp_cmp(const void *a, const void *b)
{
	const double x = ((const GpBatch *)a)->area, y = ((const GpBatch *)b)->area;	/* area holds the ms when sorting */
	return x < y ? 1 : x > y ? -1 : 0;
}
static void gp_batch_report(double translucent_ms)
{
	if (gp_level() < 2)
		return;
	static GpBatch top[GP_MAXB];
	u32 n = 0;
	double sum = 0, cover = 0;
	for (u32 i = 0; i < gp_nb; i++)
	{
		GLuint64 ns = 0;
		q_GetQueryObjectui64v(gp_b[i].q, GL_QUERY_RESULT, &ns);
		if (gp_b[i].phase != 3)
			continue;
		top[n] = gp_b[i];
		cover += gp_b[i].area;
		top[n].area = (float)(ns / 1e6);	/* ms from here on */
		top[n].s.alpha_ref = gp_b[i].area;	/* the screens covered, kept for the print */
		sum += ns / 1e6;
		n++;
	}
	qsort(top, n, sizeof(top[0]), gp_cmp);
	const double f = 1000.0 / (double)SDL_GetPerformanceFrequency();
	printf("  translucent: %u batches %.2f ms of %.2f (%.0f%%), covering %.1f screens; state_for %.2f ms (other phases %.2f), of it %u uploads %.2f ms (other phases %u, %.2f)\n", n, sum,
		   translucent_ms, translucent_ms > 0 ? 100 * sum / translucent_ms : 0.0, cover, gp_cpu[1] * f, gp_cpu[0] * f, gp_uploads[1], gp_upload[1] * f,
		   gp_uploads[0], gp_upload[0] * f);
	gp_cpu[0] = gp_cpu[1] = 0, gp_upload[0] = gp_upload[1] = 0, gp_uploads[0] = gp_uploads[1] = 0;
	static const char *const bf[] = { "0", "1", "Cs", "1-Cs", "As", "1-As", "Ad", "1-Ad" };
	for (u32 i = 0; i < n && i < 12; i++)
	{
		const GpBatch *g = &top[i];
		const u32 src = (g->tsp >> 29) & 7, dst = (g->tsp >> 26) & 7;
		const u32 tw = 8u << ((g->tsp >> 3) & 7), th = (g->tcw >> 31) ? tw : 8u << (g->tsp & 7);
		printf("    %.2f ms: %u polys, %.2f screens, blend %s/%s, %s tex %08x %ux%u filter %u mode %d, flags %d, depth func %u write %d, fog %d\n",
			   (double)g->area, g->n, (double)g->s.alpha_ref, bf[src], bf[dst], (g->isp >> 25) & 1 ? "" : "no", g->tcw, tw, th,
			   (g->tsp >> 13) & 3, g->s.mode, g->s.flags, (g->isp >> 29) & 7, g->s.depth_mask, g->s.fog);
	}
}

static void draw_lists(TaCtx *c)
{
	gp_ctx = c, gp_nb = 0;
	gp_mark(0);
	fog_table_update();
	glEnable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glBindVertexArray(vao);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((c->nv + 3 * c->nmv + 4) * sizeof(TaVert)), 0, GL_STREAM_DRAW);
	glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(c->nv * sizeof(TaVert)), c->v);
	upload_volumes(c);
	text_runs(c);
	fight_panels(c);
	tiled_layer(c);
	/* drawing order: opaque, punch-through, then translucent. Unless ISP_FEED_CFG bit 0
	   (presort) is set, the PowerVR sorts translucent polygons by depth itself: here per strip,
	   back to front by the strip's mean 1/w (smaller = farther) */
	static u32 *idx;
	static float *key;
	static u32 cap;
	if (c->np > cap)
	{
		cap = c->np;
		idx = realloc(idx, cap * sizeof(u32));
		key = realloc(key, cap * sizeof(float));
	}
	u32 n = 0, ntr = 0;
	static const int order[2] = { 0, 4 };
	for (int pass = 0; pass < 2; pass++)
		for (u32 i = 0; i < c->np; i++)
			if (c->p[i].list == order[pass])
				idx[n++] = i;
	const u32 first_tr = n;
	for (u32 i = 0; i < c->np; i++)
		if (c->p[i].list == 2)
		{
			float sum = 0;
			for (u32 k = 0; k < c->p[i].count; k++)
				sum += c->v[c->p[i].first + k].z;
			key[i] = c->p[i].count ? sum / c->p[i].count : 0;
			idx[n++] = i;
			ntr++;
		}
	if (!(hw_reg(0x005f8098) & 1))
	{
		/* stable insertion sort keeps submission order for equal depths */
		for (u32 a = first_tr + 1; a < n; a++)
		{
			const u32 v = idx[a];
			u32 b = a;
			while (b > first_tr && key[idx[b - 1]] > key[v])
			{
				idx[b] = idx[b - 1];
				b--;
			}
			idx[b] = v;
		}
	}
	(void)ntr;
	const int replaced = side_fill(c, idx, n);	/* 16:9 sides of a 4:3 background */
	int shaded = 0;
	/* polygons with the same state as the one before join its batch (state_apply, batch_flush) */
	glUniform1f(u_xoff, 0);
	st_valid = 0;
	for (u32 j = 0; j < n; j++)
		{
			if (j == 0)
				batch_flush(), gp_mark(1);
			if (j == first_tr && !shaded)
			{
				state_invalidate();
				batch_flush(), gp_mark(2);
				shadow_pass(c);
				shaded = 1;
				gp_mark(3);
			}
			const TaPoly *p = &c->p[idx[j]];
			if (p->count < 3 || (replaced == 1 && bg_layer[idx[j]]))
				continue;	/* a replaced background's own pieces */
			static int skip_list = -2;	/* M1_SKIPLIST=n (debugging): leave out display list n */
			if (skip_list == -2)
				skip_list = getenv("M1_SKIPLIST") ? atoi(getenv("M1_SKIPLIST")) : -1;
			if ((int)p->list == skip_list)
				continue;
			static int skip_a = -2, skip_b = -1;	/* M1_SKIPPOLY=a[:b] (debugging): leave out polygons a..b */
			if (skip_a == -2)
			{
				skip_a = -1;
				if (getenv("M1_SKIPPOLY") && sscanf(getenv("M1_SKIPPOLY"), "%d:%d", &skip_a, &skip_b) == 1)
					skip_b = skip_a;
			}
			if ((int)idx[j] >= skip_a && (int)idx[j] <= skip_b)
				continue;
			static int only = -2;	/* M1_ONLYPOLY=n (debugging): draw polygon n alone */
			if (only == -2)
				only = getenv("M1_ONLYPOLY") ? atoi(getenv("M1_ONLYPOLY")) : -1;
			if (only >= 0 && (int)idx[j] != only)
				continue;
			PolyState s;
			const u64 gp_s0 = gp_level() >= 2 ? SDL_GetPerformanceCounter() : 0;
			state_for(p, p->list, &s);
			if (gp_s0)
				gp_cpu[gp_phase == 3] += SDL_GetPerformanceCounter() - gp_s0;
			{
				/* M1_FLATPOLY=a:b (debugging): polygons a..b without their texture */
				static int fa = -2, fb = -1;
				if (fa == -2)
				{
					fa = -1;
					if (getenv("M1_FLATPOLY"))
						sscanf(getenv("M1_FLATPOLY"), "%d:%d", &fa, &fb);
				}
				if ((int)idx[j] >= fa && (int)idx[j] <= fb)
				{
					if (!getenv("M1_FLATKEEPTEX"))
						s.mode = -1;
					s.alpha_ref = -1.0f;
				}
			}
			float xa, xk, ua, uk;
			wide_map(c, p, &xa, &xk, &ua, &uk);
			if (replaced == 2 && bg_layer[idx[j]])
				xa = 320, xk = 1 / view_zoom, ua = 0, uk = 1;	/* stretched background: the picture's full width */
			s.xmap[0] = xa, s.xmap[1] = xk;
			s.umap[0] = ua, s.umap[1] = uk;
			if (p->clip == 2)
			{
				const float cx0 = 320 + (xa + (p->clip_rect[0] - xa) * xk - 320) * view_zoom,
							cx1 = 320 + (xa + (p->clip_rect[2] - xa) * xk - 320) * view_zoom;
				const int x0 = vp_x + (int)(cx0 * vp_w / tgt_w), x1 = vp_x + (int)(cx1 * vp_w / tgt_w);
				const int y0 = vp_y + vp_h - p->clip_rect[3] * vp_h / tgt_h, y1 = vp_y + vp_h - p->clip_rect[1] * vp_h / tgt_h;
				s.scissor = 1;
				s.sc[0] = x0, s.sc[1] = y0, s.sc[2] = x1 - x0, s.sc[3] = y1 - y0;
			}
			else if (bg_tiled_picture && replaced != 2 && p->cls == M1_2D_TILED)
			{
				/* the mission battle map: its scroll margin beyond the 4:3 screen (issue #17) is
				   left out, so the sides show the background setting (blur, black, ...) */
				const float cx0 = 320 + (xa + (0 - xa) * xk - 320) * view_zoom,
							cx1 = 320 + (xa + (640 - xa) * xk - 320) * view_zoom;
				const int x0 = vp_x + (int)(cx0 * vp_w / tgt_w), x1 = vp_x + (int)(cx1 * vp_w / tgt_w);
				s.scissor = 1;
				s.sc[0] = x0, s.sc[1] = vp_y, s.sc[2] = x1 - x0, s.sc[3] = vp_h;
			}
			/* state_for may have decoded a texture: gl_upload_texture sent the batch first */
			if (!st_valid || memcmp(&s, &st_cur, sizeof(s)))
			{
				batch_flush();
				state_apply(&s, 1);
			}
			int bad = 0;
			for (u32 k = 0; k < p->count && !bad; k++)
				bad = vert_bad(&c->v[p->first + k]);
			if (!bad)
			{
				if (!b_n)
					gp_batch_tcw = p->tcw, gp_batch_tsp = p->tsp, gp_batch_isp = p->isp, gp_batch_list = p->list;
				batch_add(p->first, p->count);
			}
			else
			{
				batch_flush();
				draw_strip(c, p);	/* without the triangles that have a bad vertex */
			}
			if (p->cls == M1_2D_TILED && xk != 1 && tiled_period > 0 && !bg_tiled_picture && !(replaced == 2 && bg_layer[idx[j]]))
			{
				/* copies one period or more to either side fill 16:9 (virtual x -249..889); a 4:3
				   layer's copies only where the layer itself draws nothing, so nothing is drawn twice */
				batch_flush();
				float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f;
				for (u32 k = 0; k < p->count; k++)
				{
					const TaVert *v = &c->v[p->first + k];
					x0 = fminf(x0, v->x), x1 = fmaxf(x1, v->x), y0 = fminf(y0, v->y);
				}
				const int r = tiled_row_of(y0);
				const float rx0 = r >= 0 ? tiled_row[r].x0 : tiled_x0, rx1 = r >= 0 ? tiled_row[r].x1 : tiled_x1;
				int moved = 0;
				for (int m = -4; m <= 4; m++)
				{
					const float d = m * tiled_period;
					if (m == 0 || x1 + d < -250 || x0 + d > 890 || (x1 + d > rx0 + 1 && x0 + d < rx1 - 1))
						continue;
					glUniform1f(u_xoff, d * xk);
					moved = 1;
					draw_strip(c, p);
				}
				if (moved)
					glUniform1f(u_xoff, 0);
			}
		}
	batch_flush();
	if (!shaded)
		gp_mark(2), shadow_pass(c);
	gp_mark(4);
	glDisable(GL_BLEND);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_STENCIL_TEST);
}

/* Render to texture: FB_W_SOF1 bit 24 set (as Flycast decides). The picture goes to VRAM
   (64-bit layout) at FB_W_SOF1, FB_X_CLIP/FB_Y_CLIP wide/high, FB_W_LINESTRIDE * 8 bytes per
   line, in the FB_W_CTRL pack mode (0 0555, 1 565, 2 4444, 3 1555 with alpha threshold,
   4-6 32-bit). */
void hw_vram_touch(u32 offset, u32 len);
static double perf_rtt[3];	/* M1_PERF: render to texture draw, read back, to VRAM */
static u64 perf_now(void) { return SDL_GetPerformanceCounter(); }
static GLuint rtt_fbo, rtt_color, rtt_depth;
static u64 rtt_count;

static void render_to_texture(TaCtx *c)
{
	const u32 sof = hw_reg(0x005f8060) & (VRAM_SIZE - 1);
	const u32 xc = hw_reg(0x005f8068), yc = hw_reg(0x005f806c);
	const u32 w = ((xc >> 16) & 0x7ff) + 1, h = ((yc >> 16) & 0x3ff) + 1;
	const u32 ctrl = hw_reg(0x005f8048), pack = ctrl & 7, bpp = pack >= 4 ? 4 : 2;
	u32 stride = (hw_reg(0x005f804c) & 0x1ff) * 8;
	if (!stride)
		stride = w * bpp;
	if (w > 1024 || h > 1024)
		return;
	if (!rtt_fbo)
	{
		glGenFramebuffers(1, &rtt_fbo);
		glGenTextures(1, &rtt_color);
		glGenRenderbuffers(1, &rtt_depth);
		glBindTexture(GL_TEXTURE_2D, rtt_color);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1024, 1024, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
		glBindRenderbuffer(GL_RENDERBUFFER, rtt_depth);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, 1024, 1024);
		glBindFramebuffer(GL_FRAMEBUFFER, rtt_fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rtt_color, 0);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rtt_depth);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, rtt_fbo);
	glViewport(0, 0, (GLsizei)w, (GLsizei)h);
	glUniform2f(u_target, (float)w, (float)h);
	glUniform1f(u_xzoom, 1);
	vp_x = vp_y = 0; vp_w = tgt_w = (int)w; vp_h = tgt_h = (int)h;
	const u32 bg = background_colour();
	glClearColor(((bg >> 16) & 255) / 255.0f, ((bg >> 8) & 255) / 255.0f, (bg & 255) / 255.0f, (bg >> 24) / 255.0f);
	glClearDepth(1.0);
	glDepthMask(GL_TRUE);
	glStencilMask(0xff);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	const u64 t0 = perf_now();
	draw_lists(c);	/* tgt_w != 640 here: no widescreen mapping inside render targets */
	const u64 t1 = perf_now();
	static u8 px[1024 * 1024 * 4];
	glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, px);
	const u64 t2 = perf_now();
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	u8 *vram = hw_vram();
	const u32 threshold = (ctrl >> 16) & 255;
	for (u32 y = 0; y < h; y++)
	{
		const u8 *src = px + (h - 1 - y) * w * 4;	/* GL rows are bottom-up */
		const u32 line = sof + y * stride;
		for (u32 x = 0; x < w; x++, src += 4)
		{
			const u32 r = src[0], g = src[1], b = src[2], a = src[3];
			u32 v;
			switch (pack)
			{
			case 0: v = (r >> 3) << 10 | (g >> 3) << 5 | b >> 3 | (ctrl & 0x8000); break;
			case 1: v = (r >> 3) << 11 | (g >> 2) << 5 | b >> 3; break;
			case 2: v = (a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | b >> 4; break;
			case 3: v = (a >= threshold ? 0x8000u : 0) | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3; break;
			default: v = a << 24 | r << 16 | g << 8 | b; break;
			}
			memcpy(vram + ((line + x * bpp) & (VRAM_SIZE - 1)), &v, bpp);
		}
		hw_vram_touch(line, w * bpp);
	}
	const double f = 1000.0 / (double)SDL_GetPerformanceFrequency();
	perf_rtt[0] += (double)(t1 - t0) * f, perf_rtt[1] += (double)(t2 - t1) * f, perf_rtt[2] += (double)(perf_now() - t2) * f;
	rtt_count++;
}

u64 render_rtt_count(void) { return rtt_count; }
u64 render_count(void) { return rendered; }
double video_ms(void) { return (double)SDL_GetPerformanceCounter() * 1000.0 / (double)SDL_GetPerformanceFrequency(); }	/* wall clock (tex.c) */

/* M1_RECORD=out.mp4 (footage): every V Blank, the last finished frame (at the drawing resolution,
   M1_RESOLUTION) goes to ffmpeg (M1_FFMPEG, default "ffmpeg") at exactly 60 frames a second, so
   it lines up with M1_WAV, which is made from the same virtual clock; a frame is repeated while
   the game draws nothing (loading). Runs at any speed (M1_FAST): nothing depends on real time.
   M1_RECORD_FROM=n: start at V Blank n. */
/* an output name not taken yet: a lobby match restarts the program, and the new process must
   not overwrite its parent's recording (name.mp4, then name-1.mp4, ...); also for M1_WAV */
const char *record_name(const char *path)
{
	static char buf[4][1024];
	static int k;
	char *b = buf[k++ & 3];
	snprintf(b, 1024, "%s", path);
	const char *dot = strrchr(path, '.');
	const int stem = dot ? (int)(dot - path) : (int)strlen(path);
	for (int n = 1; n < 100; n++)
	{
		FILE *f = fopen(b, "rb");
		if (!f)
			break;
		fclose(f);
		snprintf(b, 1024, "%.*s-%d%s", stem, path, n, dot ? dot : "");
	}
	return b;
}
static FILE *rec_pipe;
static u8 *rec_px;
static int rec_w, rec_h;
static u64 rec_vblank, rec_frames;
static void rec_close(void)
{
	if (rec_pipe)
	{
#ifdef _WIN32
		_pclose(rec_pipe);
#else
		pclose(rec_pipe);
#endif
		rec_pipe = 0;
		printf("record: %llu frames\n", (unsigned long long)rec_frames);
	}
}
void video_record_vblank(void)
{
	static int on = -1;
	static u64 from;
	if (on < 0)
	{
		on = getenv("M1_RECORD") != 0 && video_enabled();
		from = getenv("M1_RECORD_FROM") ? strtoull(getenv("M1_RECORD_FROM"), 0, 10) : 0;
	}
	const u64 vb = rec_vblank++;
	if (!on || vb < from)
		return;
	if (!rec_pipe)
	{
		/* the size comes from M1_RESOLUTION: the first V Blanks come before the first frame and
		   are recorded black, so the video starts with the audio (M1_WAV) */
		if (!res_w)
		{
			printf("record: set M1_RESOLUTION (for example 3840x2160)\n");
			on = 0;
			return;
		}
		rec_w = res_w, rec_h = res_h;
		char cmd[2048];
		const char *ff = getenv("M1_FFMPEG") ? getenv("M1_FFMPEG") : "ffmpeg";
		const char *out = record_name(getenv("M1_RECORD"));
		snprintf(cmd, sizeof(cmd), "\"\"%s\" -loglevel error -y -f rawvideo -pix_fmt rgb24 -s %dx%d -r 60 -i - -vf vflip "
				 "-c:v libx264 -preset fast -crf 12 -pix_fmt yuv420p \"%s\"\"", ff, rec_w, rec_h, out);
#ifdef _WIN32
		rec_pipe = _popen(cmd, "wb");
#else
		rec_pipe = popen(cmd, "w");
#endif
		if (!rec_pipe)
		{
			printf("record: cannot start %s\n", ff);
			on = 0;
			return;
		}
		rec_px = malloc((size_t)rec_w * rec_h * 3);
		atexit(rec_close);
		printf("record: %dx%d at 60 fps from V Blank %llu to %s\n", rec_w, rec_h, (unsigned long long)vb, getenv("M1_RECORD"));
	}
	if (!resolve_fbo || frame_w != rec_w || frame_h != rec_h)
		memset(rec_px, 0, (size_t)rec_w * rec_h * 3);	/* nothing drawn yet: black */
	else
	{
		glBindFramebuffer(GL_READ_FRAMEBUFFER, resolve_fbo);
		glPixelStorei(GL_PACK_ALIGNMENT, 1);
		glReadPixels(0, 0, rec_w, rec_h, GL_RGB, GL_UNSIGNED_BYTE, rec_px);
		glBindFramebuffer(GL_FRAMEBUFFER, frame_fbo);
	}
	fwrite(rec_px, 1, (size_t)rec_w * rec_h * 3, rec_pipe);
	rec_frames++;
}

static void render_frame_draw(TaCtx *c);

/* Text over the finished picture in the BIOS font, like the game's help line: white with a dark
   outline and shadow; nothing without the font. Output only, never the game's state. */
const char *online_latest(void);
static GLuint label_prog;
static GLint u_v_col;
typedef struct
{
	GLuint tex;
	int w;
	char text[48];
} Label;
/* text 18 of 480 lines high, 8 lines above the bottom: at the right edge (right, 10 lines in)
   or centred, at opacity a */
static void draw_label(Label *lb, const char *text, int rw, int rh, int right, float a)
{
	const u8 *font = hw_font_12x24();
	if (!font)
		return;
	if (!blur_prog)
		blur_init();	/* its full-target triangle and vertex shader */
	if (!label_prog)
	{
		static const char *fs =
			"#version 330 core\n"
			"in vec2 uv; out vec4 o;\n"
			"uniform sampler2D src; uniform vec4 col;\n"
			"void main() { o = vec4(col.rgb, col.a * texture(src, uv).r); }\n";
		label_prog = glCreateProgram();
		glAttachShader(label_prog, shader(GL_VERTEX_SHADER, blur_vs));
		glAttachShader(label_prog, shader(GL_FRAGMENT_SHADER, fs));
		glLinkProgram(label_prog);
		u_v_col = glGetUniformLocation(label_prog, "col");
	}
	if (!lb->tex || strcmp(text, lb->text))
	{
		snprintf(lb->text, sizeof(lb->text), "%s", text);
		if (lb->tex)
			glDeleteTextures(1, &lb->tex);
		/* the text as a coverage mask, bottom row first (GL); glyph i is ASCII 32 + i */
		const int n = (int)strlen(lb->text);
		lb->w = n * 12;
		u8 *px = calloc((size_t)lb->w * 24, 1);
		for (int k = 0; k < n; k++)
		{
			const int ch = (u8)lb->text[k];
			if (ch < 33 || ch > 126)
				continue;
			const u8 *g = font + (ch - 32) * 36;
			for (int y = 0; y < 24; y++)
				for (int x = 0; x < 12; x++)
				{
					const int bit = y * 12 + x;
					if (g[bit >> 3] & (0x80 >> (bit & 7)))
						px[(23 - y) * lb->w + k * 12 + x] = 255;
				}
		}
		glGenTextures(1, &lb->tex);
		glBindTexture(GL_TEXTURE_2D, lb->tex);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, lb->w, 24, 0, GL_RED, GL_UNSIGNED_BYTE, px);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
		free(px);
	}
	const float s = rh / 480.0f * 0.75f;
	const int w = (int)(lb->w * s + 0.5f), h = (int)(24 * s + 0.5f);
	const int x = right ? rw - (int)(10 * rh / 480.0f) - w : (rw - w) / 2, y = (int)(8 * rh / 480.0f);
	const int sh = rh >= 1440 ? 3 : rh >= 720 ? 2 : 1;	/* outline width in pixels */
	glUseProgram(label_prog);
	glBindVertexArray(blur_vao);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_STENCIL_TEST);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glBindTexture(GL_TEXTURE_2D, lb->tex);
	/* a dark outline (8 directions) and a drop shadow, so it reads over any background (the
	   unlocked main menu's bright collage too) */
	glUniform4f(u_v_col, 0, 0, 0, 0.85f * a);
	for (int dy = -1; dy <= 1; dy++)
		for (int dx = -1; dx <= 1; dx++)
			if (dx || dy)
			{
				glViewport(x + dx * sh, y + dy * sh, w, h);
				glDrawArrays(GL_TRIANGLES, 0, 3);
			}
	glViewport(x + 2 * sh, y - 2 * sh, w, h);
	glUniform4f(u_v_col, 0, 0, 0, 0.6f * a);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glViewport(x, y, w, h);
	glUniform4f(u_v_col, 0.92f, 0.92f, 0.92f, 0.9f * a);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glViewport(0, 0, rw, rh);
	glUseProgram(prog);
	glBindVertexArray(vao);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glEnable(GL_DEPTH_TEST);
}

/* "ver. 1.0.1" in the main menu's lower right corner. A newer release (online.c's update check)
   is added: "ver. 1.1.6 (1.1.7 is out)". */
static void draw_version(int rw, int rh)
{
	static Label lb;
	char text[48];
	if (online_latest()[0])
		snprintf(text, sizeof(text), "ver. %s (%s is out)", VOLDORIUM_VERSION, online_latest());
	else
		snprintf(text, sizeof(text), "ver. %s", VOLDORIUM_VERSION);
	draw_label(&lb, text, rw, rh, 1, 1);
}

/* Esc quits only when pressed twice (issue #16): the first press shows "Press Esc again to quit"
   at the bottom centre for ESC_MS; a second press within that time quits (or leaves an online
   match). The game runs on meanwhile. EXIT GAME in the main menu and closing the window still
   quit straight away. Wall-clock time: output only, never the game's state. */
#define ESC_MS 2000
static u32 esc_at;	/* SDL_GetTicks of the first press, 0: none */
static int esc_confirmed(void)
{
	const u32 now = SDL_GetTicks();
	if (esc_at && now - esc_at < ESC_MS)
		return 1;
	esc_at = now ? now : 1;
	return 0;
}
static void draw_esc_prompt(int rw, int rh)
{
	static Label lb;
	if (!esc_at)
		return;
	const u32 t = SDL_GetTicks() - esc_at;
	if (t >= ESC_MS)
	{
		esc_at = 0;
		return;
	}
	const float a = t < ESC_MS - 400 ? 1.0f : (float)(ESC_MS - t) / 400.0f;	/* fades out at the end */
	draw_label(&lb, "Press Esc again to quit", rw, rh, 0, a);
}
/* M1_PERF=1: every 300 renders, the mean wall time per frame spent in the game (everything
   between two renders, pacing included unless M1_FAST) and in drawing, with the game mode */
static u64 perf_prev;
static double perf_game, perf_draw, perf_polys;
static void perf_report(u64 t0, u64 t1, const TaCtx *c)
{
	static int on = -1;
	if (on < 0)
		on = getenv("M1_PERF") != 0;
	if (!on)
		return;
	const double f = 1000.0 / (double)SDL_GetPerformanceFrequency();
	{
		/* M1_PERF=slow (or with a number: the threshold in ms, default 12): also every frame
		   whose game plus draw time is over it, with its texture decodes */
		static double slow = -1;
		static u64 last_dec;
		if (slow < 0)
		{
			const char *e = getenv("M1_PERF");
			slow = !strncmp(e, "slow", 4) ? (e[4] ? atof(e + 4) : 12.0) : 0;
		}
		u64 tex_decodes(void);
		const double g = perf_prev ? (double)(t0 - perf_prev) * f : 0, d = (double)(t1 - t0) * f;
		{
			extern double tex_decode_ms;
			extern u64 tex_lookups, tex_probes;
			u32 tex_slots_used(void);
			if (slow > 0 && g + d > slow)
				printf("perf slow: render %llu mode %u: game %.1f ms, draw %.1f ms, %u polygons, %llu texture decodes (%.1f ms), %llu lookups %llu probes, %u slots used\n",
					   (unsigned long long)rendered, rd32(0x8c24a584), g, d, c ? c->np : 0,
					   (unsigned long long)(tex_decodes() - last_dec), tex_decode_ms,
					   (unsigned long long)tex_lookups, (unsigned long long)tex_probes, tex_slots_used());
			tex_decode_ms = 0;
			tex_lookups = tex_probes = 0;
		}
		last_dec = tex_decodes();
	}
	if (perf_prev)
		perf_game += (double)(t0 - perf_prev) * f;
	perf_draw += (double)(t1 - t0) * f;
	perf_polys += c ? c->np : 0;
	perf_prev = t1;
	if (rendered % 300 == 0)
	{
		printf("perf: renders %llu-%llu, mode %u: game %.2f ms, draw %.2f ms per frame, %.0f polygons\n",
			   (unsigned long long)rendered - 299, (unsigned long long)rendered, rd32(0x8c24a584),
			   perf_game / 300, perf_draw / 300, perf_polys / 300);
		{
			u64 tex_decodes(void), tex_rehashed(void);
			static u64 last, last_same;
			printf("      texture decodes: %.1f per frame, %.1f rewritten unchanged\n", (double)(tex_decodes() - last) / 300,
				   (double)(tex_rehashed() - last_same) / 300);
			last = tex_decodes();
			last_same = tex_rehashed();
		}
		{
			extern double aica_ms;
			u64 m1_cycles(void);
			static u64 last_cyc;
			printf("      sound chip %.2f ms per frame, %.2f V Blanks per frame\n", aica_ms / 300,
				   (double)(m1_cycles() - last_cyc) / 3333333.0 / 300);
			{
				u64 m1_calls(void);
				static u64 last_calls;
				printf("      %.0f dispatcher calls per frame\n", (double)(m1_calls() - last_calls) / 300);
				extern u64 cyc_src[4];
				static u64 last_src[4];
				printf("      cycles per frame: calls %.0f, hardware %.0f, loops %.0f, other %.0f\n",
					   (double)(cyc_src[0] - last_src[0]) / 300, (double)(cyc_src[1] - last_src[1]) / 300,
					   (double)(cyc_src[2] - last_src[2]) / 300, (double)(cyc_src[3] - last_src[3]) / 300);
				memcpy(last_src, cyc_src, sizeof(last_src));
				last_calls = m1_calls();
			}
			aica_ms = 0;
			last_cyc = m1_cycles();
		}
		if (perf_rtt[0] + perf_rtt[1] + perf_rtt[2] > 0)
			printf("      of which render to texture: draw %.2f, read back %.2f, to VRAM %.2f ms\n",
				   perf_rtt[0] / 300, perf_rtt[1] / 300, perf_rtt[2] / 300);
		perf_game = perf_draw = perf_polys = perf_rtt[0] = perf_rtt[1] = perf_rtt[2] = 0;
	}
}

void render_frame(TaCtx *c)
{
	extern int m1_resim;
	if (m1_resim)
		return;	/* GGPO re-simulation (netplay.c): not shown */
	const u64 perf_t0 = SDL_GetPerformanceCounter();
	render_frame_draw(c);
	gp_mark(5);
	if (gp_on > 0)
	{
		double t = 0;
		for (int i = 0; i < 5; i++)
			t += gp_ms[i];
		if (t > 4)
		{
			printf("gpuprof render %llu mode %u: setup %.2f opaque %.2f shadows %.2f translucent %.2f rest %.2f ms, %u polys %u volumes, flushes %u\n",
				   (unsigned long long)rendered, rd32(0x8c24a584), gp_ms[0], gp_ms[1], gp_ms[2], gp_ms[3], gp_ms[4], c ? c->np : 0, c ? c->nmv : 0, gp_flush[0]);
			gp_batch_report(gp_ms[3]);
		}
		gp_nb = 0;
		memset(gp_ms, 0, sizeof(gp_ms)), gp_flush[0] = 0, gp_t = 0;
	}
	perf_report(perf_t0, SDL_GetPerformanceCounter(), c);
}

static void render_frame_draw(TaCtx *c)
{
	rendered++;
	{
		/* M1_MODELOG=1: each game mode change, with the render and the video frame (to compare
		   timings with Flycast's heartbeat) */
		static u32 last = ~0u;
		if (getenv("M1_MODELOG") && rd32(0x8c24a584) != last)
		{
			extern u64 m1_cycles(void);
			printf("modelog render %llu frame %llu: mode %u\n", (unsigned long long)rendered, (unsigned long long)(m1_cycles() / 3333333u),
				   last = rd32(0x8c24a584));
		}
	}
	dump_list(c);
	if (!video_enabled() || !c)
		return;
	if (hw_reg(0x005f8060) & 0x1000000u)
	{
		render_to_texture(c);
		return;
	}
	glUniform2f(u_target, 640.0f, 480.0f);
	int ww, wh;
	SDL_GL_GetDrawableSize(win, &ww, &wh);
	/* the picture centred in the window, drawn at the chosen resolution; its shape is the
	   resolution's (native: the window's), from 4:3 to 16:9 */
	view_aspect = picture_aspect(res_w ? res_w : ww, res_w ? res_h : wh);
	view_zoom = (16.0f / 9) / view_aspect;
	glUniform1f(u_xzoom, view_zoom);
	int vw = ww, vh = (int)(ww / view_aspect + 0.5f);
	if (vh > wh) { vh = wh; vw = (int)(wh * view_aspect + 0.5f); }
	int rw = res_w ? res_w : vw, rh = res_w ? res_h : vh;
	if (!res_w && res_pct)	/* a share of native, upscaled with FSR into the window */
	{
		rw = vw * res_pct / 100, rh = vh * res_pct / 100;
		rw = rw < 64 ? 64 : rw, rh = rh < 48 ? 48 : rh;
	}
	frame_target(rw, rh);
	glBindFramebuffer(GL_FRAMEBUFFER, frame_fbo);
	glViewport(0, 0, rw, rh);
	vp_x = 0; vp_y = 0; vp_w = rw; vp_h = rh; tgt_w = 640; tgt_h = 480;
	const u32 bg = background_colour();
	glDisable(GL_SCISSOR_TEST);
	glClearColor(((bg >> 16) & 255) / 255.0f, ((bg >> 8) & 255) / 255.0f, (bg & 255) / 255.0f, 1);
	glClearDepth(1.0);
	glDepthMask(GL_TRUE);
	glStencilMask(0xff);
	/* VS screen -> fight (game modes 16 and 17, then 0): once the stage's textures are copied into
	   video memory, the VS screen's last few frames would draw its pictures from the stage's
	   textures. The last whole picture is shown again instead, until the fight starts. The VS
	   screen's pictures: the pages of video memory written as it opens (mode 16 and its first
	   frame in mode 17); a later bulk write over them (or a megabyte or more anywhere: the stage
	   before Inferno's is small) starts the hold. Pages written since the previous frame, from
	   their generations */
	static int hold;
	{
		static u32 gens[VRAM_SIZE >> 12], last_mode;
		static u8 vs_page[VRAM_SIZE >> 12];
		const u32 mode = rd32(0x8c24a584);
		const int opening = mode == 16 || (mode == 17 && last_mode != 17);
		if (mode == 16 && last_mode != 16)
			memset(vs_page, 0, sizeof(vs_page));
		u32 changed = 0, over = 0;
		for (u32 pg = 0; pg < (VRAM_SIZE >> 12); pg++)
		{
			const u32 g = hw_vram_gen(pg);
			if (g != gens[pg])
			{
				changed++;
				over += vs_page[pg];
				if (opening)
					vs_page[pg] = 1;
			}
			gens[pg] = g;
		}
		if (mode != 17 && mode != 0)
			hold = 0;
		else if (!opening && (changed >= 256 || over >= 16))
			hold = 1;
		if (getenv("M1_MODELOG") && (mode == 16 || mode == 17 || mode == 0) && changed)
			printf("modelog render %llu: mode %u pages %u over %u hold %d\n", (unsigned long long)rendered, mode, changed, over, hold);
		last_mode = mode;
	}
	if (!hold)
	{
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
		draw_lists(c);
	}
	if (rd32(0x8c24a584) == 11)	/* game mode 11: the main menu */
		draw_version(rw, rh);
	draw_esc_prompt(rw, rh);
	/* resolve the samples, then the screenshots read the finished picture */
	glDisable(GL_SCISSOR_TEST);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, frame_fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolve_fbo);
	glBlitFramebuffer(0, 0, rw, rh, 0, 0, rw, rh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, resolve_fbo);
	for (int i = 0; i < nshots; i++)
		if (shots[i] == rendered)
		{
			screenshot(rendered, rw, rh);
			/* with each test screenshot: the frame's polygons by TSP fog mode (0 table, 1 per
			   vertex, 2 none, 3 table mode 2) and the fog registers */
			u32 n[4] = { 0 };
			for (u32 k = 0; k < c->np; k++)
				n[(c->p[k].tsp >> 22) & 3]++;
			printf("fog: render %llu mode %u: table %u, vertex %u, none %u, table2 %u; FOG_COL_RAM %06x FOG_COL_VERT %06x "
				   "FOG_DENSITY %04x table %04x %04x .. %04x\n", (unsigned long long)rendered, rd32(0x8c24a584), n[0], n[1], n[2], n[3],
				   hw_reg(0x005f80b0) & 0xffffff, hw_reg(0x005f80b4) & 0xffffff, hw_reg(0x005f80b8) & 0xffff,
				   hw_reg(0x005f8200) & 0xffff, hw_reg(0x005f8204) & 0xffff, hw_reg(0x005f83fc) & 0xffff);
		}
	{
		static long long at = -2;	/* M1_SCREENSHOT_AT=n (tests): as F12 at render n */
		if (at == -2)
			at = getenv("M1_SCREENSHOT_AT") ? atoll(getenv("M1_SCREENSHOT_AT")) : -1;
		if ((long long)rendered == at)
			player_shot = 1;
	}
	if (player_shot)
	{
		player_shot = 0;
		player_screenshot(rw, rh);
	}
	{
		int art_frame(const TaCtx *c);
		void art_shot(const u8 *rgb, int w, int h);
		if (art_frame(c))
		{
			u8 *px = malloc((size_t)rw * rh * 3);
			glPixelStorei(GL_PACK_ALIGNMENT, 1);
			glReadPixels(0, 0, rw, rh, GL_RGB, GL_UNSIGNED_BYTE, px);
			art_shot(px, rw, rh);
			free(px);
		}
	}
	/* to the window, scaled to the picture's area */
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	glViewport(0, 0, ww, wh);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	const int x0 = (ww - vw) / 2, y0 = (wh - vh) / 2;
	/* smaller than its area: FSR 1 (unless [video] upscale = linear); otherwise copied or scaled */
	if (!(rw < vw && rh < vh && fsr_present(rw, rh, x0, y0, vw, vh)))
	{
		glBindFramebuffer(GL_READ_FRAMEBUFFER, resolve_fbo);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
		glBlitFramebuffer(0, 0, rw, rh, x0, y0, x0 + vw, y0 + vh, GL_COLOR_BUFFER_BIT, rw == vw && rh == vh ? GL_NEAREST : GL_LINEAR);
	}
	{
		/* M1_WINSHOT=n (tests): the window's picture at render n, after upscaling, as win_n.ppm */
		static long long at = -2;
		if (at == -2)
			at = getenv("M1_WINSHOT") ? atoll(getenv("M1_WINSHOT")) : -1;
		if ((long long)rendered == at)
		{
			u8 *px = malloc((size_t)ww * wh * 3);
			glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
			glPixelStorei(GL_PACK_ALIGNMENT, 1);
			glReadPixels(0, 0, ww, wh, GL_RGB, GL_UNSIGNED_BYTE, px);
			char name[64];
			snprintf(name, sizeof(name), "win_%lld.ppm", at);
			FILE *f = fopen(name, "wb");
			if (f)
			{
				fprintf(f, "P6\n%d %d\n255\n", ww, wh);
				for (int y = wh - 1; y >= 0; y--)
					fwrite(px + (size_t)y * ww * 3, 1, (size_t)ww * 3, f);
				fclose(f);
			}
			free(px);
			printf("saved %s (%dx%d)\n", name, ww, wh);
		}
	}
	glBindFramebuffer(GL_FRAMEBUFFER, frame_fbo);
	SDL_GL_SwapWindow(win);
	{
		/* M1_FRAMETIME=1 (tests): every 300 swaps, the time between them: mean, shortest,
		   longest, and how many took more than 1.5 frames (a frame held an extra refresh) */
		static int on = -1;
		static u64 last, n, slow;
		static double sum, lo = 1e9, hi;
		if (on < 0)
			on = getenv("M1_FRAMETIME") != 0;
		if (on)
		{
			const u64 now = SDL_GetPerformanceCounter();
			if (last)
			{
				const double ms = (double)(now - last) * 1000.0 / (double)SDL_GetPerformanceFrequency();
				sum += ms, n++;
				lo = ms < lo ? ms : lo;
				hi = ms > hi ? ms : hi;
				slow += ms > 25.0;
				if (n == 300)
				{
					printf("frametime: mean %.2f ms, min %.2f, max %.2f, %llu over 25 ms (mode %u)\n", sum / n, lo, hi,
						   (unsigned long long)slow, rd32(0x8c24a584));
					n = slow = 0, sum = hi = 0, lo = 1e9;
				}
			}
			last = now;
		}
	}
}
