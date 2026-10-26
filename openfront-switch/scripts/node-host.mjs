// Runs the engine bundle under Node with the same `host` API the native side
// provides, and plays a scripted game. A quick check that the bundle, the
// romfs data and the adapter work before building for the Switch.
//
// usage: node scripts/node-host.mjs <engine.js> <romfs-dir> [map-folder] [ticks]
import { existsSync, readFileSync } from "node:fs";
import { join, resolve } from "node:path";
import vm from "node:vm";
import { gunzipSync } from "node:zlib";

const [bundle, romfsArg, mapFolder = "britannia", ticksArg = "600"] =
  process.argv.slice(2);
const romfs = resolve(romfsArg) + "/";

const host = {
  log: (m) => console.log("[js] " + m),
  now: () => performance.now(),
  readFile(path) {
    const p = path.replace(/^romfs:\//, romfs);
    let data = null;
    if (existsSync(p)) data = readFileSync(p);
    else if (existsSync(p + ".gz")) data = gunzipSync(readFileSync(p + ".gz"));
    if (data === null) return null;
    return data.buffer.slice(data.byteOffset, data.byteOffset + data.length);
  },
};

// A bare context, like QuickJS: no Node globals, only what the prelude adds.
const ctx = vm.createContext({ host });
vm.runInContext(readFileSync(bundle, "utf8"), ctx, { filename: "engine.js" });
const OF = ctx.OF;
OF.init("es");

const names = OF.mapNames();
const mapIndex = [...Array(names.length).keys()].find(
  (i) => OF.mapFolder(i) === mapFolder,
);
if (mapIndex === undefined) throw new Error("map not in romfs: " + mapFolder);
console.log(`maps: ${names.length}; playing ${names[mapIndex]}`);

OF.start({
  map: mapIndex,
  difficulty: 1,
  bots: 100,
  nations: -1,
  name: "Switch",
});
for (let i = 0; i < 100 && !OF.ready() && !OF.error(); i++) {
  await new Promise((r) => setTimeout(r, 10));
}
if (!OF.ready()) throw new Error("not ready: " + OF.error());

const w = OF.width();
const h = OF.height();
const terrain = new Uint8Array(OF.terrainBuffer());
const state = new Uint16Array(OF.stateBuffer());
console.log(`map ${w}x${h}, terrain ${terrain.length}, state ${state.length}`);

// Spawn on the land tile nearest the centre.
let spawned = false;
for (let r = 0; r < Math.max(w, h) && !spawned; r += 3) {
  for (let a = 0; a < 16 && !spawned; a++) {
    const x = Math.round(w / 2 + r * Math.cos((a * Math.PI) / 8));
    const y = Math.round(h / 2 + r * Math.sin((a * Math.PI) / 8));
    if (x < 0 || y < 0 || x >= w || y >= h) continue;
    if (terrain[y * w + x] & 0x80 && (state[y * w + x] & 0xfff) === 0) {
      spawned = OF.primary(x, y) === "spawn";
      if (spawned) console.log(`spawn at ${x},${y}`);
    }
  }
}

const ticks = Number(ticksArg);
let tileUpdates = 0;
let worst = 0;
const t0 = performance.now();
for (let i = 0; i < ticks; i++) {
  const s = performance.now();
  OF.tick();
  worst = Math.max(worst, performance.now() - s);
  tileUpdates += new Uint32Array(OF.tileUpdates()).length / 2;
  const hud = OF.hud();
  if (!hud.spawn && hud.alive && i % 20 === 0) {
    // Expand into neutral land, then attack whoever is next door.
    const c = OF.myCenter();
    if (c) {
      for (let k = 0; k < 64; k++) {
        const x = Math.round(c[0] + Math.cos(k) * (10 + k));
        const y = Math.round(c[1] + Math.sin(k) * (10 + k));
        if (OF.primary(x, y)) break;
      }
    }
  }
  for (const m of OF.messages()) console.log(`  msg[${m.kind}] ${m.text}`);
  if (hud.winner) {
    console.log("winner: " + hud.winner);
    break;
  }
}
const elapsed = performance.now() - t0;
const hud = OF.hud();
console.log(
  `${ticks} ticks in ${elapsed.toFixed(0)} ms (avg ${(elapsed / ticks).toFixed(2)} ms, worst ${worst.toFixed(1)} ms)`,
);
console.log(`tile updates: ${tileUpdates}`);
console.log("hud:", JSON.stringify(hud));
console.log("labels:", OF.labels().length / 5, "units:", OF.units().length / 5);
console.log("build menu:", JSON.stringify(OF.buildMenu(...OF.myCenter())));
