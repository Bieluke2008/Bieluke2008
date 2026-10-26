// Replaces engine methods whose per-tile loops dominate a tick with the
// native kernels of src/native.c. Each one computes exactly what the method
// it replaces does, in the same order (the game stays deterministic).
import { AttackExecution } from "@openfront/engine/execution/AttackExecution";
import { PlayerExecution } from "@openfront/engine/execution/PlayerExecution";
import { GameImpl } from "@openfront/engine/game/GameImpl";
import { PlayerImpl } from "@openfront/engine/game/PlayerImpl";
import { NationAttackBehavior } from "@openfront/engine/execution/nation/NationAttackBehavior";
import { bumpTraversalGeneration, tileTraversalScratch } from "@openfront/engine/game/TileTraversalScratch";
import { SpatialQuery } from "@openfront/engine/pathfinding/spatial/SpatialQuery";
import { AStarWaterBounded } from "@openfront/engine/pathfinding/algorithms/AStar.WaterBounded";
import { WaterManager } from "@openfront/engine/game/WaterManager";
import { SharedWaterCache } from "@openfront/engine/execution/nation/SharedWaterCache";
import { PathFinding } from "@openfront/engine/pathfinding/PathFinder";

// AttackExecution.ts's module constant (not exported).
const malusForRetreat = 25;

let installed = false;

/** The mini map src/native.c has in slot 1 (bindNativeMaps), or null. */
let boundMini: object | null = null;
export function setBoundMini(mini: object | null): void {
  boundMini = mini;
}

export function installKernels(): boolean {
  const native = (globalThis as any).__native;
  if (!native?.addNeighbors) return false;
  if (installed) return true;
  installed = true;

  const attack = AttackExecution.prototype as any;
  attack.addNeighbors = function (this: any, tile: number) {
    if (this.attack === null) {
      throw new Error("Attack not initialized");
    }
    native.addNeighbors(
      tile,
      this.mg.ticks(),
      this.targetSmallID,
      this.ownerSmallID,
      this.random,
      this.toConquer,
      this.attack,
    );
  };

  // tick() as in the TS, with the loop's skipped tiles handled by
  // nextConquerTile (dequeue, drop from the border, recheck) in one call.
  attack.tick = function (this: any, _ticks: number) {
    if (this.attack === null) {
      throw new Error("Attack not initialized");
    }
    let troopCount = this.attack.troops();
    const targetIsPlayer = this.target.isPlayer();
    const targetPlayer = targetIsPlayer ? this.target : null;

    if (!this.attack.isActive()) {
      this.active = false;
      return;
    }
    if (this.attack.retreated()) {
      if (targetIsPlayer) {
        this.retreat(malusForRetreat);
      } else {
        this.retreat();
      }
      this.active = false;
      return;
    }
    if (this.attack.retreating()) {
      return;
    }
    if (targetPlayer && this._owner.isFriendly(targetPlayer)) {
      this.retreat();
      return;
    }

    const borderSize = this.attack.borderSize() + this.random.nextInt(0, 5);
    let tickBudget = 1;
    const config = this.mg.config();
    while (tickBudget > 0) {
      if (troopCount < 1) {
        this.attack.delete();
        this.active = false;
        return;
      }
      const tileToConquer = native.nextConquerTile(this.toConquer, this.attack, this.ownerSmallID, this.targetSmallID);
      if (tileToConquer < 0) {
        this.refreshToConquer();
        this.retreat();
        return;
      }
      this.addNeighbors(tileToConquer);
      const { attackerTroopLoss, defenderTroopLoss, tickFraction } = config.attackLogic(
        this.attackLogicInput(troopCount, tileToConquer, borderSize),
      );
      tickBudget -= tickFraction;
      troopCount -= attackerTroopLoss;
      this.attack.setTroops(troopCount);
      if (targetPlayer) {
        targetPlayer.removeTroops(defenderTroopLoss);
      }
      this._owner.conquer(tileToConquer);
      this.handleDeadDefender();
    }
  };

  (GameImpl.prototype as any).updateBorders = function (this: any, tile: number) {
    native.updateBorders(tile, this._playersBySmallID);
  };

  // closestShore / closestReachableShore: bfsNearest's walk in C
  // (shoreNearest); the water-component half of the predicate stays here.
  const spatial = SpatialQuery.prototype as any;
  const shoreNearest = (game: any, from: number, maxDist: number, ownerId: number, extra: any) => {
    if (!game.map().isValidRef(from)) return null;
    const scratch = tileTraversalScratch(game);
    const gen = bumpTraversalGeneration(scratch);
    return native.shoreNearest(from, maxDist, ownerId, scratch.visited, gen, extra);
  };
  spatial.closestShore = function (this: any, owner: any, tile: number, maxDist: number = 50) {
    return shoreNearest(this.game, tile, maxDist, owner.smallID(), null);
  };
  spatial.closestReachableShore = function (
    this: any,
    targetOwner: any,
    attacker: any,
    tile: number,
    maxDist: number = 50,
  ) {
    const gm = this.game;
    const targetId = targetOwner.smallID();
    const reachable = this.reachableComponents(attacker);
    if (reachable.size === 0) return null;
    const reachableComponent = (t: number) => {
      const component = gm.getWaterComponent(t);
      return component !== null && reachable.has(component);
    };
    if (
      maxDist >= 0 &&
      gm.map().isValidRef(tile) &&
      gm.isShore(tile) &&
      gm.isLand(tile) &&
      gm.ownerID(tile) === targetId &&
      reachableComponent(tile)
    ) {
      return tile;
    }
    return shoreNearest(gm, tile, maxDist, targetId, reachableComponent);
  };

  // AStarWaterBounded.searchBounded: the stamp bump and area check as in the
  // TS, the search itself in C (astarBounded).
  (AStarWaterBounded.prototype as any).searchBounded = function (this: any, start: any, goal: number, bounds: any) {
    this.stamp++;
    if (this.stamp > 0xffffffff) {
      this.closedStamp.fill(0);
      this.gScoreStamp.fill(0);
      this.stamp = 1;
    }
    const { minX, maxX, minY, maxY } = bounds;
    if ((maxX - minX + 1) * (maxY - minY + 1) > this.closedStamp.length) return null;
    return native.astarBounded(
      this.terrain,
      this.mapWidth,
      this.closedStamp,
      this.gScoreStamp,
      this.gScore,
      this.cameFrom,
      this.stamp,
      start,
      goal,
      minX,
      maxX,
      minY,
      maxY,
      this.heuristicWeight,
      this.maxIterations,
    );
  };

  // WaterManager.getWaterComponent: the mini-map neighbour search and the
  // union-find lookup in C (waterComponent); the TS handles what C leaves
  // undefined (no graph yet, a coordinate it would throw on).
  const water = WaterManager.prototype as any;
  const jsWaterComponent = water.getWaterComponent;
  water.getWaterComponent = function (this: any, tile: number) {
    const graph = this._miniWaterGraph;
    if (!graph) return 0;
    const cc = graph._waterComponents;
    if (cc && this.miniMap === boundMini) {
      const r = native.waterComponent(tile, cc);
      if (r !== undefined) return r;
    }
    return jsWaterComponent.call(this, tile);
  };

  // The mini water graph's ConnectedComponents, when the native lookup can
  // stand in for WaterManager.getWaterComponent.
  const waterCC = (game: any): any => {
    const wm = game._waterManager;
    if (!wm || wm.miniMap !== boundMini) return null;
    return wm._miniWaterGraph?._waterComponents ?? null;
  };

  // closestShoreByWater: the border scan for shores on the target's water
  // component in C (shoreScan mode 0); the rest as in the TS.
  const jsClosestShoreByWater = spatial.closestShoreByWater;
  spatial.closestShoreByWater = function (this: any, owner: any, target: number) {
    const gm = this.game;
    const cc = waterCC(gm);
    if (cc === null || !owner.isPlayer()) return jsClosestShoreByWater.call(this, owner, target);
    if (!gm.isWater(target) && !gm.isShore(target)) return null;
    const targetComponent = gm.getWaterComponent(target);
    if (targetComponent === null) return null;
    const shores = native.shoreScan(owner.borderTiles(), cc, 0, targetComponent);
    if (shores === undefined) return jsClosestShoreByWater.call(this, owner, target);
    if (shores.length === 0) return null;
    const path = PathFinding.Water(gm).findPath(shores, target);
    if (!path || path.length === 0) return null;
    return this.refineStartTile(path, shores, gm);
  };

  // reachableComponents: same memo rule as the TS (the attacker's tiles and
  // the map's water unchanged), the scan in C (shoreScan mode 1).
  const reachable = new WeakMap<object, { tileVersion: number; waterVersion: number; components: Set<number> }>();
  const jsReachableComponents = spatial.reachableComponents;
  spatial.reachableComponents = function (this: any, attacker: any) {
    const gm = this.game;
    const tileVersion = attacker.tileChangeVersion();
    const waterVersion = gm.map().waterVersion();
    const cached = reachable.get(attacker);
    if (cached !== undefined && cached.tileVersion === tileVersion && cached.waterVersion === waterVersion) {
      return cached.components;
    }
    const cc = waterCC(gm);
    const list = cc === null ? undefined : native.shoreScan(attacker.borderTiles(), cc, 1, 0);
    const components: Set<number> = list === undefined ? jsReachableComponents.call(this, attacker) : new Set(list);
    reachable.set(attacker, { tileVersion, waterVersion, components });
    return components;
  };

  // SharedWaterCache.waterFor: the scan of the water next to the border in C
  // (lakeScan); the memo as in the TS.
  const shared = SharedWaterCache.prototype as any;
  const jsWaterFor = shared.waterFor;
  shared.waterFor = function (this: any, player: any, waterVersion: number) {
    const tileVersion = player.tileChangeVersion();
    const cached = this.playerWater.get(player);
    if (cached !== undefined && cached.tileVersion === tileVersion && cached.waterVersion === waterVersion) {
      return cached;
    }
    const cc = waterCC(this.game);
    const scan = cc === null ? undefined : native.lakeScan(player.borderTiles(), cc);
    if (scan === undefined) return jsWaterFor.call(this, player, waterVersion);
    const entry = { tileVersion, waterVersion, hasOcean: scan[0] as boolean, lakes: new Set<number>(scan.slice(1)) };
    this.playerWater.set(player, entry);
    return entry;
  };

  // PlayerImpl.computeNearby (with shoreReachableNeighbors) in C: the owners
  // come back as small ids in the Set's insertion order.
  (PlayerImpl.prototype as any).computeNearby = function (this: any) {
    const ids: number[] = native.nearbyIds(this.borderTiles(), this.smallID());
    return ids.map((id) => this.mg.playerBySmallID(id));
  };

  // NationAttackBehavior.chooseAttack as in the TS, its border scan in C
  // (borderScan; ids in discovery order, which the stable sort relies on).
  (NationAttackBehavior.prototype as any).chooseAttack = function (this: any) {
    const scan: any[] = native.borderScan(this.player.borderTiles(), this.player.smallID());
    const borderHasNonNukedTerraNullius = scan[0] as boolean;
    const borderingPlayerSet = new Set<any>();
    for (let i = 1; i < scan.length; i++) borderingPlayerSet.add(this.game.playerBySmallID(scan[i]));
    this.landNeighbors = new Set(borderingPlayerSet);
    const playerNeighbors = this.player.nearby();
    for (const n of playerNeighbors) {
      if (n.isPlayer()) borderingPlayerSet.add(n);
    }
    const borderingPlayers = [...borderingPlayerSet].sort((a, b) => a.troops() - b.troops());
    const borderingFriends = borderingPlayers.filter((o) => this.player?.isFriendly(o) === true);
    const borderingEnemies = borderingPlayers.filter((o) => this.player?.isFriendly(o) === false);

    const hasNonNukedTerraNullius = borderHasNonNukedTerraNullius || playerNeighbors.some((n: any) => !n.isPlayer());
    if (hasNonNukedTerraNullius) {
      if (this.sendAttack(this.game.terraNullius())) return;
    }

    const holdBoats = this.holdsBoatsUnderAttack();
    if (borderingEnemies.length === 0) {
      if (!holdBoats && this.random.chance(5)) {
        this.attackWithRandomBoat();
      }
    } else {
      if (!holdBoats && this.random.chance(10)) {
        this.attackWithRandomBoat(borderingEnemies);
        return;
      }

      this.allianceBehavior.maybeSendAllianceRequests(borderingEnemies);
    }

    this.attackBestTarget(borderingFriends, borderingEnemies);
  };

  // PlayerImpl.canAttack as in the TS; for an unowned tile the flood over
  // the unowned land around it (up to 200 tiles away) runs in C.
  (PlayerImpl.prototype as any).canAttack = function (this: any, tile: number) {
    const owner = this.mg.owner(tile);
    if (owner === this) return false;
    if (owner.isPlayer() && !this.canAttackPlayer(owner)) return false;
    if (!this.mg.isLand(tile) || this.mg.isImpassable(tile)) return false;
    if (this.mg.hasOwner(tile)) return this.sharesBorderWith(owner);
    return native.unownedTouches(tile, this.smallID(), 200);
  };

  const player = PlayerExecution.prototype as any;
  player.calculateClusters = function (this: any) {
    const borderTiles = this.player.borderTiles();
    if (borderTiles.size === 0) return { clusters: [], boxes: new Int32Array(0) };
    const state = this.traversalState();
    const borderGen = this.bumpGenerations(2);
    return native.clusters(borderTiles, state.visited, borderGen, borderGen + 1);
  };
  return true;
}
