////////////////////////////////////////////////////////////////////////////////
// BPP_Vectorscope: Cb/Cr vectorscope for BeholdTV (YUY2 input).
// Shown in its own window, opened from the plugin's settings button.
// The frame is only read, never changed.
////////////////////////////////////////////////////////////////////////////////

#include <windows.h>
#include <commctrl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Common.h"
#include "resource.h"

#define BH_PLUGIN_EXP	__declspec(dllexport)

// Plot plane: 8-bit Cb across, Cr up. 128 = zero, 16..240 = colour-difference range (SMPTE ST 125)
enum { LEVELS = 256, ZERO = 128, LIM_LOW = 16, LIM_HIGH = 240, MAX_LINE = 2048 };
enum { GAIN_ALL = 32, GAIN_LINE = 180 };	// trace brightness per sqrt(hits), all lines / one line
#define DEG			(3.14159265358979 / 180)
#define SKIN_DEG	123.0					// skin-tone line, from the +Cb axis
#define BOX_DEG		3.0						// target box: +-3 degrees,
#define BOX_AMP		0.05					// +-5 % amplitude (scope makers' convention, not a standard)

static HINSTANCE		g_hInst;
static HWND volatile	g_hDlg;
static HWND volatile	g_hScope;
static CRITICAL_SECTION	g_lock;

// Format from StartStreaming, read by the window for its status line
static volatile BOOL	g_started;
static volatile ePixFmt	g_fmt;
static volatile int		g_frameW, g_frameH;

static volatile int		g_line;			// 0 = all lines, else 1-based frame line (set by the window)

static unsigned short	g_work[LEVELS][LEVELS];		// [255 - Cr][Cb], streaming thread only
static unsigned short	g_shown[LEVELS][LEVELS];	// guarded by g_lock
static int				g_gain;						// guarded by g_lock

// Colour bars in the order they appear, BT.471 names
struct Bar { const char *name; int r, g, b; };
static const Bar g_bars[] =
{
	{"Yl", 1, 1, 0}, {"Cy", 0, 1, 1}, {"G", 0, 1, 0}, {"Mg", 1, 0, 1}, {"R", 1, 0, 0}, {"B", 0, 0, 1},
};
enum { NUM_BARS = sizeof(g_bars) / sizeof(g_bars[0]) };


////////////////////////////////////////////////////////////////////////////////
// Vectorscope
////////////////////////////////////////////////////////////////////////////////

// Count Cb/Cr samples for frame lines first..last (0-based). YUY2 is Y0 Cb Y1 Cr:
// one (Cb, Cr) point per pixel pair.
static void Accumulate(const BYTE *buf, int w, int pitch, int first, int last, unsigned short (*grid)[LEVELS])
{
	memset(grid, 0, sizeof(unsigned short) * LEVELS * LEVELS);
	for (int y = first; y <= last; y++)
	{
		const BYTE *p = buf + y * pitch;
		for (int x = 0; x + 1 < w; x += 2, p += 4)
		{
			unsigned short *c = &grid[LEVELS - 1 - p[3]][p[1]];
			if (*c < 0xFFFF)
				(*c)++;
		}
	}
}

// Exact BT.601 Cb, Cr of a bar at 'level' (0.75 = 75 % bars, 1.0 = 100 %), 8-bit limited range
static void BarTarget(const Bar &bar, double level, double *cb, double *cr)
{
	double r = bar.r * level, g = bar.g * level, b = bar.b * level;
	*cb = ZERO + 224 * (-0.168736 * r - 0.331264 * g + 0.5 * b);
	*cr = ZERO + 224 * (0.5 * r - 0.418688 * g - 0.081312 * b);
}

// Square plot area inside the scope control
struct Plane { int x0, y0, side; };

static POINT ToScreen(const Plane &pl, double cb, double cr)
{
	POINT p = {(LONG) (pl.x0 + (cb + 0.5) * pl.side / LEVELS), (LONG) (pl.y0 + (LEVELS - 1 - cr + 0.5) * pl.side / LEVELS)};
	return p;
}

// Point at radius r (codes) and angle a (radians from +Cb) around the centre
static POINT Polar(const Plane &pl, double r, double a)
{
	return ToScreen(pl, ZERO + r * cos(a), ZERO + r * sin(a));
}

static void DrawTargetBox(HDC dc, const Plane &pl, double cb, double cr)
{
	double r = hypot(cb - ZERO, cr - ZERO), a = atan2(cr - ZERO, cb - ZERO);
	double rs[4] = {r * (1 - BOX_AMP), r * (1 + BOX_AMP), r * (1 + BOX_AMP), r * (1 - BOX_AMP)};
	double as[4] = {a - BOX_DEG * DEG, a - BOX_DEG * DEG, a + BOX_DEG * DEG, a + BOX_DEG * DEG};
	POINT pt[5];
	for (int i = 0; i < 5; i++)
		pt[i] = Polar(pl, rs[i % 4], as[i % 4]);
	Polyline(dc, pt, 5);
}

// Axes, 16..240 range square, skin-tone line, 75 % (solid) and 100 % (dotted) bar targets
static void DrawGraticule(HDC dc, const Plane &pl)
{
	HPEN axis = CreatePen(PS_SOLID, 1, RGB(70, 70, 70));
	HPEN range = CreatePen(PS_DOT, 1, RGB(90, 90, 90));
	HPEN skin = CreatePen(PS_DOT, 1, RGB(200, 140, 90));
	HPEN bar75 = CreatePen(PS_SOLID, 1, RGB(200, 200, 200));
	HPEN bar100 = CreatePen(PS_DOT, 1, RGB(130, 130, 130));
	HGDIOBJ old = SelectObject(dc, axis);

	POINT a[2] = {ToScreen(pl, 0, ZERO), ToScreen(pl, LEVELS - 1, ZERO)};
	POINT b[2] = {ToScreen(pl, ZERO, 0), ToScreen(pl, ZERO, LEVELS - 1)};
	Polyline(dc, a, 2);
	Polyline(dc, b, 2);

	SelectObject(dc, range);
	POINT sq[5] = {ToScreen(pl, LIM_LOW, LIM_LOW), ToScreen(pl, LIM_HIGH, LIM_LOW), ToScreen(pl, LIM_HIGH, LIM_HIGH),
		ToScreen(pl, LIM_LOW, LIM_HIGH), ToScreen(pl, LIM_LOW, LIM_LOW)};
	Polyline(dc, sq, 5);

	SelectObject(dc, skin);
	POINT s[2] = {Polar(pl, 0, 0), Polar(pl, LIM_HIGH - ZERO, SKIN_DEG * DEG)};
	Polyline(dc, s, 2);

	SetTextColor(dc, RGB(200, 200, 200));
	SetTextAlign(dc, TA_CENTER);
	for (int i = 0; i < NUM_BARS; i++)
	{
		double cb, cr;
		BarTarget(g_bars[i], 1.0, &cb, &cr);
		SelectObject(dc, bar100);
		DrawTargetBox(dc, pl, cb, cr);
		// label just outside the 100 % box
		POINT t = Polar(pl, hypot(cb - ZERO, cr - ZERO) + 12, atan2(cr - ZERO, cb - ZERO));
		TextOutA(dc, t.x, t.y - 6, g_bars[i].name, (int) strlen(g_bars[i].name));

		BarTarget(g_bars[i], 0.75, &cb, &cr);
		SelectObject(dc, bar75);
		DrawTargetBox(dc, pl, cb, cr);
	}

	SelectObject(dc, old);
	DeleteObject(axis);
	DeleteObject(range);
	DeleteObject(skin);
	DeleteObject(bar75);
	DeleteObject(bar100);
}

static void DrawScope(const DRAWITEMSTRUCT *di)
{
	static DWORD pix[LEVELS * LEVELS];
	int w = di->rcItem.right - di->rcItem.left;
	int h = di->rcItem.bottom - di->rcItem.top;
	Plane pl = {0, 0, w < h ? w : h};
	pl.x0 = (w - pl.side) / 2;
	pl.y0 = (h - pl.side) / 2;

	EnterCriticalSection(&g_lock);
	for (int i = 0; i < LEVELS * LEVELS; i++)
	{
		unsigned n = (&g_shown[0][0])[i];
		// shortcut: sqrt keeps both single hits and dense areas visible; tune the gains on real pictures
		unsigned c = n ? (unsigned) (sqrt((double) n) * g_gain) : 0;
		if (c > 255) c = 255;
		pix[i] = ((c / 4) << 16) | (c << 8) | (c / 4);	// green trace, 0x00RRGGBB
	}
	LeaveCriticalSection(&g_lock);

	HDC mem = CreateCompatibleDC(di->hDC);
	HBITMAP bmp = CreateCompatibleBitmap(di->hDC, w, h);
	HGDIOBJ oldBmp = SelectObject(mem, bmp);

	RECT r = {0, 0, w, h};
	FillRect(mem, &r, (HBRUSH) GetStockObject(BLACK_BRUSH));

	BITMAPINFO bi = {};
	bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
	bi.bmiHeader.biWidth = LEVELS;
	bi.bmiHeader.biHeight = -LEVELS;		// top-down
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	SetStretchBltMode(mem, COLORONCOLOR);
	StretchDIBits(mem, pl.x0, pl.y0, pl.side, pl.side, 0, 0, LEVELS, LEVELS, pix, &bi, DIB_RGB_COLORS, SRCCOPY);

	SetBkMode(mem, TRANSPARENT);
	SelectObject(mem, GetStockObject(DEFAULT_GUI_FONT));
	DrawGraticule(mem, pl);

	char status[96];
	if (!g_started)
		strcpy(status, "No video");
	else if (g_fmt != fYUY2)
		sprintf(status, "%dx%d RGB24: needs YUY2 input", g_frameW, g_frameH);
	else
	{
		int n = sprintf(status, "%dx%d YUY2", g_frameW, g_frameH);
		int line = g_line;
		if (line > 0)
			sprintf(status + n, "  line %d", line > g_frameH ? g_frameH : line);
	}
	SetTextColor(mem, RGB(200, 200, 200));
	SetTextAlign(mem, TA_RIGHT);
	TextOutA(mem, w - 4, 4, status, (int) strlen(status));
	SetTextAlign(mem, TA_LEFT);
	TextOutA(mem, 4, 4, "75 % solid", 10);
	TextOutA(mem, 4, 18, "100 % dotted", 12);

	BitBlt(di->hDC, di->rcItem.left, di->rcItem.top, w, h, mem, 0, 0, SRCCOPY);
	SelectObject(mem, oldBmp);
	DeleteObject(bmp);
	DeleteDC(mem);
}


////////////////////////////////////////////////////////////////////////////////
// Window
////////////////////////////////////////////////////////////////////////////////

// %APPDATA%\BeholdTV\Plugins\BPP_Vectorscope.ini, next to the other plugins' settings
static BOOL GetIniFile(char *path)
{
	const char *appData = getenv("APPDATA");
	if (!appData)
		return FALSE;
	_snprintf(path, MAX_PATH, "%s\\BeholdTV\\Plugins\\BPP_Vectorscope.ini", appData);
	path[MAX_PATH - 1] = 0;
	return TRUE;
}

static void RestorePos(HWND hDlg, HWND hParent)
{
	char ini[MAX_PATH];
	RECT d, p;
	GetWindowRect(hDlg, &d);

	if (GetIniFile(ini))
	{
		int x = GetPrivateProfileIntA("DIALOG", "DlgWndPosX", INT_MIN, ini);
		int y = GetPrivateProfileIntA("DIALOG", "DlgWndPosY", INT_MIN, ini);
		if (x != INT_MIN && y != INT_MIN)
		{
			SetWindowPos(hDlg, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
			return;
		}
	}
	if (hParent && GetWindowRect(hParent, &p))
		SetWindowPos(hDlg, NULL, (p.left + p.right - (d.right - d.left)) / 2, (p.top + p.bottom - (d.bottom - d.top)) / 2,
			0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void WriteIniInt(const char *section, const char *key, long value, const char *ini)
{
	char num[16];
	sprintf(num, "%ld", value);
	WritePrivateProfileStringA(section, key, num, ini);
}

static void SaveSettings(HWND hDlg)
{
	char ini[MAX_PATH];
	RECT r;
	if (!GetIniFile(ini))
		return;
	if (GetWindowRect(hDlg, &r))
	{
		WriteIniInt("DIALOG", "DlgWndPosX", r.left, ini);
		WriteIniInt("DIALOG", "DlgWndPosY", r.top, ini);
	}
	WriteIniInt("GLOBAL", "AllLines", IsDlgButtonChecked(hDlg, IDC_ALL_LINES) == BST_CHECKED, ini);
	WriteIniInt("GLOBAL", "Line", (long) GetDlgItemInt(hDlg, IDC_LINE, NULL, FALSE), ini);
}

static void LoadControls(HWND hDlg)
{
	char ini[MAX_PATH];
	BOOL all = TRUE;
	int line = 1;
	if (GetIniFile(ini))
	{
		all = GetPrivateProfileIntA("GLOBAL", "AllLines", 1, ini);
		line = GetPrivateProfileIntA("GLOBAL", "Line", 1, ini);
	}
	// all controls before the line spin: setting its position fires EN_CHANGE -> ApplyControls
	CheckRadioButton(hDlg, IDC_ALL_LINES, IDC_ONE_LINE, all ? IDC_ALL_LINES : IDC_ONE_LINE);
	SendDlgItemMessageA(hDlg, IDC_LINE, EM_LIMITTEXT, 4, 0);
	SendDlgItemMessageA(hDlg, IDC_LINE_SPIN, UDM_SETRANGE32, 1, MAX_LINE);
	SendDlgItemMessageA(hDlg, IDC_LINE_SPIN, UDM_SETPOS32, 0, line < 1 ? 1 : line > MAX_LINE ? MAX_LINE : line);
}

// Controls -> settings the streaming thread reads
static void ApplyControls(HWND hDlg)
{
	BOOL one = IsDlgButtonChecked(hDlg, IDC_ONE_LINE) == BST_CHECKED;
	int line = (int) GetDlgItemInt(hDlg, IDC_LINE, NULL, FALSE);
	if (line < 1) line = 1;		// empty while typing

	EnableWindow(GetDlgItem(hDlg, IDC_LINE), one);
	EnableWindow(GetDlgItem(hDlg, IDC_LINE_SPIN), one);
	g_line = one ? line : 0;

	HWND scope = g_hScope;
	if (scope)
		InvalidateRect(scope, NULL, FALSE);
}

static INT_PTR CALLBACK DialogProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
	case WM_INITDIALOG:
		LoadControls(hDlg);
		ApplyControls(hDlg);
		g_hScope = GetDlgItem(hDlg, IDC_SCOPE);
		RestorePos(hDlg, (HWND) lParam);
		return TRUE;

	case WM_DRAWITEM:
		if (wParam == IDC_SCOPE)
		{
			DrawScope((const DRAWITEMSTRUCT *) lParam);
			return TRUE;
		}
		break;

	case WM_COMMAND:
		if (LOWORD(wParam) == IDCANCEL)		// Close button, Esc and the caption's X
		{
			DestroyWindow(hDlg);
			return TRUE;
		}
		if ((LOWORD(wParam) == IDC_LINE && HIWORD(wParam) == EN_CHANGE) ||
			(LOWORD(wParam) != IDC_LINE && HIWORD(wParam) == BN_CLICKED))
		{
			ApplyControls(hDlg);
			return TRUE;
		}
		break;

	case WM_DESTROY:
		g_hScope = NULL;
		SaveSettings(hDlg);
		g_hDlg = NULL;
		break;
	}
	return FALSE;
}


////////////////////////////////////////////////////////////////////////////////
// Plugin functions
////////////////////////////////////////////////////////////////////////////////

void __cdecl VS_Init(INIT_STR *)
{
}

BOOL __cdecl VS_Start(PARAM_STR *pPrPar)
{
	if (pPrPar)
	{
		g_fmt = pPrPar->PixFmt;
		g_frameW = pPrPar->FrameWidth;
		g_frameH = pPrPar->FrameHeight;
		g_started = TRUE;
	}
	HWND scope = g_hScope;
	if (scope)
		InvalidateRect(scope, NULL, FALSE);
	return TRUE;
}

// Measure the frame into the window's vectorscope (only while the window is open)
BOOL __cdecl VS_Proc(PROC_STR *pPr)
{
	HWND scope = g_hScope;
	if (!scope || !pPr || !pPr->SrcBuff[0] || g_fmt != fYUY2 || pPr->FrameWidth <= 0 || pPr->FrameHeight <= 0)
		return TRUE;

	int h = pPr->FrameHeight;
	int first = 0, last = h - 1, line = g_line;
	if (line > 0)
		first = last = (line > h ? h : line) - 1;

	Accumulate(pPr->SrcBuff[0], pPr->FrameWidth, pPr->SrcPitch, first, last, g_work);

	EnterCriticalSection(&g_lock);
	memcpy(g_shown, g_work, sizeof(g_shown));
	g_gain = first == last ? GAIN_LINE : GAIN_ALL;
	LeaveCriticalSection(&g_lock);

	InvalidateRect(scope, NULL, FALSE);
	return TRUE;
}

void __cdecl VS_Stop(void)
{
	g_started = FALSE;
	HWND scope = g_hScope;
	if (scope)
		InvalidateRect(scope, NULL, FALSE);
}

void __cdecl VS_ShowDialog(DLG_STR *pDlgPar)
{
	if (g_hDlg)
	{
		SetForegroundWindow(g_hDlg);
		return;
	}
	INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_UPDOWN_CLASS};
	InitCommonControlsEx(&icc);
	g_hDlg = CreateDialogParamA(g_hInst, MAKEINTRESOURCEA(IDD_VECTORSCOPE), pDlgPar->hwndParent, DialogProc, (LPARAM) pDlgPar->hwndParent);
	if (g_hDlg)
		ShowWindow(g_hDlg, SW_SHOW);
}

void __cdecl VS_Uninit(void)
{
	if (g_hDlg)
		DestroyWindow(g_hDlg);
}

static PROC_PLUGIN Vectorscope_PL =
{
	(char *) "Vectorscope",
	(char *) "Cb/Cr vectorscope, 75 % and 100 % bar targets, skin-tone line, line select (YUY2)",
	VS_Init,
	VS_Start,
	VS_Proc,
	VS_Stop,
	VS_ShowDialog,
	VS_Uninit
};

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		g_hInst = hInst;
		InitializeCriticalSection(&g_lock);
	}
	else if (reason == DLL_PROCESS_DETACH)
		DeleteCriticalSection(&g_lock);
	return TRUE;
}

extern "C" {

BH_PLUGIN_EXP ULONG BeholdTV_Plugin_Version()
{
	return DSPLUGINVER;
}

BH_PLUGIN_EXP PROC_PLUGIN* BeholdTV_Plugin_PR()
{
	return &Vectorscope_PL;
}

}


////////////////////////////////////////////////////////////////////////////////
// Self-test: build with -DVECTORSCOPE_TEST as an exe.
// With a .bmp path argument it also renders 75 % bars onto the scope there.
////////////////////////////////////////////////////////////////////////////////

#ifdef VECTORSCOPE_TEST
#include <assert.h>

static void FillBars(BYTE *frame, int w, int h, int pitch, double level)
{
	for (int y = 0; y < h; y++)
		for (int x = 0; x + 1 < w; x += 2)
		{
			BYTE *p = frame + y * pitch + x * 2;
			int bar = x * NUM_BARS / w;
			double cb, cr;
			BarTarget(g_bars[bar], level, &cb, &cr);
			p[0] = p[2] = 128;
			p[1] = (BYTE) floor(cb + 0.5);
			p[3] = (BYTE) floor(cr + 0.5);
		}
}

static void Render(const char *path)
{
	enum { W = 720, H = 576, SW = 372, SH = 302 };	// scope control size at 96 dpi
	static BYTE frame[H * W * 2];
	FillBars(frame, W, H, W * 2, 0.75);
	Accumulate(frame, W, W * 2, 0, H - 1, g_shown);
	g_gain = GAIN_ALL;
	g_started = TRUE;
	g_fmt = fYUY2;
	g_frameW = W;
	g_frameH = H;

	BITMAPINFO bi = {};
	bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
	bi.bmiHeader.biWidth = SW;
	bi.bmiHeader.biHeight = SH;		// bottom-up, as a .bmp file stores it
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	void *bits;
	HDC dc = CreateCompatibleDC(NULL);
	HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
	SelectObject(dc, dib);

	DRAWITEMSTRUCT di = {};
	di.hDC = dc;
	SetRect(&di.rcItem, 0, 0, SW, SH);
	DrawScope(&di);
	GdiFlush();

	BITMAPFILEHEADER fh = {};
	fh.bfType = 0x4D42;
	fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader);
	fh.bfSize = fh.bfOffBits + SW * SH * 4;
	FILE *f = fopen(path, "wb");
	assert(f);
	fwrite(&fh, sizeof(fh), 1, f);
	fwrite(&bi.bmiHeader, sizeof(bi.bmiHeader), 1, f);
	fwrite(bits, SW * SH * 4, 1, f);
	fclose(f);
	DeleteDC(dc);
	DeleteObject(dib);
	printf("rendered %s\n", path);
}

int main(int argc, char **argv)
{
	InitializeCriticalSection(&g_lock);

	enum { W = 8, H = 3, PITCH = W * 2 + 4 };	// padded pitch to check line stepping
	BYTE frame[H][PITCH];
	memset(frame, 0xEE, sizeof(frame));
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x += 2)
		{
			BYTE *p = &frame[y][x * 2];
			p[0] = 16; p[2] = 235;			// Y must be ignored
			p[1] = (BYTE) (100 + x);		// Cb: 100, 102, 104, 106
			p[3] = (BYTE) (200 - x);		// Cr: 200, 198, 196, 194
		}

	static unsigned short grid[LEVELS][LEVELS];
	Accumulate(&frame[0][0], W, PITCH, 0, H - 1, grid);
	assert(grid[255 - 200][100] == H && grid[255 - 194][106] == H);
	assert(grid[255 - 235][16] == 0 && grid[255 - 0xEE][0xEE] == 0);	// Y and padding not counted
	unsigned total = 0;
	for (int i = 0; i < LEVELS * LEVELS; i++)
		total += (&grid[0][0])[i];
	assert(total == W / 2 * H);

	// single line, odd width: the unpaired last pixel is skipped
	frame[1][1] = 50;
	Accumulate(&frame[0][0], W - 1, PITCH, 1, 1, grid);
	assert(grid[255 - 200][50] == 1 && grid[255 - 200][100] == 0 && grid[255 - 194][106] == 0);

	// bar targets: BT.601 8-bit 75 % bars (Cb/Cr) and 100 % red
	const int expect75[NUM_BARS][2] = {{44, 142}, {156, 44}, {72, 58}, {184, 198}, {100, 212}, {212, 114}};
	for (int i = 0; i < NUM_BARS; i++)
	{
		double cb, cr;
		BarTarget(g_bars[i], 0.75, &cb, &cr);
		assert((int) floor(cb + 0.5) == expect75[i][0] && (int) floor(cr + 0.5) == expect75[i][1]);
	}
	double cb, cr;
	BarTarget(g_bars[4], 1.0, &cb, &cr);
	assert(fabs(cr - LIM_HIGH) < 0.01 && (int) floor(cb + 0.5) == 90);

	// screen mapping: Cb right, Cr up, codes centred in their cells
	Plane pl = {10, 20, LEVELS * 2};
	POINT p = ToScreen(pl, 0, LEVELS - 1);
	assert(p.x == 11 && p.y == 21);
	p = ToScreen(pl, LEVELS - 1, 0);
	assert(p.x == 10 + LEVELS * 2 - 1 && p.y == 20 + LEVELS * 2 - 1);

	if (argc > 1)
		Render(argv[1]);
	puts("vectorscope_test OK");
	return 0;
}
#endif
