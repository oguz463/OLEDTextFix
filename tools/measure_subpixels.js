// Measure subpixel positions from a macro photo of an RGB-triad (QD-OLED) panel.
//
// Usage: open the photo by itself in a browser tab, open the developer console (F12),
// paste this whole file and press Enter. The photo should show a white (or mostly white) area
// so the subpixels are lit; dark text on white is fine.
//
// Optional: to measure only part of a larger image (e.g. one tile of a comparison chart), set
// REGION below to [x0, y0, x1, y1] in the image's own pixel coordinates before running.
//
// It prints the red and blue subpixel centres relative to green, in units of the pixel pitch,
// as values for the OLEDTextFix sliders (Green X/Y stay 0). It assumes a triad with green in one
// row and red + blue in the row below it (all QD-OLED panels up to 2025).

const REGION = null; // e.g. [593, 144, 830, 333]

// Photos of matte (anti-glare) screens look grainy, which splits subpixels into fragments. If the
// result looks inconsistent (e.g. the two pitch values differ a lot), set this to 4-8: the photo is
// shrunk and slightly blurred before measuring. Results are in pixel pitch, so scale does not matter.
const DOWNSCALE = 1;

(async () => {
  const img = document.querySelector("img");
  await img.decode();
  const f = Math.max(1, DOWNSCALE);
  const [rx0, ry0, rx1, ry1] = REGION || [0, 0, img.naturalWidth, img.naturalHeight];
  const W = Math.round((rx1 - rx0) / f), H = Math.round((ry1 - ry0) / f);
  const canvas = document.createElement("canvas");
  canvas.width = W; canvas.height = H;
  const ctx = canvas.getContext("2d");
  ctx.imageSmoothingQuality = "high";
  if (f > 1) ctx.filter = "blur(1px)";
  ctx.drawImage(img, rx0, ry0, rx1 - rx0, ry1 - ry0, 0, 0, W, H);
  const d = ctx.getImageData(0, 0, W, H).data;

  // 1. Classify bright pixels by dominant channel.
  const cls = new Int8Array(W * H).fill(-1);
  for (let i = 0; i < W * H; i++) {
    const r = d[i * 4], g = d[i * 4 + 1], b = d[i * 4 + 2], m = Math.max(r, g, b);
    if (m < 60) continue;
    // modest ratios: over-exposed subpixel centres turn pinkish/whitish
    if (r === m && r > g * 1.15 && r > b * 1.15) cls[i] = 0;
    else if (g === m && g > r * 1.15 && g > b * 1.15) cls[i] = 1;
    else if (b === m && b > r * 1.1 && b > g * 1.1) cls[i] = 2;
  }

  // 2. Connected blobs per colour, intensity-weighted centroids (skip blobs touching the border).
  const seen = new Uint8Array(W * H), blobs = [], stack = [];
  for (let s = 0; s < W * H; s++) {
    if (cls[s] < 0 || seen[s]) continue;
    const c = cls[s];
    let sx = 0, sy = 0, sw = 0, n = 0, edge = false;
    stack.push(s); seen[s] = 1;
    while (stack.length) {
      const p = stack.pop(), x = p % W, y = (p / W) | 0, w = d[p * 4 + c];
      sx += x * w; sy += y * w; sw += w; n++;
      if (x <= 1 || y <= 1 || x >= W - 2 || y >= H - 2) edge = true;
      for (const q of [p - 1, p + 1, p - W, p + W]) {
        if (q < 0 || q >= W * H || seen[q] || cls[q] !== c || Math.abs((q % W) - x) > 1) continue;
        seen[q] = 1; stack.push(q);
      }
    }
    if (n >= 4 && !edge) blobs.push({ c, x: sx / sw, y: sy / sw, n });
  }
  const median = a => { const s = [...a].sort((p, q) => p - q); return s[s.length >> 1]; };
  // Drop specks much smaller than a typical subpixel (noise, JPEG artefacts).
  const typical = median(blobs.map(b => b.n));
  const keep = blobs.filter(b => b.n >= typical * 0.25);
  const R = keep.filter(b => b.c === 0), G = keep.filter(b => b.c === 1), B = keep.filter(b => b.c === 2);

  // 3. Pixel pitch from the green lattice (nearest green to the right / below).
  const dxs = [], dys = [];
  for (const g of G) {
    let bx = Infinity, by = Infinity;
    for (const h of G) {
      const dx = h.x - g.x, dy = h.y - g.y;
      if (dx > 2 && Math.abs(dy) < 0.3 * dx) bx = Math.min(bx, dx);
      if (dy > 2 && Math.abs(dx) < 0.3 * dy) by = Math.min(by, dy);
    }
    if (isFinite(bx)) dxs.push(bx);
    if (isFinite(by)) dys.push(by);
  }
  if (!dxs.length || !dys.length) { console.log("Could not find a regular grid of green subpixels."); return; }
  const px = median(dxs), py = median(dys);

  // 4. For each green, the red and blue in the row below (same triad).
  const rel = (arr, g) => {
    let best = null, bd = Infinity;
    for (const b of arr) {
      const dx = (b.x - g.x) / px, dy = (b.y - g.y) / py;
      if (dy < 0.25 || dy > 0.75 || Math.abs(dx) > 0.6) continue;
      const dd = dx * dx + dy * dy;
      if (dd < bd) { bd = dd; best = [dx, dy]; }
    }
    return best;
  };
  const rs = [], bs = [];
  for (const g of G) {
    const r = rel(R, g), b = rel(B, g);
    if (r && b && Math.sign(r[0]) !== Math.sign(b[0])) { rs.push(r); bs.push(b); }
  }
  if (!rs.length) { console.log("Could not find complete triads in this image."); return; }
  const out = {
    pitch_px: [+px.toFixed(1), +py.toFixed(1)],
    triads: rs.length,
    red: [median(rs.map(v => v[0])), median(rs.map(v => v[1]))].map(v => +v.toFixed(2)),
    blue: [median(bs.map(v => v[0])), median(bs.map(v => v[1]))].map(v => +v.toFixed(2)),
  };
  console.log("OLEDTextFix slider values (Green X = 0, Green Y = 0):");
  console.log(`  Red  X = ${out.red[0]}   Red  Y = ${out.red[1]}`);
  console.log(`  Blue X = ${out.blue[0]}   Blue Y = ${out.blue[1]}`);
  if (out.triads < 20) console.log("  (few triads found — use a larger or sharper photo for a reliable result)");
  if (Math.abs(px - py) > 0.05 * Math.max(px, py))
    console.log("  (horizontal and vertical pitch differ — for a matte screen photo try DOWNSCALE = 6)");
  console.log(out);
  return out;
})();
