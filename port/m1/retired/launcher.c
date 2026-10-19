/* Launcher (Windows): shown when the executable starts without arguments. Picks the disc dump,
 * video and sound options, and offline or online play (GGPO: your UDP port, the opponent's
 * address:port, your side and the input delay), then saves them to sc-port.ini (config.c). */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *config_get(const char *section, const char *key);
void config_set(const char *section, const char *key, const char *value);
void config_save(void);

enum
{
	ID_DISC = 100, ID_BROWSE, ID_SOUND, ID_PORT, ID_PEER, ID_P1, ID_P2, ID_DELAY,
	ID_OFFLINE, ID_ONLINE, ID_QUIT,
};
static int result;	/* 0 cancelled, 1 offline, 2 online */
static HWND wnd;
static HFONT font;

static HWND add(const char *cls, const char *text, DWORD style, int x, int y, int w, int h, int id)
{
	HWND c = CreateWindowExA(!strcmp(cls, "EDIT") ? WS_EX_CLIENTEDGE : 0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, wnd,
							 (HMENU)(INT_PTR)id, GetModuleHandleA(0), 0);
	SendMessageA(c, WM_SETFONT, (WPARAM)font, TRUE);
	return c;
}

static void get_text(int id, char *buf, int n) { GetDlgItemTextA(wnd, id, buf, n); }
static int checked(int id) { return IsDlgButtonChecked(wnd, id) == BST_CHECKED; }

static void save(void)
{
	char buf[512];
	get_text(ID_DISC, buf, sizeof(buf));
	config_set("game", "disc", buf);
	config_set("audio", "enabled", checked(ID_SOUND) ? "1" : "0");
	get_text(ID_PORT, buf, sizeof(buf));
	config_set("net", "port", buf);
	get_text(ID_PEER, buf, sizeof(buf));
	config_set("net", "peer", buf);
	config_set("net", "player", checked(ID_P2) ? "2" : "1");
	get_text(ID_DELAY, buf, sizeof(buf));
	config_set("net", "delay", buf);
	config_set("net", "mode", result == 2 ? "online" : "offline");
	config_save();
}

static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_COMMAND)
	{
		const int id = LOWORD(wp);
		if (id == ID_BROWSE)
		{
			char file[512] = "";
			GetDlgItemTextA(h, ID_DISC, file, sizeof(file));
			OPENFILENAMEA of = { sizeof(of) };
			of.hwndOwner = h;
			of.lpstrFilter = "Disc dump (*.gdi)\0*.gdi\0All files\0*.*\0";
			of.lpstrFile = file;
			of.nMaxFile = sizeof(file);
			of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
			if (GetOpenFileNameA(&of))
				SetDlgItemTextA(h, ID_DISC, file);
		}
		else if (id == ID_OFFLINE || id == ID_ONLINE)
		{
			char disc[512], peer[256];
			GetDlgItemTextA(h, ID_DISC, disc, sizeof(disc));
			GetDlgItemTextA(h, ID_PEER, peer, sizeof(peer));
			if (!disc[0])
				MessageBoxA(h, "Choose the disc dump (.gdi) first.", "Soulcalibur", MB_ICONINFORMATION);
			else if (GetFileAttributesA(disc) == INVALID_FILE_ATTRIBUTES)	/* e.g. a path from another PC's settings */
				MessageBoxA(h, "The disc dump was not found at that path. Click the disc button and choose the .gdi file on this PC.",
							"Soulcalibur", MB_ICONINFORMATION);
			else if (id == ID_ONLINE && !strchr(peer, ':'))
				MessageBoxA(h, "Enter the opponent's address as address:port (for example 203.0.113.5:7000).", "Soulcalibur",
							MB_ICONINFORMATION);
			else
			{
				result = id == ID_ONLINE ? 2 : 1;
				save();
				DestroyWindow(h);
			}
		}
		else if (id == ID_QUIT)
			DestroyWindow(h);
		return 0;
	}
	if (msg == WM_DESTROY)
	{
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcA(h, msg, wp, lp);
}

static const char *get(const char *section, const char *key, const char *def)
{
	const char *v = config_get(section, key);
	return v ? v : def;
}

int launcher_run(void)
{
	WNDCLASSA wc = { 0 };
	wc.lpfnWndProc = proc;
	wc.hInstance = GetModuleHandleA(0);
	wc.hCursor = LoadCursor(0, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
	wc.lpszClassName = "SCLauncher";
	RegisterClassA(&wc);
	NONCLIENTMETRICSA ncm = { sizeof(ncm) };
	SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
	font = CreateFontIndirectA(&ncm.lfMessageFont);
	RECT r = { 0, 0, 520, 330 };
	AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
	wnd = CreateWindowExA(0, "SCLauncher", "Soulcalibur - native port", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
						  CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, 0, 0, wc.hInstance, 0);

	add("STATIC", "Disc dump (.gdi):", 0, 16, 16, 120, 20, -1);
	add("EDIT", get("game", "disc", ""), ES_AUTOHSCROLL, 16, 38, 400, 24, ID_DISC);
	add("BUTTON", "Browse...", BS_PUSHBUTTON, 424, 37, 80, 26, ID_BROWSE);

	add("BUTTON", "Sound", BS_AUTOCHECKBOX, 16, 76, 80, 22, ID_SOUND);
	add("STATIC", "Display settings: GRAPHICS in the game's main menu.", 0, 120, 79, 384, 20, -1);
	CheckDlgButton(wnd, ID_SOUND, atoi(get("audio", "enabled", "1")) ? BST_CHECKED : BST_UNCHECKED);

	add("BUTTON", "Online (both players start at the same time)", BS_GROUPBOX, 16, 112, 488, 150, -1);
	add("STATIC", "Your UDP port:", 0, 32, 140, 100, 20, -1);
	add("EDIT", get("net", "port", "7000"), ES_AUTOHSCROLL | ES_NUMBER, 140, 137, 70, 24, ID_PORT);
	add("STATIC", "Opponent (address:port):", 0, 228, 140, 150, 20, -1);
	add("EDIT", get("net", "peer", ""), ES_AUTOHSCROLL, 380, 137, 110, 24, ID_PEER);
	add("STATIC", "You play:", 0, 32, 176, 100, 20, -1);
	add("BUTTON", "Player 1", BS_AUTORADIOBUTTON | WS_GROUP, 140, 174, 80, 22, ID_P1);
	add("BUTTON", "Player 2", BS_AUTORADIOBUTTON, 228, 174, 80, 22, ID_P2);
	CheckDlgButton(wnd, atoi(get("net", "player", "1")) == 2 ? ID_P2 : ID_P1, BST_CHECKED);
	add("STATIC", "Input delay (frames):", 0, 32, 212, 130, 20, -1);
	add("EDIT", get("net", "delay", "2"), ES_AUTOHSCROLL | ES_NUMBER, 168, 209, 42, 24, ID_DELAY);
	add("STATIC", "One player picks 1, the other 2; each enters the other's address.", 0, 228, 206, 270, 40, -1);

	add("BUTTON", "Play offline", BS_DEFPUSHBUTTON, 16, 282, 150, 32, ID_OFFLINE);
	add("BUTTON", "Play online", BS_PUSHBUTTON, 176, 282, 150, 32, ID_ONLINE);
	add("BUTTON", "Quit", BS_PUSHBUTTON, 404, 282, 100, 32, ID_QUIT);

	ShowWindow(wnd, SW_SHOW);
	UpdateWindow(wnd);
	if (getenv("M1_LAUNCHER_SHOT"))
	{
		/* test hook: save the window as a .bmp and quit */
		RECT rc;
		GetWindowRect(wnd, &rc);
		const int w = rc.right - rc.left, h = rc.bottom - rc.top;
		HDC wdc = GetDC(wnd), mdc = CreateCompatibleDC(wdc);
		HBITMAP bmp = CreateCompatibleBitmap(wdc, w, h);
		SelectObject(mdc, bmp);
		PrintWindow(wnd, mdc, 0);
		BITMAPINFOHEADER bi = { sizeof(bi), w, -h, 1, 24, BI_RGB };
		const int stride = (w * 3 + 3) & ~3;
		unsigned char *px = malloc((size_t)stride * h);
		GetDIBits(mdc, bmp, 0, (UINT)h, px, (BITMAPINFO *)&bi, DIB_RGB_COLORS);
		FILE *f = fopen(getenv("M1_LAUNCHER_SHOT"), "wb");
		if (f)
		{
			BITMAPFILEHEADER fh = { 0x4d42, (DWORD)(sizeof(fh) + sizeof(bi) + stride * h), 0, 0, (DWORD)(sizeof(fh) + sizeof(bi)) };
			bi.biHeight = -h;
			fwrite(&fh, sizeof(fh), 1, f);
			fwrite(&bi, sizeof(bi), 1, f);
			fwrite(px, 1, (size_t)stride * h, f);
			fclose(f);
		}
		free(px);
		DestroyWindow(wnd);
		return 0;
	}
	MSG m;
	while (GetMessageA(&m, 0, 0, 0) > 0)
	{
		if (!IsDialogMessageA(wnd, &m))
		{
			TranslateMessage(&m);
			DispatchMessageA(&m);
		}
	}
	return result;
}
#endif
