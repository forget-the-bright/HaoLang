import fs from "fs";
import path from "path";

const icons = path.resolve("icons");

// Prefer exact preview fidelity: SVG wraps transparent PNG (true vector paths of 3D jelly are noisy).
const pngPath = path.join(icons, "hao-mascot-256.png");
const png = fs.readFileSync(pngPath);
const b64 = png.toString("base64");
const svg = `<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" viewBox="0 0 256 256" width="256" height="256">
  <title>HaoLang mascot</title>
  <image width="256" height="256" preserveAspectRatio="xMidYMid meet" href="data:image/png;base64,${b64}" xlink:href="data:image/png;base64,${b64}"/>
</svg>
`;
fs.writeFileSync(path.join(icons, "hao-dark.svg"), svg);
fs.writeFileSync(path.join(icons, "hao-light.svg"), svg);

// Also keep PNG language icons as fallback / alternate
fs.copyFileSync(pngPath, path.join(icons, "hao-dark.png"));
fs.copyFileSync(pngPath, path.join(icons, "hao-light.png"));

console.log("wrote hao-dark.svg / hao-light.svg", svg.length, "bytes");

// Clean auto-trace if present: drop full-canvas gray fills
const traced = path.join(icons, "hao-traced.svg");
if (fs.existsSync(traced)) {
  let s = fs.readFileSync(traced, "utf8");
  const before = s.length;
  s = s.replace(/<path[^>]*fill="#F[0-9A-F]{5}"[^>]*transform="translate\(0,0\)"\s*\/>/gi, "");
  s = s.replace(/<path d="M0 0 C337\.92[^"]+"[^>]*\/>/g, "");
  fs.writeFileSync(path.join(icons, "hao-traced-clean.svg"), s);
  console.log("traced clean", before, "->", s.length);
}
