// Swaps the map accessors the engine calls per tile (GameMapImpl.isLand,
// ownerID, neighbors4...) for the native ones of src/native.c, on the map
// objects and on GameImpl, whose versions only delegate to its map.
import { Game } from "@openfront/engine/game/Game";
import { installKernels } from "./kernels";

const MAP_METHODS = [
  "isLand", "isImpassable", "isOcean", "isShoreline", "magnitude", "terrainByte", "ownerID", "hasOwner",
  "hasFallout", "hasDefenseBonus", "isBorder", "isWater", "isShore", "cost", "terrainType", "x", "y",
  "isValidCoord", "neighbors4", "neighbors8", "forEachNeighbor", "forEachNeighborWithDiag", "manhattanDist",
  "euclideanDistSquared", "isOceanShore", "isOnEdgeOfMap",
];

// GameImpl methods that are exactly `return this._map.<name>(...)`.
const GAME_METHODS = [
  "x", "y", "isValidCoord", "isLand", "isImpassable", "isOceanShore", "isOcean", "isShoreline", "magnitude",
  "terrainByte", "ownerID", "hasOwner", "hasFallout", "isBorder", "forEachNeighbor", "neighbors4", "neighbors8",
  "isWater", "isShore", "cost", "terrainType", "manhattanDist", "euclideanDistSquared", "forEachNeighborWithDiag",
];

export function bindNativeMaps(game: Game): boolean {
  const native = (globalThis as any).__native;
  if (!native) return false;
  const bind = (target: object, map: any, names: string[]) =>
    native.bindMap(target, map.terrain, map.state, map.width(), map.height(), names);
  native.unbindMaps();
  const main = game.map() as any;
  bind(main, main, MAP_METHODS);
  const mini = game.miniMap() as any;
  if (mini && mini !== main) bind(mini, mini, MAP_METHODS);
  bind(game, main, GAME_METHODS);
  installKernels();
  return true;
}
