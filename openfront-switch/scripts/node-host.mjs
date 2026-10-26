// Runs the engine bundle under Node with the same `host` API the native side
// provides, plays a scripted game and exercises the HUD/menu RPCs. A quick
// check that the bundle, the romfs data and the adapter work before building
// for the Switch.
//
// usage: node scripts/node-host.mjs <engine.js> <romfs-dir> [map-folder] [ticks]
import { existsSync, readFileSync } from "node:fs";
import { resolve } from "node:path";
import vm from "node:vm";
import { gunzipSync } from "node:zlib";

const [bundle, romfsArg, mapFolder = "britannia", ticksArg = "600"] = process.argv.slice(2);
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
const call = (m, a) => JSON.parse(OF.call(m, JSON.stringify(a ?? {})));
const assert = (c, msg) => {
  if (!c) {
    console.error("FAIL: " + msg);
    process.exitCode = 1;
  }
};

const names = OF.mapNames();
const mapIndex = [...Array(names.length).keys()].find((i) => OF.mapFolder(i) === mapFolder);
if (mapIndex === undefined) throw new Error("map not in romfs: " + mapFolder);
console.log(`maps: ${names.length}; playing ${names[mapIndex]}`);

OF.start({ map: mapIndex, difficulty: 1, bots: 60, nations: -1, name: "Switch", instantBuild: true, infiniteGold: true });
for (let i = 0; i < 100 && !OF.ready() && !OF.error(); i++) await new Promise((r) => setTimeout(r, 10));
if (!OF.ready()) throw new Error("not ready: " + OF.error());

const w = OF.width();
const h = OF.height();
const terrain = new Uint8Array(OF.terrainBuffer());
const state = new Uint16Array(OF.stateBuffer());
console.log(`map ${w}x${h}`);

// Spawn on the land tile nearest the centre.
let spawn = null;
for (let r = 0; r < Math.max(w, h) && !spawn; r += 3) {
  for (let a = 0; a < 16 && !spawn; a++) {
    const x = Math.round(w / 2 + r * Math.cos((a * Math.PI) / 8));
    const y = Math.round(h / 2 + r * Math.sin((a * Math.PI) / 8));
    if (x < 0 || y < 0 || x >= w || y >= h) continue;
    if (terrain[y * w + x] & 0x80 && (state[y * w + x] & 0xfff) === 0 && call("click", { x, y }) === "spawn") {
      spawn = [x, y];
    }
  }
}
console.log("spawn", spawn);
const spawnRadial = call("radial", { x: spawn[0] + 20, y: spawn[1] });
console.log("spawn-phase radial center disabled:", spawnRadial?.center?.disabled);

const ticks = Number(ticksArg);
const t0 = performance.now();
let frame = null;
let mySid = -1;
for (let i = 0; i < ticks; i++) {
  OF.tick();
  OF.tileUpdates();
  frame = JSON.parse(OF.frame());
  mySid = frame.me?.sid ?? -1;
  if (!frame.spawn && i % 25 === 0) {
    const loc = call("playerLocation", { sid: mySid });
    if (loc) {
      for (let k = 0; k < 40; k++) {
        if (call("click", { x: Math.round(loc.x + Math.cos(k) * (8 + k)), y: Math.round(loc.y + Math.sin(k) * (8 + k)) })) break;
      }
    }
  }
  if (frame.winner) break;
}
console.log(`${ticks} ticks in ${(performance.now() - t0).toFixed(0)} ms`);
console.log("timer", frame.timer, "heads", frame.heads);
console.log("control panel", JSON.stringify(frame.cp));
console.log("hotbar", frame.hotbar?.map((h) => `${h.hotkey}:${h.name}=${h.count ?? "-"}${h.can ? "" : "(x)"}`).join(" "));
console.log("leaderboard", JSON.stringify(frame.lb?.rows?.slice(0, 2)), "pinned", JSON.stringify(frame.lb?.pinned));
console.log("attacks", JSON.stringify(frame.attacks?.slice(0, 3)));
console.log("events", JSON.stringify(frame.events)?.slice(0, 400));
assert(frame.cp && frame.hotbar && frame.lb, "HUD model present");

const me = call("playerLocation", { sid: mySid });
const own = call("radial", { x: Math.round(me.x), y: Math.round(me.y) });
console.log("own radial:", own.items.map((i) => `${i.id}${i.disabled ? "(x)" : ""}${i.sub ? "[" + i.sub.length + "]" : ""}`).join(" "));
assert(own.items.some((i) => i.id === "build" && i.sub), "build submenu on own territory");
const build = own.items.find((i) => i.id === "build");
console.log("  build:", build.sub.map((s) => `${s.icon}${s.disabled ? "(x)" : ""}`).join(" "));
const cityIndex = build.sub.findIndex((s) => s.icon === "CityIconWhite");
const before = frame.hotbar.find((h) => h.hotkey === "1").count;
console.log("  build city ->", JSON.stringify(call("radialSelect", { x: Math.round(me.x), y: Math.round(me.y), path: [own.items.indexOf(build), cityIndex] })));
for (let i = 0; i < 5; i++) OF.tick();
frame = JSON.parse(OF.frame());
console.log("  cities", before, "->", frame.hotbar.find((h) => h.hotkey === "1").count);

// Find a neighbour through the leaderboard and open its menus.
const other = frame.lb.rows.find((r) => r.sid !== mySid);
const loc = call("playerLocation", { sid: other.sid });
const hover = call("hover", { x: Math.round(loc.x), y: Math.round(loc.y) });
console.log("hover:", JSON.stringify(hover)?.slice(0, 300));
const enemy = call("radial", { x: Math.round(loc.x), y: Math.round(loc.y) });
console.log("enemy radial:", enemy.items.map((i) => `${i.id}${i.disabled ? "(x)" : ""}${i.sub ? "[" + i.sub.length + "]" : ""}`).join(" "), "center", JSON.stringify(enemy.center));
const panel = call("panel", { sid: other.sid });
console.log("panel:", JSON.stringify(panel)?.slice(0, 400));
const ghost = call("ghost", { type: "City", x: Math.round(me.x), y: Math.round(me.y) });
console.log("ghost city:", JSON.stringify(ghost));
const nukeGhost = call("ghost", { type: "Atom Bomb", x: Math.round(loc.x), y: Math.round(loc.y) });
console.log("ghost nuke:", JSON.stringify(nukeGhost)?.slice(0, 300));
console.log("labels", OF.labels().length / 7, "units", OF.units().length / 9, "relations", call("relations").length / 3);
