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
  HumansVsNations,
  maps,
  PlayerType,
  Duos,
  Quads,
  Trios,
  UnitType,
} from "@openfront/engine-api/game/GameTypes";
import { DOOMSDAY_CLOCK_SPEEDS } from "@openfront/engine-lib/game/DoomsdayClock";
import { ErrorUpdate, GameUpdateViewData } from "@openfront/engine-api/game/GameUpdates";
import { MapManifest } from "@openfront/engine-api/game/MapFiles";
import { flattenedEmojiTable, GameStartInfo, StampedIntent, TeamCountConfig } from "@openfront/engine-api/Schemas";
import { renderTroops } from "@openfront/engine-lib/Format";
import { createGameRunner } from "@openfront/engine/GameRunner";
import type { Unit } from "@openfront/engine/game/Game";
import { ghost, placeGhost } from "./build";
import { bindNativeMaps } from "./native/maps";
import { colorNewPlayers, colorTable, resetColors } from "./colors";
import { actionableClick, processUpdates, resetEvents, sounds } from "./events";
import { resetSounds, soundUpdates } from "./sound";
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
  /** Heading of the picker section and of the group within it the map opens, else "". */
  section: string;
  group: string;
}

// The picker's order: the world and continent maps first (in this order),
// then real places by region, then the made-up ones; the group titles are
// the web client's map_categories.
const CONTINENTS = [
  "World",
  "GiantWorldMap",
  "Europe",
  "EuropeClassic",
  "Africa",
  "NorthAmerica",
  "SouthAmerica",
  "Asia",
  "Oceania",
  "Antarctica",
];
const REAL_GROUPS = ["europe", "asia", "north_america", "africa", "south_america", "oceania", "antarctica", "countries"];
const FICTIONAL_GROUPS = ["fictional", "cosmic", "arcade", "tournament"];

let mapEntries: MapEntry[] | null = null;

function mapList(): MapEntry[] {
  if (mapEntries !== null) return mapEntries;
  const available = loadJson(ROMFS + "maps/index.json") as string[] | null;
  const have = new Set(available ?? []);
  const sorted: { entry: MapEntry; section: number; group: number; rank: number }[] = [];
  for (const key of Object.keys(GameMapType)) {
    const type = (GameMapType as Record<string, GameMapType>)[key];
    const folder = key.toLowerCase();
    if (available !== null && !have.has(folder)) continue;
    const info = maps.find((m) => m.type === type);
    const translated = info ? t(info.translationKey) : type;
    const cats: string[] = info?.categories ?? [];
    let section: number, group: number, rank = 0;
    const fictional = FICTIONAL_GROUPS.findIndex((c) => cats.includes(c));
    const real = REAL_GROUPS.findIndex((c) => cats.includes(c));
    if (CONTINENTS.includes(info?.id ?? "")) {
      section = 0;
      group = 0;
      rank = CONTINENTS.indexOf(info!.id);
    } else if (fictional >= 0 || real < 0) {
      section = 2;
      group = Math.max(fictional, 0);
    } else {
      section = 1;
      group = real;
    }
    sorted.push({
      entry: { folder, type, name: translated.startsWith("map.") ? type : translated, section: "", group: "" },
      section,
      group,
      rank,
    });
  }
  sorted.sort(
    (a, b) => a.section - b.section || a.group - b.group || a.rank - b.rank || a.entry.name.localeCompare(b.entry.name),
  );
  const sectionTitles = [t("port.continents"), t("port.real_maps"), t("port.fictional_maps")];
  const groupKeys = [[], REAL_GROUPS, FICTIONAL_GROUPS];
  mapEntries = sorted.map((m, i) => {
    const prev = sorted[i - 1];
    if (!prev || prev.section !== m.section) m.entry.section = sectionTitles[m.section];
    if (m.section > 0 && (!prev || prev.section !== m.section || prev.group !== m.group)) {
      m.entry.group = t("map_categories." + groupKeys[m.section][m.group]);
    }
    return m.entry;
  });
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
  randomSpawn?: boolean;
  waterNukes?: boolean;
  gameMode?: number; // 0 FFA, 1 teams
  teamCount?: number; // index into TEAM_COUNTS
  doomsdaySpeed?: number; // index into DOOMSDAY_CLOCK_SPEEDS, -1 off
  maxTimer?: number; // minutes
  goldMultiplier?: number;
  startingGold?: number; // millions
  allianceMinutes?: number;
  overtime?: number; // minutes
  disabledUnits?: number; // bit i: SOLO_UNIT_OPTIONS[i] off
  seed?: string;
}

// GameConfigSettings' team count options and unit options, in its order.
const TEAM_COUNTS: TeamCountConfig[] = [2, 3, 4, 5, 6, 7, Quads, Trios, Duos, HumansVsNations];
const SOLO_UNIT_OPTIONS: UnitType[] = [
  UnitType.City,
  UnitType.DefensePost,
  UnitType.Port,
  UnitType.Warship,
  UnitType.TransportShip,
  UnitType.MissileSilo,
  UnitType.SAMLauncher,
  UnitType.AtomBomb,
  UnitType.HydrogenBomb,
  UnitType.MIRV,
  UnitType.Factory,
];

let error: string | null = null;
let turnNumber = 0;
let tileBatch: Uint32Array[] = [];
let names: Record<string, { x: number; y: number; size: number }> = {};
// OF.labels()'s icon bits per player, as of iconTick.
let iconBits = new WeakMap<object, number>();
let iconTick = -Infinity;
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
  soundUpdates(gu);
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
  const teams = opts.gameMode === 1;
  const gameStart = {
    // A fixed seed (benchmarks) makes the game, bots and nations repeat.
    gameID: opts.seed ? "SWITCH" + opts.seed : "SWITCH" + Math.floor(host.now()).toString(36).toUpperCase(),
    lobbyCreatedAt: 0,
    players: [{ clientID: CLIENT_ID, username: opts.name, clanTag: null }],
    // SinglePlayerModal.startGame()'s config.
    config: {
      gameMap: entry.type,
      gameMapSize: GameMapSize.Compact,
      gameType: GameType.Singleplayer,
      gameMode: teams ? GameMode.Team : GameMode.FFA,
      playerTeams: TEAM_COUNTS[opts.teamCount ?? 0] ?? 2,
      difficulty,
      maxTimerValue: opts.maxTimer !== undefined ? Math.max(1, Math.min(120, opts.maxTimer)) : undefined,
      bots: opts.bots,
      nations: opts.nations < 0 ? "default" : opts.nations === 0 ? "disabled" : opts.nations,
      infiniteGold: opts.infiniteGold ?? false,
      infiniteTroops: opts.infiniteTroops ?? false,
      donateGold: teams,
      donateTroops: teams,
      instantBuild: opts.instantBuild ?? false,
      randomSpawn: opts.randomSpawn ?? false,
      disabledUnits: SOLO_UNIT_OPTIONS.filter((_, i) => ((opts.disabledUnits ?? 0) >> i) & 1),
      ...(opts.goldMultiplier ? { goldMultiplier: opts.goldMultiplier } : {}),
      ...(opts.startingGold !== undefined ? { startingGold: Math.round(opts.startingGold * 1_000_000) } : {}),
      ...(opts.allianceMinutes !== undefined ? { customAllianceDuration: opts.allianceMinutes } : {}),
      ...(opts.waterNukes ? { waterNukes: true } : {}),
      ...(opts.doomsdaySpeed !== undefined && opts.doomsdaySpeed >= 0
        ? { doomsdayClock: { enabled: true, speed: DOOMSDAY_CLOCK_SPEEDS[opts.doomsdaySpeed] ?? "normal" } }
        : {}),
      ...(opts.overtime !== undefined ? { overtime: { enabled: true, startMinutes: opts.overtime } } : {}),
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
  bindNativeMaps(S.runner.game);
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
    sounds.push("spawn");
    return "spawn";
  }
  if (g.inSpawnPhase() || my === null || !my.isAlive()) return null;
  const troops = my.troops() * S.ui.attackRatio;
  // playerActions().canAttack, without the boat route it also works out:
  // that search only matters when a land attack is not possible.
  if (my.canAttack(tile)) {
    const owner = g.owner(tile);
    S.pendingIntents.push({ type: "attack", targetID: owner.isPlayer() ? owner.id() : null, troops });
    return "attack";
  }
  // canAutoBoat(): land within 100 tiles of where the boat would launch.
  const bu = my.buildableUnits(tile, [UnitType.TransportShip]).find((b) => b.type === UnitType.TransportShip);
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
    const g = game();
    for (const p of g.players()) {
      for (const ally of p.allies()) out.push(p.smallID(), ally.smallID(), 1);
      // hasEmbargoAgainst(other) is embargoes.has(other.id()): walk the map
      // instead of testing every pair.
      for (const id of (p as any).embargoes.keys()) {
        if (!g.hasPlayer(id)) continue;
        const other = g.player(id);
        if (other !== p && other.isAlive()) out.push(p.smallID(), other.smallID(), 2);
      }
    }
    return out;
  },
};

// BarPass.computeStructureProgress: deletion countdown, else construction,
// else Silo/SAM reload; in per mille, -1 for no bar. Like the client, the
// construction clock starts on the tick a unit is first seen being built.
const constructionStart = new Map<number, number>();

// OF.attackLabels(): a small stable key per attack id and its last front positions.
const attackKeys = new Map<string, number>();
const attackPositions = new Map<string, number[][]>();
let nextAttackKey = 1;
let attackPosTick = -Infinity;

function structureProgress(u: Unit, ticks: number): number {
  const config = game().config();
  const clamp = (v: number) => Math.round(Math.max(0, Math.min(1, v)) * 1000);
  const deletionAt = (u as any)._deletionAt as number | null;
  if (deletionAt !== null && deletionAt !== undefined) {
    return clamp((deletionAt - ticks) / config.deletionMarkDuration());
  }
  if (u.isUnderConstruction()) {
    let start = constructionStart.get(u.id());
    if (start === undefined) constructionStart.set(u.id(), (start = ticks));
    const duration = config.unitInfo(u.type()).constructionDuration ?? 50;
    return clamp((ticks - start) / duration);
  }
  constructionStart.delete(u.id());
  if (u.type() === UnitType.MissileSilo || u.type() === UnitType.SAMLauncher) {
    const queue = u.missileTimerQueue();
    if (queue.length === 0) return -1;
    const maxMissiles = u.level();
    const cooldown = u.type() === UnitType.SAMLauncher ? config.SAMCooldown() : config.SiloCooldown();
    let readiness = (maxMissiles - queue.length) / maxMissiles;
    for (const timer of queue) readiness += (ticks - timer) / cooldown / maxMissiles;
    if (readiness < 1) return clamp(readiness);
  }
  return -1;
}

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

  /** The picker headings map i opens: [section, group], "" when none. */
  mapHeadings(i: number): string[] {
    const m = mapList()[i];
    return [m.section, m.group];
  },

  /** The map's own nation count (the Solo modal's nations slider default). */
  mapNations(i: number): number {
    const manifest = loadJson(ROMFS + "maps/" + mapList()[i].folder + "/manifest.json") as MapManifest | null;
    return manifest?.nations.length ?? 0;
  },

  start(opts: StartOptions): void {
    S.runner = null;
    S.pendingIntents = [];
    S.paused = false;
    S.ui.ghostStructure = null;
    S.ui.attackRatio = 0.2;
    error = null;
    names = {};
    iconBits = new WeakMap();
    iconTick = -Infinity;
    turnNumber = 0;
    constructionStart.clear();
    attackKeys.clear();
    attackPositions.clear();
    attackPosTick = -Infinity;
    tileBatch = [];
    resetColors();
    resetEvents();
    resetSounds();
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
    // Seeded benchmarks: the human spawns at a fixed turn on the land tile
    // nearest the map centre, so the whole game repeats exactly.
    if (startOptions?.seed && turnNumber === 5) {
      const g = game();
      const cx = g.width() >> 1, cy = g.height() >> 1;
      for (let r = 0; r < g.width(); r++) {
        const x = cx + r;
        if (x < g.width() && g.isLand(g.ref(x, cy)) && !g.hasOwner(g.ref(x, cy))) {
          S.pendingIntents.push({ type: "spawn", tile: g.ref(x, cy) });
          break;
        }
      }
    }
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
    const ticks = g.ticks();
    // The icons change rarely and cost the most: refresh them every 4 ticks.
    const refreshIcons = ticks - iconTick >= 4 || ticks < iconTick;
    if (refreshIcons) iconTick = ticks;
    let first: ReturnType<typeof firstPlace> | undefined;
    const out: number[] = [];
    for (const p of g.players()) {
      if (!p.isAlive()) continue;
      const nv = names[p.id()];
      if (!nv || nv.size <= 0) continue;
      let bits = iconBits.get(p);
      if (refreshIcons || bits === undefined) {
        bits = 0;
        if (first === undefined) first = firstPlace();
        for (const icon of playerIcons(p, first)) bits |= ICON_BITS[icon] ?? 0;
        if (p.isTraitor()) bits |= ICON_BITS.traitor;
        if (my && my !== p && my.isAlliedWith(p)) bits |= ICON_BITS.alliance;
        iconBits.set(p, bits);
      }
      const e = p.outgoingEmojis().find((x) => x.recipientID === AllPlayers || x.recipientID === my?.smallID());
      const emojiIndex = e ? (flattenedEmojiTable as readonly string[]).indexOf(e.message) : -1;
      out.push(p.smallID(), nv.x, nv.y, nv.size, p.troops(), bits, emojiIndex + 1);
    }
    return new Float64Array(out);
  },

  /**
   * AttackingTroopsController: the troop labels at the fronts of the local
   * player's attacks on players and of non-tribe attacks on it.
   * [key, incoming, troops, nPositions, x0, y0, x1, y1] per attack.
   */
  attackLabels(): Float64Array {
    const g = game();
    const my = me();
    const out: number[] = [];
    if (!my || !my.isAlive()) return new Float64Array(0);
    // The client polls the front positions every 200 ms (2 ticks).
    const refresh = g.ticks() - attackPosTick >= 2 || g.ticks() < attackPosTick;
    if (refresh) attackPosTick = g.ticks();
    const live = new Set<string>();
    const add = (a: any, incoming: boolean) => {
      const id: string = a.id();
      live.add(id);
      let key = attackKeys.get(id);
      if (key === undefined) attackKeys.set(id, (key = nextAttackKey++));
      let pos = attackPositions.get(id);
      if (refresh || pos === undefined) {
        pos = a.clusteredPositions().map((t: number) => [g.x(t), g.y(t)]) as number[][];
        attackPositions.set(id, pos);
      }
      const p0 = pos[0] ?? [0, 0];
      const p1 = pos[1] ?? [0, 0];
      out.push(key, incoming ? 1 : 0, a.troops(), Math.min(pos.length, 2), p0[0], p0[1], p1[0], p1[1]);
    };
    for (const a of my.outgoingAttacks()) {
      if (a.target().isPlayer()) add(a, false);
    }
    for (const a of my.incomingAttacks()) {
      const attacker = a.attacker();
      if (attacker.isPlayer() && attacker.type() !== PlayerType.Bot) add(a, true);
    }
    for (const id of attackKeys.keys()) {
      if (!live.has(id)) {
        attackKeys.delete(id);
        attackPositions.delete(id);
      }
    }
    return new Float64Array(out);
  },

  /** Units: [kind, x, y, owner, level, flags, id, targetX, targetY, progress‰ or -1] per unit. */
  units(): Int32Array {
    const g = game();
    const my = me();
    const ticks = g.ticks();
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
        structureProgress(u, ticks),
      );
    }
    return new Int32Array(out);
  },

  frame(): string {
    return frameModel();
  },

  /** The cues queued since the last call, one per line (src/sound.c plays them). */
  sounds(): string {
    return sounds.splice(0, sounds.length).join("\n");
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

  /** Determinism check: a hash of every tile's state and each player's troops and gold. */
  checksum(): string {
    const g = game();
    const st = (g.map() as any).state as Uint16Array;
    let h = 2166136261;
    for (let i = 0; i < st.length; i++) h = Math.imul(h ^ st[i], 16777619);
    for (const p of g.allPlayers()) {
      h = Math.imul(h ^ Math.floor(p.troops()), 16777619);
      h = Math.imul(h ^ Number(p.gold() % 1000003n), 16777619);
    }
    return g.ticks() + ":" + (h >>> 0).toString(16);
  },

  troops(n: number): string {
    return renderTroops(n);
  },
};

(globalThis as any).OF = OF;
