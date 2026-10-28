# BPP_Waveform: video waveform monitor plugin

Processing plugin (`BPP_*.dll`) for BeholdTV 5.28 that will draw a luma waveform over the picture. API and gcc build steps: [../../PLUGIN_SDK.md](../../PLUGIN_SDK.md).

Last updated 2026-10-28.

---

## 0. Build, install, use

| File | What it is |
|---|---|
| `BPP_Waveform.cpp` | The plugin, plus a `-DWAVEFORM_TEST` self-test of the sample counting |
| `BPP_Waveform.rc`, `resource.h` | The window (tool window, like BPP_FreqFilter's) |
| `build.sh` | Runs the self-test, then builds `BPP_Waveform.dll`. Set `GCC_BIN` for another gcc |

1. `sh build.sh`
2. Close BeholdTV, copy `BPP_Waveform.dll` into `C:\Program Files (x86)\BeholdTV\Plugins\` (admin).
3. Start BeholdTV, add "Waveform monitor" to the **Preview** chain, restart BeholdTV.
4. Press the plugin's settings button: the waveform window opens and stays open beside the video. Close it with Close, Esc or the X; its position is saved in `%APPDATA%\BeholdTV\Plugins\BPP_Waveform.ini` (`[DIALOG] DlgWndPosX/Y`, same keys as FreqFilter).

What it shows:
- Luma (Y) only, one column per pixel pair (360 columns for 720 wide), brighter green = more samples at that level.
- Vertical scale 1–254 (grey lines). Codes 0 and 255 are drawn on the 1 and 254 lines.
- Limited range 16 and 235: red dotted lines.
- Top right: frame size and format from BeholdTV (`720x576 YUY2`), which also answers §1's open question. With RGB24 input it shows a notice and no trace.
- The video is never changed. While the window is closed the plugin returns at once.

---

## 1. Sample format BeholdTV gives a plugin

**8 bits per component. There is no 10-bit path.**

| Source | What it says |
|---|---|
| SDK `Common.h`, `ePixFmt` | Only two formats exist: `fYUY2` (0) and `fRGB24` |
| SDK `PARAM_STR.BitPerPixel` | "2 if 16 bpp, 3 if 24 bpp", so YUY2 = 8-bit Y + 8-bit Cb/Cr (4:2:2), RGB24 = 8-bit B, G, R |
| `DSPLUGINVER` 0x0003 | Unchanged since 2009; the 5.28 plugins still use the same exports |

Layout, from the YUY2 FourCC: bytes `Y0 Cb Y1 Cr` per pixel pair, top line first. Line stride is `SrcPitch` (= width × 2).

**Not yet confirmed: which of the two formats 5.28 actually sends.** The plugin gets it in `PARAM_STR.PixFmt` at Start, so it must handle both anyway, and should log it the first time it runs. The `BPP_Inverse_diag.dll` build (logs to `%TEMP%\BPP_Inverse.log`) answers this too, but it isn't enabled: `Plugins.ini` [Preview] has `BPP_Inverse.dll`, the normal build.

Nor confirmed: whether the levels are studio range (Y 16–235) or full range (0–255). The SAA713x decoder is expected to output BT.601 studio range, and the picture controls (Brightness 128, Contrast 64, Saturation 64) change it. In RGB24 the range depends on who converts. A waveform of black/white test signals will show this directly.

## 2. Reference levels (SMPTE ST 125:2013)

[ref/st0125-2013.pdf](ref/st0125-2013.pdf): *SDTV Component Video Signal Coding 4:4:4 and 4:2:2 for 13.5 MHz and 18 MHz Systems*, free from the SMPTE public library (https://pub.smpte.org/doc/st125/). It covers 625-line 50-field systems (PAL/SECAM) and defers signal construction to ITU-R BT.601 §2.

ST 125:2013 states levels in 10 bits (Tables 1a/1b, §5.1) and notes 8-bit systems from earlier versions still exist. 8-bit = 10-bit ÷ 4:

| Level | 10-bit | 8-bit | Analog (625, component) |
|---|---|---|---|
| Luma black | 64 | **16** | 0 mV |
| Luma peak white | 940 | **235** | 700 mV |
| Luma steps black→white | 877 | 220 | |
| Colour-difference zero | 512 | **128** | 0 mV |
| Colour-difference extremes | 64 / 960 | **16 / 240** (225 levels) | ±350 mV |
| Usable code range | 4–1019 | **1–254** | |
| Timing reference only | 0–3, 1020–1023 | 0, 255 | |

Coding (Table 1a): Y' = 0.299 R' + 0.587 G' + 0.114 B'.

Graticule for an 8-bit waveform: 16 = 0 %, 235 = 100 %, so 1 % ≈ 2.19 codes. Values below 16 or above 235 are legal footroom/headroom, not errors; 0 and 255 never appear in a valid stream.
