////////////////////////////////////////////////////////////////////////////////
// BPP_Waveform: luma waveform monitor for BeholdTV (YUY2 input).
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

// Vertical scale: 8-bit codes 1..254 (0 and 255 are timing references, SMPTE ST 125)
enum { LVL_MIN = 1, LVL_MAX = 254, LVL_ROWS = LVL_MAX - LVL_MIN + 1, MAX_COLS = 1024 };
enum { LIM_BLACK = 16, LIM_WHITE = 235 };	// limited (studio) range
enum { TRACE_GAIN = 24 };					// brightness per sample hit, all lines
enum { MAX_W = 4096, MAX_LINE = 2048, LPF_MAX_RADIUS = 16 };
#define LPF_CUTOFF_HZ	1.0e6				// -3 dB point of the display low pass

static HINSTANCE		g_hInst;
static HWND volatile	g_hDlg;
static HWND volatile	g_hScope;
static CRITICAL_SECTION	g_lock;

// Format from StartStreaming, read by the window for its status line
static volatile BOOL	g_started;
static volatile ePixFmt	g_fmt;
static volatile int		g_frameW, g_frameH;

// Set by the window, read by the streaming thread
static volatile int		g_line;			// 0 = all lines, else 1-based frame line
static volatile BOOL	g_lowpass;

static unsigned short	g_work[LVL_ROWS][MAX_COLS];		// streaming thread only
static unsigned short	g_shown[LVL_ROWS][MAX_COLS];	// guarded by g_lock
static int				g_cols, g_gain;					// guarded by g_lock


////////////////////////////////////////////////////////////////////////////////
// Waveform
////////////////////////////////////////////////////////////////////////////////

// Gaussian low pass taps (in 1/4096) for a line 'w' luma samples wide; returns the radius.
// Like a scope's LPF view: keeps the average level, drops fine detail and chroma residue.
static int MakeLowPass(int w, int *taps)
{
	double fs = 13.5e6 * w / 720;				// BT.601 luma rate, scaled to this width
	double sigma = 0.1325 * fs / LPF_CUTOFF_HZ;	// Gaussian: f(-3 dB) = 0.1325 fs / sigma
	int r = (int) ceil(3 * sigma);
	if (r > LPF_MAX_RADIUS) r = LPF_MAX_RADIUS;

	double g[2 * LPF_MAX_RADIUS + 1], sum = 0;
	for (int i = -r; i <= r; i++)
		sum += g[i + r] = exp(-0.5 * i * i / (sigma * sigma));

	int total = 0;
	for (int i = 0; i <= 2 * r; i++)
		total += taps[i] = (int) floor(4096 * g[i] / sum + 0.5);
	taps[r] += 4096 - total;		// exact unity gain, so flat areas keep their level
	return r;
}

// Count luma samples per (column, level) for frame lines first..last (0-based).
// YUY2 is Y0 Cb Y1 Cr, so Y is every even byte. Row 0 is code 254 (top of the scope).
// radius 0 = no low pass.
static void Accumulate(const BYTE *buf, int w, int pitch, int first, int last, int cols,
	const int *taps, int radius, unsigned short (*wave)[MAX_COLS])
{
	static int luma[MAX_W];
	memset(wave, 0, sizeof(unsigned short) * LVL_ROWS * MAX_COLS);
	if (w > MAX_W) w = MAX_W;	// shortcut: wider frames lose their right part, raise MAX_W if one appears

	for (int y = first; y <= last; y++)
	{
		const BYTE *line = buf + y * pitch;
		for (int x = 0; x < w; x++)
			luma[x] = line[x * 2];

		for (int x = 0; x < w; x++)
		{
			int v = luma[x];
			if (radius)
			{
				int acc = 0;
				for (int i = -radius; i <= radius; i++)
				{
					int xi = x + i;
					xi = xi < 0 ? 0 : xi >= w ? w - 1 : xi;		// repeat the edge samples
					acc += taps[i + radius] * luma[xi];
				}
				v = (acc + 2048) >> 12;
			}
			// pin the reserved codes to the edges so they stay visible
			if (v < LVL_MIN) v = LVL_MIN;
			else if (v > LVL_MAX) v = LVL_MAX;

			unsigned short *c = &wave[LVL_MAX - v][x * cols / w];
			if (*c < 0xFFFF)
				(*c)++;
		}
	}
}

// Centre of a level's row in a scope 'height' pixels tall
static int LevelToY(int v, int height)
{
	return ((LVL_MAX - v) * 2 + 1) * height / (2 * LVL_ROWS);
}

static void DrawLevel(HDC dc, int v, int w, int h, COLORREF color, int penStyle)
{
	HPEN pen = CreatePen(penStyle, 1, color);
	HGDIOBJ old = SelectObject(dc, pen);
	int y = LevelToY(v, h);

	MoveToEx(dc, 0, y, NULL);
	LineTo(dc, w, y);
	SelectObject(dc, old);
	DeleteObject(pen);

	char label[8];
	sprintf(label, "%d", v);
	SetTextColor(dc, color);
	TextOutA(dc, 2, v > 128 ? y + 1 : y - 14, label, (int) strlen(label));
}

static void DrawScope(const DRAWITEMSTRUCT *di)
{
	static DWORD pix[LVL_ROWS * MAX_COLS];
	int w = di->rcItem.right - di->rcItem.left;
	int h = di->rcItem.bottom - di->rcItem.top;
	int cols;

	EnterCriticalSection(&g_lock);
	cols = g_cols;
	for (int i = 0; i < LVL_ROWS; i++)
		for (int x = 0; x < cols; x++)
		{
			unsigned c = g_shown[i][x] * g_gain;
			if (c > 255) c = 255;
			pix[i * cols + x] = ((c / 4) << 16) | (c << 8) | (c / 4);	// green trace, 0x00RRGGBB
		}
	LeaveCriticalSection(&g_lock);

	HDC mem = CreateCompatibleDC(di->hDC);
	HBITMAP bmp = CreateCompatibleBitmap(di->hDC, w, h);
	HGDIOBJ oldBmp = SelectObject(mem, bmp);

	RECT r = {0, 0, w, h};
	FillRect(mem, &r, (HBRUSH) GetStockObject(BLACK_BRUSH));

	if (cols > 0)
	{
		BITMAPINFO bi = {};
		bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
		bi.bmiHeader.biWidth = cols;
		bi.bmiHeader.biHeight = -LVL_ROWS;		// top-down
		bi.bmiHeader.biPlanes = 1;
		bi.bmiHeader.biBitCount = 32;
		bi.bmiHeader.biCompression = BI_RGB;
		SetStretchBltMode(mem, COLORONCOLOR);
		StretchDIBits(mem, 0, 0, w, h, 0, 0, cols, LVL_ROWS, pix, &bi, DIB_RGB_COLORS, SRCCOPY);
	}

	SetBkMode(mem, TRANSPARENT);
	SelectObject(mem, GetStockObject(DEFAULT_GUI_FONT));
	DrawLevel(mem, LVL_MAX, w, h, RGB(90, 90, 90), PS_SOLID);
	DrawLevel(mem, LVL_MIN, w, h, RGB(90, 90, 90), PS_SOLID);
	DrawLevel(mem, LIM_WHITE, w, h, RGB(230, 60, 60), PS_DOT);
	DrawLevel(mem, LIM_BLACK, w, h, RGB(230, 60, 60), PS_DOT);

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
			n += sprintf(status + n, "  line %d", line > g_frameH ? g_frameH : line);
		if (g_lowpass)
			strcpy(status + n, "  LPF");
	}
	SetTextColor(mem, RGB(200, 200, 200));
	SetTextAlign(mem, TA_RIGHT);
	TextOutA(mem, w - 4, 4, status, (int) strlen(status));

	BitBlt(di->hDC, di->rcItem.left, di->rcItem.top, w, h, mem, 0, 0, SRCCOPY);
	SelectObject(mem, oldBmp);
	DeleteObject(bmp);
	DeleteDC(mem);
}


////////////////////////////////////////////////////////////////////////////////
// Window
////////////////////////////////////////////////////////////////////////////////

// %APPDATA%\BeholdTV\Plugins\BPP_Waveform.ini, next to the other plugins' settings
static BOOL GetIniFile(char *path)
{
	const char *appData = getenv("APPDATA");
	if (!appData)
		return FALSE;
	_snprintf(path, MAX_PATH, "%s\\BeholdTV\\Plugins\\BPP_Waveform.ini", appData);
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
	WriteIniInt("GLOBAL", "LowPass", g_lowpass, ini);
}

static void LoadControls(HWND hDlg)
{
	char ini[MAX_PATH];
	BOOL all = TRUE, lowpass = FALSE;
	int line = 1;
	if (GetIniFile(ini))
	{
		all = GetPrivateProfileIntA("GLOBAL", "AllLines", 1, ini);
		line = GetPrivateProfileIntA("GLOBAL", "Line", 1, ini);
		lowpass = GetPrivateProfileIntA("GLOBAL", "LowPass", 0, ini);
	}
	CheckRadioButton(hDlg, IDC_ALL_LINES, IDC_ONE_LINE, all ? IDC_ALL_LINES : IDC_ONE_LINE);
	CheckDlgButton(hDlg, IDC_LOWPASS, lowpass ? BST_CHECKED : BST_UNCHECKED);
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
	g_lowpass = IsDlgButtonChecked(hDlg, IDC_LOWPASS) == BST_CHECKED;

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

BOOL __cdecl WF_Start(PARAM_STR *pPrPar)
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

BOOL __cdecl WF_Proc(PROC_STR *pPr)
{
	HWND scope = g_hScope;
	// nothing to do while the window is closed
	if (!scope || !pPr || !pPr->SrcBuff[0] || g_fmt != fYUY2 || pPr->FrameWidth <= 0 || pPr->FrameHeight <= 0)
		return TRUE;

	int w = pPr->FrameWidth, h = pPr->FrameHeight;
	int cols = w / 2;					// one column per YUY2 pixel pair
	if (cols > MAX_COLS) cols = MAX_COLS;

	int first = 0, last = h - 1, line = g_line;
	if (line > 0)
		first = last = (line > h ? h : line) - 1;

	static int taps[2 * LPF_MAX_RADIUS + 1], radius, tapsW;
	if (w != tapsW)
	{
		radius = MakeLowPass(w, taps);
		tapsW = w;
	}
	BOOL lowpass = g_lowpass;

	Accumulate(pPr->SrcBuff[0], w, pPr->SrcPitch, first, last, cols, taps, lowpass ? radius : 0, g_work);

	EnterCriticalSection(&g_lock);
	memcpy(g_shown, g_work, sizeof(g_shown));
	g_cols = cols;
	g_gain = first == last ? 255 : TRACE_GAIN;		// a single line has at most 2 hits per cell
	LeaveCriticalSection(&g_lock);

	InvalidateRect(scope, NULL, FALSE);
	return TRUE;
}

void __cdecl WF_Stop(void)
{
	g_started = FALSE;
	HWND scope = g_hScope;
	if (scope)
		InvalidateRect(scope, NULL, FALSE);
}

void __cdecl WF_ShowDialog(DLG_STR *pDlgPar)
{
	if (g_hDlg)
	{
		SetForegroundWindow(g_hDlg);
		return;
	}
	INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_UPDOWN_CLASS};
	InitCommonControlsEx(&icc);
	g_hDlg = CreateDialogParamA(g_hInst, MAKEINTRESOURCEA(IDD_WAVEFORM), pDlgPar->hwndParent, DialogProc, (LPARAM) pDlgPar->hwndParent);
	if (g_hDlg)
		ShowWindow(g_hDlg, SW_SHOW);
}

void __cdecl WF_Uninit(void)
{
	if (g_hDlg)
		DestroyWindow(g_hDlg);
}

static PROC_PLUGIN Waveform_PL =
{
	(char *) "Waveform monitor",
	(char *) "Luma waveform, levels 1-254, limited range 16/235 marked, line select, low pass (YUY2)",
	NULL,
	WF_Start,
	WF_Proc,
	WF_Stop,
	WF_ShowDialog,
	WF_Uninit
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
	return &Waveform_PL;
}

}


////////////////////////////////////////////////////////////////////////////////
// Self-test of the waveform counting: build with -DWAVEFORM_TEST as an exe
////////////////////////////////////////////////////////////////////////////////

#ifdef WAVEFORM_TEST
#include <assert.h>

int main()
{
	enum { W = 8, H = 3, PITCH = W * 2 + 4 };	// padded pitch to check line stepping
	BYTE frame[H][PITCH];
	memset(frame, 0xEE, sizeof(frame));
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
		{
			frame[y][x * 2] = x == 0 ? 0 : x == 1 ? 255 : (BYTE) (14 + x);	// Y: 0, 255, 16..21
			frame[y][x * 2 + 1] = 128;										// Cb/Cr must be ignored
		}

	static unsigned short wave[LVL_ROWS][MAX_COLS];
	Accumulate(&frame[0][0], W, PITCH, 0, H - 1, W / 2, NULL, 0, wave);

	assert(wave[LVL_MAX - 1][0] == H);		// code 0 pinned to 1, column 0
	assert(wave[LVL_MAX - 254][0] == H);	// code 255 pinned to 254, column 0
	assert(wave[LVL_MAX - 16][1] == H);		// x = 2 -> code 16, column 1
	assert(wave[LVL_MAX - 21][3] == H);		// x = 7 -> code 21, column 3
	assert(wave[LVL_MAX - 128][0] == 0);	// chroma bytes not counted

	unsigned total = 0;
	for (int i = 0; i < LVL_ROWS; i++)
		for (int x = 0; x < MAX_COLS; x++)
			total += wave[i][x];
	assert(total == W * H);

	// single line: only that line is counted
	frame[1][2 * 2] = 200;
	Accumulate(&frame[0][0], W, PITCH, 1, 1, W / 2, NULL, 0, wave);
	assert(wave[LVL_MAX - 200][1] == 1 && wave[LVL_MAX - 16][1] == 0 && wave[LVL_MAX - 17][1] == 1);
	total = 0;
	for (int i = 0; i < LVL_ROWS; i++)
		for (int x = 0; x < MAX_COLS; x++)
			total += wave[i][x];
	assert(total == W);

	// low pass taps: unity gain, symmetric, sensible width at 720
	int taps[2 * LPF_MAX_RADIUS + 1];
	int r = MakeLowPass(720, taps), sum = 0;
	for (int i = 0; i <= 2 * r; i++)
	{
		sum += taps[i];
		assert(taps[i] == taps[2 * r - i] || i == r);
	}
	assert(sum == 4096 && r >= 4 && r <= 8);

	// low pass on a 720-wide line: flat stays flat, a black-to-white step gets in-between levels
	enum { LW = 720 };
	static BYTE wide[2][LW * 2];
	for (int x = 0; x < LW; x++)
	{
		wide[0][x * 2] = 100;
		wide[1][x * 2] = x < LW / 2 ? 16 : 235;
		wide[0][x * 2 + 1] = wide[1][x * 2 + 1] = 128;
	}
	Accumulate(&wide[0][0], LW, LW * 2, 0, 0, LW / 2, taps, r, wave);
	assert(wave[LVL_MAX - 100][0] == 2 && wave[LVL_MAX - 100][LW / 2 - 1] == 2);
	Accumulate(&wide[0][0], LW, LW * 2, 1, 1, LW / 2, NULL, 0, wave);
	int between = 0;
	for (int v = 17; v < 235; v++)
		for (int x = 0; x < LW / 2; x++)
			between += wave[LVL_MAX - v][x];
	assert(between == 0);		// without low pass the step is clean
	Accumulate(&wide[0][0], LW, LW * 2, 1, 1, LW / 2, taps, r, wave);
	for (int v = 17; v < 235; v++)
		for (int x = 0; x < LW / 2; x++)
			between += wave[LVL_MAX - v][x];
	assert(between >= 4 && between <= 2 * r);		// only samples within the filter radius of the edge move
	assert(wave[LVL_MAX - 16][0] == 2 && wave[LVL_MAX - 235][LW / 2 - 1] == 2);

	assert(LevelToY(LVL_MAX, LVL_ROWS) == 0 && LevelToY(LVL_MIN, LVL_ROWS) == LVL_ROWS - 1);
	assert(LevelToY(LIM_WHITE, 2 * LVL_ROWS) == 39);		// 2 px per level: row 19, centre pixel 39

	puts("waveform_test OK");
	return 0;
}
#endif
