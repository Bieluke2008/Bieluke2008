// OpenFront engine adapter for the Nintendo Switch port.
//
// The browser client runs the engine in a Web Worker and its HUD in the page.
// Here the engine runs in-process inside QuickJS, driven by the native host
// (src/), and this file plus the modules next to it stand in for the web
// client's controllers: they build the HUD's data with the client's own
// logic, and turn clicks into the same intents.
import {
  AllPlayers,
  Difficulty,
  GameMapSize,
  GameMapType,
  GameMode,
  GameType,
  maps,
  UnitType,
} from "@openfront/engine-api/game/GameTypes";
import { ErrorUpdate, GameUpdateViewData } from "@openfront/engine-api/game/GameUpdates";
import { MapManifest } from "@openfront/engine-api/game/MapFiles";
import { flattenedEmojiTable, GameStartInfo, StampedIntent } from "@openfront/engine-api/Schemas";
import { renderTroops } from "@openfront/engine-lib/Format";
import { createGameRunner } from "@openfront/engine/GameRunner";
import { ghost, placeGhost } from "./build";
import { colorNewPlayers, colorTable, resetColors } from "./colors";
import { actionableClick, processUpdates, resetEvents } from "./events";
import { attackAction, frameModel, hudUpdates, LB, resetHud } from "./hud";
import { initI18n, loadJson, ROMFS, t } from "./i18n";
import { emojiList, hoverInfo, playerAction, playerIcons, playerPanel, quickChat, sendEmoji, sendQuickChat } from "./panel";
import { radialCenter, radialMenu, radialSelect } from "./radial";
import { CLIENT_ID, firstPlace, flagOf, game, me, playerBySmall, S, setNationFlags } from "./state";

declare const host: {
  log(msg: string): void;
  now(): number;
  readFile(path: string): ArrayBuffer | null;
};

// --- Map list ---------------------------------------------------------------

interface MapEntry {
  folder: string;
  type: GameMapType;
  name: string;
}

let mapEntries: MapEntry[] | null = null;

function mapList(): MapEntry[] {
  if (mapEntries !== null) return mapEntries;
  const available = loadJson(ROMFS + "maps/index.json") as string[] | null;
  const have = new Set(available ?? []);
  mapEntries = [];
  for (const key of Object.keys(GameMapType)) {
    const type = (GameMapType as Record<string, GameMapType>)[key];
    const folder = key.toLowerCase();
    if (available !== null && !have.has(folder)) continue;
    const info = maps.find((m) => m.type === type);
    const translated = info ? t(info.translationKey) : type;
    mapEntries.push({ folder, type, name: translated.startsWith("map.") ? type : translated });
  }
  mapEntries.sort((a, b) => a.name.localeCompare(b.name));
  return mapEntries;
}

// --- Game ---------------------------------------------------------------------

interface StartOptions {
  map: number;
  difficulty: number;
  bots: number;
  nations: number; // -1 = map default, 0 = none
  name: string;
  instantBuild?: boolean;
  infiniteGold?: boolean;
  infiniteTroops?: boolean;
}

let error: string | null = null;
let turnNumber = 0;
let tileBatch: Uint32Array[] = [];
let names: Record<string, { x: number; y: number; size: number }> = {};
let startOptions: StartOptions | null = null;
let playersVersion = 0;

function onUpdate(gu: GameUpdateViewData | ErrorUpdate): void {
  if (!("updates" in gu)) {
    error = gu.errMsg;
    console.error("engine error:", gu.errMsg, gu.stack ?? "");
    return;
  }
  tileBatch.push(gu.packedTileUpdates);
  if (gu.playerNameViewData) names = gu.playerNameViewData;
  if (colorNewPlayers()) playersVersion++;
  processUpdates(gu);
  hudUpdates(gu);
}

async function start(opts: StartOptions): Promise<void> {
  const entry = mapList()[opts.map];
  const base = ROMFS + "maps/" + entry.folder + "/";
  const manifest = loadJson(base + "manifest.json") as MapManifest;
  setNationFlags([...manifest.nations, ...(manifest.additionalNations ?? [])]);
  const bin = (name: string) => {
    const b = host.readFile(base + name);
    if (b === null) throw new Error("missing " + base + name);
    return new Uint8Array(b);
  };
  const difficulty = [Difficulty.Easy, Difficulty.Medium, Difficulty.Hard, Difficulty.Impossible][opts.difficulty];
  const gameStart = {
    gameID: "SWITCH" + Math.floor(host.now()).toString(36).toUpperCase(),
    lobbyCreatedAt: 0,
    players: [{ clientID: CLIENT_ID, username: opts.name, clanTag: null }],
    config: {
      gameMap: entry.type,
      gameMapSize: GameMapSize.Compact,
      gameType: GameType.Singleplayer,
      gameMode: GameMode.FFA,
      difficulty,
      bots: opts.bots,
      nations: opts.nations < 0 ? "default" : opts.nations === 0 ? "disabled" : opts.nations,
      infiniteGold: opts.infiniteGold ?? false,
      infiniteTroops: opts.infiniteTroops ?? false,
      donateGold: false,
      donateTroops: false,
      instantBuild: opts.instantBuild ?? false,
      randomSpawn: false,
      disabledUnits: [],
    },
  } as unknown as GameStartInfo;
  S.runner = await createGameRunner(
    gameStart,
    CLIENT_ID,
    {
      map: entry.type,
      mapSize: GameMapSize.Compact,
      manifest,
      map4xBin: bin("map4x.bin"),
      map16xBin: bin("map16x.bin"),
    },
    onUpdate,
  );
  colorNewPlayers();
  playersVersion++;
}

/** Name label status bits (the name pass's status row, in its order). */
const ICON_BITS: Record<string, number> = {
  crown: 1,
  traitor: 2,
  disconnected: 4,
  alliance: 8,
  allianceRequest: 16,
  target: 32,
  embargo: 64,
  nukeWhite: 128,
  nukeRed: 256,
};

// Order matters: src/units.h indexes this list.
const UNIT_KINDS: UnitType[] = [
  UnitType.City,
  UnitType.Port,
  UnitType.DefensePost,
  UnitType.MissileSilo,
  UnitType.SAMLauncher,
  UnitType.Factory,
  UnitType.Warship,
  UnitType.TransportShip,
  UnitType.TradeShip,
  UnitType.AtomBomb,
  UnitType.HydrogenBomb,
  UnitType.MIRV,
  UnitType.MIRVWarhead,
  UnitType.Train,
  UnitType.Shell,
  UnitType.SAMMissile,
];

// --- RPC for the native host ------------------------------------------------------

type Handler = (a: any) => unknown;

function clickTile(a: { x: number; y: number }): string | null {
  // ClientGameRunner.inputEvent(): what a left click on the map does.
  const g = game();
  if (!g.isValidCoord(a.x, a.y)) return null;
  const tile = g.ref(a.x, a.y);
  const my = me();
  if (g.isLand(tile) && !g.hasOwner(tile) && g.inSpawnPhase() && !g.config().isRandomSpawn()) {
    S.pendingIntents.push({ type: "spawn", tile });
    return "spawn";
  }
  if (g.inSpawnPhase() || my === null || !my.isAlive()) return null;
  const actions = S.runner!.playerActions(my.id(), a.x, a.y, [UnitType.TransportShip]);
  const troops = my.troops() * S.ui.attackRatio;
  if (actions.canAttack) {
    const owner = g.owner(tile);
    S.pendingIntents.push({ type: "attack", targetID: owner.isPlayer() ? owner.id() : null, troops });
    return "attack";
  }
  // canAutoBoat(): land within 100 tiles of where the boat would launch.
  const bu = actions.buildableUnits.find((b) => b.type === UnitType.TransportShip);
  const src = bu?.canBuild ?? false;
  if (g.isLand(tile) && src !== false && g.euclideanDistSquared(tile, src) < 100 * 100) {
    S.pendingIntents.push({ type: "boat", troops, dst: tile });
    return "boat";
  }
  return null;
}

const methods: Record<string, Handler> = {
  radial: (a) => radialMenu(a.x, a.y),
  radialSelect: (a) => radialSelect(a.x, a.y, a.path),
  radialCenter: (a) => radialCenter(a.x, a.y),
  hover: (a) => hoverInfo(a.x, a.y),
  panel: (a) => playerPanel(a.sid),
  playerAction: (a) => playerAction(a.sid, a.action, a.amount),
  emoji: (a) => sendEmoji(a.sid, a.index),
  emojis: () => emojiList(),
  quickChat: (a) => quickChat(a.sid),
  sendQuickChat: (a) => sendQuickChat(a.sid, a.key),
  ghost: (a) => ghost(a.type, a.x, a.y),
  placeGhost: (a) => placeGhost(a.type, a.x, a.y),
  setGhost: (a) => {
    S.ui.ghostStructure = a.type ?? null;
  },
  attackAction: (a) => attackAction(a.kind, String(a.id)),
  eventButton: (a) => actionableClick(a.id, a.button),
  ratio: (a) => {
    let r = Math.round(a.value * 100) / 100;
    if (r < 0.01) r = 0.01;
    if (r > 1) r = 1;
    S.ui.attackRatio = r;
  },
  ratioStep: (a) => {
    // ControlPanel's AttackRatioEvent handler.
    let r = S.ui.attackRatio + a.delta / 100;
    if (r < 0.01) r = 0.01;
    if (r > 1) r = 1;
    if (Math.abs(r - 0.11) < 1e-9 && Math.abs(S.ui.attackRatio - 0.01) < 1e-9) r = 0.1;
    S.ui.attackRatio = Math.round(r * 100) / 100;
  },
  leaderboard: (a) => {
    if (a.sort !== undefined) {
      if (LB.sortKey === a.sort) LB.sortAsc = !LB.sortAsc;
      else {
        LB.sortKey = a.sort;
        LB.sortAsc = false;
      }
    }
    if (a.scroll !== undefined) LB.offset = Math.max(0, LB.offset + a.scroll);
  },
  /** Where a player's name sits (GoToPlayerEvent). */
  playerLocation: (a) => {
    const p = playerBySmall(a.sid);
    if (!p) return null;
    const nv = names[p.id()];
    if (nv && nv.size > 0) return { x: nv.x, y: nv.y };
    const s = p.spawnTile();
    return s === undefined ? null : { x: game().x(s), y: game().y(s) };
  },
  unitLocation: (a) => {
    const u = game().unit(a.id);
    return u ? { x: game().x(u.tile()), y: game().y(u.tile()) } : null;
  },
  click: (a) => clickTile(a),
  /** Boat attack to a tile (the web client's boat keybind). */
  boat: (a) => {
    const g = game();
    const my = me();
    if (!my || !g.isValidCoord(a.x, a.y)) return false;
    const bu = S.runner!.playerBuildables(my.id(), a.x, a.y, [UnitType.TransportShip]).find(
      (b) => b.type === UnitType.TransportShip,
    );
    if (!bu || bu.canBuild === false) return false;
    S.pendingIntents.push({ type: "boat", troops: my.troops() * S.ui.attackRatio, dst: g.ref(a.x, a.y) });
    return true;
  },
  /** Own warships near a tile (WarshipSelectionController). */
  warshipsAt: (a) => {
    const g = game();
    const my = me();
    if (!my || !g.isValidCoord(a.x, a.y)) return [];
    const tile = g.ref(a.x, a.y);
    const r = a.r ?? 10;
    return my
      .units(UnitType.Warship)
      .filter((u) => u.isActive() && g.euclideanDistSquared(u.tile(), tile) <= r * r)
      .map((u) => u.id());
  },
  moveWarships: (a) => {
    const g = game();
    if (!g.isValidCoord(a.x, a.y) || a.ids.length === 0) return;
    S.pendingIntents.push({ type: "move_warship", unitIds: a.ids, tile: g.ref(a.x, a.y) });
  },
  pause: (a) => {
    S.paused = !!a.paused;
  },
  t: (a) => t(a.key, a.params ?? {}),
  playerInfo: (a) => {
    const p = playerBySmall(a.sid);
    if (!p) return null;
    return { name: p.displayName(), flag: flagOf(p), type: p.type(), me: p === me() };
  },
  /** RelationMatrix.ts: [a, b, rel] triples, 1 friendly, 2 embargo. */
  relations: () => {
    const out: number[] = [];
    const players = game().players();
    for (const p of players) {
      for (const ally of p.allies()) out.push(p.smallID(), ally.smallID(), 1);
      for (const other of players) {
        if (p !== other && p.hasEmbargoAgainst(other)) out.push(p.smallID(), other.smallID(), 2);
      }
    }
    return out;
  },
};

const OF = {
  init(lang: string): void {
    initI18n(lang);
  },

  t(key: string): string {
    return t(key);
  },

  mapNames(): string[] {
    return mapList().map((m) => m.name);
  },

  mapFolder(i: number): string {
    return mapList()[i].folder;
  },

  start(opts: StartOptions): void {
    S.runner = null;
    S.pendingIntents = [];
    S.paused = false;
    S.ui.ghostStructure = null;
    S.ui.attackRatio = 0.2;
    error = null;
    names = {};
    turnNumber = 0;
    tileBatch = [];
    resetColors();
    resetEvents();
    resetHud();
    startOptions = opts;
    start(opts).catch((e: unknown) => {
      error = e instanceof Error ? e.message + "\n" + e.stack : String(e);
      console.error("start failed:", error);
    });
  },

  ready(): boolean {
    return S.runner !== null;
  },

  error(): string | null {
    return error;
  },

  restart(): void {
    if (startOptions) OF.start(startOptions);
  },

  width(): number {
    return game().width();
  },

  height(): number {
    return game().height();
  },

  terrainBuffer(): ArrayBuffer {
    return (game().map() as any).terrain.buffer;
  },

  stateBuffer(): ArrayBuffer {
    return (game().map() as any).state.buffer;
  },

  colors(): ArrayBuffer {
    return colorTable.buffer;
  },

  playersVersion(): number {
    return playersVersion;
  },

  tick(): boolean {
    if (S.runner === null) return false;
    const intents: StampedIntent[] = S.pendingIntents.map((i) => ({ ...i, clientID: CLIENT_ID }) as StampedIntent);
    S.pendingIntents = [];
    S.runner.addTurn({ turnNumber: turnNumber++, intents });
    return S.runner.executeNextTick();
  },

  tileUpdates(): ArrayBuffer {
    let out: Uint32Array;
    if (tileBatch.length === 1) out = tileBatch[0].slice();
    else {
      let n = 0;
      for (const b of tileBatch) n += b.length;
      out = new Uint32Array(n);
      let o = 0;
      for (const b of tileBatch) {
        out.set(b, o);
        o += b.length;
      }
    }
    tileBatch = [];
    return out.buffer as ArrayBuffer;
  },

  /** Name labels: [sid, x, y, size, troops, iconBits, emoji+1] per player. */
  labels(): Float64Array {
    const g = game();
    const my = me();
    const first = firstPlace();
    const out: number[] = [];
    for (const p of g.players()) {
      if (!p.isAlive()) continue;
      const nv = names[p.id()];
      if (!nv || nv.size <= 0) continue;
      let bits = 0;
      for (const icon of playerIcons(p, first)) bits |= ICON_BITS[icon] ?? 0;
      if (p.isTraitor()) bits |= ICON_BITS.traitor;
      if (my && my !== p && my.isAlliedWith(p)) bits |= ICON_BITS.alliance;
      const e = p.outgoingEmojis().find((x) => x.recipientID === AllPlayers || x.recipientID === my?.smallID());
      const emojiIndex = e ? (flattenedEmojiTable as readonly string[]).indexOf(e.message) : -1;
      out.push(p.smallID(), nv.x, nv.y, nv.size, p.troops(), bits, emojiIndex + 1);
    }
    return new Float64Array(out);
  },

  /** Units: [kind, x, y, owner, level, flags, id, targetX, targetY] per unit. */
  units(): Int32Array {
    const g = game();
    const my = me();
    const out: number[] = [];
    for (const u of g.units(UNIT_KINDS)) {
      if (!u.isActive()) continue;
      const tile = u.tile();
      let flags = 0;
      if (u.isUnderConstruction()) flags |= 1;
      if (u.isMarkedForDeletion()) flags |= 2;
      if (u.type() === UnitType.TransportShip && u.transportShipState().isRetreating) flags |= 4;
      if (u.type() === UnitType.TradeShip && my) {
        if (u.owner() === my) flags |= 64;
        else if (my.isAlliedWith(u.owner()) || my.isOnSameTeam(u.owner())) flags |= 8;
      }
      if (u.type() === UnitType.MissileSilo && u.isInCooldown()) flags |= 32;
      const dst = u.targetTile();
      out.push(
        UNIT_KINDS.indexOf(u.type()),
        g.x(tile),
        g.y(tile),
        u.owner().smallID(),
        u.level(),
        flags,
        u.id(),
        dst !== undefined ? g.x(dst) : -1,
        dst !== undefined ? g.y(dst) : -1,
      );
    }
    return new Int32Array(out);
  },

  frame(): string {
    return frameModel();
  },

  /** RPC from the renderer: OF.call("radial", '{"x":1,"y":2}') -> JSON. */
  call(method: string, args: string): string {
    const fn = methods[method];
    if (!fn) return JSON.stringify({ error: "no method " + method });
    const result = fn(args ? JSON.parse(args) : {});
    return JSON.stringify(result === undefined ? null : result, (_k, v) =>
      typeof v === "bigint" ? Number(v) : v,
    );
  },

  troops(n: number): string {
    return renderTroops(n);
  },
};

(globalThis as any).OF = OF;
