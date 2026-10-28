# BPP_Vectorscope: video vectorscope plugin

Processing plugin (`BPP_*.dll`) for BeholdTV 5.28 that plots Cb/Cr of each YUY2 pixel pair. Window, settings and build follow [../BPP_Waveform](../BPP_Waveform/README.md). API and gcc build steps: [../../PLUGIN_SDK.md](../../PLUGIN_SDK.md).

Last updated 2026-10-28.

---

## Build, install, use

| File | What it is |
|---|---|
| `BPP_Vectorscope.cpp` | The plugin, plus a `-DVECTORSCOPE_TEST` self-test |
| `BPP_Vectorscope.rc`, `resource.h` | The window |
| `build.sh` | Runs the self-test, then builds `BPP_Vectorscope.dll`. Set `GCC_BIN` for another gcc |

1. `sh build.sh`. `./vectorscope_test.exe out.bmp` also renders synthetic 75 % bars onto the scope, to check the graticule without BeholdTV.
2. Close BeholdTV, copy `BPP_Vectorscope.dll` into `C:\Program Files (x86)\BeholdTV\Plugins\` (admin).
3. Start BeholdTV, add "Vectorscope" to the **Preview** chain, restart BeholdTV.
4. Press the plugin's settings button: the window opens beside the video. Close with Close, Esc or the X; position and line setting go to `%APPDATA%\BeholdTV\Plugins\BPP_Vectorscope.ini` (`[DIALOG] DlgWndPosX/Y`, `[GLOBAL] AllLines`, `Line`).

What it shows:
- One point per YUY2 pixel pair: Cb across (right = +B−Y), Cr up (+R−Y), centre = 128/128 (no colour). Brighter green = more samples; brightness follows the square root of the count so both single points and dense areas show.
- Dotted grey square: the 16..240 colour-difference range.
- Target boxes, ±3° and ±5 % around each bar's vector: solid for 75 % bars (BT.471 100/0/75/0, the EBU bars on PAL), dotted and labelled for 100 % bars. Targets are exact BT.601 values (75 % red = Cb 100, Cr 212).
- Orange dotted line at 123°: the skin-tone line.
- **All lines / Line N**: whole frame or one frame line, as in the waveform.
- The video is never changed. While the window is closed the plugin returns at once.
- RGB24 input shows a notice and no trace.

With EBU bars on the input, the dots should sit in the solid boxes. Dots rotated off the boxes = hue error; dots inside or outside along the vector = saturation (gain) error, i.e. the picture controls or decoder aren't at unity.

---

## References

No SMPTE or ITU standard defines the vectorscope itself or its graticule: target boxes and their tolerances (e.g. ±3° / ±5 %) come from scope makers. The standards define the coding and the test signal.

| File | What it gives the vectorscope |
|---|---|
| [../BPP_Waveform/ref/st0125-2013.pdf](../BPP_Waveform/ref/st0125-2013.pdf) | SMPTE ST 125:2013. 8-bit Cb/Cr: 128 = zero, 16/240 = ±350 mV (see the waveform README §2) |
| [ref/R-REC-BT.471-1-198607.pdf](ref/R-REC-BT.471-1-198607.pdf) | ITU-R BT.471-1, colour bar nomenclature. 100/0/75/0 = type (b), the EBU bars on PAL |
| [ref/ebu-n010-1998.pdf](ref/ebu-n010-1998.pdf) | EBU Tech N10-1998, analogue component levels: Y 0–700 mV, E'CB/E'CR ±350 mV, Y' = 0.299 R' + 0.587 G' + 0.114 B' |
| [ref/rp0219-1-2014.pdf](ref/rp0219-1-2014.pdf) | SMPTE RP 219-1:2014, HD/SD bars with 10-bit code tables. These are BT.709 (HD) values: not SD targets, use for context only |

Sources: https://www.itu.int/rec/R-REC-BT.471-1-198607-I/en, https://tech.ebu.ch (N10), https://pub.smpte.org/doc/rp219-1/. SMPTE EG 1-1990 (NTSC composite bars, −I/+Q) had no free copy.

Graticule targets for SD therefore come from BT.601 coding of the bar colours (`BarTarget`): Cb = 128 + 224 (−0.168736 R′ − 0.331264 G′ + 0.5 B′), Cr = 128 + 224 (0.5 R′ − 0.418688 G′ − 0.081312 B′), with R′G′B′ = 0.75 or 1.0. This is computed in floating point rather than with BPP_Waveform's integer `RgbToYuv`, which rounds 75 % yellow's Cr to 141 instead of 142.
