// Replaces engine methods whose per-tile loops dominate a tick with the
// native kernels of src/native.c. Each one computes exactly what the method
// it replaces does, in the same order (the game stays deterministic).
import { AttackExecution } from "@openfront/engine/execution/AttackExecution";
import { PlayerExecution } from "@openfront/engine/execution/PlayerExecution";
import { GameImpl } from "@openfront/engine/game/GameImpl";

// AttackExecution.ts's module constant (not exported).
const malusForRetreat = 25;

let installed = false;

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
