// Ports of the web client's PlayerInfoOverlay (the hover card at the top),
// PlayerPanel (the "info" modal), SendResourceModal and EmojiTable data.
import { AllPlayers, PlayerType, Relation, UnitType } from "@openfront/engine-api/game/GameTypes";
import { flattenedEmojiTable } from "@openfront/engine-api/Schemas";
import { renderNumber, renderTroops } from "@openfront/engine-lib/Format";
import { Player } from "@openfront/engine/game/Game";
import quickChatData from "../resources/QuickChat.json";
import { emojiSections } from "../src/client/EmojiKeys";
import { t } from "./i18n";
import { sendAllianceRequest } from "./radial";
import {
  attackingTroops,
  firstPlace,
  flagOf,
  game,
  hasEmbargo,
  intent,
  isFriendly,
  isRequestingAllianceWith,
  me,
  playerBySmall,
  playerTypeKey,
  S,
  totalUnitLevels,
  traitorRemainingTicks,
} from "./state";

/** renderDuration() of the web client. */
export function renderDuration(totalSeconds: number): string {
  const whole = Math.floor(totalSeconds);
  if (whole <= 0) return `0${t("common.duration_second_short")}`;
  const h = Math.floor(whole / 3600);
  const m = Math.floor((whole % 3600) / 60);
  const s = whole % 60;
  const parts: string[] = [];
  if (h > 0) parts.push(`${h}${t("common.duration_hour_short")}`);
  if (m > 0) parts.push(`${m}${t("common.duration_minute_short")}`);
  if (s > 0 || parts.length === 0) parts.push(`${s}${t("common.duration_second_short")}`);
  return parts.join(" ");
}

/** getPlayerIcons() of the web client, as status-atlas / image names. */
export function playerIcons(p: Player, first: Player | null): string[] {
  const my = me();
  const g = game();
  const icons: string[] = [];
  if (p === first) icons.push("crown");
  if (p.isDisconnected()) icons.push("disconnected");
  if (my && isRequestingAllianceWith(p, my)) icons.push("allianceRequest");
  if (my && my.transitiveTargets().includes(p)) icons.push("target");
  if (my && hasEmbargo(my, p)) icons.push("embargo");
  if (!my || p !== my) {
    let active = false;
    let targetsMe = false;
    for (const nuke of p.units(UnitType.AtomBomb, UnitType.HydrogenBomb, UnitType.MIRV)) {
      if (!nuke.isActive()) continue;
      active = true;
      const dst = nuke.targetTile();
      if (my && dst !== undefined && g.owner(dst) === my) targetsMe = true;
    }
    if (active) icons.push(targetsMe ? "nukeRed" : "nukeWhite");
  }
  return icons;
}

function emojiOf(p: Player): string | null {
  const my = me();
  const e = p.outgoingEmojis().find((x) => x.recipientID === AllPlayers || x.recipientID === my?.smallID());
  return e ? e.message : null;
}

// --- PlayerInfoOverlay -----------------------------------------------------------

/** The hover card for the tile under the cursor (null hides it). */
export function hoverInfo(x: number, y: number): Record<string, unknown> | null {
  const g = game();
  if (!g.isValidCoord(x, y)) return null;
  const tile = g.ref(x, y);
  const owner = g.owner(tile);
  const my = me();
  if (owner.isPlayer()) {
    const p = owner;
    const cfg = g.config();
    const traitorTicks = traitorRemainingTicks(p);
    let alliance: string | null = null;
    if (my && my.isAlliedWith(p)) {
      const a = my.alliances().find((al) => al.other(my) === p);
      if (a) alliance = renderDuration(Math.max(0, Math.floor((a.expiresAt() - g.ticks()) / 10)));
    }
    let actions: Record<string, boolean> | null = null;
    if (my && p !== my && !g.inSpawnPhase()) {
      const a = S.runner!.playerActions(my.id(), x, y, [UnitType.TransportShip]);
      const can = a.interaction;
      actions = {
        requestAlliance: can?.canSendAllianceRequest ?? false,
        breakAlliance: can?.canBreakAlliance ?? false,
        target: can?.canTarget ?? false,
        boat: a.buildableUnits.some((u) => u.type === UnitType.TransportShip && u.canBuild !== false),
      };
    }
    let relation = "";
    if (my && my !== p && p.type() === PlayerType.Nation) {
      const r = p.relation(my);
      if (r !== Relation.Neutral) relation = r === Relation.Hostile || r === Relation.Distrustful ? "bad" : "good";
    }
    const units = [
      [UnitType.City, "CityIconWhite"],
      [UnitType.Factory, "FactoryIconWhite"],
      [UnitType.Port, "PortIcon"],
      [UnitType.MissileSilo, "MissileSiloIconWhite"],
      [UnitType.SAMLauncher, "SamLauncherIconWhite"],
      [UnitType.Warship, "BattleshipIconWhite"],
    ] as const;
    const attacking = attackingTroops(p);
    const max = cfg.maxTroops(p);
    return {
      kind: "player",
      sid: p.smallID(),
      me: p === my,
      name: p.displayName(),
      flag: flagOf(p),
      type: t(playerTypeKey(p)),
      friendly: my ? isFriendly(my, p) : false,
      gold: renderNumber(p.gold()),
      attacking: renderTroops(attacking),
      attackingActive: attacking > 0,
      troops: renderTroops(p.troops()),
      max: renderTroops(max),
      troopFrac: p.troops() / Math.max(max, 1),
      attackFrac: attacking / Math.max(max, 1),
      icons: playerIcons(p, firstPlace()),
      emoji: emojiOf(p),
      traitor: traitorTicks > 0 ? renderDuration(Math.floor(traitorTicks / 10)) : null,
      alliance,
      relation,
      units: units
        .filter(([type]) => !cfg.isUnitDisabled(type))
        .map(([type, icon]) => ({ icon, n: totalUnitLevels(p, type) })),
      actions,
      canChat: my !== null && p !== my,
    };
  }
  if (!g.isLand(tile)) {
    let best = null;
    let bestD = 50 * 50;
    for (const u of g.units(UnitType.Warship, UnitType.TradeShip, UnitType.TransportShip)) {
      const d = g.euclideanDistSquared(tile, u.tile());
      if (d < bestD) {
        best = u;
        bestD = d;
      }
    }
    if (best) {
      const ally = best.owner() === my || (my !== null && isFriendly(my, best.owner()));
      return {
        kind: "unit",
        owner: best.owner().displayName(),
        ally,
        type: best.type(),
        health: best.hasHealth() ? best.health() : null,
        troops: best.type() === UnitType.TransportShip ? renderTroops(best.troops()) : null,
      };
    }
  }
  return null;
}

// --- PlayerPanel --------------------------------------------------------------

export function playerPanel(sid: number): Record<string, unknown> | null {
  const g = game();
  const other = playerBySmall(sid);
  const my = me();
  if (!other) return null;
  const viewer = my ?? other;
  const isSpectator = !my || !my.isAlive();
  const tile = other.borderTiles().values().next().value ?? other.spawnTile();
  let actions: ReturnType<NonNullable<typeof S.runner>["playerActions"]> | null = null;
  if (my && tile !== undefined) {
    actions = S.runner!.playerActions(my.id(), g.x(tile), g.y(tile), null);
  }
  const i = actions?.interaction;
  const expiry = new Map<Player, number>();
  for (const a of other.alliances()) expiry.set(a.other(other), a.expiresAt());
  const allies = other
    .allies()
    .map((p) => {
      const exp = expiry.get(p);
      const secs = exp === undefined ? null : Math.max(0, Math.floor((exp - g.ticks()) / 10));
      return { name: p.displayName(), secs, text: secs === null ? "" : renderDuration(secs) };
    })
    .sort((a, b) => (a.secs ?? Infinity) - (b.secs ?? Infinity) || a.name.localeCompare(b.name));
  let allianceExpiry: string | null = null;
  let allianceSecs = 0;
  if (my && my !== other && my.isAlliedWith(other)) {
    const a = my.alliances().find((al) => al.other(my) === other);
    if (a) {
      allianceSecs = Math.max(0, Math.floor((a.expiresAt() - g.ticks()) / 10));
      allianceExpiry = renderDuration(allianceSecs);
    }
  }
  let relation: string | null = null;
  if (my && other.type() === PlayerType.Nation && !other.isTraitor() && !my.isAlliedWith(other)) {
    const r = other.relation(my);
    relation = ["hostile", "distrustful", "neutral", "friendly"][r] ?? "neutral";
  }
  const traitorTicks = traitorRemainingTicks(other);
  return {
    sid,
    name: other.displayName(),
    flag: flagOf(other),
    type: other.type(),
    typeLabel: t(playerTypeKey(other)),
    self: other === my,
    spectator: isSpectator,
    traitor: other.isTraitor() ? (traitorTicks > 0 ? renderDuration(Math.floor(traitorTicks / 10)) : "") : null,
    traitorSecs: Math.floor(traitorTicks / 10),
    relation,
    relationLabel: relation ? t("relation." + relation) : "",
    gold: renderNumber(other.gold()),
    troops: renderTroops(other.troops()),
    betrayals: other.betrayals(),
    trading: my ? !other.hasEmbargoAgainst(my) : true,
    allies,
    allianceExpiry,
    allianceSecs,
    rocketUp: S.ui.rocketDirectionUp,
    can: {
      emoji: other === my ? actions?.canSendEmojiAllPlayers ?? false : i?.canSendEmoji ?? false,
      target: i?.canTarget ?? false,
      donateTroops: i?.canDonateTroops ?? false,
      donateGold: i?.canDonateGold ?? false,
      embargo: i?.canEmbargo ?? false,
      breakAlliance: i?.canBreakAlliance ?? false,
      sendAlliance: i?.canSendAllianceRequest ?? false,
      embargoAll: actions?.canEmbargoAll ?? false,
    },
    myGold: Number(viewer.gold()),
    myTroops: viewer.troops(),
    targetCap: Math.max(0, g.config().maxTroops(other) - other.troops()),
  };
}

/** PlayerPanel / overlay buttons. */
export function playerAction(sid: number, action: string, amount?: number): void {
  const other = playerBySmall(sid);
  const my = me();
  if (!other || !my) return;
  const id = other.id();
  switch (action) {
    case "target":
      intent({ type: "targetPlayer", target: id });
      break;
    case "alliance":
      sendAllianceRequest(my, other);
      break;
    case "breakAlliance":
      intent({ type: "breakAlliance", recipient: id });
      break;
    case "embargo":
      intent({ type: "embargo", targetID: id, action: "start" });
      break;
    case "stopEmbargo":
      intent({ type: "embargo", targetID: id, action: "stop" });
      break;
    case "embargoAll":
      intent({ type: "embargo_all", action: "start" });
      break;
    case "stopEmbargoAll":
      intent({ type: "embargo_all", action: "stop" });
      break;
    case "donateTroops":
      if (amount !== undefined && amount > 0) intent({ type: "donate_troops", recipient: id, troops: amount });
      break;
    case "donateGold":
      if (amount !== undefined && amount > 0) intent({ type: "donate_gold", recipient: id, gold: Math.floor(amount) });
      break;
    case "boat": {
      const tile = other.borderTiles().values().next().value;
      if (tile !== undefined) {
        intent({ type: "boat", troops: S.ui.attackRatio * my.troops(), dst: tile });
      }
      break;
    }
    case "rocketDirection":
      S.ui.rocketDirectionUp = !S.ui.rocketDirectionUp;
      break;
  }
}

export function sendEmoji(sid: number, index: number): void {
  const my = me();
  if (!my) return;
  const target = sid < 0 || playerBySmall(sid) === my ? AllPlayers : playerBySmall(sid)?.id();
  if (target === undefined) return;
  intent({ type: "emoji", recipient: target as any, emoji: index });
}

/** EmojiTable's data: the wire table and the sections it shows. */
export function emojiList(): Record<string, unknown> {
  return {
    table: flattenedEmojiTable as unknown as string[],
    sections: emojiSections.map((s) => ({
      title: t(s.title),
      columns: s.columns ?? 0,
      emojis: s.emojis.map((e) => (flattenedEmojiTable as readonly string[]).indexOf(e)),
    })),
  };
}

export { hasEmbargo, isFriendly };

// --- ChatModal (quick chat) ---------------------------------------------------------

const CHAT_CATEGORIES = ["help", "attack", "defend", "greet", "misc", "warnings"];

/** The phrases that need no second player, by category, translated. */
export function quickChat(sid: number): Record<string, unknown> | null {
  const other = playerBySmall(sid);
  if (!other) return null;
  const data = quickChatData as Record<string, { key: string; requiresPlayer: boolean }[]>;
  return {
    title: t("chat.title"),
    name: other.displayName(),
    categories: CHAT_CATEGORIES.filter((c) => data[c]).map((c) => ({
      id: c,
      label: t("chat.cat." + c),
      phrases: data[c]
        .filter((p) => !p.requiresPlayer)
        .map((p) => ({ key: c + "." + p.key, text: t("chat." + c + "." + p.key) })),
    })),
  };
}

export function sendQuickChat(sid: number, key: string): void {
  const other = playerBySmall(sid);
  if (!other || !me()) return;
  intent({ type: "quick_chat", recipient: other.id(), quickChatKey: key } as any);
}
