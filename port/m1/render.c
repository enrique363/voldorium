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
	X(PFNGLUNIFORM2FPROC, glUniform2f) \
	X(PFNGLUNIFORM3FPROC, glUniform3f) \
	X(PFNGLBUFFERSUBDATAPROC, glBufferSubData) \
	X(PFNGLUNIFORM4FPROC, glUniform4f) \
	X(PFNGLUSEPROGRAMPROC, glUseProgram) \
	X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer)
#define X(T, n) static T p_##n;
GL_FUNCS(X)
#undef X
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

u32 hw_reg(u32 addr);
unsigned tex_get(u32 tcw, u32 tsp);
unsigned tex_get_hi(u32 tcw, u32 tsp, int *scale);

static SDL_Window *win;
static SDL_GLContext glc;
static SDL_GameController *pads[2];	/* player 1 (with the keyboard), player 2 */
static int enabled = -1;
static GLuint prog, vao, vbo;
static GLint u_tex, u_mode, u_flags, u_alpha_ref, u_target, u_xmap, u_umap, u_xoff, u_fog, u_fog_col, u_ymap;
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
	"void main() {\n"
	"  v_bad = any(isinf(pos)) || any(isnan(pos)) ? 1.0 : 0.0;\n"
	"  float iw = clamp(pos.z, 1e-7, 1e30);\n"
	"  vec2 p = clamp(pos.xy, vec2(-1e30), vec2(1e30));\n"
	"  float x = xmap.x + (p.x - xmap.x) * xmap.y + xoff;\n"
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
	"uniform int flags;     // 1 use alpha, 2 ignore texture alpha, 4 offset colour\n"
	"uniform float alpha_ref; // PT_ALPHA_REF (0-255), < 0: no alpha test\n"
	"uniform int fog;       // TSP fog mode: 1 = per vertex (offset alpha)\n"
	"uniform vec3 fog_col;  // FOG_COL_VERT\n"
	"out vec4 frag;\n"
	"void main() {\n"
	"  if (v_bad > 0.0) discard;  // the PowerVR cannot set such a triangle up and drops it\n"
	"  float w = 1.0 / v_uvw.z;\n"
	"  vec2 v_uv = v_uvw.xy * w;\n"
	"  vec4 ofs = v_ofs * w;\n"
	"  gl_FragDepth = 1.0 / (1.0 + v_uvw.z);\n"
	"  vec4 c = v_col * w;\n"
	"  if ((flags & 1) == 0) c.a = 1.0;\n"
	"  if (mode >= 0) {\n"
	"    vec4 t = texture(tex, v_uv);\n"
	"    if ((flags & 2) != 0) t.a = 1.0;\n"
	"    if (mode == 0) c = vec4(t.rgb, t.a);\n"
	"    else if (mode == 1) c = vec4(t.rgb * c.rgb, t.a);\n"
	"    else if (mode == 2) c = vec4(mix(c.rgb, t.rgb, t.a), c.a);\n"
	"    else c = t * c;\n"
	"    if ((flags & 4) != 0) c.rgb += ofs.rgb;\n"
	"  }\n"
	"  if (fog == 1) c.rgb = mix(c.rgb, fog_col, ofs.a);\n"
	"  // the PowerVR compares 8-bit alphas: alpha 1.0 interpolated across a polygon comes out a\n"
	"  // hair below 1.0 in floats and would fail PT_ALPHA_REF 255\n"
	"  if (alpha_ref >= 0.0 && floor(c.a * 255.0 + 0.5) < alpha_ref) discard;\n"
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

void config_set(const char *section, const char *key, const char *value);
void config_save(void);

static void video_settings_load(void)
{
	const char *r = getenv("M1_RESOLUTION");
	if (r && sscanf(r, "%dx%d", &res_w, &res_h) != 2)
		res_w = res_h = 0;
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

/* the monitor's size (the desktop mode of the window's display) */
static void display_size(int *w, int *h)
{
	SDL_DisplayMode m;
	if (!win || SDL_GetDesktopDisplayMode(SDL_GetWindowDisplayIndex(win), &m))
		*w = 1920, *h = 1080;
	else
		*w = m.w, *h = m.h;
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
		h = u.h * 9 / 10;
		w = h * 16 / 9;
		if (w > u.w * 9 / 10)
			w = u.w * 9 / 10, h = w * 9 / 16;
	}
	SDL_SetWindowSize(win, w, h);
	SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}

int video_is_fullscreen(void) { return win && (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0; }

/* a GRAPHICS change also goes into this process's environment: a restart into or out of an
   online match (online.c relaunch) inherits it, and the settings file is only read where the
   environment has nothing */
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
	config_set("video", "fullscreen", full ? "1" : "0");
	config_save();
	setting_env("M1_FULLSCREEN", full ? "1" : "0");
}

/* the resolutions offered: native, then the usual 16:9 sizes up to the monitor's */
static const int sizes[][2] = { { 1280, 720 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 }, { 3200, 1800 }, { 3840, 2160 },
								{ 5120, 2880 }, { 7680, 4320 } };

void video_resolution_label(char *buf, size_t n)
{
	int w, h;
	display_size(&w, &h);
	if (res_w)
		snprintf(buf, n, "%d X %d", res_w, res_h);
	else
		snprintf(buf, n, "NATIVE %d X %d", w, h * 16 / 9 > w ? w * 9 / 16 : h);
}

void video_next_resolution(void)
{
	int dw, dh;
	display_size(&dw, &dh);
	const int n = (int)(sizeof(sizes) / sizeof(sizes[0]));
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
	char v[32] = "native";
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
static u32 pad_bits(SDL_GameController *pad)
{
	if (!pad)
		return 0;
	u32 b = 0;
	/* left stick as the D-pad */
	const int dz = 16000;
	const int ax = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX), ay = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
	if (ax < -dz) b |= 0x0040;
	if (ax > dz) b |= 0x0080;
	if (ay < -dz) b |= 0x0010;
	if (ay > dz) b |= 0x0020;
	/* triggers, or the shoulder buttons */
	if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > dz) b |= 0x1000;
	if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > dz) b |= 0x2000;
	static const struct { int btn; u32 bit; } map[] = {
		{ SDL_CONTROLLER_BUTTON_A, 0x0004 }, { SDL_CONTROLLER_BUTTON_B, 0x0002 },
		{ SDL_CONTROLLER_BUTTON_X, 0x0400 }, { SDL_CONTROLLER_BUTTON_Y, 0x0200 },
		{ SDL_CONTROLLER_BUTTON_START, 0x0008 }, { SDL_CONTROLLER_BUTTON_DPAD_UP, 0x0010 },
		{ SDL_CONTROLLER_BUTTON_DPAD_DOWN, 0x0020 }, { SDL_CONTROLLER_BUTTON_DPAD_LEFT, 0x0040 },
		{ SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 0x0080 },
		{ SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 0x1000 }, { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 0x2000 },
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
	SDL_GL_SetSwapInterval(0);	/* the emulated V Blank paces frames */
	printf("video: %s / %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
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
	u_target = glGetUniformLocation(prog, "target");
	u_ymap = glGetUniformLocation(prog, "ymap");
	glUniform2f(u_ymap, 0, 1);
	u_xmap = glGetUniformLocation(prog, "xmap");
	u_umap = glGetUniformLocation(prog, "umap");
	u_xoff = glGetUniformLocation(prog, "xoff");
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
	for (int i = 0; i < SDL_NumJoysticks(); i++)
		pad_added(i);
	return enabled;
}

unsigned gl_upload_texture(unsigned id, u32 w, u32 h, const u32 *rgba)
{
	if (!id)
		glGenTextures(1, &id);
	glBindTexture(GL_TEXTURE_2D, id);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
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
	SDL_Event e;
	while (SDL_PollEvent(&e))
	{
		if (e.type == SDL_QUIT)
			m1_stop("window closed");
		if (e.type == SDL_KEYDOWN && !e.key.repeat)
		{
			const SDL_Keycode k = e.key.keysym.sym;
			if (k == SDLK_F11 || (k == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT)))
			{
				video_toggle_fullscreen();	/* remembered in voldorium.ini */
			}
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

static void set_state(const TaPoly *p, int list)
{
	const u32 isp = p->isp, tsp = p->tsp;
	const int textured = (isp >> 25) & 1;
	glDepthFunc(depth_func[(isp >> 29) & 7]);
	glDepthMask(((isp >> 26) & 1) ? GL_FALSE : GL_TRUE);
	/* ISP culling mode (28-27): 2 and 3 cull one winding (the game relies on it for polygons
	   with vertices behind the camera); 1 (cull if small) is not modelled */
	const int cull = (isp >> 27) & 3;
	if (cull >= 2 && !p->sprite)
	{
		glEnable(GL_CULL_FACE);
		glCullFace(cull == 2 ? GL_FRONT : GL_BACK);
	}
	else
		glDisable(GL_CULL_FACE);
	if (list == 2)
	{
		glEnable(GL_BLEND);
		glBlendFunc(blend_src[(tsp >> 29) & 7], blend_dst[(tsp >> 26) & 7]);
	}
	else
		glDisable(GL_BLEND);
	int flags = ((tsp >> 20) & 1) | (((tsp >> 19) & 1) << 1) | (((isp >> 24) & 1) << 2);
	glUniform1i(u_flags, flags);
	glUniform1f(u_alpha_ref, list == 4 ? (float)(hw_reg(0x005f811c) & 255) : -1.0f);
	glUniform1i(u_fog, (int)((tsp >> 22) & 3));
	{
		const u32 fc = hw_reg(0x005f80b4);	/* FOG_COL_VERT */
		glUniform3f(u_fog_col, ((fc >> 16) & 255) / 255.0f, ((fc >> 8) & 255) / 255.0f, (fc & 255) / 255.0f);
	}
	/* opaque and punch-through polygons mark whether modifier volumes shade them (stencil
	   bit 7, from PCW bit 7) */
	if (list != 2)
	{
		glEnable(GL_STENCIL_TEST);
		glStencilMask(0x80);
		glStencilFunc(GL_ALWAYS, p->shadow ? 0x80 : 0, 0xff);
		glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
	}
	else
		glDisable(GL_STENCIL_TEST);
	if (!textured)
	{
		glUniform1i(u_mode, -1);
		return;
	}
	glUniform1i(u_mode, (tsp >> 6) & 3);
	GLint filt = ((tsp >> 13) & 3) ? GL_LINEAR : GL_NEAREST;
	/* text and other point-sampled 2D drawn above 640x480 would show each texel as a block: it
	   gets the texture upscaled by Scale2x (tex.c tex_get_hi) and bilinear filtering, so the
	   shapes stay the same size with smooth edges; punch-through then keeps texels at least half
	   opaque, as the filtered edges fall between the two. M1_SMOOTH2D=0 ([video] text = sharp):
	   off. */
	static int smooth = -1;
	if (smooth < 0)
		smooth = !getenv("M1_SMOOTH2D") || atoi(getenv("M1_SMOOTH2D"));
	int hi = 1;
	if (smooth && tgt_w == 640 && vp_h > 600 && p->cls >= M1_2D_HUD && p->cls <= M1_2D_SCREEN)
	{
		glBindTexture(GL_TEXTURE_2D, tex_get_hi(p->tcw, tsp, &hi));
		if (hi > 1)
		{
			filt = GL_LINEAR;
			if (list == 4 && (hw_reg(0x005f811c) & 255) > 128)
				glUniform1f(u_alpha_ref, 128.0f);
		}
	}
	else
		glBindTexture(GL_TEXTURE_2D, tex_get(p->tcw, tsp));
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, ((tsp >> 16) & 1) ? GL_CLAMP_TO_EDGE : ((tsp >> 18) & 1) ? GL_MIRRORED_REPEAT : GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, ((tsp >> 15) & 1) ? GL_CLAMP_TO_EDGE : ((tsp >> 17) & 1) ? GL_MIRRORED_REPEAT : GL_REPEAT);
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
 * above 360 -> 640, else centre 320; text by the extent of its whole run of glyphs, so one string
 * is never split between anchors); on all other screens about the centre (pillarbox). Untextured
 * full-width fills stretch. Tiled layers (the menu wallpaper of menu_bg_scroll_draw, which
 * wraps every 1024 virtual pixels) are compressed like other 2D and drawn twice more, one wrap
 * period to either side, so they fill the width without distortion. */
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

static void wide_map(const TaCtx *c, const TaPoly *p, float *xa, float *xk, float *ua, float *uk)
{
	*xa = 0, *xk = 1, *ua = 0, *uk = 1;
	if (tgt_w != 640)
		return;	/* render targets: no mapping */
	float x0 = 1e30f, x1 = -1e30f, z0 = 1e30f, z1 = -1e30f, us = 0;
	for (u32 k = 0; k < p->count; k++)
	{
		const TaVert *t = &c->v[p->first + k];
		x0 = t->x < x0 ? t->x : x0;
		x1 = t->x > x1 ? t->x : x1;

		z0 = t->z < z0 ? t->z : z0;
		z1 = t->z > z1 ? t->z : z1;
		us += t->u;
	}
	const int flat = z1 - z0 <= 1e-6f * (z1 > 1 ? z1 : 1);
	if (!flat)
		return;	/* 3D */
	const int battle = rd32(0x8c24a584) == 1;	/* game mode 1 = fight */
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
	if (p->cls == 0 && battle)
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
		*xa = cx < 280 ? 0 : cx > 360 ? 640 : 320;
	}
}

/* M1_DUMPLIST=n: print render n's polygons (list, ISP, TSP, TCW, first vertex) */
static void dump_list(TaCtx *c)
{
	static long want = -2;
	if (want == -2)
		want = getenv("M1_DUMPLIST") ? atol(getenv("M1_DUMPLIST")) : -1;
	if ((long)rendered != want || !c)
		return;
	FILE *f = fopen("list_dump.txt", "w");
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
		if (p->sprite)
			for (u32 k = 0; k < p->count; k++)
			{
				const TaVert *t = &c->v[p->first + k];
				fprintf(f, "      v%u %7.2f %7.2f z %.4g uv %.4f %.4f%c", k, t->x, t->y, t->z, t->u, t->v, 10);
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
		/* M1_TEXDUMP=tcw: that texture as decoded now (tex.c), tex_dump.ppm */
		const u32 want_tcw = (u32)strtoul(getenv("M1_TEXDUMP"), 0, 16);
		for (u32 i = 0; i < c->np; i++)
			if (c->p[i].tcw == want_tcw)
			{
				u32 tw, th;
				static u32 px[1024 * 1024];
				u32 tex_decode_rgba(u32 tcw, u32 tsp, u32 *out, u32 *w, u32 *h);
				tex_decode_rgba(want_tcw, c->p[i].tsp, px, &tw, &th);
				FILE *tf = fopen("tex_dump.ppm", "wb");
				fprintf(tf, "P6\n%u %u\n255\n", tw, th);
				for (u32 k = 0; k < tw * th; k++)
				{
					const u32 a = px[k] >> 24;
					const u8 rgb[3] = { (u8)((px[k] & 255) * a / 255), (u8)((px[k] >> 8 & 255) * a / 255), (u8)((px[k] >> 16 & 255) * a / 255) };
					fwrite(rgb, 1, 3, tf);
				}
				fclose(tf);
				tf = fopen("tex_alpha.pgm", "wb");
				fprintf(tf, "P5\n%u %u\n255\n", tw, th);
				for (u32 k = 0; k < tw * th; k++)
					fputc((int)(px[k] >> 24), tf);
				fclose(tf);
				printf("texdump: PT_ALPHA_REF %08x PAL_RAM_CTRL %08x\n", hw_reg(0x005f811c), hw_reg(0x005f8108));
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

/* marks the polygons of the frame's 4:3 background layer; returns how many */
static u32 bg_mask(const TaCtx *c, u8 *mask)
{
	const u32 mode = rd32(0x8c24a584);
	const int battle = mode == 1 || mode == 23 || mode == 45;	/* fight, demo fight, demo replay */
	memset(mask, 0, c->np);
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
		pz[i] = z1, px0[i] = x0, px1[i] = x1;
		if (p->cls == M1_2D_TILED)
			tx0 = fminf(tx0, x0), tx1 = fmaxf(tx1, x1);
	}
	const int wallpaper = tx0 < -32 || tx1 > 672;
	for (u32 i = 0; i < c->np; i++)
		if (pz[i] >= 0 && (px0[i] <= 1 || px1[i] >= 639) && !(wallpaper && c->p[i].cls == M1_2D_TILED))
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
			  text in front stay undistorted). Returns 2: see the draw loop
	 replace  its 16:9 replacement picture if there is one (returns 1: the layer's own
			  polygons are then not drawn, see bg_layer), else the blur fill
	 blur	 the blur fill only
   M1_BLURFILL=0 turns the blur fill off. */
static u8 *bg_layer;
static int side_fill(TaCtx *c, const u32 *order, u32 n)
{
	static int on = -1, mode = -1;
	if (on < 0)
	{
		on = !getenv("M1_BLURFILL") || atoi(getenv("M1_BLURFILL"));
		const char *m = getenv("M1_BGMODE");
		mode = m && !strcmp(m, "replace") ? 1 : m && !strcmp(m, "blur") ? 0 : 2;	/* default: stretch */
	}
	static u32 cap;
	if (tgt_w != 640)
		return 0;
	if (c->np > cap)
		bg_layer = realloc(bg_layer, cap = c->np);
	if (!bg_mask(c, bg_layer))
		return 0;
	if (mode == 2)
		return 2;
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
			glDrawArrays(GL_TRIANGLE_STRIP, (GLint)p->first, (GLsizei)p->count);
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

static void draw_lists(TaCtx *c)
{
	glEnable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glBindVertexArray(vao);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((c->nv + 3 * c->nmv + 4) * sizeof(TaVert)), 0, GL_STREAM_DRAW);
	glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(c->nv * sizeof(TaVert)), c->v);
	upload_volumes(c);
	text_runs(c);
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
	for (u32 j = 0; j < n; j++)
		{
			if (j == first_tr && !shaded)
			{
				shadow_pass(c);
				shaded = 1;
			}
			const TaPoly *p = &c->p[idx[j]];
			if (p->count < 3 || (replaced == 1 && bg_layer[idx[j]]))
				continue;	/* a replaced background's own pieces */
			static int skip_list = -2;	/* M1_SKIPLIST=n (debugging): leave out display list n */
			if (skip_list == -2)
				skip_list = getenv("M1_SKIPLIST") ? atoi(getenv("M1_SKIPLIST")) : -1;
			if ((int)p->list == skip_list)
				continue;
			set_state(p, p->list);
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
						glUniform1i(u_mode, -1);
					glUniform1f(u_alpha_ref, -1.0f);
				}
			}
			float xa, xk, ua, uk;
			wide_map(c, p, &xa, &xk, &ua, &uk);
			if (replaced == 2 && bg_layer[idx[j]])
				xa = 320, xk = 1, ua = 0, uk = 1;	/* stretched background: full width */
			glUniform2f(u_xmap, xa, xk);
			glUniform2f(u_umap, ua, uk);
			if (p->clip == 2)
			{
				const float cx0 = xa + (p->clip_rect[0] - xa) * xk, cx1 = xa + (p->clip_rect[2] - xa) * xk;
				const int x0 = vp_x + (int)(cx0 * vp_w / tgt_w), x1 = vp_x + (int)(cx1 * vp_w / tgt_w);
				const int y0 = vp_y + vp_h - p->clip_rect[3] * vp_h / tgt_h, y1 = vp_y + vp_h - p->clip_rect[1] * vp_h / tgt_h;
				glEnable(GL_SCISSOR_TEST);
				glScissor(x0, y0, x1 - x0, y1 - y0);
			}
			else
				glDisable(GL_SCISSOR_TEST);
			glUniform1f(u_xoff, 0);
			glDrawArrays(GL_TRIANGLE_STRIP, (GLint)p->first, (GLsizei)p->count);
			if (p->cls == M1_2D_TILED && xk != 1 && !(replaced == 2 && bg_layer[idx[j]]))
				for (int side = -1; side <= 1; side += 2)
				{
					glUniform1f(u_xoff, side * 1024 * xk);
					glDrawArrays(GL_TRIANGLE_STRIP, (GLint)p->first, (GLsizei)p->count);
				}
		}
	if (!shaded)
		shadow_pass(c);
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

static void render_frame_draw(TaCtx *c);

/* "ver. 1.0.1" in the lower right corner, in the BIOS font like the game's help line, white with
   a dark shadow; nothing without the font */
static GLuint ver_prog, ver_tex;
static GLint u_v_col;
static int ver_w;
static void draw_version(int rw, int rh)
{
	const u8 *font = hw_font_12x24();
	if (!font)
		return;
	if (!blur_prog)
		blur_init();	/* its full-target triangle and vertex shader */
	if (!ver_prog)
	{
		static const char *fs =
			"#version 330 core\n"
			"in vec2 uv; out vec4 o;\n"
			"uniform sampler2D src; uniform vec4 col;\n"
			"void main() { o = vec4(col.rgb, col.a * texture(src, uv).r); }\n";
		ver_prog = glCreateProgram();
		glAttachShader(ver_prog, shader(GL_VERTEX_SHADER, blur_vs));
		glAttachShader(ver_prog, shader(GL_FRAGMENT_SHADER, fs));
		glLinkProgram(ver_prog);
		u_v_col = glGetUniformLocation(ver_prog, "col");
		/* the text as a coverage mask, bottom row first (GL); glyph i is ASCII 32 + i */
		static const char text[] = "ver. " VOLDORIUM_VERSION;
		const int n = (int)sizeof(text) - 1;
		ver_w = n * 12;
		u8 *px = calloc((size_t)ver_w * 24, 1);
		for (int k = 0; k < n; k++)
		{
			const int ch = (u8)text[k];
			if (ch < 33 || ch > 126)
				continue;
			const u8 *g = font + (ch - 32) * 36;
			for (int y = 0; y < 24; y++)
				for (int x = 0; x < 12; x++)
				{
					const int bit = y * 12 + x;
					if (g[bit >> 3] & (0x80 >> (bit & 7)))
						px[(23 - y) * ver_w + k * 12 + x] = 255;
				}
		}
		glGenTextures(1, &ver_tex);
		glBindTexture(GL_TEXTURE_2D, ver_tex);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, ver_w, 24, 0, GL_RED, GL_UNSIGNED_BYTE, px);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
		free(px);
	}
	/* 18 of 480 lines high, 10 lines from the bottom and the right edge */
	const float s = rh / 480.0f * 0.75f;
	const int w = (int)(ver_w * s + 0.5f), h = (int)(24 * s + 0.5f);
	const int x = rw - (int)(10 * rh / 480.0f) - w, y = (int)(8 * rh / 480.0f);
	const int sh = rh >= 720 ? 2 : 1;
	glUseProgram(ver_prog);
	glBindVertexArray(blur_vao);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_STENCIL_TEST);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glBindTexture(GL_TEXTURE_2D, ver_tex);
	glViewport(x + sh, y - sh, w, h);
	glUniform4f(u_v_col, 0, 0, 0, 0.7f);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glViewport(x, y, w, h);
	glUniform4f(u_v_col, 0.92f, 0.92f, 0.92f, 0.9f);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glViewport(0, 0, rw, rh);
	glUseProgram(prog);
	glBindVertexArray(vao);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glEnable(GL_DEPTH_TEST);
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
	perf_report(perf_t0, SDL_GetPerformanceCounter(), c);
}

static void render_frame_draw(TaCtx *c)
{
	rendered++;
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
	/* 16:9 picture centred in the window, drawn at the chosen resolution (native: the window's) */
	int vw = ww, vh = ww * 9 / 16;
	if (vh > wh) { vh = wh; vw = wh * 16 / 9; }
	const int rw = res_w ? res_w : vw, rh = res_w ? res_h : vh;
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
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	draw_lists(c);
	if (rd32(0x8c24a584) == 11)	/* game mode 11: the main menu */
		draw_version(rw, rh);
	/* resolve the samples, then the screenshots read the finished picture */
	glDisable(GL_SCISSOR_TEST);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, frame_fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolve_fbo);
	glBlitFramebuffer(0, 0, rw, rh, 0, 0, rw, rh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, resolve_fbo);
	for (int i = 0; i < nshots; i++)
		if (shots[i] == rendered)
			screenshot(rendered, rw, rh);
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
	/* to the window, scaled to its 16:9 area */
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	glViewport(0, 0, ww, wh);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	const int x0 = (ww - vw) / 2, y0 = (wh - vh) / 2;
	glBlitFramebuffer(0, 0, rw, rh, x0, y0, x0 + vw, y0 + vh, GL_COLOR_BUFFER_BIT, rw == vw && rh == vh ? GL_NEAREST : GL_LINEAR);
	glBindFramebuffer(GL_FRAMEBUFFER, frame_fbo);
	SDL_GL_SwapWindow(win);
}
