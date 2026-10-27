// Multiplayer on an OpenFront server (LAN): the part of the web client's
// Transport and ClientGameRunner that talks to the game server. The native
// host owns the WebSocket (src/net.c) and hands every frame here; frames are
// encoded and decoded with OpenFront's own wire schemas, so the server sees a
// client like the web one.
import { GameMapSize, GameType } from "@openfront/engine-api/game/GameTypes";
import { GameUpdateType, GameUpdateViewData, HashUpdate } from "@openfront/engine-api/game/GameUpdates";
import { GameStartInfo, Intent, Turn } from "@openfront/engine-api/Schemas";
import { createGameWireContext, decodeServerMessage, encodeClientMessage } from "@openfront/shared/ZbinWire";
import type { ClientMessage, GameInfo, ServerMessage } from "@openfront/shared/WireSchemas";
import { configFromOptions, mapEntryOf, resetGame, setError, StartOptions, startRunner } from "./adapter";
import { t } from "./i18n";
import { S, setClientID } from "./state";

declare const host: {
  log(msg: string): void;
  now(): number;
  wsOpen(host: string, port: number, path: string): void;
  wsSend(data: ArrayBuffer): void;
  wsClose(): void;
};

type Phase = "idle" | "connecting" | "lobby" | "loading" | "game" | "closed";

const MP = {
  phase: "idle" as Phase,
  host: "",
  port: 0,
  workerPath: "",
  gameID: "",
  token: "",
  name: "",
  myClientID: "",
  lobby: null as GameInfo | null,
  error: "",
  ctx: undefined as ReturnType<typeof createGameWireContext> | undefined,
  start: null as GameStartInfo | null,
  startTurns: [] as Turn[],
  turns: [] as Turn[],
  turnsSeen: 0,
  lastPing: 0,
  notice: "",
};

export function mpActive(): boolean {
  return MP.phase !== "idle";
}

export function mpInGame(): boolean {
  return MP.phase === "game";
}

function send(msg: ClientMessage): void {
  const bytes = encodeClientMessage(msg, MP.ctx);
  host.wsSend(bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength) as ArrayBuffer);
}

export function sendIntent(intent: Intent): void {
  send({ type: "intent", intent } as ClientMessage);
}

/** Joins game `gameID` on the server at host:port (worker `workerPath`, e.g. "w0"). */
export function mpConnect(a: { host: string; port: number; workerPath: string; gameID: string; token: string; name: string }): void {
  host.wsClose();
  Object.assign(MP, {
    phase: "connecting",
    host: a.host,
    port: a.port,
    workerPath: a.workerPath,
    gameID: a.gameID,
    token: a.token,
    name: a.name,
    myClientID: "",
    lobby: null,
    error: "",
    ctx: undefined,
    start: null,
    startTurns: [],
    turns: [],
    turnsSeen: 0,
    notice: "",
  });
  host.wsOpen(a.host, a.port, "/" + a.workerPath);
}

export function mpLeave(): void {
  host.wsClose();
  MP.phase = "idle";
  MP.lobby = null;
  MP.turns = [];
}

function fail(msg: string): void {
  MP.error = msg;
  MP.phase = "closed";
  host.wsClose();
}

/** A WebSocket event from the host: 0 open, 1 message, 2 close. */
export function mpNetEvent(kind: number, data: ArrayBuffer | string, code: number): void {
  if (MP.phase === "idle") return;
  if (kind === 0) {
    send({
      type: "join",
      token: MP.token,
      gameID: MP.gameID,
      username: MP.name,
      clanTag: null,
      turnstileToken: null,
      gitCommit: "DEV",
    } as ClientMessage);
    return;
  }
  if (kind === 2) {
    if (MP.phase === "closed") return;
    const reason = typeof data === "string" ? data : "";
    fail(code === 1000 && reason === "" ? t("port.mp_closed") : `${t("port.mp_closed")} (${code} ${reason})`);
    return;
  }
  let msg: ServerMessage;
  try {
    msg = decodeServerMessage(new Uint8Array(data as ArrayBuffer), MP.ctx);
  } catch (e) {
    console.error("bad frame:", e);
    return;
  }
  onMessage(msg);
}

function onMessage(msg: ServerMessage): void {
  switch (msg.type) {
    case "lobby_info":
      MP.lobby = msg.lobby;
      MP.myClientID = msg.myClientID;
      if (MP.phase === "connecting") MP.phase = "lobby";
      break;
    case "prestart":
      if (msg.gameMapSize !== GameMapSize.Compact) MP.notice = t("port.mp_need_compact");
      break;
    case "start": {
      MP.ctx = createGameWireContext(msg.gameStartInfo.players);
      if (msg.myClientID) MP.myClientID = msg.myClientID;
      MP.start = msg.gameStartInfo as unknown as GameStartInfo;
      MP.startTurns = msg.turns as Turn[];
      if (MP.phase !== "game") MP.phase = "loading";
      break;
    }
    case "turn":
      if (MP.phase === "game" || MP.phase === "loading") addTurn(msg.turn as Turn);
      break;
    case "desync":
      MP.notice = t("port.mp_desync");
      break;
    case "error":
      fail(
        msg.error === "version_mismatch"
          ? t("port.mp_version", { commit: msg.gitCommit ?? "?" })
          : msg.error + (msg.message ? ": " + msg.message : ""),
      );
      break;
    default:
      break;
  }
}

/** ClientGameRunner: turns in order, filling any gap the start message left. */
function addTurn(turn: Turn): void {
  if (turn.turnNumber < MP.turnsSeen) return;
  while (turn.turnNumber > MP.turnsSeen) {
    MP.turns.push({ turnNumber: MP.turnsSeen, intents: [] });
    MP.turnsSeen++;
  }
  MP.turns.push(turn);
  MP.turnsSeen++;
}

/**
 * Called by the host every loop: sends queued intents and pings. Returns the
 * number of server turns waiting, or -1 when the game should be started
 * (OF.mpStartGame) first.
 */
export function mpPoll(): number {
  if (MP.phase === "idle" || MP.phase === "closed") return 0;
  const now = host.now();
  if (now - MP.lastPing > 5000) {
    MP.lastPing = now;
    send({ type: "ping", sentAt: Math.floor(now) } as ClientMessage);
  }
  if (MP.phase === "loading" && MP.start !== null && S.runner === null) return -1;
  if (MP.phase !== "game") return 0;
  if (S.pendingIntents.length > 0) {
    for (const i of S.pendingIntents) sendIntent(i);
    S.pendingIntents = [];
  }
  return MP.turns.length;
}

/** Creates the game from the server's start message. */
export async function mpStartGame(): Promise<void> {
  const info = MP.start!;
  resetGame();
  MP.turns = [];
  MP.turnsSeen = 0;
  for (const turn of MP.startTurns) addTurn(turn);
  MP.phase = "game";
  if (info.config.gameMapSize !== GameMapSize.Compact) {
    setError(t("port.mp_need_compact"));
    fail(t("port.mp_need_compact"));
    return;
  }
  const entry = mapEntryOf(info.config.gameMap);
  if (!entry) {
    fail(t("port.mp_no_map"));
    setError(t("port.mp_no_map"));
    return;
  }
  setClientID(MP.myClientID);
  await startRunner(info, MP.myClientID, entry);
}

/** HeadsUpMessage in a multiplayer game: a lost connection or desync, the creator's pause, catching up. */
export function mpHeadsUp(paused: boolean): string | null {
  if (MP.phase === "idle") return null;
  if (MP.error) return MP.error;
  if (MP.notice) return MP.notice;
  if (paused) return t("heads_up_message.multiplayer_game_paused");
  if (MP.turns.length > 20) return t("heads_up_message.catching_up");
  return null;
}

/** The next server turn for GameRunner.addTurn, or null. */
export function nextTurn(): Turn | null {
  return MP.turns.shift() ?? null;
}

/** ClientGameRunner: report the engine's state hashes so the server can spot desyncs. */
export function mpUpdate(gu: GameUpdateViewData): void {
  if (MP.phase !== "game") return;
  for (const hu of gu.updates[GameUpdateType.Hash] as HashUpdate[]) {
    if ((globalThis as any).MP_HASHLOG) console.log("H", hu.tick, hu.hash);
    send({ type: "hash", turnNumber: hu.tick, hash: hu.hash } as ClientMessage);
  }
}

/** Host only: the lobby's settings from the Solo modal's options. */
export function mpConfig(opts: StartOptions): void {
  if (MP.phase !== "lobby") return;
  const { gameType: _ignored, ...config } = configFromOptions(opts, GameType.Private);
  sendIntent({ type: "update_game_config", config } as Intent);
}

/** Host only: HostLobbyModal's start button. */
export function mpStartTimer(): void {
  if (MP.phase === "lobby") sendIntent({ type: "toggle_game_start_timer" } as Intent);
}

export function mpPause(paused: boolean): void {
  if (MP.phase === "game") sendIntent({ type: "toggle_pause", paused } as Intent);
}

/** The lobby for the multiplayer screens. */
export function mpLobby(): unknown {
  const l = MP.lobby;
  const c = l?.gameConfig;
  const entry = c ? mapEntryOf(c.gameMap) : undefined;
  return {
    phase: MP.phase,
    error: MP.error,
    notice: MP.notice,
    gameID: MP.gameID,
    me: MP.myClientID,
    isHost: !!l && l.lobbyCreatorClientID === MP.myClientID,
    starting: !!l?.startsAt,
    players: (l?.clients ?? []).map((cl) => ({ id: cl.clientID, name: cl.username, host: cl.clientID === l?.lobbyCreatorClientID })),
    map: c ? (entry ? entry.name : c.gameMap) : "",
    mapFolder: entry?.folder ?? "",
    compact: c ? c.gameMapSize === GameMapSize.Compact : true,
    mode: c?.gameMode ?? "",
    difficulty: c ? t("difficulty." + String(c.difficulty).toLowerCase()) : "",
    bots: c?.bots ?? 0,
  };
}
