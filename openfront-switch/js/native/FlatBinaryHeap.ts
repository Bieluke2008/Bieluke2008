// FlatBinaryHeap: the native one (src/native.c) when the host provides it,
// else OpenFront's own. scripts/build-bundle.mjs points the engine's imports
// of FlatBinaryHeap here.
import { FlatBinaryHeap as JsFlatBinaryHeap } from "../../packages/engine/src/execution/utils/FlatBinaryHeap";

const native = (globalThis as any).__native;
export const FlatBinaryHeap: typeof JsFlatBinaryHeap = native?.FlatBinaryHeap ?? JsFlatBinaryHeap;
export type FlatBinaryHeap = JsFlatBinaryHeap;
