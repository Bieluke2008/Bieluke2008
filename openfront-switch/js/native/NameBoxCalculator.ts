// NameBoxCalculator with placeName's grid scan and rectangle search done by
// src/native.c (nameRect). scripts/build-bundle.mjs points the engine's
// imports of NameBoxCalculator here.
import { Cell, NameViewData } from "@openfront/engine-api/game/GameTypes";
import { calculateBoundingBox } from "@openfront/engine-lib/Util";
import { Game, Player } from "../../packages/engine/src/game/Game";
import * as Js from "../../packages/engine/src/game/NameBoxCalculator";

export * from "../../packages/engine/src/game/NameBoxCalculator";

const native = (globalThis as any).__native;

function nativePlaceName(game: Game, player: Player): NameViewData {
  const boundingBox = player.largestClusterBoundingBox ?? calculateBoundingBox(game, player.borderTiles());
  const { min, max } = boundingBox;
  if (![min.x, min.y, max.x, max.y].every(Number.isInteger) || max.x < min.x || max.y < min.y) {
    return Js.placeName(game, player);
  }

  let scalingFactor: number;
  const size = Math.min(max.x - min.x, max.y - min.y);
  if (size < 25) scalingFactor = 1;
  else if (size < 50) scalingFactor = 2;
  else if (size < 100) scalingFactor = 4;
  else if (size < 250) scalingFactor = 8;
  else if (size < 500) scalingFactor = 16;
  else scalingFactor = 32;

  const [x, y, w, h] = native.nameRect(player.smallID(), min.x, min.y, max.x, max.y, scalingFactor);
  const largestRectangle = {
    x: x * scalingFactor,
    y: y * scalingFactor,
    width: w * scalingFactor,
    height: h * scalingFactor,
  };
  let center = new Cell(
    Math.floor(largestRectangle.x + largestRectangle.width / 2 + min.x),
    Math.floor(largestRectangle.y + largestRectangle.height / 2 + min.y),
  );
  const fontSize = Js.calculateFontSize(largestRectangle, player.displayName());
  center = new Cell(center.x, center.y - fontSize / 3);
  return { x: Math.ceil(center.x), y: Math.ceil(center.y), size: fontSize };
}

export const placeName: typeof Js.placeName = native?.nameRect ? nativePlaceName : Js.placeName;
