// engine-lib's DetMath with exp and log computed by src/native.c (detExp,
// detLog): the same operations in the same order, built without fused
// multiply-add, so the same bits. They run per conquered tile
// (EngineConfig.attackLogic's largeTerritoryBonus). scripts/build-bundle.mjs
// points the engine's imports of DetMath here.
import * as Js from "../../packages/engine-lib/src/DetMath";

export * from "../../packages/engine-lib/src/DetMath";

const native = (globalThis as any).__native;
export const exp: typeof Js.exp = native?.detExp ?? Js.exp;
export const log: typeof Js.log = native?.detLog ?? Js.log;
