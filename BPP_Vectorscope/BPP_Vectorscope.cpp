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
enum { LEVELS = 256, ZERO = 128, LIM_LOW = 16, LIM_HIGH = 240, RING = LIM_HIGH - ZERO, MAX_LINE = 2048 };
enum { GAIN_ALL = 32, GAIN_LINE = 180 };	// trace brightness per sqrt(hits) at gain 10, all lines / one line
enum { GAIN_DEFAULT = 10, GAIN_MAX = 100 };
enum { GRAT_NONE, GRAT_75, GRAT_100, GRAT_BOTH };
// Fields of an interlaced (woven) frame: top = frame lines 1, 3, 5..., bottom = 2, 4, 6...
enum { FIELD_ALL, FIELD_TOP, FIELD_BOTTOM };
enum { ARC_PTS = 9 };						// points per arc of a polar box
#define DEG			(3.14159265358979 / 180)
#define I_DEG		123.0					// I axis, from +Cb; also the skin-tone line
#define Q_DEG		33.0
// Target tolerances, scope makers' convention (not a standard): small box +-3 deg / +-5 %, sector +-10 deg / +-10 %
#define BOX_DEG		3.0
#define BOX_AMP		0.05
#define SECTOR_DEG	10.0
#define SECTOR_AMP	0.10

static HINSTANCE		g_hInst;
static HWND volatile	g_hDlg;
static HWND volatile	g_hScope;
static CRITICAL_SECTION	g_lock;

// Format from StartStreaming, read by the window for its status line
static volatile BOOL	g_started;
static volatile ePixFmt	g_fmt;
static volatile int		g_frameW, g_frameH;

static volatile int		g_line;			// 0 = all lines, else 1-based frame line (set by the window)
static volatile int		g_field;		// FIELD_*, used when g_line is 0 (set by the window)

// Display settings (window thread only)
static BOOL				g_colorize = TRUE;
static int				g_userGain = GAIN_DEFAULT;
static int				g_grat = GRAT_75;

static unsigned short	g_work[LEVELS][LEVELS];		// [255 - Cr][Cb], streaming thread only
static unsigned short	g_shown[LEVELS][LEVELS];	// guarded by g_lock
static int				g_gain;						// guarded by g_lock

// Colour bars in the order they appear, BT.471 names, with the graticule colour for each
struct Bar { const char *name; int r, g, b; COLORREF color; };
static const Bar g_bars[] =
{
	{"Yl", 1, 1, 0, RGB(220, 190, 60)}, {"Cy", 0, 1, 1, RGB(70, 200, 200)}, {"G", 0, 1, 0, RGB(70, 200, 90)},
	{"Mg", 1, 0, 1, RGB(210, 80, 210)}, {"R", 1, 0, 0, RGB(235, 80, 80)}, {"B", 0, 0, 1, RGB(80, 130, 235)},
};
enum { NUM_BARS = sizeof(g_bars) / sizeof(g_bars[0]) };


////////////////////////////////////////////////////////////////////////////////
// Vectorscope
////////////////////////////////////////////////////////////////////////////////

// Count Cb/Cr samples for frame lines first..last (0-based), every 'step'th line (2 = one field).
// YUY2 is Y0 Cb Y1 Cr: one (Cb, Cr) point per pixel pair.
static void Accumulate(const BYTE *buf, int w, int pitch, int first, int last, int step, unsigned short (*grid)[LEVELS])
{
	memset(grid, 0, sizeof(unsigned short) * LEVELS * LEVELS);
	for (int y = first; y <= last; y += step)
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

// Trace colour of each (Cb, Cr) cell, same layout as g_shown: the hue at its brightest
// (BT.601 RGB of Y = 0.5 with that chroma, scaled so the largest component is 255).
// The centre comes out white.
static DWORD g_hue[LEVELS * LEVELS];

static void MakeHues()
{
	for (int cr = 0; cr < LEVELS; cr++)
		for (int cb = 0; cb < LEVELS; cb++)
		{
			double pb = (cb - ZERO) / 224.0, pr = (cr - ZERO) / 224.0;
			double c[3] = {0.5 + 1.402 * pr, 0.5 - 0.344136 * pb - 0.714136 * pr, 0.5 + 1.772 * pb};
			double m = 0;
			for (int i = 0; i < 3; i++)
			{
				if (c[i] < 0) c[i] = 0;
				if (c[i] > m) m = c[i];
			}
			if (m == 0) m = 1;
			DWORD v = 0;
			for (int i = 0; i < 3; i++)
				v = v << 8 | (DWORD) (255 * c[i] / m + 0.5);
			g_hue[(LEVELS - 1 - cr) * LEVELS + cb] = v;		// 0x00RRGGBB
		}
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

// Outline of the region within +-amp (fraction) and +-deg around a target, curved like the graticule
static void PolarBox(const Plane &pl, double cb, double cr, double amp, double deg, POINT *pt)
{
	double r = hypot(cb - ZERO, cr - ZERO), a = atan2(cr - ZERO, cb - ZERO);
	for (int i = 0; i < ARC_PTS; i++)
	{
		double t = a + (2.0 * i / (ARC_PTS - 1) - 1) * deg * DEG;
		pt[i] = Polar(pl, r * (1 + amp), t);
		pt[2 * ARC_PTS - 1 - i] = Polar(pl, r * (1 - amp), t);
	}
}

static BOOL ShowLevel(int grat, double level)
{
	return grat == GRAT_BOTH || grat == (level == 0.75 ? GRAT_75 : GRAT_100);
}

static COLORREF Dim(COLORREF c, int pct)
{
	return RGB(GetRValue(c) * pct / 100, GetGValue(c) * pct / 100, GetBValue(c) * pct / 100);
}

// Filled target sectors, drawn under the trace
static void DrawSectors(HDC dc, const Plane &pl, int grat)
{
	for (int i = 0; i < NUM_BARS; i++)
		for (double level = 0.75; level <= 1.0; level += 0.25)
		{
			if (!ShowLevel(grat, level))
				continue;
			double cb, cr;
			POINT pt[2 * ARC_PTS];
			BarTarget(g_bars[i], level, &cb, &cr);
			PolarBox(pl, cb, cr, SECTOR_AMP, SECTOR_DEG, pt);

			HBRUSH fill = CreateSolidBrush(Dim(g_bars[i].color, 22));
			HPEN edge = CreatePen(PS_SOLID, 1, Dim(g_bars[i].color, 50));
			HGDIOBJ oldBrush = SelectObject(dc, fill), oldPen = SelectObject(dc, edge);
			Polygon(dc, pt, 2 * ARC_PTS);
			SelectObject(dc, oldBrush);
			SelectObject(dc, oldPen);
			DeleteObject(fill);
			DeleteObject(edge);
		}
}

static void Line(HDC dc, POINT a, POINT b)
{
	MoveToEx(dc, a.x, a.y, NULL);
	LineTo(dc, b.x, b.y);
}

// Ring with degree ticks, Cb/Cr axes, I/Q lines, 16..240 range square, target boxes and labels
static void DrawGraticule(HDC dc, const Plane &pl, int grat)
{
	HPEN white = CreatePen(PS_SOLID, 1, RGB(230, 230, 230));
	HPEN ring = CreatePen(PS_SOLID, 2, RGB(230, 230, 230));
	HPEN range = CreatePen(PS_DOT, 1, RGB(90, 90, 90));
	HGDIOBJ oldPen = SelectObject(dc, ring), oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));

	POINT tl = ToScreen(pl, ZERO - RING, ZERO + RING), br = ToScreen(pl, ZERO + RING, ZERO - RING);
	Ellipse(dc, tl.x, tl.y, br.x + 1, br.y + 1);

	SelectObject(dc, white);
	for (int d = 0; d < 360; d += 2)
		Line(dc, Polar(pl, RING, d * DEG), Polar(pl, RING - (d % 10 ? 2 : 5), d * DEG));
	Line(dc, Polar(pl, RING, 0), Polar(pl, RING, 180 * DEG));
	Line(dc, Polar(pl, RING, 90 * DEG), Polar(pl, RING, 270 * DEG));

	const struct { double deg; const char *name; } iq[] = {{I_DEG, "I"}, {Q_DEG, "Q"}, {I_DEG + 180, "-I"}, {Q_DEG + 180, "-Q"}};
	SetTextColor(dc, RGB(170, 170, 170));
	SetTextAlign(dc, TA_CENTER);
	for (int i = 0; i < 4; i++)
	{
		Line(dc, Polar(pl, RING * 0.2, iq[i].deg * DEG), Polar(pl, RING - 6, iq[i].deg * DEG));
		POINT t = Polar(pl, RING - 12, (iq[i].deg + 7) * DEG);
		TextOutA(dc, t.x, t.y - 6, iq[i].name, (int) strlen(iq[i].name));
	}

	SelectObject(dc, range);
	POINT sq[5] = {ToScreen(pl, LIM_LOW, LIM_LOW), ToScreen(pl, LIM_HIGH, LIM_LOW), ToScreen(pl, LIM_HIGH, LIM_HIGH),
		ToScreen(pl, LIM_LOW, LIM_HIGH), ToScreen(pl, LIM_LOW, LIM_LOW)};
	Polyline(dc, sq, 5);

	for (int i = 0; i < NUM_BARS; i++)
	{
		HPEN pen = CreatePen(PS_SOLID, 1, g_bars[i].color);
		SelectObject(dc, pen);
		double cb, cr, inner = 0, outer = 0;
		for (double level = 0.75; level <= 1.0; level += 0.25)
		{
			if (!ShowLevel(grat, level))
				continue;
			POINT pt[2 * ARC_PTS];
			BarTarget(g_bars[i], level, &cb, &cr);
			PolarBox(pl, cb, cr, BOX_AMP, BOX_DEG, pt);
			Polygon(dc, pt, 2 * ARC_PTS);
			POINT c = ToScreen(pl, cb, cr);
			const POINT &o = pt[0], &in = pt[2 * ARC_PTS - 1];	// outer and inner corner on one side
			int arm = (int) hypot(o.x - in.x, o.y - in.y) / 2;	// crosshair half the box's radial size
			MoveToEx(dc, c.x - arm, c.y, NULL);
			LineTo(dc, c.x + arm + 1, c.y);
			MoveToEx(dc, c.x, c.y - arm, NULL);
			LineTo(dc, c.x, c.y + arm + 1);
			outer = hypot(cb - ZERO, cr - ZERO);
			if (!inner)
				inner = outer;
		}
		if (outer > 0)
		{
			// label just outside the outermost sector shown, or inside the innermost when that would hit the ring
			double r = outer * (1 + SECTOR_AMP) + 8;
			if (r > RING - 6)
				r = inner * (1 - SECTOR_AMP) - 8;
			POINT t = Polar(pl, r, atan2(cr - ZERO, cb - ZERO));
			SetTextColor(dc, g_bars[i].color);
			TextOutA(dc, t.x, t.y - 6, g_bars[i].name, (int) strlen(g_bars[i].name));
		}
		SelectObject(dc, white);
		DeleteObject(pen);
	}

	SelectObject(dc, oldPen);
	SelectObject(dc, oldBrush);
	DeleteObject(white);
	DeleteObject(ring);
	DeleteObject(range);
}

static void DrawScope(const DRAWITEMSTRUCT *di)
{
	static DWORD pix[LEVELS * LEVELS];
	static BOOL huesMade;
	if (!huesMade)
	{
		MakeHues();
		huesMade = TRUE;
	}
	int w = di->rcItem.right - di->rcItem.left;
	int h = di->rcItem.bottom - di->rcItem.top;
	Plane pl = {0, 0, w < h ? w : h};
	pl.x0 = (w - pl.side) / 2;
	pl.y0 = (h - pl.side) / 2;

	EnterCriticalSection(&g_lock);
	double gain = g_gain * g_userGain / (double) GAIN_DEFAULT;
	for (int i = 0; i < LEVELS * LEVELS; i++)
	{
		unsigned n = (&g_shown[0][0])[i];
		// sqrt keeps both single hits and dense areas visible
		unsigned c = n ? (unsigned) (sqrt((double) n) * gain) : 0;
		if (c > 255) c = 255;
		if (g_colorize)
		{
			DWORD hue = g_hue[i];
			pix[i] = ((hue >> 16 & 255) * c / 255) << 16 | ((hue >> 8 & 255) * c / 255) << 8 | (hue & 255) * c / 255;
		}
		else
			pix[i] = ((c / 4) << 16) | (c << 8) | (c / 4);	// green trace
	}
	LeaveCriticalSection(&g_lock);

	HDC mem = CreateCompatibleDC(di->hDC);
	HBITMAP bmp = CreateCompatibleBitmap(di->hDC, w, h);
	HGDIOBJ oldBmp = SelectObject(mem, bmp);

	RECT r = {0, 0, w, h};
	FillRect(mem, &r, (HBRUSH) GetStockObject(BLACK_BRUSH));
	DrawSectors(mem, pl, g_grat);

	BITMAPINFO bi = {};
	bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
	bi.bmiHeader.biWidth = LEVELS;
	bi.bmiHeader.biHeight = -LEVELS;		// top-down
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	SetStretchBltMode(mem, COLORONCOLOR);
	// OR onto the sectors, so the trace shows through them
	StretchDIBits(mem, pl.x0, pl.y0, pl.side, pl.side, 0, 0, LEVELS, LEVELS, pix, &bi, DIB_RGB_COLORS, SRCPAINT);

	SetBkMode(mem, TRANSPARENT);
	SelectObject(mem, GetStockObject(DEFAULT_GUI_FONT));
	DrawGraticule(mem, pl, g_grat);

	char status[96];
	if (!g_started)
		strcpy(status, "No video");
	else if (g_fmt != fYUY2)
		sprintf(status, "%dx%d RGB24: needs YUY2 input", g_frameW, g_frameH);
	else
	{
		int n = sprintf(status, "%dx%d YUY2", g_frameW, g_frameH);
		int line = g_line, field = g_field;
		if (line > 0)
			sprintf(status + n, "  line %d", line > g_frameH ? g_frameH : line);
		else if (field != FIELD_ALL)
			strcpy(status + n, field == FIELD_TOP ? "  top field" : "  bottom field");
	}
	SetTextColor(mem, RGB(200, 200, 200));
	SetTextAlign(mem, TA_LEFT);
	TextOutA(mem, 4, 4, status, (int) strlen(status));

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

// Resizing: the scope takes the change in size; controls right of or below it move with those edges
struct Anchored { HWND h; RECT r; };
static Anchored		g_ctl[32];
static int			g_numCtl;			// 0 until WM_INITDIALOG has recorded the layout
static SIZE			g_client0, g_minSize;
static RECT			g_scope0;

static BOOL CALLBACK AddControl(HWND h, LPARAM hDlg)
{
	if (g_numCtl < (int) (sizeof(g_ctl) / sizeof(g_ctl[0])))
	{
		Anchored &a = g_ctl[g_numCtl++];
		a.h = h;
		GetWindowRect(h, &a.r);
		MapWindowPoints(NULL, (HWND) hDlg, (POINT *) &a.r, 2);
	}
	return TRUE;
}

// Record the dialog's initial layout; its size is also the minimum
static void InitLayout(HWND hDlg)
{
	RECT r;
	GetClientRect(hDlg, &r);
	g_client0.cx = r.right;
	g_client0.cy = r.bottom;
	GetWindowRect(hDlg, &r);
	g_minSize.cx = r.right - r.left;
	g_minSize.cy = r.bottom - r.top;
	GetWindowRect(GetDlgItem(hDlg, IDC_SCOPE), &g_scope0);
	MapWindowPoints(NULL, hDlg, (POINT *) &g_scope0, 2);
	g_numCtl = 0;
	EnumChildWindows(hDlg, AddControl, (LPARAM) hDlg);
}

static void Layout(HWND hDlg, int cx, int cy)
{
	int dx = cx - g_client0.cx, dy = cy - g_client0.cy;
	HWND scope = GetDlgItem(hDlg, IDC_SCOPE);
	HDWP dwp = BeginDeferWindowPos(g_numCtl);
	for (int i = 0; i < g_numCtl && dwp; i++)
	{
		RECT r = g_ctl[i].r;
		if (g_ctl[i].h == scope)
		{
			r.right += dx;
			r.bottom += dy;
		}
		else
		{
			if (r.right >= g_scope0.right)
				OffsetRect(&r, dx, 0);
			if (r.bottom >= g_scope0.bottom)
				OffsetRect(&r, 0, dy);
		}
		dwp = DeferWindowPos(dwp, g_ctl[i].h, NULL, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
	}
	if (dwp)
		EndDeferWindowPos(dwp);
	InvalidateRect(hDlg, NULL, TRUE);
}

static void RestorePos(HWND hDlg, HWND hParent)
{
	char ini[MAX_PATH];
	RECT d, p;

	if (GetIniFile(ini))
	{
		int w = GetPrivateProfileIntA("DIALOG", "DlgWndWidth", 0, ini);
		int h = GetPrivateProfileIntA("DIALOG", "DlgWndHeight", 0, ini);
		if (w >= g_minSize.cx && h >= g_minSize.cy)
			SetWindowPos(hDlg, NULL, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
	}
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

// "Always on top" in the window's system menu (command IDs there must be below 0xF000, low 4 bits clear)
enum { IDM_TOPMOST = 0x0010, WM_APP_TOPMOST = WM_APP + 1 };
static BOOL			g_topmost;			// window thread only

static void SetTopmost(HWND hDlg, BOOL on)
{
	g_topmost = on;
	SetWindowPos(hDlg, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	CheckMenuItem(GetSystemMenu(hDlg, FALSE), IDM_TOPMOST, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
}

static void InitTopmost(HWND hDlg)
{
	char ini[MAX_PATH];
	HMENU menu = GetSystemMenu(hDlg, FALSE);
	AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
	AppendMenuA(menu, MF_STRING, IDM_TOPMOST, "Always on top");
	SetTopmost(hDlg, GetIniFile(ini) && GetPrivateProfileIntA("DIALOG", "Topmost", 0, ini));
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
	if (!IsIconic(hDlg) && GetWindowRect(hDlg, &r))		// closed while minimized: keep the last saved place
	{
		WriteIniInt("DIALOG", "DlgWndPosX", r.left, ini);
		WriteIniInt("DIALOG", "DlgWndPosY", r.top, ini);
		WriteIniInt("DIALOG", "DlgWndWidth", r.right - r.left, ini);
		WriteIniInt("DIALOG", "DlgWndHeight", r.bottom - r.top, ini);
	}
	WriteIniInt("DIALOG", "Topmost", g_topmost, ini);
	WriteIniInt("GLOBAL", "AllLines", IsDlgButtonChecked(hDlg, IDC_ALL_LINES) == BST_CHECKED, ini);
	WriteIniInt("GLOBAL", "Line", (long) GetDlgItemInt(hDlg, IDC_LINE, NULL, FALSE), ini);
	WriteIniInt("GLOBAL", "Field", g_field, ini);
	WriteIniInt("GLOBAL", "Colorize", g_colorize, ini);
	WriteIniInt("GLOBAL", "Gain", g_userGain, ini);
	WriteIniInt("GLOBAL", "Graticule", g_grat, ini);
}

static int Clamp(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static void LoadControls(HWND hDlg)
{
	char ini[MAX_PATH];
	BOOL all = TRUE, colorize = TRUE;
	int line = 1, field = FIELD_ALL, gain = GAIN_DEFAULT, grat = GRAT_75;
	if (GetIniFile(ini))
	{
		all = GetPrivateProfileIntA("GLOBAL", "AllLines", 1, ini);
		line = GetPrivateProfileIntA("GLOBAL", "Line", 1, ini);
		field = GetPrivateProfileIntA("GLOBAL", "Field", FIELD_ALL, ini);
		colorize = GetPrivateProfileIntA("GLOBAL", "Colorize", 1, ini);
		gain = GetPrivateProfileIntA("GLOBAL", "Gain", GAIN_DEFAULT, ini);
		grat = GetPrivateProfileIntA("GLOBAL", "Graticule", GRAT_75, ini);
	}
	// all controls before the spins: setting their position fires EN_CHANGE -> ApplyControls
	CheckDlgButton(hDlg, IDC_COLORIZE, colorize ? BST_CHECKED : BST_UNCHECKED);
	CheckRadioButton(hDlg, IDC_GRAT_NONE, IDC_GRAT_BOTH, IDC_GRAT_NONE + Clamp(grat, GRAT_NONE, GRAT_BOTH));
	CheckRadioButton(hDlg, IDC_ALL_LINES, IDC_ONE_LINE, all ? IDC_ALL_LINES : IDC_ONE_LINE);
	CheckRadioButton(hDlg, IDC_FIELD_ALL, IDC_FIELD_BOTTOM, IDC_FIELD_ALL + Clamp(field, FIELD_ALL, FIELD_BOTTOM));
	SendDlgItemMessageA(hDlg, IDC_GAIN, EM_LIMITTEXT, 3, 0);
	SendDlgItemMessageA(hDlg, IDC_GAIN_SPIN, UDM_SETRANGE32, 1, GAIN_MAX);
	SendDlgItemMessageA(hDlg, IDC_GAIN_SPIN, UDM_SETPOS32, 0, Clamp(gain, 1, GAIN_MAX));
	SendDlgItemMessageA(hDlg, IDC_LINE, EM_LIMITTEXT, 4, 0);
	SendDlgItemMessageA(hDlg, IDC_LINE_SPIN, UDM_SETRANGE32, 1, MAX_LINE);
	SendDlgItemMessageA(hDlg, IDC_LINE_SPIN, UDM_SETPOS32, 0, Clamp(line, 1, MAX_LINE));
}

// Controls -> settings
static void ApplyControls(HWND hDlg)
{
	BOOL one = IsDlgButtonChecked(hDlg, IDC_ONE_LINE) == BST_CHECKED;
	int line = (int) GetDlgItemInt(hDlg, IDC_LINE, NULL, FALSE);
	if (line < 1) line = 1;		// empty while typing

	EnableWindow(GetDlgItem(hDlg, IDC_LINE), one);
	EnableWindow(GetDlgItem(hDlg, IDC_LINE_SPIN), one);
	g_line = one ? line : 0;
	// a single line already belongs to one field
	for (int id = IDC_FIELD_ALL; id <= IDC_FIELD_BOTTOM; id++)
	{
		EnableWindow(GetDlgItem(hDlg, id), !one);
		if (IsDlgButtonChecked(hDlg, id) == BST_CHECKED)
			g_field = id - IDC_FIELD_ALL;
	}

	g_colorize = IsDlgButtonChecked(hDlg, IDC_COLORIZE) == BST_CHECKED;
	g_userGain = Clamp((int) GetDlgItemInt(hDlg, IDC_GAIN, NULL, FALSE), 1, GAIN_MAX);
	for (int id = IDC_GRAT_NONE; id <= IDC_GRAT_BOTH; id++)
		if (IsDlgButtonChecked(hDlg, id) == BST_CHECKED)
			g_grat = id - IDC_GRAT_NONE;

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
		InitLayout(hDlg);
		RestorePos(hDlg, (HWND) lParam);
		InitTopmost(hDlg);
		return TRUE;

	case WM_WINDOWPOSCHANGED:
		// BeholdTV (Delphi VCL) clears topmost on its thread's windows when it loses focus: put it back.
		// Posted, as SetWindowPos can't undo it from inside the change.
		if (g_topmost && !(GetWindowLong(hDlg, GWL_EXSTYLE) & WS_EX_TOPMOST))
			PostMessage(hDlg, WM_APP_TOPMOST, 0, 0);
		break;

	case WM_APP_TOPMOST:
		if (g_topmost)
			SetTopmost(hDlg, TRUE);
		return TRUE;

	case WM_SYSCOMMAND:
		if ((wParam & 0xFFF0) == IDM_TOPMOST)
		{
			SetTopmost(hDlg, !g_topmost);
			return TRUE;
		}
		break;

	case WM_SIZE:
		if (g_numCtl && wParam != SIZE_MINIMIZED)
			Layout(hDlg, LOWORD(lParam), HIWORD(lParam));
		return TRUE;

	case WM_GETMINMAXINFO:
		if (g_numCtl)
		{
			((MINMAXINFO *) lParam)->ptMinTrackSize.x = g_minSize.cx;
			((MINMAXINFO *) lParam)->ptMinTrackSize.y = g_minSize.cy;
		}
		return TRUE;

	case WM_DRAWITEM:
		if (wParam == IDC_SCOPE)
		{
			DrawScope((const DRAWITEMSTRUCT *) lParam);
			return TRUE;
		}
		break;

	case WM_COMMAND:
	{
		int id = LOWORD(wParam), code = HIWORD(wParam);
		if (id == IDCANCEL)		// Close button, Esc and the caption's X
		{
			DestroyWindow(hDlg);
			return TRUE;
		}
		BOOL edit = id == IDC_LINE || id == IDC_GAIN;
		if ((edit && code == EN_CHANGE) || (!edit && code == BN_CLICKED))
		{
			ApplyControls(hDlg);
			return TRUE;
		}
		break;
	}

	case WM_DESTROY:
		g_hScope = NULL;
		SaveSettings(hDlg);
		g_numCtl = 0;
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
	int first = 0, last = h - 1, step = 1, line = g_line, field = g_field;
	if (line > 0)
		first = last = (line > h ? h : line) - 1;
	else if (field != FIELD_ALL)
	{
		first = field == FIELD_TOP ? 0 : 1;
		step = 2;
	}

	Accumulate(pPr->SrcBuff[0], pPr->FrameWidth, pPr->SrcPitch, first, last, step, g_work);

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
	{
		ShowWindow(g_hDlg, SW_SHOW);
		SetTopmost(g_hDlg, g_topmost);		// set during WM_INITDIALOG it doesn't stick
	}
}

void __cdecl VS_Uninit(void)
{
	if (g_hDlg)
		DestroyWindow(g_hDlg);
}

static PROC_PLUGIN Vectorscope_PL =
{
	(char *) "Vectorscope",
	(char *) "Cb/Cr vectorscope, colour trace, 75 % / 100 % bar targets, I/Q lines, gain, line select (YUY2)",
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

// Bars with a little noise, so the trace has some spread like a real signal
static void FillBars(BYTE *frame, int w, int h, int pitch, double level)
{
	srand(1);
	for (int y = 0; y < h; y++)
		for (int x = 0; x + 1 < w; x += 2)
		{
			BYTE *p = frame + y * pitch + x * 2;
			int bar = x * NUM_BARS / w;
			double cb, cr;
			BarTarget(g_bars[bar], level, &cb, &cr);
			p[0] = p[2] = 128;
			p[1] = (BYTE) floor(cb + 0.5 + rand() % 5 - 2);
			p[3] = (BYTE) floor(cr + 0.5 + rand() % 5 - 2);
		}
}

static void Render(const char *path)
{
	enum { W = 720, H = 576, SW = 349, SH = 367 };	// scope control size at 96 dpi
	static BYTE frame[H * W * 2];
	FillBars(frame, W, H, W * 2, 0.75);
	Accumulate(frame, W, W * 2, 0, H - 1, 1, g_shown);
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
	Accumulate(&frame[0][0], W, PITCH, 0, H - 1, 1, grid);
	assert(grid[255 - 200][100] == H && grid[255 - 194][106] == H);
	assert(grid[255 - 235][16] == 0 && grid[255 - 0xEE][0xEE] == 0);	// Y and padding not counted
	unsigned total = 0;
	for (int i = 0; i < LEVELS * LEVELS; i++)
		total += (&grid[0][0])[i];
	assert(total == W / 2 * H);

	// single line, odd width: the unpaired last pixel is skipped
	frame[1][1] = 50;
	Accumulate(&frame[0][0], W - 1, PITCH, 1, 1, 1, grid);
	assert(grid[255 - 200][50] == 1 && grid[255 - 200][100] == 0 && grid[255 - 194][106] == 0);

	// fields: top = lines 0 and 2, bottom = line 1 (the one with Cb 50)
	Accumulate(&frame[0][0], W, PITCH, 0, H - 1, 2, grid);
	assert(grid[255 - 200][100] == 2 && grid[255 - 200][50] == 0);
	Accumulate(&frame[0][0], W, PITCH, 1, H - 1, 2, grid);
	assert(grid[255 - 200][50] == 1 && grid[255 - 200][100] == 0 && grid[255 - 194][106] == 1);

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

	// trace colours: centre white, 75 % red reddish, 75 % blue bluish
	MakeHues();
	assert(g_hue[(255 - ZERO) * LEVELS + ZERO] == 0xFFFFFF);
	DWORD red = g_hue[(255 - 212) * LEVELS + 100], blue = g_hue[(255 - 114) * LEVELS + 212];
	assert((red >> 16) == 255 && (red >> 8 & 255) < 128 && (red & 255) < 128);
	assert((blue & 255) == 255 && (blue >> 16) < 128);

	// graticule choice
	assert(ShowLevel(GRAT_75, 0.75) && !ShowLevel(GRAT_75, 1.0) && ShowLevel(GRAT_100, 1.0));
	assert(ShowLevel(GRAT_BOTH, 0.75) && ShowLevel(GRAT_BOTH, 1.0) && !ShowLevel(GRAT_NONE, 0.75));

	if (argc > 1)
		Render(argv[1]);
	puts("vectorscope_test OK");
	return 0;
}
#endif
