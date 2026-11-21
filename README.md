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

Requirements: Windows 10 version 2004 or later, or Windows 11; a DirectX 11 GPU; HDR off (the
app works in SDR and pauses itself while HDR is on).

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
PG27AQDM, PG32UCDP) need the preset chosen by hand.

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

`tools/sim_qd_strength.py` sweeps strength from 0.5 to 1.2 on every measured QD-OLED class. For all
of them, fringing reduction peaks at 1.00–1.10 and sharpness peaks at 0.90–1.00, with both falling
off above that (overshoot). So **1.00 is the best setting** for every filter, and the presets use it.
Going lower only gives up fringing reduction: at 0.70 it is about 45% instead of 58% with Sharp.

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
0.20, which reduced fringing (about 28–30% on average) and kept text sharp in every simulated case
(`tools/sim_woled.py`); 0.30 and above made bright content worse.

1. Use grayscale antialiasing and turn ClearType-compatible mode off.
2. Pick the preset for your panel and keep the Sharp filter.
3. Check with **Compare**; try strengths between 0.10 and 0.30.

Expect a modest improvement at best. Reports from WOLED users are very welcome.

## Performance

Options at the bottom of the Settings window:

- **Processing GPU**: Automatic uses the GPU the monitor is connected to. Another GPU can be
  selected (experimental); capture and presentation still happen on the monitor's GPU and changed
  regions are copied through the CPU, which can add latency while scrolling.
- **Frame cap**: limits processed frames per second while something is changing.
- **Skip unchanged frames**: only processes frames whose content really changed.
- **Show moving regions without latency** (default on): 64×64 tiles that keep changing (scrolling,
  video, dragged windows) are left transparent, so the real desktop shows through with no added
  delay; they are corrected again once still (250 ms). Tunable in the INI file with
  `MotionFrames`, `MotionWindowMs` and `MotionSettleMs`.
- **Fully opaque overlay** (default on): lets Windows show the overlay pixel for pixel. Turn it off
  if an application stops updating underneath or the screen flickers.

The status line shows **processed / skipped / captured** frames per second and the GPU in use.

**Higher idle power?** Watch the status line with the screen completely still. If the captured
count is high, "Skip unchanged frames" should keep the processed count near 0. If all counts are 0
and power is still higher, the GPU may be keeping its memory clock up because of the fullscreen
overlay; try toggling "Fully opaque overlay" or another Processing GPU, and report the result with
`OLEDTextFix.log`.

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

- **SDR only**; pauses while HDR is on.
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
Pillow): `sim.py` (QD-OLED model), `sim_filters.py`, `sim_cleartype.py` and `sim_woled.py`.

## License

[MIT](LICENSE)
