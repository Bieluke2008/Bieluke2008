// The game server a Switch runs when it hosts a game for other Switches in
// the same network: the part of OpenFront's GameServer that a private lobby
// needs (joining, the host's settings and start, one turn every 100 ms with
// everyone's intents, pause, pings and the desync check), speaking the same
// protocol, so the players' side is the client used for OpenFront's own
// servers (mp.ts). Runs in its own QuickJS runtime on its own thread
// (src/lanhost.c), which owns the sockets.
import {
  Difficulty,
  GameMapSize,
  GameMapType,
  GameMode,
  GameType,
} from "@openfront/engine-api/game/GameTypes";
import type { ClientID, GameConfig, GameConfigPatch, Intent, StampedIntent, Turn } from "@openfront/engine-api/Schemas";
import type { ClientMessage, ServerMessage } from "@openfront/shared/WireSchemas";
import { createGameWireContext, decodeClientMessage, encodeServerMessage } from "@openfront/shared/ZbinWire";
import { applyGameConfigPatch } from "../src/server/ConfigPatch";
import { DesyncDetector } from "../src/server/DesyncDetector";

declare const host: {
  log(msg: string): void;
  now(): number;
  send(conn: number, data: ArrayBuffer): void;
  close(conn: number): void;
};

const TURN_MS = 100;
// GameServer: prestart goes out when the countdown ends, start two seconds later.
const START_DELAY_MS = 1000;
const PRESTART_MS = 2000;

interface Client {
  conn: number;
  clientID: ClientID;
  persistentID: string;
  username: string;
  hashes: Map<number, number>;
  started: boolean; // got the start message: its frames use the game's dictionary
}

const ID_CHARS = "123456789abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ";

/** SharedUtil.generateID: 8 chars without the look-alikes 0, l, I, O. */
function generateID(): string {
  let id = "";
  for (let i = 0; i < 8; i++) id += ID_CHARS[Math.floor(Math.random() * ID_CHARS.length)];
  return id;
}

const G = {
  gameID: "",
  version: "",
  createdAt: 0,
  creatorPID: "",
  config: null as unknown as GameConfig,
  clients: new Map<number, Client>(), // by connection
  known: new Map<string, { clientID: ClientID; username: string }>(), // everyone who joined, by persistent id
  order: [] as ClientID[], // join order: the start message's players
  lobbyCreatorID: "",
  startsAt: 0,
  prestartSent: false,
  started: false,
  ctx: undefined as ReturnType<typeof createGameWireContext> | undefined,
  startInfo: null as unknown,
  turns: [] as Turn[],
  pending: [] as StampedIntent[],
  paused: false,
  nextTurnAt: 0,
  lastLobbyInfo: 0,
  desync: new DesyncDetector(),
};

function send(c: Client, msg: ServerMessage): void {
  const bytes = encodeServerMessage(msg, c.started ? G.ctx : undefined);
  host.send(c.conn, bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength) as ArrayBuffer);
}

function active(): Client[] {
  return [...G.clients.values()];
}

function lobbyInfo(): void {
  const now = Math.floor(host.now());
  G.lastLobbyInfo = now;
  const clients = G.order
    .map((id) => [...G.known.values()].find((k) => k.clientID === id))
    .filter((k) => k !== undefined && active().some((c) => c.clientID === k.clientID))
    .map((k) => ({ clientID: k!.clientID, username: k!.username, clanTag: null }));
  for (const c of active()) {
    send(c, {
      type: "lobby_info",
      myClientID: c.clientID,
      lobby: {
        gameID: G.gameID,
        clients,
        lobbyCreatorClientID: G.lobbyCreatorID || undefined,
        startsAt: G.startsAt || undefined,
        serverTime: now,
        gameConfig: G.config,
      },
    } as ServerMessage);
  }
}

function sendStart(c: Client, fromTurn: number): void {
  c.started = true;
  send(c, {
    type: "start",
    turns: G.turns.slice(fromTurn),
    gameStartInfo: G.startInfo,
    lobbyCreatedAt: G.createdAt,
    myClientID: c.clientID,
  } as ServerMessage);
}

function startGame(): void {
  const players = G.order
    .filter((id) => active().some((c) => c.clientID === id))
    .map((id) => {
      const k = [...G.known.values()].find((x) => x.clientID === id)!;
      return { clientID: id, username: k.username, clanTag: null, cosmetics: {}, isLobbyCreator: id === G.lobbyCreatorID };
    });
  G.startInfo = {
    gameID: G.gameID,
    lobbyCreatedAt: G.createdAt,
    listed: false,
    config: G.config,
    players,
  };
  G.ctx = createGameWireContext(players);
  G.started = true;
  for (const c of active()) sendStart(c, 0);
  G.nextTurnAt = host.now() + TURN_MS;
}

function endTurn(): void {
  const turn: Turn = { turnNumber: G.turns.length, intents: G.pending };
  G.pending = [];
  G.turns.push(turn);
  for (const c of active()) if (c.started) send(c, { type: "turn", turn } as ServerMessage);
  // DesyncDetector: compare the hashes reported for the turn 10 back.
  const check = G.desync.check(G.turns.length, active() as never);
  if (check && check.mostCommonHash !== null) {
    for (const c of G.desync.record(check.outOfSyncClients) as unknown as Client[]) {
      send(c, {
        type: "desync",
        turn: check.turn,
        correctHash: check.mostCommonHash,
        clientsWithCorrectHash: active().length - check.outOfSyncClients.length,
        totalActiveClients: active().length,
        yourHash: c.hashes.get(check.turn),
      } as ServerMessage);
    }
  }
}

function onJoin(c: Client | undefined, conn: number, msg: { gameID: string; token: string; username?: string }): void {
  if (msg.gameID !== G.gameID) {
    host.close(conn);
    return;
  }
  const pid = msg.token;
  const old = G.known.get(pid);
  if (G.started && !old) {
    // Like a private lobby: no new players once the game is running.
    const err = encodeServerMessage({ type: "error", error: "game_started" } as ServerMessage, undefined);
    host.send(conn, err.buffer.slice(err.byteOffset, err.byteOffset + err.byteLength) as ArrayBuffer);
    host.close(conn);
    return;
  }
  // A reconnect: drop the old connection of the same player.
  for (const other of active()) {
    if (other.persistentID === pid && other.conn !== conn) {
      G.clients.delete(other.conn);
      host.close(other.conn);
    }
  }
  const clientID = old?.clientID ?? generateID();
  const username = (msg.username ?? old?.username ?? "Player").slice(0, 27);
  G.known.set(pid, { clientID, username });
  if (!old) G.order.push(clientID);
  if (pid === G.creatorPID) G.lobbyCreatorID = clientID;
  const client: Client = c ?? { conn, clientID, persistentID: pid, username, hashes: new Map(), started: false };
  Object.assign(client, { clientID, persistentID: pid, username });
  G.clients.set(conn, client);
  if (G.started) {
    sendStart(client, 0);
    G.pending.push({ type: "mark_disconnected", clientID, isDisconnected: false } as StampedIntent);
  } else {
    lobbyInfo();
  }
}

function onIntent(c: Client, intent: Intent): void {
  const creator = c.clientID === G.lobbyCreatorID;
  switch (intent.type) {
    case "update_game_config":
      if (!creator || G.started) return;
      applyGameConfigPatch(G.config, intent.config as GameConfigPatch);
      // The Switch only has the compact maps; a lobby here is always private.
      G.config.gameMapSize = GameMapSize.Compact;
      G.config.gameType = GameType.Private;
      lobbyInfo();
      return;
    case "toggle_game_start_timer":
      if (!creator || G.started) return;
      G.startsAt = G.startsAt ? 0 : Math.floor(host.now()) + START_DELAY_MS;
      G.prestartSent = false;
      lobbyInfo();
      return;
    case "toggle_pause": {
      if (!creator || !G.started) return;
      const stamped = { ...intent, clientID: c.clientID } as StampedIntent;
      // GameServer: flush the pause into a turn before stopping; clear it before resuming.
      if (intent.paused) {
        G.pending.push(stamped);
        endTurn();
        G.paused = true;
      } else {
        G.paused = false;
        G.pending.push(stamped);
        endTurn();
        G.nextTurnAt = host.now() + TURN_MS;
      }
      return;
    }
    case "kick_player":
    case "mark_disconnected":
      return;
    default:
      if (G.started && !G.paused) G.pending.push({ ...intent, clientID: c.clientID } as StampedIntent);
  }
}

const SERVER = {
  /** Creates the lobby; returns its id. */
  init(a: { creatorToken: string; version: string }): string {
    G.gameID = generateID();
    G.version = a.version;
    G.createdAt = Math.floor(host.now());
    G.creatorPID = a.creatorToken;
    // GameManager.createGame's defaults, with the port's compact map.
    G.config = {
      donateGold: false,
      donateTroops: false,
      gameMap: GameMapType.Europe,
      gameType: GameType.Private,
      gameMapSize: GameMapSize.Compact,
      difficulty: Difficulty.Easy,
      nations: "default",
      infiniteGold: false,
      infiniteTroops: false,
      maxTimerValue: undefined,
      instantBuild: false,
      randomSpawn: false,
      gameMode: GameMode.FFA,
      bots: 400,
      disabledUnits: [],
    } as GameConfig;
    return G.gameID;
  },

  message(conn: number, data: ArrayBuffer): void {
    const c = G.clients.get(conn);
    let msg: ClientMessage;
    try {
      msg = decodeClientMessage(new Uint8Array(data), c?.started ? G.ctx : undefined);
    } catch (e) {
      console.error("bad client frame:", e);
      return;
    }
    if (msg.type === "join" || msg.type === "rejoin") {
      onJoin(c, conn, msg as never);
      return;
    }
    if (!c) return;
    switch (msg.type) {
      case "intent":
        onIntent(c, msg.intent);
        break;
      case "hash":
        c.hashes.set(msg.turnNumber, msg.hash);
        break;
      case "ping":
        send(c, { type: "pong", sentAt: msg.sentAt } as ServerMessage);
        break;
      default:
        break;
    }
  },

  close(conn: number): void {
    const c = G.clients.get(conn);
    if (!c) return;
    G.clients.delete(conn);
    if (G.started) G.pending.push({ type: "mark_disconnected", clientID: c.clientID, isDisconnected: true } as StampedIntent);
    else lobbyInfo();
  },

  /** Called by the host's loop: the countdown, the turns, the lobby refresh. */
  poll(): void {
    const now = host.now();
    if (!G.started) {
      if (G.startsAt && now >= G.startsAt && !G.prestartSent) {
        G.prestartSent = true;
        for (const c of active()) {
          send(c, { type: "prestart", gameMap: G.config.gameMap, gameMapSize: G.config.gameMapSize } as ServerMessage);
        }
      }
      if (G.startsAt && now >= G.startsAt + PRESTART_MS) startGame();
      else if (now - G.lastLobbyInfo >= 1000) lobbyInfo();
      return;
    }
    if (G.paused) return;
    // One turn every 100 ms; after a long stall, resume instead of bursting.
    if (now - G.nextTurnAt > 1000) G.nextTurnAt = now;
    while (now >= G.nextTurnAt) {
      endTurn();
      G.nextTurnAt += TURN_MS;
    }
  },

  /** GET /api/game/:id/exists, plus the port version so other Switches can check theirs. */
  exists(id: string): string {
    return JSON.stringify({ exists: id === G.gameID, switch: G.version });
  },

  /** The lobby's broadcast for the other Switches' game list. */
  announce(port: number): string {
    const k = [...G.known.values()].find((x) => x.clientID === G.lobbyCreatorID);
    return JSON.stringify({
      of: "openfront-switch",
      v: G.version,
      id: G.gameID,
      port,
      host: k?.username ?? "",
      map: Object.keys(GameMapType).find((k) => (GameMapType as Record<string, string>)[k] === G.config.gameMap)?.toLowerCase() ?? "",
      players: G.clients.size,
      started: G.started,
    });
  },
};

(globalThis as any).SERVER = SERVER;
