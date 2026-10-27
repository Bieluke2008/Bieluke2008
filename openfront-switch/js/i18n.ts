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
    music_missing: "OpenFront's music isn't included: copy its mp3s to /switch/openfront/music/",
    home_hint: "A: select · Y: change name · +: Solo · X: Multiplayer",
    multiplayer: "Multiplayer (local network)",
    multiplayer_short: "Multiplayer",
    mp_server: "Server (PC IP:port)",
    mp_help:
      "Run OpenFront's server on a PC in your network (see LEEME.txt) and type its IP and port, e.g. 192.168.1.20:9000. Friends play from their browser at http://IP:9000 with Join Lobby and the lobby code. Lobbies made on the web must turn on Compact map.",
    mp_connecting: "Connecting…",
    mp_closed: "Disconnected from the server",
    mp_need_compact: "The host must turn on «Compact map»: the Switch only has the compact maps.",
    mp_desync: "Desync: this game no longer matches the server's.",
    mp_version: "The server runs another OpenFront version ({commit}). Start it from the commit in LEEME.txt with npm run dev.",
    mp_no_map: "This map isn't in the Switch version.",
    mp_no_server: "No server answers at that address. Check the IP, the port and that the server is running.",
    mp_no_network: "No network connection.",
    mp_bad_address: "Write the server as IP:port, e.g. 192.168.1.20:9000.",
    mp_code: "Lobby code",
    mp_code_hint: "Friends: Join Lobby on the web and type this code",
    mp_lobby: "Private lobby",
    mp_leave: "Leave lobby",
    mp_host_here: "Create a game on this Switch",
    mp_lan_games: "Games in your network",
    mp_searching: "Looking for other Switches' games in your Wi-Fi…",
    mp_same_wifi: "The Switches must be on the same Wi-Fi network (a phone's hotspot works too).",
    mp_pc_server: "Server on a PC (optional, for browsers)",
    mp_create_short: "Create",
    mp_join_short: "Join",
    mp_in_progress: "In progress",
    mp_version_diff: "Another version of the port",
    mp_switch_hint: "Other Switches: Multiplayer and pick this game (or IP {ip})",
    mp_host_failed: "Could not create the game on this Switch.",
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
    music_missing: "La música de OpenFront no va incluida: copia sus mp3 a /switch/openfront/music/",
    home_hint: "A: elegir · Y: cambiar nombre · +: Solo · X: Multijugador",
    multiplayer: "Multijugador (red local)",
    multiplayer_short: "Multijugador",
    mp_server: "Servidor (IP:puerto del PC)",
    mp_help:
      "Arranca el servidor de OpenFront en un PC de tu red (mira LEEME.txt) y escribe su IP y puerto, p. ej. 192.168.1.20:9000. Tus amigos juegan desde el navegador en http://IP:9000 con «Unirse al lobby» y el código de la sala. Las salas creadas en la web deben activar «Mapa compacto».",
    mp_connecting: "Conectando…",
    mp_closed: "Desconectado del servidor",
    mp_need_compact: "El anfitrión debe activar «Mapa compacto»: la Switch solo tiene los mapas compactos.",
    mp_desync: "Desincronizado: esta partida ya no coincide con la del servidor.",
    mp_version: "El servidor tiene otra versión de OpenFront ({commit}). Arráncalo desde el commit de LEEME.txt con npm run dev.",
    mp_no_map: "Este mapa no está en la versión de Switch.",
    mp_no_server: "No responde ningún servidor en esa dirección. Revisa la IP, el puerto y que el servidor esté arrancado.",
    mp_no_network: "Sin conexión de red.",
    mp_bad_address: "Escribe el servidor como IP:puerto, p. ej. 192.168.1.20:9000.",
    mp_code: "Código de sala",
    mp_code_hint: "Tus amigos: «Unirse al lobby» en la web y este código",
    mp_lobby: "Sala privada",
    mp_leave: "Salir de la sala",
    mp_host_here: "Crear partida en esta Switch",
    mp_lan_games: "Partidas en tu red",
    mp_searching: "Buscando partidas de otras Switch en tu Wi-Fi…",
    mp_same_wifi: "Las Switch deben estar en la misma red Wi-Fi (vale la de un móvil compartiendo datos).",
    mp_pc_server: "Servidor en un PC (opcional, para navegadores)",
    mp_create_short: "Crear",
    mp_join_short: "Unirse",
    mp_in_progress: "En curso",
    mp_version_diff: "Otra versión del port",
    mp_switch_hint: "Otras Switch: Multijugador y elige esta partida (o IP {ip})",
    mp_host_failed: "No se pudo crear la partida en esta Switch.",
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
