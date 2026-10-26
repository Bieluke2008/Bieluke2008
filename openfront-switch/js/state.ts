// Shared game state of the adapter, and the PlayerView helpers the web
// client's HUD code relies on, written against the engine's own objects.
import { TileRef } from "@openfront/engine-api/game/GameMap";
import { PlayerType, UnitType } from "@openfront/engine-api/game/GameTypes";
import { Intent } from "@openfront/engine-api/Schemas";
import { GameRunner } from "@openfront/engine/GameRunner";
import { Game, Player, Unit } from "@openfront/engine/game/Game";

export const CLIENT_ID = "SWITCH01";

export const S = {
  runner: null as GameRunner | null,
  pendingIntents: [] as Intent[],
  /** UIState of the web client. */
  ui: {
    attackRatio: 0.2,
    ghostStructure: null as UnitType | null,
    rocketDirectionUp: true,
  },
  paused: false,
};

export function game(): Game {
  return S.runner!.game;
}

export function me(): Player | null {
  return S.runner?.game.playerByClientID(CLIENT_ID) ?? null;
}

export function intent(i: Intent): void {
  S.pendingIntents.push(i);
}

export function playerBySmall(id: number): Player | null {
  const p = game().playerBySmallID(id);
  return p.isPlayer() ? p : null;
}

// --- PlayerView equivalents -------------------------------------------------

export function totalUnitLevels(p: Player, type: UnitType): number {
  let n = 0;
  for (const u of p.units(type)) {
    if (!u.isUnderConstruction()) n += u.level();
  }
  return n;
}

export function readyMissileCount(p: Player): number {
  let n = 0;
  for (const silo of p.units(UnitType.MissileSilo)) {
    if (silo.isUnderConstruction()) continue;
    n += Math.max(0, silo.level() - silo.missileTimerQueue().length);
  }
  return n;
}

export function deleteUnitCooldown(p: Player): number {
  const g = game();
  const last = (p as any).lastDeleteUnitTick as number;
  return (
    Math.max(0, g.config().deleteUnitCooldown() - (g.ticks() + 1 - last)) / 10
  );
}

export function isFriendly(a: Player, b: Player): boolean {
  return a.isAlliedWith(b) || a.isOnSameTeam(b);
}

export function isRequestingAllianceWith(a: Player, b: Player): boolean {
  return a.outgoingAllianceRequests().some((r) => r.recipient() === b);
}

export function hasEmbargo(a: Player, b: Player): boolean {
  return a.hasEmbargoAgainst(b) || b.hasEmbargoAgainst(a);
}

/** PlayerImpl has it; the Player interface does not declare it. */
export function traitorRemainingTicks(p: Player): number {
  return p.isTraitor() ? ((p as any).getTraitorRemainingTicks() as number) : 0;
}

export function attackingTroops(p: Player): number {
  let n = 0;
  for (const a of p.outgoingAttacks()) n += a.troops();
  return n;
}

export function playerTypeKey(p: Player): string {
  switch (p.type()) {
    case PlayerType.Bot:
      return "player_type.bot";
    case PlayerType.Nation:
      return "player_type.nation";
    default:
      return "player_type.player";
  }
}

/** Flag code of a nation (resources/flags/<code>.svg), from the manifest. */
const flags = new Map<string, string>();
export function setNationFlags(list: { name: string; flag?: string }[]): void {
  flags.clear();
  for (const n of list) if (n.flag) flags.set(n.name, n.flag);
}
export function flagOf(p: Player): string {
  if (p.type() !== PlayerType.Nation) return "";
  return flags.get(p.name()) ?? "";
}

export function firstPlace(): Player | null {
  let best: Player | null = null;
  for (const p of game().players()) {
    if (!p.isAlive()) continue;
    if (best === null || p.numTilesOwned() > best.numTilesOwned()) best = p;
  }
  return best;
}

export function unitTile(u: Unit): TileRef {
  return u.tile();
}

export function xy(tile: TileRef): [number, number] {
  const g = game();
  return [g.x(tile), g.y(tile)];
}
