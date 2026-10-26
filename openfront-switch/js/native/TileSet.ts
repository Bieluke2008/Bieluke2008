// TileSet: the native one (src/native.c) when the host provides it, else
// OpenFront's own. scripts/build-bundle.mjs points the engine's imports of
// TileSet here.
import { TileSet as JsTileSet } from "../../packages/engine-lib/src/game/TileSet";

const native = (globalThis as any).__native;
export const TileSet: typeof JsTileSet = native?.TileSet ?? JsTileSet;
export type TileSet = JsTileSet;
