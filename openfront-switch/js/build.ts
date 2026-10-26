// Port of the web client's BuildPreviewController: the "ghost" structure
// that follows the cursor after picking something on the hotbar, its range
// circle, the nuke trajectory preview, and placing it on click.
import { TileRef } from "@openfront/engine-api/game/GameMap";
import { BuildableUnit, bulkCost, Structures, UnitType } from "@openfront/engine-api/game/GameTypes";
import { listNukeBreakAlliance } from "@openfront/engine-lib/execution/NukeAlliance";
import { renderNumber } from "@openfront/engine-lib/Format";
import { buildNukeTrajectory, SAMInfo } from "../src/client/render/gl/utils/NukeTrajectory";
import { game, intent, me, readyMissileCount, S } from "./state";

const T_BLOCKED_DST = 0.9999;

function samThreatens(owner: number, teammates: Set<number>, allies: Set<number>, betrayed: ReadonlySet<number>): boolean {
  if (teammates.has(owner)) return false;
  return !allies.has(owner) || betrayed.has(owner);
}

function buildableAt(type: UnitType, tile: TileRef | undefined): BuildableUnit | undefined {
  const my = me();
  if (!my) return undefined;
  const g = game();
  const x = tile !== undefined ? g.x(tile) : undefined;
  const y = tile !== undefined ? g.y(tile) : undefined;
  return S.runner!.playerBuildables(my.id(), x, y, [type as any]).find((b) => b.type === type);
}

/** The ghost preview for `type` at tile x,y (GhostPreviewData + trajectory). */
export function ghost(type: UnitType, x: number, y: number): Record<string, unknown> | null {
  const g = game();
  const my = me();
  if (!my || !g.isValidCoord(x, y)) return null;
  let tile: TileRef | undefined = g.ref(x, y);
  const trajectoryTile = tile;
  if (g.isImpassable(tile)) tile = undefined;
  const u = buildableAt(type, tile);
  if (!u || tile === undefined) {
    return { valid: false, trajectory: trajectory(type, trajectoryTile) };
  }
  const placement = u.canUpgrade === false && Structures.has(u.type) && u.canBuild !== false ? u.canBuild : tile;
  let upgradeTarget: number | null = null;
  if (u.canUpgrade !== false) upgradeTarget = g.unit(u.canUpgrade)?.tile() ?? null;
  const cfg = g.config();
  let range = 0;
  switch (u.type) {
    case UnitType.SAMLauncher: {
      const lvl = u.canUpgrade !== false ? (g.unit(u.canUpgrade)?.level() ?? 0) + 1 : 1;
      range = cfg.samRange(lvl);
      break;
    }
    case UnitType.AtomBomb:
    case UnitType.HydrogenBomb:
      range = cfg.nukeMagnitudes(u.type).outer;
      break;
    case UnitType.Factory:
      range = cfg.trainStationMaxRange();
      break;
    case UnitType.DefensePost:
      range = cfg.defensePostRange();
      break;
  }
  const rangeTile = range > 0 && upgradeTarget !== null ? upgradeTarget : placement;
  const isNuke = u.type === UnitType.AtomBomb;
  const cost = bulkCost(u, 1);
  let canAfford = my.gold() >= cost;
  if (isNuke) canAfford &&= readyMissileCount(my) >= 1;
  return {
    valid: true,
    type: u.type,
    x: g.x(placement),
    y: g.y(placement),
    snap: placement !== tile,
    rx: g.x(rangeTile),
    ry: g.y(rangeTile),
    range,
    canBuild: u.canBuild !== false,
    canUpgrade: u.canUpgrade !== false,
    upgradeX: upgradeTarget !== null ? g.x(upgradeTarget) : -1,
    upgradeY: upgradeTarget !== null ? g.y(upgradeTarget) : -1,
    cost: renderNumber(cost),
    canAfford,
    trajectory: trajectory(type, trajectoryTile),
  };
}

function trajectory(type: UnitType, tile: TileRef): Record<string, number> | null {
  if (type !== UnitType.AtomBomb && type !== UnitType.HydrogenBomb) return null;
  const g = game();
  const my = me();
  if (!my) return null;
  const silos = my
    .units(UnitType.MissileSilo)
    .filter((u) => u.isActive() && !u.isInCooldown() && !u.isUnderConstruction());
  if (silos.length === 0) return null;
  const dx = g.x(tile);
  const dy = g.y(tile);
  silos.sort(
    (a, b) =>
      Math.abs(g.x(a.tile()) - dx) + Math.abs(g.y(a.tile()) - dy) - (Math.abs(g.x(b.tile()) - dx) + Math.abs(g.y(b.tile()) - dy)),
  );
  const silo = silos[0];
  const teammates = new Set<number>();
  for (const p of g.players()) if (my.isOnSameTeam(p)) teammates.add(p.smallID());
  const allies = new Set<number>();
  for (const a of my.allies()) allies.add(a.smallID());
  const betrayed: ReadonlySet<number> =
    allies.size > 0
      ? listNukeBreakAlliance({
          game: g as any,
          targetTile: tile,
          magnitude: g.config().nukeMagnitudes(type),
          threshold: g.config().nukeAllianceBreakThreshold(),
        })
      : new Set();
  const sams: SAMInfo[] = [];
  for (const s of g.units(UnitType.SAMLauncher)) {
    if (!s.isActive() || s.owner() === my) continue;
    if (!samThreatens(s.owner().smallID(), teammates, allies, betrayed)) continue;
    sams.push({ x: g.x(s.tile()), y: g.y(s.tile()), r: g.config().samRange(s.level()) });
  }
  const data = buildNukeTrajectory(
    g.x(silo.tile()),
    g.y(silo.tile()),
    dx,
    dy,
    g.height(),
    S.ui.rocketDirectionUp,
    sams,
  );
  if (g.isImpassable(tile)) data.tSamIntercept = Math.min(data.tSamIntercept, T_BLOCKED_DST);
  return data as unknown as Record<string, number>;
}

/**
 * A click with the ghost: build or upgrade like createStructure(). Returns
 * whether the ghost stays selected (nukes keep it for the next shot).
 */
export function placeGhost(type: UnitType, x: number, y: number): boolean {
  const g = game();
  if (!g.isValidCoord(x, y)) return true;
  const target = g.ref(x, y);
  const tile = g.isImpassable(target) ? undefined : target;
  const u = buildableAt(type, tile);
  if (!u || (u.canBuild === false && u.canUpgrade === false)) return false;
  if (u.canUpgrade !== false) {
    intent({ type: "upgrade_structure", unit: u.type, unitId: u.canUpgrade, amount: 1 });
    return false;
  }
  const isNuke = type === UnitType.AtomBomb;
  const rocketDirectionUp =
    type === UnitType.AtomBomb || type === UnitType.HydrogenBomb ? S.ui.rocketDirectionUp : undefined;
  intent({ type: "build_unit", unit: type, tile: target, rocketDirectionUp, amount: isNuke ? 1 : undefined });
  return type === UnitType.AtomBomb || type === UnitType.HydrogenBomb;
}
