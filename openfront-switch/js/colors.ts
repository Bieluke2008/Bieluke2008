// Player colours from the web client's own theme code (ThemeProvider's
// SettingsTheme with default-theme.json), so territories get exactly the
// colours they get in the browser.
import { createThemeSettings } from "../src/client/render/gl/RenderSettings";
import { SettingsTheme } from "../src/client/theme/ThemeProvider";
import { Player } from "@openfront/engine/game/Game";
import { CLIENT_ID, game } from "./state";

/** Per small id: territory rgb, valid flag, border rgb, kind (0 other, 1 me). */
export const STRIDE = 8;
export let colorTable = new Uint8Array(4096 * STRIDE);
let theme = new SettingsTheme(createThemeSettings("default"));
let colored = 0;

export function resetColors(): void {
  colorTable = new Uint8Array(4096 * STRIDE);
  theme = new SettingsTheme(createThemeSettings("default"));
  colored = 0;
}

function assign(p: Player): void {
  const territory = theme.territoryColor(p as any);
  const isMe = p.clientID() === CLIENT_ID;
  // resolvePlayerColors(): the local player's border is the focused colour.
  const border = isMe ? theme.focusedBorderColor() : theme.borderColor(territory);
  const t = territory.toRgb();
  const b = border.toRgb();
  const o = p.smallID() * STRIDE;
  colorTable[o] = Math.round(t.r);
  colorTable[o + 1] = Math.round(t.g);
  colorTable[o + 2] = Math.round(t.b);
  colorTable[o + 3] = 255;
  colorTable[o + 4] = Math.round(b.r);
  colorTable[o + 5] = Math.round(b.g);
  colorTable[o + 6] = Math.round(b.b);
  colorTable[o + 7] = isMe ? 1 : 0;
}

/** Colours players added since the last call; true if any were new. */
export function colorNewPlayers(): boolean {
  const all = game().allPlayers();
  if (all.length === colored) return false;
  for (const p of all) {
    if (colorTable[p.smallID() * STRIDE + 3] === 0) assign(p);
  }
  colored = all.length;
  return true;
}

export function territoryHex(p: Player): string {
  const o = p.smallID() * STRIDE;
  const h = (v: number) => v.toString(16).padStart(2, "0");
  return "#" + h(colorTable[o]) + h(colorTable[o + 1]) + h(colorTable[o + 2]);
}
