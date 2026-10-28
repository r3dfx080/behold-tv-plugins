////////////////////////////////////////////////////////////////////////////////
// BPP_Waveform: luma waveform monitor for BeholdTV (YUY2 input).
// Shown in its own window, opened from the plugin's settings button.
// The frame is only read, never changed.
////////////////////////////////////////////////////////////////////////////////

#include <windows.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Common.h"
#include "resource.h"

#define BH_PLUGIN_EXP	__declspec(dllexport)

// Vertical scale: 8-bit codes 1..254 (0 and 255 are timing references, SMPTE ST 125)
enum { LVL_MIN = 1, LVL_MAX = 254, LVL_ROWS = LVL_MAX - LVL_MIN + 1, MAX_COLS = 1024 };
enum { LIM_BLACK = 16, LIM_WHITE = 235 };	// limited (studio) range
enum { TRACE_GAIN = 24 };					// brightness per sample hit

static HINSTANCE		g_hInst;
static HWND volatile	g_hDlg;
static HWND volatile	g_hScope;
static CRITICAL_SECTION	g_lock;

// Format from StartStreaming, read by the window for its status line
static volatile BOOL	g_started;
static volatile ePixFmt	g_fmt;
static volatile int		g_frameW, g_frameH;

static unsigned short	g_work[LVL_ROWS][MAX_COLS];		// streaming thread only
static unsigned short	g_shown[LVL_ROWS][MAX_COLS];	// guarded by g_lock
static int				g_cols;							// guarded by g_lock


////////////////////////////////////////////////////////////////////////////////
// Waveform
////////////////////////////////////////////////////////////////////////////////

// Count luma samples per (column, level). YUY2 is Y0 Cb Y1 Cr, so Y is every even byte.
// Row 0 is code 254 (top of the scope).
static void Accumulate(const BYTE *buf, int w, int h, int pitch, int cols, unsigned short (*wave)[MAX_COLS])
{
	memset(wave, 0, sizeof(unsigned short) * LVL_ROWS * MAX_COLS);

	for (int y = 0; y < h; y++)
	{
		const BYTE *line = buf + y * pitch;
		for (int x = 0; x < w; x++)
		{
			int v = line[x * 2];
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
			unsigned c = g_shown[i][x] * TRACE_GAIN;
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

	char status[64];
	if (!g_started)
		strcpy(status, "No video");
	else if (g_fmt != fYUY2)
		sprintf(status, "%dx%d RGB24: needs YUY2 input", g_frameW, g_frameH);
	else
		sprintf(status, "%dx%d YUY2", g_frameW, g_frameH);
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

static void SavePos(HWND hDlg)
{
	char ini[MAX_PATH], num[16];
	RECT r;
	if (!GetIniFile(ini) || !GetWindowRect(hDlg, &r))
		return;
	sprintf(num, "%ld", r.left);
	WritePrivateProfileStringA("DIALOG", "DlgWndPosX", num, ini);
	sprintf(num, "%ld", r.top);
	WritePrivateProfileStringA("DIALOG", "DlgWndPosY", num, ini);
}

static INT_PTR CALLBACK DialogProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
	case WM_INITDIALOG:
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
		break;

	case WM_DESTROY:
		g_hScope = NULL;
		SavePos(hDlg);
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
	if (!scope || !pPr || !pPr->SrcBuff[0] || g_fmt != fYUY2 || pPr->FrameWidth <= 0)
		return TRUE;

	int cols = pPr->FrameWidth / 2;		// one column per YUY2 pixel pair
	if (cols > MAX_COLS) cols = MAX_COLS;

	Accumulate(pPr->SrcBuff[0], pPr->FrameWidth, pPr->FrameHeight, pPr->SrcPitch, cols, g_work);

	EnterCriticalSection(&g_lock);
	memcpy(g_shown, g_work, sizeof(g_shown));
	g_cols = cols;
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
	(char *) "Luma waveform, levels 1-254, limited range 16/235 marked (YUY2)",
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
	Accumulate(&frame[0][0], W, H, PITCH, W / 2, wave);

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

	assert(LevelToY(LVL_MAX, LVL_ROWS) == 0 && LevelToY(LVL_MIN, LVL_ROWS) == LVL_ROWS - 1);
	assert(LevelToY(LIM_WHITE, 2 * LVL_ROWS) == 39);		// 2 px per level: row 19, centre pixel 39

	puts("waveform_test OK");
	return 0;
}
#endif
