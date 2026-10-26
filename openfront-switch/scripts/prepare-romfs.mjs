// Fills romfs/ with what the port reads at runtime, taken from an OpenFront
// checkout: compact map data (gzipped), the translation sections the port
// shows, and the UI font converted from WOFF to TTF.
//
// usage: node scripts/prepare-romfs.mjs <openfront-checkout> <romfs-dir> [map ...]
//
// With map folder names after the romfs dir (e.g. `africa europe`), only those
// maps are copied, which keeps test builds small.
import {
  existsSync,
  mkdirSync,
  readdirSync,
  readFileSync,
  writeFileSync,
} from "node:fs";
import { join, resolve } from "node:path";
import { gzipSync, inflateSync } from "node:zlib";

const [ofArg, romfsArg, ...only] = process.argv.slice(2);
if (!ofArg || !romfsArg) {
  console.error(
    "usage: node prepare-romfs.mjs <openfront-checkout> <romfs-dir> [map ...]",
  );
  process.exit(1);
}
const ofDir = resolve(ofArg);
const romfs = resolve(romfsArg);
const res = join(ofDir, "resources");

// --- Maps -------------------------------------------------------------------

const mapsOut = join(romfs, "maps");
mkdirSync(mapsOut, { recursive: true });
const index = [];
let bytes = 0;
for (const folder of readdirSync(join(res, "maps")).sort()) {
  if (only.length > 0 && !only.includes(folder)) continue;
  const src = join(res, "maps", folder);
  const files = ["manifest.json", "map4x.bin", "map16x.bin"];
  if (!files.every((f) => existsSync(join(src, f)))) continue;
  const dst = join(mapsOut, folder);
  mkdirSync(dst, { recursive: true });
  const manifest = readFileSync(join(src, "manifest.json"));
  writeFileSync(join(dst, "manifest.json"), manifest);
  for (const f of files.slice(1)) {
    const gz = gzipSync(readFileSync(join(src, f)), { level: 9 });
    writeFileSync(join(dst, f + ".gz"), gz);
    bytes += gz.length;
  }
  index.push(folder);
}
writeFileSync(join(mapsOut, "index.json"), JSON.stringify(index));
console.log(
  `maps: ${index.length} (${(bytes / 1024 / 1024).toFixed(1)} MiB gzipped)`,
);

// --- Translations -----------------------------------------------------------

// Only the sections the port shows; the full files are mostly web UI.
const SECTIONS = [
  "events_display",
  "unit_type",
  "heads_up_message",
  "win_modal",
  "map",
  "difficulty",
  "game_settings",
  "player_panel",
  "player_type",
  "leaderboard",
  "build_menu",
  "common",
  "main",
];
const langOut = join(romfs, "lang");
mkdirSync(langOut, { recursive: true });
let langs = 0;
for (const f of readdirSync(join(res, "lang"))) {
  if (!f.endsWith(".json") || f === "metadata.json") continue;
  const all = JSON.parse(readFileSync(join(res, "lang", f), "utf8"));
  const picked = {};
  for (const s of SECTIONS) if (all[s] !== undefined) picked[s] = all[s];
  writeFileSync(join(langOut, f), JSON.stringify(picked));
  langs++;
}
console.log(`languages: ${langs}`);

// --- Font -------------------------------------------------------------------

/** WOFF 1.0 to a plain sfnt (TTF/OTF): inflate each table, rebuild the directory. */
function woffToSfnt(woff) {
  const u32 = (o) => woff.readUInt32BE(o);
  const u16 = (o) => woff.readUInt16BE(o);
  if (u32(0) !== 0x774f4646) throw new Error("not a WOFF file");
  const flavor = u32(4);
  const numTables = u16(12);
  const tables = [];
  for (let i = 0; i < numTables; i++) {
    const e = 44 + i * 20;
    const tag = u32(e);
    const offset = u32(e + 4);
    const compLength = u32(e + 8);
    const origLength = u32(e + 12);
    const checksum = u32(e + 16);
    let data = woff.subarray(offset, offset + compLength);
    if (compLength < origLength) data = inflateSync(data);
    tables.push({ tag, checksum, data });
  }
  let searchRange = 1;
  let entrySelector = 0;
  while (searchRange * 2 <= numTables) {
    searchRange *= 2;
    entrySelector++;
  }
  searchRange *= 16;
  const headerLen = 12 + numTables * 16;
  let total = headerLen;
  for (const t of tables) total += (t.data.length + 3) & ~3;
  const out = Buffer.alloc(total);
  out.writeUInt32BE(flavor, 0);
  out.writeUInt16BE(numTables, 4);
  out.writeUInt16BE(searchRange, 6);
  out.writeUInt16BE(entrySelector, 8);
  out.writeUInt16BE(numTables * 16 - searchRange, 10);
  let off = headerLen;
  tables.forEach((t, i) => {
    const e = 12 + i * 16;
    out.writeUInt32BE(t.tag, e);
    out.writeUInt32BE(t.checksum, e + 4);
    out.writeUInt32BE(off, e + 8);
    out.writeUInt32BE(t.data.length, e + 12);
    t.data.copy(out, off);
    off += (t.data.length + 3) & ~3;
  });
  return out;
}

const fontOut = join(romfs, "fonts");
mkdirSync(fontOut, { recursive: true });
for (const name of ["overpass", "overpass-bold"]) {
  const woff = readFileSync(join(res, "fonts", name + ".woff"));
  writeFileSync(join(fontOut, name + ".ttf"), woffToSfnt(woff));
}
console.log("fonts: overpass, overpass-bold");
