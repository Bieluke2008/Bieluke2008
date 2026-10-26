// Ports of the web client's always-on HUD pieces (src/client/hud/layers/):
// ControlPanel, UnitDisplay, the player stats table, GameRightSidebar's
// timer, HeadsUpMessage and AttacksDisplay. One JSON model per tick.
import {
  BuildMenus,
  GameType,
  MessageType,
  UnitType,
} from "@openfront/engine-api/game/GameTypes";
import { GameUpdateType, GameUpdateViewData } from "@openfront/engine-api/game/GameUpdates";
import { renderNumber, renderTroops } from "@openfront/engine-lib/Format";
import { Player } from "@openfront/engine/game/Game";
import { eventsModel, sounds } from "./events";
import { t } from "./i18n";
import {
  attackingTroops,
  CLIENT_ID,
  game,
  intent,
  isFriendly,
  me,
  playerBySmall,
  S,
  totalUnitLevels,
} from "./state";

// --- ControlPanel state ------------------------------------------------------

let lastTroopIncreaseRate = 0;
let troopRateIsIncreasing = true;
let goldGain: { amount: string; until: number } | null = null;
let winner: { text: string; won: boolean; id: number } | null = null;
let winnerSeq = 0;
let deathShown = false;

export function resetHud(): void {
  lastTroopIncreaseRate = 0;
  troopRateIsIncreasing = true;
  goldGain = null;
  winner = null;
  deathShown = false;
}

export function hudUpdates(gu: GameUpdateViewData): void {
  const my = me();
  const u = gu.updates;
  if (my) {
    const myID = my.id();
    const gain = (amount: bigint) => {
      goldGain = { amount: renderNumber(amount), until: host_now() + 2000 };
    };
    for (const ev of u[GameUpdateType.BonusEvent]) {
      if (ev.player === myID && ev.gold > 0) gain(BigInt(ev.gold));
    }
    for (const ev of u[GameUpdateType.ConquestEvent]) {
      if (ev.conquerorId === myID && ev.gold > 0n) gain(ev.gold);
    }
    for (const ev of u[GameUpdateType.DonateEvent]) {
      if (ev.donationType === "gold" && ev.recipientId === myID && ev.amount > 0n) gain(ev.amount);
    }
  }
  // WinModal: "You died" once, when the local player is eliminated.
  if (my && !deathShown && my.hasSpawned() && !my.isAlive()) {
    deathShown = true;
    winner = { text: t("win_modal.died"), won: false, id: ++winnerSeq };
  }
  for (const w of u[GameUpdateType.Win]) {
    const win = w.winner;
    if (!win) continue;
    if (win[0] === "player") {
      const p = game().playerByClientID(win[1]);
      winner =
        win[1] === CLIENT_ID
          ? { text: t("win_modal.you_won"), won: true, id: ++winnerSeq }
          : { text: t("win_modal.other_won", { player: p?.displayName() ?? win[1] }), won: false, id: ++winnerSeq };
    } else if (win[0] === "nation") {
      winner = { text: t("win_modal.nation_won", { nation: win[1] }), won: false, id: ++winnerSeq };
    } else {
      winner = { text: t("win_modal.other_won", { player: win[1] }), won: false, id: ++winnerSeq };
    }
  }
}

declare const host: { now(): number };
function host_now(): number {
  return host.now();
}

function controlPanel(my: Player | null): Record<string, unknown> | null {
  const g = game();
  if (my === null || !my.isAlive() || g.inSpawnPhase()) return null;
  const cfg = g.config();
  const rate = cfg.troopIncreaseRate(my);
  troopRateIsIncreasing = rate >= lastTroopIncreaseRate;
  lastTroopIncreaseRate = rate;
  const troops = my.troops();
  let notif: { type: string; text: string } | null = null;
  if (troops < 10000 && troops > 0) {
    notif = { type: "warning", text: t("control_panel.low_troops_warning") };
  }
  if (goldGain !== null && host_now() > goldGain.until) goldGain = null;
  return {
    rate: "+" + renderTroops(rate * 10) + "/s",
    rateUp: troopRateIsIncreasing,
    troops: renderTroops(troops),
    max: renderTroops(cfg.maxTroops(my)),
    troopFrac: troops / Math.max(cfg.maxTroops(my), 1),
    attackFrac: attackingTroops(my) / Math.max(cfg.maxTroops(my), 1),
    gold: renderNumber(my.gold()),
    goldGain: goldGain?.amount ?? null,
    ratio: S.ui.attackRatio,
    ratioLabel:
      (S.ui.attackRatio * 100).toFixed(0) + "% (" + renderTroops(troops * S.ui.attackRatio) + ")",
    notif,
  };
}

// --- UnitDisplay (hotbar) ------------------------------------------------------

const HOTBAR: { type: UnitType; icon: string; key: string; counted: boolean; hotkey: string }[] = [
  { type: UnitType.City, icon: "CityIconWhite", key: "city", counted: true, hotkey: "1" },
  { type: UnitType.Factory, icon: "FactoryIconWhite", key: "factory", counted: true, hotkey: "2" },
  { type: UnitType.Port, icon: "PortIcon", key: "port", counted: true, hotkey: "3" },
  { type: UnitType.DefensePost, icon: "ShieldIconWhite", key: "defense_post", counted: true, hotkey: "4" },
  { type: UnitType.MissileSilo, icon: "MissileSiloIconWhite", key: "missile_silo", counted: true, hotkey: "5" },
  { type: UnitType.SAMLauncher, icon: "SamLauncherIconWhite", key: "sam_launcher", counted: true, hotkey: "6" },
  { type: UnitType.Warship, icon: "BattleshipIconWhite", key: "warship", counted: true, hotkey: "7" },
  { type: UnitType.AtomBomb, icon: "NukeIconWhite", key: "atom_bomb", counted: false, hotkey: "8" },
  { type: UnitType.HydrogenBomb, icon: "MushroomCloudIconWhite", key: "hydrogen_bomb", counted: false, hotkey: "9" },
  { type: UnitType.MIRV, icon: "MIRVIcon", key: "mirv", counted: false, hotkey: "0" },
];

export function hotbarTypes(): UnitType[] {
  return HOTBAR.map((h) => h.type);
}

function hotbar(my: Player | null): Record<string, unknown>[] | null {
  const g = game();
  if (my === null || !my.isAlive() || g.inSpawnPhase()) return null;
  const buildables = S.runner!.playerBuildables(my.id(), undefined, undefined, BuildMenus.types);
  const cost = (type: UnitType) => buildables.find((b) => b.type === type)?.cost ?? 0n;
  const canBuild = (type: UnitType) => {
    if (g.config().isUnitDisabled(type)) return false;
    const c = cost(type) <= my.gold();
    switch (type) {
      case UnitType.AtomBomb:
      case UnitType.HydrogenBomb:
      case UnitType.MIRV:
        return c && my.units(UnitType.MissileSilo).length > 0;
      case UnitType.Warship:
        return c && my.units(UnitType.Port).length > 0;
      default:
        return c;
    }
  };
  return HOTBAR.filter((h) => !g.config().isUnitDisabled(h.type)).map((h) => ({
    type: h.type,
    icon: h.icon,
    hotkey: h.hotkey,
    count: h.counted ? renderNumber(totalUnitLevels(my, h.type)) : null,
    can: canBuild(h.type),
    sel: S.ui.ghostStructure === h.type,
    name: t("unit_type." + h.key),
    desc: t("build_menu.desc." + h.key),
    cost: renderNumber(cost(h.type)),
    warship: h.type === UnitType.Warship ? t("build_menu.warship_shift_hint") : null,
  }));
}

// --- Player stats table (leaderboard) ------------------------------------------

export const LB = { sortKey: "tiles", sortAsc: false, offset: 0 };

function leaderboard(my: Player | null): Record<string, unknown> | null {
  const g = game();
  const validTiles = g.numLandTiles() - g.numTilesWithFallout();
  const cfg = g.config();
  const value = (p: Player): number => {
    switch (LB.sortKey) {
      case "gold":
        return Number(p.gold());
      case "maxtroops":
        return cfg.maxTroops(p);
      default:
        return p.numTilesOwned();
    }
  };
  const rows = g
    .players()
    .filter((p) => p.isAlive())
    .map((p) => ({ p, v: value(p) }))
    .sort((a, b) => (LB.sortAsc ? a.v - b.v : b.v - a.v));
  const myIndex = my ? rows.findIndex((r) => r.p === my) : -1;
  const row = (p: Player, rank: number) => ({
    rank,
    sid: p.smallID(),
    name: p.name(),
    clan: p.clanTag() ?? "",
    pct: ((validTiles > 0 ? p.numTilesOwned() / validTiles : 0) * 100).toFixed(1) + "%",
    gold: renderNumber(p.gold()),
    maxt: renderTroops(cfg.maxTroops(p)),
    emph: my !== null && (p === my || p.isOnSameTeam(my)),
  });
  // Like StatsTable: my row is pinned below the list when it is further
  // down than the visible rows; the list scrolls.
  const PINNED_VISIBLE_THRESHOLD = 4;
  const pinned = myIndex > PINNED_VISIBLE_THRESHOLD ? row(my!, myIndex + 1) : null;
  const list = rows.filter((_, i) => pinned === null || i !== myIndex);
  const visible = pinned ? 4 : 5;
  const maxOffset = Math.max(0, list.length - visible);
  if (LB.offset > maxOffset) LB.offset = maxOffset;
  const shown = list.slice(LB.offset, LB.offset + visible).map((r, i) => {
    const index = LB.offset + i;
    return row(r.p, pinned !== null && index >= myIndex ? index + 2 : index + 1);
  });
  return {
    rows: shown,
    pinned,
    total: list.length,
    offset: LB.offset,
    sort: LB.sortKey,
    asc: LB.sortAsc,
    gameID: (S.runner as any).execManager?.gameID?.() ?? "",
  };
}

// --- AttacksDisplay ---------------------------------------------------------

function attacks(my: Player | null): Record<string, unknown>[] {
  const g = game();
  const out: Record<string, unknown>[] = [];
  if (my === null || !my.isAlive() || g.inSpawnPhase()) return out;
  for (const a of my.outgoingAttacks()) {
    const target = a.target();
    if (target.isPlayer()) {
      out.push({
        kind: "out",
        id: a.id(),
        troops: renderTroops(a.troops()),
        name: target.displayName(),
        retreating: a.retreating(),
        focus: target.smallID(),
      });
    } else {
      out.push({
        kind: "land",
        id: a.id(),
        troops: renderTroops(a.troops()),
        name: t("help_modal.ui_wilderness"),
        retreating: a.retreating(),
      });
    }
  }
  for (const boat of my.units(UnitType.TransportShip)) {
    if (!boat.isActive()) continue;
    const target = boat.targetTile();
    let name = "";
    if (target !== undefined) {
      const owner = g.owner(target);
      name = owner.isPlayer() ? owner.displayName() : t("help_modal.ui_wilderness");
    }
    out.push({
      kind: "boat",
      id: boat.id(),
      troops: renderTroops(boat.troops()),
      name,
      retreating: boat.transportShipState().isRetreating,
      owner: my.smallID(),
    });
  }
  for (const a of my.incomingAttacks()) {
    const attacker = a.attacker();
    out.push({
      kind: "in",
      id: a.id(),
      troops: renderTroops(a.troops()),
      name: attacker.displayName(),
      retreating: a.retreating(),
      focus: attacker.smallID(),
      raw: a.troops(),
    });
  }
  for (const p of g.players()) {
    if (p === my) continue;
    for (const boat of p.units(UnitType.TransportShip)) {
      if (!boat.isActive()) continue;
      const target = boat.targetTile();
      if (target === undefined || g.owner(target) !== my) continue;
      out.push({
        kind: "inBoat",
        id: boat.id(),
        troops: renderTroops(boat.troops()),
        name: p.displayName(),
        owner: p.smallID(),
      });
    }
  }
  return out;
}

/** AttacksDisplay buttons. */
export function attackAction(kind: string, id: string): void {
  const my = me();
  if (!my) return;
  if (kind === "cancel") intent({ type: "cancel_attack", attackID: id });
  else if (kind === "cancelBoat") intent({ type: "cancel_boat", unitID: Number(id) });
  else if (kind === "retaliate") {
    const attack = my.incomingAttacks().find((a) => a.id() === id);
    if (!attack) return;
    const available = S.ui.attackRatio * my.troops();
    const troops = Number.isFinite(attack.troops()) ? Math.min(attack.troops(), available) : available;
    intent({ type: "attack", targetID: attack.attacker().id(), troops });
  }
}

// --- GameRightSidebar timer and HeadsUpMessage ---------------------------------

function timer(): string {
  const g = game();
  const cfg = g.config().gameConfig();
  let seconds: number;
  if (g.inSpawnPhase()) {
    seconds = cfg.gameType === GameType.Singleplayer ? (cfg.maxTimerValue ?? 0) * 60 : 0;
  } else {
    const elapsed = Math.floor(g.elapsedGameSeconds());
    seconds = cfg.maxTimerValue ? Math.max(0, cfg.maxTimerValue * 60 - elapsed) : elapsed;
  }
  const pad = (n: number) => (n < 10 ? `0${n}` : `${n}`);
  const h = Math.floor(seconds / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  const s = Math.floor(seconds % 60);
  return h !== 0 ? `${pad(h)}:${pad(m)}:${pad(s)}` : `${pad(m)}:${pad(s)}`;
}

function headsUp(): string | null {
  const g = game();
  if (S.paused) return t("heads_up_message.singleplayer_game_paused");
  if (g.inSpawnPhase()) {
    return g.config().isRandomSpawn()
      ? t("heads_up_message.random_spawn")
      : t("heads_up_message.choose_spawn");
  }
  return null;
}

/** SettingsModal's modifier tags for this game. */
function modifiers(): string[] {
  const cfg = game().config().gameConfig();
  const out: string[] = [];
  if (cfg.infiniteGold) out.push(t("game_settings.infinite_gold"));
  if (cfg.infiniteTroops) out.push(t("game_settings.infinite_troops"));
  if (cfg.instantBuild) out.push(t("game_settings.instant_build"));
  if (cfg.gameMapSize === "Compact") out.push(t("game_settings.compact_map"));
  return out;
}

// --- The model -------------------------------------------------------------------

export function frameModel(): string {
  const g = game();
  const my = me();
  const drained = sounds.splice(0, sounds.length);
  return JSON.stringify({
    tick: g.ticks(),
    spawn: g.inSpawnPhase(),
    paused: S.paused,
    timer: timer(),
    heads: headsUp(),
    me: my ? { sid: my.smallID(), alive: my.isAlive(), spawned: my.hasSpawned() } : null,
    cp: controlPanel(my),
    hotbar: hotbar(my),
    lb: leaderboard(my),
    attacks: attacks(my),
    events: eventsModel(),
    winner,
    modifiers: modifiers(),
    sounds: drained,
    msgType: MessageType.CHAT,
  });
}

export { isFriendly, playerBySmall };
