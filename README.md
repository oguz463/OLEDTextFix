# OLEDTextFix

[![build](https://github.com/oguz463/OLEDTextFix/actions/workflows/build.yml/badge.svg)](https://github.com/oguz463/OLEDTextFix/actions/workflows/build.yml)

OLEDTextFix reduces the colour fringing around text on OLED monitors on Windows. It works on the
final desktop image, so it applies to every application.

- **QD-OLED** (Samsung Display, triangle subpixels): presets measured from macro photos for every
  common panel size and generation.
- **WOLED** (LG Display, RWBG / RGWB columns): presets measured from macro photos; not yet tested
  on a real panel.
- **RGB-stripe OLED** (2026 "V-Stripe" QD-OLED and Tandem RGB WOLED): already matches what Windows
  expects, so no correction is needed.

> **Experimental.** If anything goes wrong, `Ctrl+Alt+F` turns the effect off instantly. Please
> report problems in [Issues](https://github.com/oguz463/OLEDTextFix/issues) and attach
> `OLEDTextFix.log`.

## Download

Get the latest build from [Releases](https://github.com/oguz463/OLEDTextFix/releases), extract the
zip and run `OLEDTextFix.exe`. Releases are built automatically by GitHub Actions from this
repository. The executable is not code-signed, so Windows SmartScreen may show a warning the first
time; choose "More info" → "Run anyway", or right-click the file → Properties → **Unblock**.

Requirements: Windows 10 version 2004 or later, or Windows 11; a DirectX 11 GPU. HDR is
**experimental** (see [HDR and Auto Color Management](#hdr-and-auto-color-management)).

## Why OLED text has colour fringes

Windows renders text for an LCD-style RGB stripe: red, green and blue side by side. Many OLED
panels arrange their subpixels differently:

- **QD-OLED** places them in a triangle: green on top, red and blue below. Light text on a dark
  background gets a green fringe along the top edge and a magenta fringe along the bottom.
- **WOLED** uses four vertical columns including a white one, in the order R W B G (older panels)
  or R G W B (newer panels), which gives red/green fringes beside vertical strokes.

## How it works

OLEDTextFix does not hook text rendering like MacType. Instead it:

1. captures the monitor with **DXGI Desktop Duplication**;
2. resamples each colour channel on the GPU at **that colour's physical subpixel position**;
3. shows the result in a click-through, topmost overlay that is excluded from screenshots and
   screen recordings.

## Quick start

1. Put `OLEDTextFix.exe` in a permanent folder (e.g. `C:\Tools\OLEDTextFix\`). Its settings
   (`OLEDTextFix.ini`) and log (`OLEDTextFix.log`) are written there.
2. Run it. On first run it picks a preset from the monitor's resolution, refresh rate, size and
   manufacturer, and opens the **Settings** window.
3. Check that the chosen preset matches your panel (see [Presets](#presets)) and compare with the
   **Compare** checkbox.

Later, open Settings by clicking the tray icon or pressing `Ctrl+Alt+C`.

## Settings window

Settings are on the left and a **live test pattern** is on the right (dark and light backgrounds,
grayscale and ClearType text), showing the corrected image as you move the sliders.

- **Effect enabled** (`Ctrl+Alt+F`) and **Compare** (temporarily off, for before/after).
- **Process in linear light**: the physically correct method; keep it on.
- **ClearType-compatible mode**: see [ClearType](#cleartype). The line underneath shows the
  current Windows ClearType setting.
- **Remove ClearType colour fringes (WOLED, experimental)**: see
  [ClearType colour removal](#cleartype-colour-removal-experimental-120).
- **Pause for fullscreen games/video** (including borderless fullscreen) and **Start with Windows**.
- **Monitor**: which display to correct.
- **Preset**, plus **Save as preset** / **Delete** for your own presets.
- **Red/Green/Blue X-Y**: each subpixel's position in pixels (Y positive downwards).
- **Strength** (1.00 = full correction) and **Filter** (see [Filters](#filters)).
- Performance options at the bottom (see [Performance](#performance)).

Your own presets are stored as `[Preset:Name]` sections in `OLEDTextFix.ini`, so the file can be
copied to another computer.

### Fine-tuning

1. Look at the text and the thin horizontal lines on the dark half of the test pattern.
2. If the top edge is still **green**, move *Red Y* and *Blue Y* slightly more positive; if the
   bottom edge turns green instead, move them back towards zero.
3. If **red/blue** appears beside vertical lines, adjust the *X* values. If it gets worse, swap the
   signs of *Red X* and *Blue X*.
4. If text looks soft, lower **Strength** or use the Sharp filter. If fringing remains, try the
   Balanced filter.

## Presets

### QD-OLED

Measured with `tools/measure_subpixels.js` from macro photos (RTINGS, Notebookcheck, PC Monitors,
TFTCentral). Each preset is the median of all photos of its class; photos within a class agree to
about ±0.02 pixel. Positions are relative to green, in pixels; green stays fixed because it
carries most of the brightness, which keeps text sharp.

| Preset | Red (X, Y) | Blue (X, Y) |
|---|---|---|
| 27" 1440p 240/280 Hz | −0.22, +0.50 | +0.27, +0.52 |
| 27" 1440p 360 Hz (Gen 3, 2024) | −0.22, +0.50 | +0.27, +0.52 |
| 27" 1440p 500 Hz (2025) | −0.22, +0.50 | +0.27, +0.52 |
| 27" 4K 240 Hz (Gen 4, 2025) | −0.17, +0.50 | +0.32, +0.50 |
| 32" 4K 165 Hz | −0.13, +0.50 | +0.37, +0.50 |
| 32" 4K 240 Hz (Gen 3, 2024) | −0.13, +0.50 | +0.37, +0.50 |
| 34" 3440x1440 165/175 Hz (Gen 1) | −0.31, +0.49 | +0.22, +0.49 |
| 34" 3440x1440 240 Hz (Gen 2) | −0.31, +0.49 | +0.22, +0.49 |
| 49" 5120x1440 144/240 Hz (Gen 2) | −0.18, +0.50 | +0.32, +0.50 |

Also included: *Any QD-OLED triad — vertical only* and *QD-OLED legacy estimate*.

### WOLED

Strength 0.20; positions relative to the white column, measured with `tools/measure_woled.js`
from RTINGS macro photos of 13 monitors.

| Preset | Order | Red | Green | Blue |
|---|---|---|---|---|
| 27" 1440p 240 Hz | R W B G | −0.32 | +0.47 | +0.20 |
| 34" / 39" / 45" 3440x1440 240 Hz | R W B G | −0.32 | +0.47 | +0.20 |
| 27" 1440p 480 Hz | R G W B | −0.50 | −0.25 | +0.22 |
| 32" 4K 240 Hz | R G W B | −0.50 | −0.25 | +0.22 |
| 45" / 39" 5120x2160 | R G W B | −0.50 | −0.25 | +0.22 |

*Any WOLED — RWBG* and *Any WOLED — RGWB* cover other models. Every readable photo confirmed its
panel's column order; red is the most reliably measured column, green and blue rest on fewer
photos. See [WOLED notes](#woled-notes) before using these.

### RGB stripe

*RGB stripe — no correction* is for panels that already use an RGB stripe (e.g. ASUS PG34WCDN,
MSI MPG 341CQR X36, ASUS PG27UCWM / PG32UCWM). Normal ClearType works well on them.

### Automatic choice on first run

The preset is picked from resolution, refresh rate, physical size and the monitor's manufacturer
code (LG-branded OLED monitors use WOLED; 480 Hz at 1440p is WOLED; 39" and 45" ultrawides are
WOLED). Other brands' WOLED monitors with the same size and refresh rate as a QD-OLED (e.g. ASUS
PG27AQDM, PG27AQDP, PG27AQWP, PG32UCDP) need the preset chosen by hand. The app shows a notification with
the auto-picked preset on those brands so you can check it.

## Filters

The filter decides how each colour is resampled when it is moved. Simulated on a 27" 1440p
QD-OLED at strength 1.00, dark background (`tools/sim_filters.py`; positive = better than no
correction):

| Filter | Less fringing | Sharpness |
|---|---|---|
| **Sharp** (Catmull-Rom, default) | 58% | +54% |
| Balanced (bicubic B 0.2, C 0.6) | 67% | +21% |
| Smooth (bicubic B 0.3, C 0.5) | 71% | ±0% |

These three are the best trade-offs found between fringing and sharpness; a sweep of the bicubic
family, Lanczos-2/3/4, windowed-sinc and Gaussian filters found nothing better. On a light
background all three reduce fringing by a similar 17–18%. **On WOLED keep Sharp**: the other two
blur text there.

### Strength on QD-OLED

`tools/sim_qd_strength.py` sweeps strength from 0.5 to 1.2 on every measured QD-OLED class. In the
simulation, fringing reduction peaks at 1.00–1.10 and falls off above that (overshoot), so 1.00 is
the most accurate setting. It is also the one that softens text the most, because the red and blue
channels are moved by about half a pixel vertically. The presets use **0.90** (since 1.2.0, before
that 0.80): in a second simulation with a perceptual colour metric it gave 10–12% less colour error
than 0.80 on every QD-OLED class, while 1.00 gained only 2–5% more and made the red and blue channels
slightly softer. Raise **Strength** towards 1.00 if colour fringes bother you more than softness;
lower it (0.80) if text still looks soft. Adding an unsharp mask on top did not help in simulation:
whatever restores crispness also restores the fringing. Settings you have already saved are not changed.

## ClearType

ClearType can stay on, but then turn on **ClearType-compatible mode**: ClearType already shifts
red and blue slightly, and compatible mode applies only the remaining correction. Without it the
horizontal shift is applied twice.

Grayscale antialiasing gives slightly better results (simulated colour error 0.013 vs 0.014,
sharper text), and modern Windows UI always uses grayscale anyway. To switch, select "Grayscale" in
[Better ClearType Tuner](https://github.com/bp2008/BetterCleartypeTuner) and turn compatible mode
off.

## WOLED notes

On WOLED panels the white subpixel is driven by the panel itself from the incoming RGB value.
Shifting colours adds colour to pixels, so the panel lights less white there; too much correction
therefore increases fringing. The presets keep the white column as the reference and use strength
0.20, which reduced fringing (about 28–30% on average) with grayscale text and kept text sharp in
every simulated case (`tools/sim_woled.py`). The best strength depends on how much of the white a
panel's own RGB-to-RGBW conversion puts into the white subpixel, which nobody has measured; in
simulation it ranged from about 0.15 to 0.40, so 0.20 is a safe default rather than an optimum.

1. Use grayscale antialiasing and turn ClearType-compatible mode off. This is the biggest single
   improvement on WOLED.
2. Pick the preset for your panel and keep the Sharp filter.
3. Check with **Compare**; try strengths between 0.10 and 0.30.

Expect a modest improvement at best, and **almost none with ClearType text**: the colour you see
around ClearType text on a WOLED is mostly ClearType's own, and shifting channels by about 0.1 px
barely changes it (simulated colour error reduced by only 2–10%). Reports from WOLED users are very
welcome.

### Luma shift (experimental)

The two **Luma** sliders (WOLED presets only, greyed out otherwise) move all three channels together, in
pixels, so they shift the brightness of the text without changing the colour correction. They are meant for
panels where the white subpixel pulls the brightness to one side of the letters (a soft "shadow" on one edge).
QD-OLED has no white subpixel and does not need them. They do not remove such an effect by themselves, and the
right values depend on the panel, so tune them by eye with the Compare option. Fractional values soften the
text slightly. Both are 0 by default, are not stored in presets and are reset when a preset is applied. INI keys: `LumShiftX`,
`LumShiftY` (`[Filter]`, range -1 to 1).

### ClearType colour removal (experimental, 1.2.0)

**Remove ClearType colour fringes (WOLED, experimental)** in Settings adds a first pass that removes
the colour ClearType puts on grey, white and black text before the normal correction runs. A pixel is
only touched when three tests agree that it is ClearType text: the colour cancels along the row, the
neighbourhood has text-like brightness contrast, and the signal at subpixel rate is smooth. Each
touched pixel keeps its exact brightness and loses its colour. Coloured text (links, syntax
highlighting), icons, photos and video are left alone.

- It is **off by default**, only acts with a WOLED preset and 8-bit desktop capture, and does nothing
  when Windows ClearType is off (the status line under the checkbox says which). It does not apply
  to QD-OLED: ClearType's red/blue placement already matches that layout, so removing the colour
  gives no gain there and softens text.
- **What was measured (simulation only):** on neutral ClearType text it removed 38–85% of the colour
  error compared with 1.1, depending on the assumed white-subpixel share, which is about what
  switching Windows to grayscale text gives. In simulated coloured UI cards 0.0% of pixels changed,
  in four real photos at most 0.03%, and text became up to about 4% softer.
- **What was not measured:** anything on a real monitor, real ClearType output (DirectWrite, GDI,
  Chrome/Skia at different Tuner settings: the thresholds come from an idealised filter), HDR and
  10-bit desktops (the pass is skipped there), and GPU cost (one extra full-screen FP16 pass; about
  66 MB at 4K). Run `python3 tools/ct_gate_report.py screenshot.png` on a lossless Win+PrtScn
  capture to see how many pixels it would change.
- Grayscale text everywhere is still better where it is available, because it is also crisper.
- The thresholds can be changed in `OLEDTextFix.ini` (`[Filter]`, keys starting with `CTCancel`).

## Performance

Options at the bottom of the Settings window:

- **Processing GPU**: Automatic uses the GPU the monitor is connected to. Another GPU can be
  selected (experimental); capture and presentation still happen on the monitor's GPU and changed
  regions are copied through the CPU, which can add latency while scrolling.
- **Frame cap**: limits processed frames per second while something is changing.
- **Show moving regions without latency** (default on): 64×64 tiles that keep changing (scrolling,
  video, dragged windows) are left transparent, so the real desktop shows through with no added
  delay; they are corrected again once still (250 ms). Tunable in the INI file with
  `MotionFrames`, `MotionWindowMs` and `MotionSettleMs`.
- **Identical frames are skipped** (since 1.2.1): Windows sometimes reports the whole screen as changed
  although no pixel is different (for example after the overlay itself was presented). Such updates are
  compared on the GPU and ignored, so a still desktop costs no GPU time and is no longer treated as
  "moving". `SkipUnchanged=0` in the `[Performance]` section of the INI file turns this off.
- **Fully opaque overlay** (default on): lets Windows show the overlay pixel for pixel. Turn it off
  if an application stops updating underneath or the screen flickers.

The status line shows **processed / captured** frames per second and the GPU in use.

**Higher idle power?** Watch the status line with the screen completely still. If the captured
count is high while nothing visibly changes, something on the desktop (an animation, a blinking
cursor, a video) keeps updating the screen; every update is processed. If all counts are 0
and power is still higher, the GPU may be keeping its memory clock up because of the fullscreen
overlay; try toggling "Fully opaque overlay" or another Processing GPU, and report the result with
`OLEDTextFix.log`.

### HDR and Auto Color Management

When HDR or Windows 11's **Auto Color Management** (Settings → Display → Advanced display →
"Automatically manage color for apps") is on, Windows composes the desktop in a 16-bit floating-point
format (scRGB) instead of 8-bit. The app captures that format directly, resamples it in linear light
and shows the result in a matching overlay, so the colours are left unchanged.

- **Auto Color Management** is handled automatically. If the status line says the format is
  unsupported, send the log (`OLEDTextFix.log`) with the line "Unsupported desktop format".
- **HDR** is experimental: tick *Experimental: process HDR desktops* in Settings. It is off by
  default and, if it cannot capture, the app pauses as before. HDR content itself (video, games) is
  processed too, which is unnecessary; only SDR text benefits.
- In these modes the Processing GPU setting is ignored (the monitor's GPU is used) and the
  linear-light option has no effect (the data is already linear).
- Not verified on a wide range of setups yet. Reports with your GPU, monitor and the log are very
  welcome.

## Shortcuts

- `Ctrl+Alt+F`: toggle the effect. Press this first if anything goes wrong.
- `Ctrl+Alt+C`: open Settings.
- Tray icon: click for Settings; right-click lists the presets.

## Updates

About 20 seconds after start, and then once a day, the app asks GitHub for the latest release. If it
is newer, the app downloads `OLEDTextFix.exe` and its `.sha256` from that release over HTTPS,
verifies the hash, and replaces its own exe (the old copy is kept as `OLEDTextFix.exe.old` until the
next start). A notification appears; choose **Restart to finish updating** in the tray menu to use
the new version. Your settings are kept.

- The tray menu has **Check for updates now** and **Check for updates automatically** (on by
  default; stored as `CheckForUpdates` in the INI). Nothing else is sent: the only requests are to
  `api.github.com` and `github.com`.
- If the exe sits in a folder you cannot write to (for example `Program Files`), the menu shows a link
  to the download page instead.
- The hash comes from the same release as the exe, so it catches corrupted downloads, not a
  compromised release.

## Known limitations

- **HDR is experimental** and off by default; with it off the app pauses while HDR is on.
- The corrected image is about **one frame behind** (~4 ms at 240 Hz) while the cursor is not, so
  a dragged window can feel slightly heavy. "Show moving regions without latency" removes this
  wherever something is moving.
- Works best with a **dark theme**: in simulation fringing drops by about 55–70% on dark
  backgrounds and 15–20% on light ones.
- The Start menu, taskbar and notifications sometimes stay above the overlay and are shown
  uncorrected.
- Screenshots capture the uncorrected image (intentional).

## Measuring your panel

If a sharp macro photo of your panel's subpixels is available (a review site's photo, or your own
phone macro photo of a white area):

1. Open the photo by itself in a browser tab and open the developer console (F12).
2. Paste `tools/measure_subpixels.js` (QD-OLED) or `tools/measure_woled.js` (WOLED and other
   vertical stripes) and press Enter.
3. Enter the printed values in the sliders, check with **Compare** and save them as a preset.

`measure_subpixels.js` can measure part of an image (`REGION`) and handle grainy matte-screen photos
(`DOWNSCALE` ≈ 6). `measure_woled.js` prints a text profile of one pixel: lenses add a thin coloured
halo on both sides of the bright white column, so a bump with a mirror-image twin is halo, not a
subpixel. Please share your measurements in [Issues](https://github.com/oguz463/OLEDTextFix/issues).

## Troubleshooting

- **Flicker:** turn "Fully opaque overlay" off (some drivers flicker when Windows moves the overlay
  to a hardware plane), set Processing GPU to Automatic and Frame cap to Unlimited. The "Overlay
  hidden" and "Fullscreen pause" lines in `OLEDTextFix.log` show why the overlay was hidden.
- **"Paused" with HDR on, or the status line mentions the desktop format:** see
  [HDR and Auto Color Management](#hdr-and-auto-color-management). If HDR is off and it still says
  so, turn "Automatically manage color for apps" off and on again and send the log.
- **No visible difference on a WOLED monitor:** check the preset in Settings. On brands other than
  LG the first-run auto-pick cannot tell WOLED from QD-OLED, so it may have chosen a QD-OLED preset
  (the app shows a notification with the choice). Select the matching WOLED preset by hand; WOLED
  corrections are mild (strength 0.20), so toggle **Compare** and look closely.
- **Screen looks frozen or wrong:** press `Ctrl+Alt+F` and check `OLEDTextFix.log`.
- **Image gets progressively blurrier:** the overlay is capturing itself; this happens on Windows
  older than 10 version 2004.
- **High frame counts while the screen is static:** please report it; some drivers report the
  overlay's own drawing as a change.

## Building from source

- **Visual Studio:** open an "x64 Native Tools Command Prompt" and run `build.bat`.
- **MinGW-w64 (Linux/MSYS2):** `./build.sh`

GitHub Actions builds every push and pull request. A release is published when `main` is pushed
(or the workflow is run manually) and no release exists yet for the version in `VERSION`, or when a
`v*` tag is pushed.

The `tools/` folder holds the simulations behind the numbers above (Python with NumPy, SciPy and
Pillow): `sim.py` (QD-OLED model), `sim_filters.py`, `sim_cleartype.py` and `sim_woled.py`. `tools/ct_gate_report.py` checks the ClearType colour-removal
pass against screenshots.

Tests: `sh tests/run.sh` builds and runs the native tests for the pure logic in `src/core.h` (filter
weights, version comparison, WOLED preset names, the ClearType gate) and, when `glslangValidator` is
installed, syntax-checks the HLSL in `src/main.cpp`. They need no Windows and no GPU.

## License

[MIT](LICENSE)
