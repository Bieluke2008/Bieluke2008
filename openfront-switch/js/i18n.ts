// Translations: OpenFront's own language files (romfs/lang/*.json, the same
// keys the web client uses) plus a few strings of the port's own.

declare const host: {
  log(msg: string): void;
  now(): number;
  readFile(path: string): ArrayBuffer | null;
};

export const ROMFS: string = (globalThis as any).ROMFS_ROOT ?? "romfs:/";

let strings: Record<string, any> = {};
let fallback: Record<string, any> = {};
let lang = "en";

export function loadJson(path: string): any {
  const buf = host.readFile(path);
  if (buf === null) return null;
  return JSON.parse(new TextDecoder().decode(new Uint8Array(buf)));
}

function lookup(table: Record<string, any>, key: string): string | undefined {
  let cur: any = table;
  for (const part of key.split(".")) {
    if (cur === undefined || cur === null) return undefined;
    cur = cur[part];
  }
  return typeof cur === "string" ? cur : undefined;
}

/** Minimal ICU message formatting: {name} plus plural/select blocks. */
export function formatIcu(msg: string, params: Record<string, unknown>): string {
  let out = "";
  let i = 0;
  while (i < msg.length) {
    const c = msg[i];
    if (c !== "{") {
      out += c;
      i++;
      continue;
    }
    let depth = 1;
    let j = i + 1;
    while (j < msg.length && depth > 0) {
      if (msg[j] === "{") depth++;
      else if (msg[j] === "}") depth--;
      j++;
    }
    const body = msg.slice(i + 1, j - 1);
    i = j;
    const parts = body.split(",");
    const name = parts[0].trim();
    if (parts.length === 1) {
      // Unfilled placeholders stay, for the native side to fill in.
      out += params[name] !== undefined ? String(params[name]) : "{" + name + "}";
      continue;
    }
    const kind = parts[1].trim();
    const rest = parts.slice(2).join(",");
    const branches: Record<string, string> = {};
    const re = /\s*(=?\w+)\s*\{/g;
    let m: RegExpExecArray | null;
    let pos = 0;
    while (((re.lastIndex = pos), (m = re.exec(rest)) !== null)) {
      let d = 1;
      let k = re.lastIndex;
      while (k < rest.length && d > 0) {
        if (rest[k] === "{") d++;
        else if (rest[k] === "}") d--;
        k++;
      }
      branches[m[1]] = rest.slice(re.lastIndex, k - 1);
      pos = k;
    }
    const value = params[name];
    let branch: string | undefined;
    if (kind === "plural") {
      const n = Number(value);
      branch =
        branches["=" + n] ??
        (n === 1 ? branches.one : undefined) ??
        branches.other;
      if (branch !== undefined) {
        branch = formatIcu(branch, params).replace(/#/g, String(n));
      }
    } else {
      branch = branches[String(value)] ?? branches.other;
      if (branch !== undefined) branch = formatIcu(branch, params);
    }
    out += branch ?? "";
  }
  return out;
}

// The port's own strings: controls help and the start menu, which the web
// client has no keys for. Languages without an entry use English.
const PORT_STRINGS: Record<string, Record<string, string>> = {
  en: {
    title: "for Nintendo Switch",
    name: "Name",
    speed: "Game speed",
    quit: "Quit",
    loading: "Loading map…",
    default: "Map default",
    continents: "World and continents",
    real_maps: "Real places",
    fictional_maps: "Fictional maps",
    credits: "OpenFront™ and Contributors · AGPL-3.0 · assets CC BY-SA 4.0",
    controls_title: "Controls",
    controls: [
      "Touch screen|Tap: click · Hold: radial menu",
      "|Drag: move the map · Pinch: zoom",
      "Left stick|Move the cursor",
      "A|Click (attack, place, buttons)",
      "X|Radial menu (build, upgrade, nukes…)",
      "Y|Boat attack to the cursor",
      "B|Back / cancel",
      "L / R|Attack ratio −10% / +10%",
      "ZL / ZR|Zoom out / in",
      "Right stick|Move the map · press: my territory",
      "D-pad ← / →|Choose a structure or bomb (hotbar)",
      "D-pad ↑ / ↓|Leaderboard / emojis",
      "+ / −|Menu / pause",
    ].join("\n"),
  },
  es: {
    title: "para Nintendo Switch",
    name: "Nombre",
    speed: "Velocidad",
    quit: "Salir",
    loading: "Cargando mapa…",
    default: "Por defecto",
    continents: "Mundo y continentes",
    real_maps: "Lugares reales",
    fictional_maps: "Mapas ficticios",
    credits: "OpenFront™ y colaboradores · AGPL-3.0 · recursos CC BY-SA 4.0",
    controls_title: "Controles",
    controls: [
      "Pantalla táctil|Toque: clic · Mantener: menú radial",
      "|Arrastrar: mover el mapa · Pellizcar: zoom",
      "Stick izquierdo|Mover el cursor",
      "A|Clic (atacar, colocar, botones)",
      "X|Menú radial (construir, mejorar, misiles…)",
      "Y|Ataque en barco hacia el cursor",
      "B|Atrás / cancelar",
      "L / R|Porcentaje de ataque −10% / +10%",
      "ZL / ZR|Alejar / acercar",
      "Stick derecho|Mover el mapa · pulsar: mi territorio",
      "Cruceta ← / →|Elegir estructura o bomba (barra)",
      "Cruceta ↑ / ↓|Clasificación / emojis",
      "+ / −|Menú / pausa",
    ].join("\n"),
  },
};
let port = PORT_STRINGS.en;

export function initI18n(code: string): void {
  lang = code;
  fallback = loadJson(ROMFS + "lang/en.json") ?? {};
  strings =
    (code !== "en" && loadJson(ROMFS + "lang/" + code + ".json")) || fallback;
  port = PORT_STRINGS[code.split("-")[0]] ?? PORT_STRINGS.en;
}

export function language(): string {
  return lang;
}

/** translateText() of the web client. */
export function t(key: string, params: Record<string, unknown> = {}): string {
  if (key.startsWith("port.")) {
    const k = key.slice(5);
    return formatIcu(port[k] ?? PORT_STRINGS.en[k] ?? key, params);
  }
  const msg = lookup(strings, key) ?? lookup(fallback, key);
  if (msg === undefined) return key;
  return formatIcu(msg, params);
}

/** A translation that exists (no fallback to the key). */
export function tOr(key: string, otherwise: string): string {
  const v = t(key);
  return v === key ? otherwise : v;
}

export function hasKey(key: string): boolean {
  return lookup(strings, key) !== undefined || lookup(fallback, key) !== undefined;
}
