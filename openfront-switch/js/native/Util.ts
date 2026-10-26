// engine-lib's Util with calculateBoundingBox reading a native TileSet's
// members in C (__bbox, src/native.c). scripts/build-bundle.mjs points the
// engine's imports of Util here.
import { Cell } from "@openfront/engine-api/game/GameTypes";
import type { GameMap, TileRef } from "@openfront/engine-api/game/GameMap";
import * as Js from "../../packages/engine-lib/src/Util";

export * from "../../packages/engine-lib/src/Util";

export function calculateBoundingBox(gm: GameMap, borderTiles: Iterable<TileRef>): { min: Cell; max: Cell } {
  const bbox = (borderTiles as any).__bbox;
  if (typeof bbox !== "function") return Js.calculateBoundingBox(gm, borderTiles);
  const b: number[] | null = bbox.call(borderTiles, gm.width());
  if (b === null) return { min: new Cell(Infinity, Infinity), max: new Cell(-Infinity, -Infinity) };
  return { min: new Cell(b[0], b[1]), max: new Cell(b[2], b[3]) };
}
