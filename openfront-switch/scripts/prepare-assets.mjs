// Copies OpenFront's own UI art into romfs/, ready for the native renderer:
//   icons/<name>.png   every resources/images/*.svg rasterized (64 px tall),
//                      plus the PNG images as they are
//   flags/<code>.png   every resources/flags/*.svg rasterized (48 px tall)
//   atlases/*          the sprite atlases the web client's renderer uses
//   fonts/Inter-*.ttf  the UI font the web client renders with
//   fonts/DejaVuSansMono*.ttf  its font-mono (player names in the hover card)
//   atlases/ui-emoji.* the emoji the HUD's text uses that OpenFront's atlas lacks
//   thumbnails/<map>.png  the map pictures of the Solo modal's map picker
//
// usage: node scripts/prepare-assets.mjs <openfront-checkout> <inter-dir> <romfs-dir>
import {
  copyFileSync,
  existsSync,
  mkdirSync,
  readdirSync,
  readFileSync,
  writeFileSync,
} from "node:fs";
import { createRequire } from "node:module";
import { basename, dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const require = createRequire(join(here, "..", "tools", "package.json"));
const { Resvg } = require("@resvg/resvg-js");
const sharp = require("sharp");

const [ofArg, interArg, romfsArg] = process.argv.slice(2);
if (!ofArg || !interArg || !romfsArg) {
  console.error(
    "usage: node prepare-assets.mjs <openfront-checkout> <inter-dir> <romfs-dir>",
  );
  process.exit(1);
}
const res = join(resolve(ofArg), "resources");
const romfs = resolve(romfsArg);

function rasterize(svgPath, height) {
  const svg = readFileSync(svgPath, "utf8");
  const r = new Resvg(svg, {
    fitTo: { mode: "height", value: height },
    background: "rgba(0,0,0,0)",
  });
  return r.render().asPng();
}

function rasterizeDir(src, dst, height) {
  mkdirSync(dst, { recursive: true });
  let n = 0;
  const failed = [];
  for (const f of readdirSync(src).sort()) {
    const name = basename(f).replace(/\.[^.]+$/, "");
    try {
      if (f.endsWith(".svg")) {
        writeFileSync(join(dst, name + ".png"), rasterize(join(src, f), height));
        n++;
      } else if (f.endsWith(".png") && !existsSync(join(src, name + ".svg"))) {
        copyFileSync(join(src, f), join(dst, f));
        n++;
      }
    } catch (e) {
      failed.push(`${f}: ${e.message}`);
    }
  }
  return { n, failed };
}

const icons = rasterizeDir(join(res, "images"), join(romfs, "icons"), 64);
console.log(`icons: ${icons.n}`);
for (const f of icons.failed) console.warn(`  skipped ${f}`);

const flags = rasterizeDir(join(res, "flags"), join(romfs, "flags"), 48);
console.log(`flags: ${flags.n}`);
for (const f of flags.failed) console.warn(`  skipped ${f}`);

const atlasOut = join(romfs, "atlases");
mkdirSync(atlasOut, { recursive: true });
for (const f of readdirSync(join(res, "atlases"))) {
  // msdf-atlas is the web renderer's signed-distance font; we draw names
  // with the same Overpass font through FreeType instead.
  if (f.startsWith("msdf")) continue;
  copyFileSync(join(res, "atlases", f), join(atlasOut, f));
}
console.log("atlases: copied");

const fontOut = join(romfs, "fonts");
mkdirSync(fontOut, { recursive: true });
for (const w of ["Regular", "Medium", "SemiBold", "Bold", "ExtraBold"]) {
  copyFileSync(
    join(resolve(interArg), `Inter-${w}.ttf`),
    join(fontOut, `Inter-${w}.ttf`),
  );
}
copyFileSync(join(resolve(interArg), "LICENSE.txt"), join(fontOut, "Inter-LICENSE.txt"));
const dejavu = join(dirname(resolve(interArg)), "dejavu");
for (const f of ["DejaVuSansMono.ttf", "DejaVuSansMono-Bold.ttf"]) {
  copyFileSync(join(dejavu, f), join(fontOut, f));
}
copyFileSync(join(dejavu, "LICENSE"), join(fontOut, "DejaVu-LICENSE.txt"));
console.log("fonts: Inter, DejaVu Sans Mono");

for (const f of ["ui-emoji.png", "ui-emoji.json"]) {
  copyFileSync(join(here, "..", "assets", f), join(atlasOut, f));
}
console.log("ui emoji: copied");

// Map thumbnails (webp in OpenFront) as PNGs at most 320 px wide.
const thumbOut = join(romfs, "thumbnails");
mkdirSync(thumbOut, { recursive: true });
let thumbs = 0;
for (const m of readdirSync(join(res, "maps"))) {
  const src = join(res, "maps", m, "thumbnail.webp");
  if (!existsSync(src)) continue;
  await sharp(src)
    .resize({ width: 320, height: 320, fit: "inside", withoutEnlargement: true })
    .png()
    .toFile(join(thumbOut, m + ".png"));
  thumbs++;
}
console.log(`thumbnails: ${thumbs}`);
