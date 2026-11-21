// Measure subpixel column positions from a macro photo of a vertical-stripe panel (LG WOLED RWBG /
// RGWB, or an RGB-stripe panel).
//
// Usage: open the photo by itself in a browser tab, open the developer console (F12), paste this
// whole file and press Enter. The photo should show a lit white area.
//
// Method: the pixel pitch comes from the white (all-channel) brightness. Each image column is then
// folded onto one pixel and three "colour scores" are averaged per position: how much red exceeds
// green and blue, and likewise for green and blue. Each colour column shows up as a bump; its position
// is printed relative to the white column, in pixel pitch, ready for the X sliders (all Y = 0).
//
// Read the result with the printed profiles: camera lenses add a thin coloured halo on BOTH sides of
// the bright white column. A bump that has a mirror-image twin on the other side of the white column
// is that halo, not a subpixel. Matte screens and blurry or dark photos may show no colour columns at
// all; then the photo cannot be measured. The red column is usually the clearest. A value near +0.5 or
// -0.5 is the same place (half a pixel from white on either side): use the sign the column order
// implies (RGWB: red -0.50).

(async () => {
  const img = document.querySelector("img");
  await img.decode();
  const W = img.naturalWidth, H = img.naturalHeight;
  const cv = document.createElement("canvas");
  cv.width = W; cv.height = H;
  const ctx = cv.getContext("2d");
  ctx.drawImage(img, 0, 0);
  const d = ctx.getImageData(0, 0, W, H).data;
  const lin = new Float32Array(256);
  for (let v = 0; v < 256; v++) { const x = v / 255; lin[v] = x <= 0.04045 ? x / 12.92 : ((x + 0.055) / 1.055) ** 2.4; }

  // Per image column: colour scores and white (= the part all three channels share).
  const S = [0, 1, 2, 3].map(() => new Float64Array(W));
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    const i = (y * W + x) * 4, r = lin[d[i]], g = lin[d[i + 1]], b = lin[d[i + 2]];
    S[0][x] += Math.max(0, r - Math.max(g, b));
    S[1][x] += Math.max(0, g - Math.max(r, b));
    S[2][x] += Math.max(0, b - Math.max(r, g));
    S[3][x] += Math.min(r, g, b);
  }

  // Pixel pitch: strongest period of the white profile (one white column per pixel).
  const power = (p, per) => { let re = 0, im = 0; for (let x = 0; x < W; x++) { const a = 2 * Math.PI * x / per; re += p[x] * Math.cos(a); im += p[x] * Math.sin(a); } return re * re + im * im; };
  let best = 0, P = 0;
  for (let p = 8; p <= W / 4; p += 0.25) { const v = power(S[3], p); if (v > best) { best = v; P = p; } }
  for (let p = P - 0.5; p <= P + 0.5; p += 0.01) { const v = power(S[3], p); if (v >= best) { best = v; P = p; } }

  // Fold onto one pixel (48 bins), white peak moved to the centre (bin 24).
  const bins = 48, F = S.map(() => new Float64Array(bins)), C = new Float64Array(bins);
  for (let x = 0; x < W; x++) { const k = Math.floor((((x / P) % 1) + 1) % 1 * bins); C[k]++; for (let c = 0; c < 4; c++) F[c][k] += S[c][x]; }
  const f = F.map(a => [...a].map((v, i) => v / Math.max(1, C[i])));
  const kW = f[3].indexOf(Math.max(...f[3]));
  const rot = a => a.map((_, i) => a[(i + kW - bins / 2 + bins) % bins]);
  const prof = f.map(rot);
  // Sub-bin position of each colour's strongest bump (parabolic peak), relative to the white peak.
  const peak = a => {
    let k = 0; a.forEach((v, i) => { if (v > a[k]) k = i; });
    const l = a[(k - 1 + bins) % bins], r = a[(k + 1) % bins], c = a[k];
    const den = l - 2 * c + r, dk = den ? (l - r) / (2 * den) : 0;
    let v = (k + dk - bins / 2) / bins; v -= Math.round(v);
    return +v.toFixed(2);
  };
  const show = a => { const mn = Math.min(...a), mx = Math.max(...a); return a.map(v => Math.round(9 * (v - mn) / (mx - mn + 1e-12))).join(""); };
  const rel = { R: peak(prof[0]), G: peak(prof[1]), B: peak(prof[2]) };
  console.log(`Pixel pitch: ${P.toFixed(1)} image pixels`);
  console.log("Strongest colour bumps relative to the white column (pixel pitch):", rel);
  console.log("Profiles over one pixel (white column in the middle, '|' marks it); check for mirror-image halos:");
  const mark = s => s.slice(0, 24) + "|" + s.slice(24);
  console.log("  R  " + mark(show(prof[0])));
  console.log("  G  " + mark(show(prof[1])));
  console.log("  B  " + mark(show(prof[2])));
  console.log("  W  " + mark(show(prof[3])));
  return { pitch_px: +P.toFixed(1), rel };
})();
