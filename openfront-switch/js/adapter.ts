// OpenFront engine adapter for the Nintendo Switch port.
//
// The browser client runs the engine in a Web Worker and talks to it with
// messages. Here the engine runs in-process inside QuickJS, driven by the
// native host (src/), which calls the functions on `globalThis.OF` and reads
// the tile grid straight out of the engine's typed arrays.

import { TileRef } from "@openfront/engine-api/game/GameMap";
import {
  Difficulty,
  GameMapSize,
  GameMapType,
  GameMode,
  GameType,
  maps,
  MessageType,
  PlayerType,
  UnitType,
} from "@openfront/engine-api/game/GameTypes";
import {
  ErrorUpdate,
  GameUpdateType,
  GameUpdateViewData,
} from "@openfront/engine-api/game/GameUpdates";
import { MapManifest } from "@openfront/engine-api/game/MapFiles";
import {
  GameStartInfo,
  Intent,
  StampedIntent,
} from "@openfront/engine-api/Schemas";
import { createGameRunner, GameRunner } from "@openfront/engine/GameRunner";
import { Player } from "@openfront/engine/game/Game";

declare const host: {
  log(msg: string): void;
  now(): number;
  readFile(path: string): ArrayBuffer | null;
};

const CLIENT_ID = "SWITCH01";
const ROMFS = (globalThis as any).ROMFS_ROOT ?? "romfs:/";

// --- Translations -----------------------------------------------------------

let strings: Record<string, any> = {};
let fallback: Record<string, any> = {};

function loadJson(path: string): any {
  const buf = host.readFile(path);
  if (buf === null) return null;
  return JSON.parse(new TextDecoder().decode(new Uint8Array(buf)));
}

function lookup(table: Record<string, any>, key: string): string | undefined {
  let cur: any = table;
  for (const part of key.split(".")) {
    if (cur === undefined || cur === null) return undefined;
    cur = cur[part];
  }
  return typeof cur === "string" ? cur : undefined;
}

/** Minimal ICU formatting: {name} substitution plus plural/select blocks. */
function formatIcu(msg: string, params: Record<string, unknown>): string {
  let out = "";
  let i = 0;
  while (i < msg.length) {
    const c = msg[i];
    if (c !== "{") {
      out += c;
      i++;
      continue;
    }
    let depth = 1;
    let j = i + 1;
    while (j < msg.length && depth > 0) {
      if (msg[j] === "{") depth++;
      else if (msg[j] === "}") depth--;
      j++;
    }
    const body = msg.slice(i + 1, j - 1);
    i = j;
    const parts = body.split(",");
    const name = parts[0].trim();
    if (parts.length === 1) {
      out += params[name] !== undefined ? String(params[name]) : "";
      continue;
    }
    const kind = parts[1].trim();
    const rest = parts.slice(2).join(",");
    const branches: Record<string, string> = {};
    const re = /\s*(=?\w+)\s*\{/g;
    let m: RegExpExecArray | null;
    let pos = 0;
    while ((re.lastIndex = pos), (m = re.exec(rest)) !== null) {
      let d = 1;
      let k = re.lastIndex;
      while (k < rest.length && d > 0) {
        if (rest[k] === "{") d++;
        else if (rest[k] === "}") d--;
        k++;
      }
      branches[m[1]] = rest.slice(re.lastIndex, k - 1);
      pos = k;
    }
    const value = params[name];
    let branch: string | undefined;
    if (kind === "plural") {
      const n = Number(value);
      branch =
        branches["=" + n] ?? (n === 1 ? branches.one : undefined) ?? branches.other;
      if (branch !== undefined) {
        branch = formatIcu(branch, params).replace(/#/g, String(n));
      }
    } else {
      branch = branches[String(value)] ?? branches.other;
      if (branch !== undefined) branch = formatIcu(branch, params);
    }
    out += branch ?? "";
  }
  return out;
}

// Strings of the port's own UI (controls, menus) that OpenFront has no key
// for. Languages without an entry fall back to English.
const PORT_STRINGS: Record<string, Record<string, string>> = {
  en: {
    title: "OpenFront for Nintendo Switch",
    map: "Map",
    speed: "Game speed",
    name: "Name",
    start: "Start game",
    quit: "Quit",
    loading: "Loading map…",
    paused: "Paused",
    resume: "Resume",
    restart: "Restart",
    main_menu: "Main menu",
    spawn_hint: "Choose where to start: move the cursor and press A",
    controls:
      "A attack/expand · B boat · X build · Y player · L/R troops % · ZL/ZR zoom · + menu",
    menu_controls: "Up/Down select · Left/Right change · A start · + quit",
    alliance_hint: "Alliance request: press − to accept (reject from the + menu)",
    attack_ratio: "Attack",
    spectating: "You have been eliminated",
    accept_all: "Accept alliances",
    reject_all: "Reject alliances",
    no_target: "Nobody to interact with here",
    engine_slow: "Engine",
    embargo: "Embargo",
    credits:
      "OpenFront © OpenFront Inc. and contributors · AGPL-3.0 · assets CC BY-SA 4.0",
    tribes: "Tribes",
    nations: "Nations",
    default: "Map default",
    build: "Build",
  },
  es: {
    title: "OpenFront para Nintendo Switch",
    map: "Mapa",
    speed: "Velocidad",
    name: "Nombre",
    start: "Empezar partida",
    quit: "Salir",
    loading: "Cargando mapa…",
    paused: "En pausa",
    resume: "Continuar",
    restart: "Reiniciar",
    main_menu: "Menú principal",
    spawn_hint: "Elige dónde empezar: mueve el cursor y pulsa A",
    controls:
      "A atacar/expandir · B barco · X construir · Y jugador · L/R % tropas · ZL/ZR zoom · + menú",
    menu_controls: "Arriba/Abajo elegir · Izq./Der. cambiar · A empezar · + salir",
    alliance_hint: "Petición de alianza: pulsa − para aceptar (rechazar desde el menú +)",
    attack_ratio: "Ataque",
    spectating: "Has sido eliminado",
    accept_all: "Aceptar alianzas",
    reject_all: "Rechazar alianzas",
    no_target: "No hay nadie con quien interactuar aquí",
    engine_slow: "Motor",
    embargo: "Embargo",
    credits:
      "OpenFront © OpenFront Inc. y colaboradores · AGPL-3.0 · recursos CC BY-SA 4.0",
    tribes: "Tribus",
    nations: "Naciones",
    default: "Por defecto",
    build: "Construir",
  },
};
let portStrings = PORT_STRINGS.en;

function t(key: string, params: Record<string, unknown> = {}): string {
  if (key.startsWith("port.")) {
    const k = key.slice(5);
    return formatIcu(portStrings[k] ?? PORT_STRINGS.en[k] ?? key, params);
  }
  const msg = lookup(strings, key) ?? lookup(fallback, key);
  if (msg === undefined) return key;
  return formatIcu(msg, params);
}

// --- Map list ---------------------------------------------------------------

interface MapEntry {
  key: string; // GameMapType enum key, e.g. "AmazonRiver"
  folder: string; // romfs folder, e.g. "amazonriver"
  type: GameMapType;
  name: string;
  nations: number;
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
    const translated = info ? lookup(strings, info.translationKey) : undefined;
    mapEntries.push({
      key,
      folder,
      type,
      name: translated ?? type,
      nations: info?.defaultNationCount ?? 0,
    });
  }
  mapEntries.sort((a, b) => a.name.localeCompare(b.name));
  return mapEntries;
}

// --- Game state ---------------------------------------------------------------

interface Message {
  text: string;
  kind: number; // 0 info, 1 good, 2 bad, 3 alliance
}

let runner: GameRunner | null = null;
let pendingIntents: Intent[] = [];
let turnNumber = 0;
let lastTiles: Uint32Array = new Uint32Array(0);
let tileBatch: Uint32Array[] = [];
let names: Record<string, { x: number; y: number; size: number }> = {};
let messages: Message[] = [];
let winnerText: string | null = null;
let attackRatio = 0.2;
let error: string | null = null;
let colorTable = new Uint8Array(4096 * 4);
let coloredPlayers = 0;
let startOptions: StartOptions | null = null;

interface StartOptions {
  map: number; // index into mapList()
  difficulty: number; // 0..3
  bots: number;
  nations: number; // -1 = map default
  name: string;
  infiniteGold?: boolean;
  infiniteTroops?: boolean;
}

function me(): Player | null {
  return runner?.game.playerByClientID(CLIENT_ID) ?? null;
}

function pushMessage(text: string, kind = 0) {
  messages.push({ text, kind });
  if (messages.length > 50) messages.shift();
}

function hsl(h: number, s: number, l: number): [number, number, number] {
  const a = s * Math.min(l, 1 - l);
  const f = (n: number) => {
    const k = (n + h / 30) % 12;
    return l - a * Math.max(-1, Math.min(k - 3, 9 - k, 1));
  };
  return [
    Math.round(f(0) * 255),
    Math.round(f(8) * 255),
    Math.round(f(4) * 255),
  ];
}

function assignColor(p: Player) {
  const id = p.smallID();
  let rgb: [number, number, number];
  if (p.clientID() === CLIENT_ID) {
    rgb = [40, 120, 255];
  } else if (p.type() === PlayerType.Bot) {
    // Tribes: muted earth tones, like the web client.
    rgb = hsl((id * 47) % 50 + 20, 0.25, 0.45 + ((id * 13) % 20) / 100);
  } else {
    // Golden-angle hues keep neighbouring ids apart.
    rgb = hsl((id * 137.508) % 360, 0.65, 0.5 + ((id * 7) % 15) / 100);
  }
  colorTable[id * 4] = rgb[0];
  colorTable[id * 4 + 1] = rgb[1];
  colorTable[id * 4 + 2] = rgb[2];
  colorTable[id * 4 + 3] = 255;
}

function onUpdate(gu: GameUpdateViewData | ErrorUpdate) {
  if (!("updates" in gu)) {
    error = gu.errMsg;
    console.error("engine error:", gu.errMsg, gu.stack ?? "");
    return;
  }
  tileBatch.push(gu.packedTileUpdates);
  if (gu.playerNameViewData) names = gu.playerNameViewData;
  const myself = me();
  const mySmall = myself?.smallID() ?? -1;
  // Tribes and late joiners are added during the game.
  const all = runner!.game.allPlayers();
  if (all.length !== coloredPlayers) {
    for (const p of all) {
      if (colorTable[p.smallID() * 4 + 3] === 0) assignColor(p);
    }
    coloredPlayers = all.length;
  }
  for (const u of gu.updates[GameUpdateType.DisplayEvent]) {
    if (u.playerID !== null && u.playerID !== mySmall) continue;
    const params: Record<string, unknown> = { ...(u.params ?? {}) };
    if (u.goldAmount !== undefined) params.gold = u.goldAmount.toString();
    const text = u.message.includes(".") ? t(u.message, params) : u.message;
    pushMessage(text, messageKind(u.messageType));
  }
  for (const u of gu.updates[GameUpdateType.AllianceRequest]) {
    if (u.recipientID !== mySmall) continue;
    const from = runner!.game.playerBySmallID(u.requestorID);
    if (from.isPlayer()) {
      pushMessage(
        t("events_display.request_alliance", { name: from.displayName() }),
        3,
      );
    }
  }
  for (const u of gu.updates[GameUpdateType.Win]) {
    const w = u.winner;
    if (w === undefined) continue;
    if (w[0] === "player") {
      const p = runner!.game.playerByClientID(w[1]);
      winnerText =
        w[1] === CLIENT_ID
          ? t("win_modal.you_won")
          : t("win_modal.other_won", { player: p?.displayName() ?? w[1] });
    } else {
      winnerText = t("win_modal.other_won", { player: w[1] });
    }
  }
}

async function start(opts: StartOptions): Promise<void> {
  const entry = mapList()[opts.map];
  const base = ROMFS + "maps/" + entry.folder + "/";
  const manifest = loadJson(base + "manifest.json") as MapManifest;
  const bin = (name: string) => {
    const b = host.readFile(base + name);
    if (b === null) throw new Error("missing " + base + name);
    return new Uint8Array(b);
  };
  const difficulty = [
    Difficulty.Easy,
    Difficulty.Medium,
    Difficulty.Hard,
    Difficulty.Impossible,
  ][opts.difficulty];
  const gameStart: GameStartInfo = {
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
      instantBuild: false,
      randomSpawn: false,
      disabledUnits: [],
    },
  } as unknown as GameStartInfo;
  runner = await createGameRunner(
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
  for (const p of runner.game.allPlayers()) assignColor(p);
  pushMessage(t("heads_up_message.choose_spawn"), 1);
}

function messageKind(type: MessageType): number {
  switch (type) {
    case MessageType.CONQUERED_PLAYER:
    case MessageType.CAPTURED_ENEMY_UNIT:
    case MessageType.SAM_HIT:
    case MessageType.ALLIANCE_ACCEPTED:
    case MessageType.DONATION_RECEIVED:
      return 1;
    case MessageType.ATTACK_FAILED:
    case MessageType.MIRV_INBOUND:
    case MessageType.NUKE_INBOUND:
    case MessageType.HYDROGEN_BOMB_INBOUND:
    case MessageType.NAVAL_INVASION_INBOUND:
    case MessageType.UNIT_DESTROYED:
    case MessageType.ALLIANCE_REJECTED:
    case MessageType.ALLIANCE_BROKEN:
      return 2;
    case MessageType.ALLIANCE_REQUEST:
    case MessageType.RENEW_ALLIANCE:
      return 3;
    default:
      return 0;
  }
}

function intent(i: Intent) {
  pendingIntents.push(i);
}

function troopsToSend(): number {
  const p = me();
  return p ? p.troops() * attackRatio : 0;
}

// --- API for the native host ------------------------------------------------

const OF = {
  /** Loads the language strings; `lang` is an OpenFront locale like "es". */
  init(lang: string): void {
    fallback = loadJson(ROMFS + "lang/en.json") ?? {};
    strings = (lang !== "en" && loadJson(ROMFS + "lang/" + lang + ".json")) || fallback;
    portStrings = PORT_STRINGS[lang.split("-")[0]] ?? PORT_STRINGS.en;
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

  /** Starts a game; the host polls `ready()` while QuickJS runs the jobs. */
  start(opts: StartOptions): void {
    runner = null;
    error = null;
    winnerText = null;
    messages = [];
    names = {};
    pendingIntents = [];
    turnNumber = 0;
    tileBatch = [];
    colorTable = new Uint8Array(4096 * 4);
    coloredPlayers = 0;
    startOptions = opts;
    start(opts).catch((e: unknown) => {
      error = e instanceof Error ? e.message + "\n" + e.stack : String(e);
      console.error("start failed:", error);
    });
  },

  ready(): boolean {
    return runner !== null;
  },

  error(): string | null {
    return error;
  },

  restart(): void {
    if (startOptions) OF.start(startOptions);
  },

  width(): number {
    return runner!.game.width();
  },

  height(): number {
    return runner!.game.height();
  },

  /** The engine's own terrain (uint8) and tile state (uint16) arrays. */
  terrainBuffer(): ArrayBuffer {
    const map = runner!.game.map() as any;
    return map.terrain.buffer;
  },

  stateBuffer(): ArrayBuffer {
    const map = runner!.game.map() as any;
    return map.state.buffer;
  },

  colors(): ArrayBuffer {
    return colorTable.buffer;
  },

  /** Runs one turn (one tick) with the intents queued since the last one. */
  tick(): boolean {
    if (runner === null || winnerText !== null) return false;
    const intents: StampedIntent[] = pendingIntents.map(
      (i) => ({ ...i, clientID: CLIENT_ID }) as StampedIntent,
    );
    pendingIntents = [];
    runner.addTurn({ turnNumber: turnNumber++, intents });
    return runner.executeNextTick();
  },

  /** Tile refs changed since the last call, as [ref, state] uint32 pairs. */
  tileUpdates(): ArrayBuffer {
    if (tileBatch.length === 1) {
      lastTiles = tileBatch[0];
    } else {
      let n = 0;
      for (const b of tileBatch) n += b.length;
      lastTiles = new Uint32Array(n);
      let o = 0;
      for (const b of tileBatch) {
        lastTiles.set(b, o);
        o += b.length;
      }
    }
    tileBatch = [];
    // Copy into a fresh buffer the host may read until the next call.
    return lastTiles.slice().buffer;
  },

  /** Name labels: flat [smallID, x, y, size, troops] per living player. */
  labels(): Float64Array {
    const g = runner!.game;
    const out: number[] = [];
    for (const p of g.players()) {
      if (!p.isAlive()) continue;
      const nv = names[p.id()];
      if (!nv || nv.size <= 0) continue;
      out.push(p.smallID(), nv.x, nv.y, nv.size, p.troops());
    }
    return new Float64Array(out);
  },

  playerName(smallID: number): string {
    const p = runner!.game.playerBySmallID(smallID);
    return p.isPlayer() ? p.displayName() : "";
  },

  /**
   * Units to draw: flat [kind, x, y, ownerSmallID, level] int32 records.
   * kind is the index into UNIT_KINDS (shared with src/units.h).
   */
  units(): Int32Array {
    const g = runner!.game;
    const out: number[] = [];
    for (const u of g.units(UNIT_KINDS)) {
      if (!u.isActive()) continue;
      const tile = u.tile();
      out.push(
        UNIT_KINDS.indexOf(u.type()),
        g.x(tile),
        g.y(tile),
        u.owner().smallID(),
        u.level(),
      );
    }
    return new Int32Array(out);
  },

  hud(): Record<string, unknown> {
    const g = runner!.game;
    const p = me();
    const leaders = g
      .players()
      .filter((x) => x.isAlive())
      .sort((a, b) => b.numTilesOwned() - a.numTilesOwned())
      .slice(0, 5)
      .map((x) => ({
        id: x.smallID(),
        name: x.displayName(),
        pct: (x.numTilesOwned() / g.totalLandTiles()) * 100,
      }));
    return {
      tick: g.ticks(),
      seconds: g.elapsedGameSeconds(),
      spawn: g.inSpawnPhase(),
      spawned: p?.hasSpawned() ?? false,
      alive: p?.isAlive() ?? false,
      me: p?.smallID() ?? -1,
      troops: p?.troops() ?? 0,
      maxTroops: p ? g.config().maxTroops(p) : 0,
      gold: p ? Number(p.gold()) : 0,
      tiles: p?.numTilesOwned() ?? 0,
      pct: p ? (p.numTilesOwned() / g.totalLandTiles()) * 100 : 0,
      attacks: p?.outgoingAttacks().length ?? 0,
      ratio: attackRatio,
      pendingAlliances: p?.incomingAllianceRequests().length ?? 0,
      winner: winnerText,
      leaders,
    };
  },

  /** Drains the message log. */
  messages(): Message[] {
    const m = messages;
    messages = [];
    return m;
  },

  setAttackRatio(r: number): void {
    attackRatio = Math.max(0.01, Math.min(1, r));
  },

  /**
   * The main action on a tile (A button / tap): spawn during the spawn
   * phase, otherwise attack the owner or send a boat, as the web client does
   * on a left click.
   */
  primary(x: number, y: number): string {
    const g = runner!.game;
    if (!g.isValidCoord(x, y)) return "";
    const tile = g.ref(x, y);
    if (g.inSpawnPhase()) {
      if (g.isLand(tile) && !g.hasOwner(tile)) {
        intent({ type: "spawn", tile });
        return "spawn";
      }
      return "";
    }
    const p = me();
    if (p === null || !p.isAlive()) return "";
    const actions = runner!.playerActions(p.id(), x, y, [UnitType.TransportShip]);
    if (actions.canAttack) {
      const owner = g.owner(tile);
      intent({
        type: "attack",
        targetID: owner.isPlayer() ? owner.id() : null,
        troops: troopsToSend(),
      });
      return "attack";
    }
    if (g.isLand(tile) && boatTarget(actions.buildableUnits, tile) !== false) {
      intent({ type: "boat", troops: troopsToSend(), dst: tile });
      return "boat";
    }
    return "";
  },

  /** Boat attack to a tile, wherever it is. */
  boat(x: number, y: number): boolean {
    const p = me();
    if (p === null || runner === null) return false;
    const g = runner.game;
    if (!g.isValidCoord(x, y)) return false;
    const tile = g.ref(x, y);
    const b = runner.playerBuildables(p.id(), x, y, [UnitType.TransportShip]);
    if (boatTarget(b, tile, true) === false) return false;
    intent({ type: "boat", troops: troopsToSend(), dst: tile });
    return true;
  },

  /** The build menu for a tile: [{kind, name, cost, canBuild}]. */
  buildMenu(x: number, y: number): Record<string, unknown>[] {
    const p = me();
    if (p === null || runner === null) return [];
    const list = runner.playerBuildables(p.id(), x, y, BUILD_KINDS);
    const gold = p.gold();
    return BUILD_KINDS.map((type) => {
      const bu = list.find((b) => b.type === type);
      const ok = bu !== undefined && (bu.canBuild !== false || bu.canUpgrade !== false);
      return {
        kind: UNIT_KINDS.indexOf(type),
        name: t("unit_type." + UNIT_KEYS[type]),
        cost: bu ? Number(bu.cost) : 0,
        canBuild: ok && bu !== undefined && bu.cost <= gold,
      };
    });
  },

  build(kind: number, x: number, y: number): boolean {
    const p = me();
    if (p === null || runner === null) return false;
    const type = UNIT_KINDS[kind];
    const list = runner.playerBuildables(p.id(), x, y, [type as any]);
    const bu = list.find((b) => b.type === type);
    if (!bu) return false;
    if (bu.canUpgrade !== false) {
      intent({ type: "upgrade_structure", unit: type, unitId: bu.canUpgrade });
      return true;
    }
    if (bu.canBuild === false) return false;
    intent({ type: "build_unit", unit: type, tile: bu.canBuild });
    return true;
  },

  /**
   * What the player menu can do with the owner of a tile:
   * {id, name, ally, canAlly, canBreak, canTarget, canEmbargo}.
   */
  playerMenu(x: number, y: number): Record<string, unknown> | null {
    const p = me();
    if (p === null || runner === null || runner.game.inSpawnPhase()) return null;
    const g = runner.game;
    if (!g.isValidCoord(x, y)) return null;
    const owner = g.owner(g.ref(x, y));
    if (!owner.isPlayer() || owner === p) return null;
    const a = runner.playerActions(p.id(), x, y, null);
    const i = a.interaction;
    return {
      id: owner.smallID(),
      name: owner.displayName(),
      troops: owner.troops(),
      ally: p.isAlliedWith(owner),
      canAlly: i?.canSendAllianceRequest ?? false,
      canBreak: i?.canBreakAlliance ?? false,
      canTarget: i?.canTarget ?? false,
      canEmbargo: i?.canEmbargo ?? false,
      canDonate: i?.canDonateTroops ?? false,
    };
  },

  /** action: 0 ally request, 1 break, 2 target, 3 embargo, 4 donate troops. */
  playerAction(smallID: number, action: number): void {
    const other = runner!.game.playerBySmallID(smallID);
    if (!other.isPlayer()) return;
    const id = other.id();
    switch (action) {
      case 0:
        intent({ type: "allianceRequest", recipient: id });
        break;
      case 1:
        intent({ type: "breakAlliance", recipient: id });
        break;
      case 2:
        intent({ type: "targetPlayer", target: id });
        break;
      case 3:
        intent({ type: "embargo", targetID: id, action: "start" });
        break;
      case 4:
        intent({ type: "donate_troops", recipient: id, troops: troopsToSend() });
        break;
    }
  },

  /** Accepts (or rejects) every pending incoming alliance request. */
  answerAlliances(accept: boolean): number {
    const p = me();
    if (p === null) return 0;
    const reqs = p.incomingAllianceRequests();
    for (const r of reqs) {
      const id = r.requestor().id();
      intent(
        accept
          ? { type: "allianceRequest", recipient: id }
          : { type: "allianceReject", requestor: id },
      );
    }
    return reqs.length;
  },

  /** Retreats the most recent outgoing attack. */
  cancelLastAttack(): boolean {
    const p = me();
    const attacks = p?.outgoingAttacks() ?? [];
    if (attacks.length === 0) return false;
    intent({ type: "cancel_attack", attackID: attacks[attacks.length - 1].id() });
    return true;
  },

  /** Where the player's territory is, for centring the camera: [x, y]. */
  myCenter(): number[] | null {
    const p = me();
    if (p === null || runner === null) return null;
    const nv = names[p.id()];
    if (nv) return [nv.x, nv.y];
    const s = p.spawnTile();
    if (s === undefined) return null;
    return [runner.game.x(s), runner.game.y(s)];
  },
};

function boatTarget(
  buildables: { type: string; canBuild: TileRef | false }[],
  tile: TileRef,
  anyDistance = false,
): TileRef | false {
  const bu = buildables.find((b) => b.type === UnitType.TransportShip);
  const src = bu?.canBuild ?? false;
  if (src === false || anyDistance) return src;
  // Same auto-boat range as the web client's left click.
  return runner!.game.euclideanDistSquared(tile, src) < 100 * 100 ? src : false;
}

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

const BUILD_KINDS = [
  UnitType.City,
  UnitType.Port,
  UnitType.DefensePost,
  UnitType.Factory,
  UnitType.MissileSilo,
  UnitType.SAMLauncher,
  UnitType.Warship,
  UnitType.AtomBomb,
  UnitType.HydrogenBomb,
  UnitType.MIRV,
] as const;

const UNIT_KEYS: Record<string, string> = {
  [UnitType.City]: "city",
  [UnitType.Port]: "port",
  [UnitType.DefensePost]: "defense_post",
  [UnitType.Factory]: "factory",
  [UnitType.MissileSilo]: "missile_silo",
  [UnitType.SAMLauncher]: "sam_launcher",
  [UnitType.Warship]: "warship",
  [UnitType.AtomBomb]: "atom_bomb",
  [UnitType.HydrogenBomb]: "hydrogen_bomb",
  [UnitType.MIRV]: "mirv",
};

(globalThis as any).OF = OF;
