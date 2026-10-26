// Replaces engine methods whose per-tile loops dominate a tick with the
// native kernels of src/native.c. Each one computes exactly what the method
// it replaces does, in the same order (the game stays deterministic).
import { AttackExecution } from "@openfront/engine/execution/AttackExecution";
import { PlayerExecution } from "@openfront/engine/execution/PlayerExecution";

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
