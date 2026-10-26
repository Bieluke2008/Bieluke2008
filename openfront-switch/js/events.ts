// Port of the web client's EventsDisplay and ActionableEvents
// (src/client/hud/layers/): the message feed and the alliance prompts with
// their buttons, built from the same game updates.
import { AllPlayers, MessageType } from "@openfront/engine-api/game/GameTypes";
import {
  AllianceExpiredUpdate,
  AllianceExtensionUpdate,
  AllianceRequestReplyUpdate,
  AllianceRequestUpdate,
  BrokeAllianceUpdate,
  DisplayChatMessageUpdate,
  DisplayMessageUpdate,
  DonateEventUpdate,
  EmojiUpdate,
  GameUpdateType,
  GameUpdateViewData,
  TargetPlayerUpdate,
  UnitIncomingUpdate,
} from "@openfront/engine-api/game/GameUpdates";
import { renderNumber, renderTroops } from "@openfront/engine-lib/Format";
import { Player } from "@openfront/engine/game/Game";
import { t } from "./i18n";
import {
  game,
  intent,
  isFriendly,
  isRequestingAllianceWith,
  me,
  playerBySmall,
  traitorRemainingTicks,
} from "./state";

interface FeedEvent {
  description: string;
  type: MessageType;
  minor?: boolean;
  createdAt: number;
  focusID?: number;
  unitID?: number;
}

interface Button {
  text: string;
  kind: "gray" | "green" | "blue";
  action: () => void;
  preventClose?: boolean;
}

interface ActionableEvent {
  id: number;
  description: string;
  type: MessageType;
  createdAt: number;
  focusID: number;
  buttons: Button[];
  priority?: number;
  allianceID?: number;
  duration?: number;
  requestorID: number;
}

const TIER_1_TYPES: ReadonlySet<MessageType> = new Set([
  MessageType.NUKE_INBOUND,
  MessageType.HYDROGEN_BOMB_INBOUND,
  MessageType.MIRV_INBOUND,
  MessageType.NUKE_DETONATED,
  MessageType.NAVAL_INVASION_INBOUND,
  MessageType.ATTACK_REQUEST,
  MessageType.ALLIANCE_ACCEPTED,
  MessageType.ALLIANCE_REJECTED,
  MessageType.ALLIANCE_BROKEN,
  MessageType.RENEW_ALLIANCE,
  MessageType.CONQUERED_PLAYER,
  MessageType.CHAT,
  MessageType.DONATION_RECEIVED,
]);

/** getMessageTypeClasses() as a severity name the renderer maps to a colour. */
export function severity(type: MessageType): string {
  switch (type) {
    case MessageType.SAM_HIT:
    case MessageType.CAPTURED_ENEMY_UNIT:
    case MessageType.CONQUERED_PLAYER:
    case MessageType.ALLIANCE_ACCEPTED:
      return "success";
    case MessageType.ATTACK_FAILED:
    case MessageType.ALLIANCE_REJECTED:
    case MessageType.ALLIANCE_BROKEN:
    case MessageType.UNIT_DESTROYED:
    case MessageType.NUKE_DETONATED:
      return "fail";
    case MessageType.ATTACK_CANCELLED:
    case MessageType.ATTACK_REQUEST:
    case MessageType.DONATION_SENT:
    case MessageType.DONATION_RECEIVED:
      return "blue";
    case MessageType.MIRV_INBOUND:
    case MessageType.NUKE_INBOUND:
    case MessageType.HYDROGEN_BOMB_INBOUND:
    case MessageType.SAM_MISS:
    case MessageType.ALLIANCE_EXPIRED:
    case MessageType.NAVAL_INVASION_INBOUND:
    case MessageType.RENEW_ALLIANCE:
      return "warn";
    case MessageType.CHAT:
    case MessageType.ALLIANCE_REQUEST:
      return "info";
    default:
      return "white";
  }
}

/** The descriptions may carry <img> tags (onlyImages()); keep the text. */
function plain(s: string): string {
  return s.replace(/<[^>]*>/g, "").replace(/&amp;/g, "&").replace(/&lt;/g, "<").replace(/&gt;/g, ">");
}

let events: FeedEvent[] = [];
let actionable: ActionableEvent[] = [];
let nextActionID = 1;
const alliancesCheckedAt = new Map<number, number>();
/** Sounds the web client would play; the renderer drains them. */
export const sounds: string[] = [];

export function resetEvents(): void {
  events = [];
  actionable = [];
  alliancesCheckedAt.clear();
  sounds.length = 0;
}

function addEvent(e: FeedEvent): void {
  events.push(e);
}

export function addFeedEvent(e: FeedEvent): void {
  addEvent(e);
}

function resolveParams(event: DisplayMessageUpdate): Record<string, string | number> {
  let params = event.params ?? {};
  if (
    (event.message === "events_display.missile_intercepted" ||
      event.message === "events_display.unit_destroyed") &&
    typeof params.unit === "string" &&
    params.unit.startsWith("unit_type.")
  ) {
    params = { ...params, unit: t(params.unit) };
  }
  if (params.name === undefined || event.focusPlayerID === undefined) return params;
  const subject = playerBySmall(event.focusPlayerID);
  if (subject === null) return params;
  return { ...params, name: subject.displayName() };
}

function onDisplayMessage(event: DisplayMessageUpdate, my: Player): void {
  if (event.playerID !== null && my.smallID() !== event.playerID) return;
  if (event.message === "events_display.received_gold_from_captured_ship") return;
  let description = event.message;
  if (event.message.startsWith("events_display.")) {
    description = t(event.message, resolveParams(event));
  }
  addEvent({
    description,
    createdAt: game().ticks(),
    type: event.messageType,
    unitID: event.unitID,
    focusID: event.focusPlayerID,
  });
}

function onDisplayChat(event: DisplayChatMessageUpdate, my: Player): void {
  if (event.playerID === null || my.smallID() !== event.playerID) return;
  const baseMessage = t(`chat.${event.category}.${event.key}`);
  let msg = baseMessage;
  if (event.target) {
    const target = game().player(event.target);
    msg = baseMessage.replace("[P1]", target?.displayName() ?? event.target);
  }
  let other = "";
  let otherID: number | undefined;
  if (event.recipient !== null) {
    const p = game().player(event.recipient);
    other = p ? p.displayName() : "";
    otherID = p?.smallID();
  }
  const sent = !event.isFrom;
  addEvent({
    description: t(sent ? "chat.to" : "chat.from", { user: other, msg }),
    createdAt: game().ticks(),
    minor: sent,
    type: MessageType.CHAT,
    focusID: otherID,
  });
  if (!sent) sounds.push("message");
}

function onAllianceRequestReplyFeed(update: AllianceRequestReplyUpdate, my: Player): void {
  if (update.request.requestorID !== my.smallID()) return;
  const recipient = playerBySmall(update.request.recipientID);
  if (!recipient) return;
  addEvent({
    description: t("events_display.alliance_request_status", {
      name: recipient.displayName(),
      status: update.accepted
        ? t("events_display.alliance_accepted")
        : t("events_display.alliance_rejected"),
    }),
    type: update.accepted ? MessageType.ALLIANCE_ACCEPTED : MessageType.ALLIANCE_REJECTED,
    createdAt: game().ticks(),
    focusID: update.request.recipientID,
  });
  sounds.push(update.accepted ? "alliance-accepted" : "alliance-declined");
}

function onBrokeAllianceFeed(update: BrokeAllianceUpdate, my: Player): void {
  const betrayed = playerBySmall(update.betrayedID);
  const traitor = playerBySmall(update.traitorID);
  if (!betrayed || !traitor || betrayed.isDisconnected()) return;
  const cfg = game().config();
  if (!betrayed.isTraitor() && traitor === my) {
    sounds.push("alliance-broken");
    const malusPercent = Math.round((1 - cfg.traitorDefenseDebuff()) * 100);
    const traitorDuration = Math.floor(cfg.traitorDuration() * 0.1);
    const durationText =
      traitorDuration === 1
        ? t("events_display.duration_second")
        : t("events_display.duration_seconds_plural", { seconds: traitorDuration });
    addEvent({
      description: t("events_display.betrayal_description", {
        name: betrayed.displayName(),
        malusPercent,
        durationText,
      }),
      type: MessageType.ALLIANCE_BROKEN,
      createdAt: game().ticks(),
      focusID: update.betrayedID,
    });
  } else if (betrayed === my) {
    sounds.push("alliance-broken");
    addEvent({
      description: t("events_display.betrayed_you", { name: traitor.displayName() }),
      type: MessageType.ALLIANCE_BROKEN,
      createdAt: game().ticks(),
      focusID: update.traitorID,
    });
  }
}

function onAllianceExpired(update: AllianceExpiredUpdate, my: Player): void {
  const otherID =
    update.player1ID === my.smallID()
      ? update.player2ID
      : update.player2ID === my.smallID()
        ? update.player1ID
        : null;
  if (otherID === null) return;
  const other = playerBySmall(otherID);
  if (!other || !my.isAlive() || !other.isAlive()) return;
  addEvent({
    description: t("events_display.alliance_expired", { name: other.displayName() }),
    type: MessageType.ALLIANCE_EXPIRED,
    createdAt: game().ticks(),
    focusID: otherID,
  });
}

function onDonate(update: DonateEventUpdate, my: Player): void {
  const isRecipient = update.recipientId === my.id();
  const isSender = update.senderId === my.id();
  if (!isRecipient && !isSender) return;
  const other = game().player(isRecipient ? update.senderId : update.recipientId);
  const isGold = update.donationType === "gold";
  const key = isRecipient
    ? isGold
      ? "events_display.received_gold_from_player"
      : "events_display.received_troops_from_player"
    : isGold
      ? "events_display.sent_gold_to_player"
      : "events_display.sent_troops_to_player";
  addEvent({
    description: t(key, {
      name: other.displayName(),
      [isGold ? "gold" : "troops"]: isGold
        ? renderNumber(update.amount)
        : renderTroops(Number(update.amount)),
    }),
    type: isRecipient ? MessageType.DONATION_RECEIVED : MessageType.DONATION_SENT,
    createdAt: game().ticks(),
    focusID: other.smallID(),
  });
}

function onTargetPlayer(event: TargetPlayerUpdate, my: Player): void {
  const other = playerBySmall(event.playerID);
  const target = playerBySmall(event.targetID);
  if (!other || !target || !isFriendly(my, other)) return;
  addEvent({
    description: t("events_display.attack_request", {
      name: other.displayName(),
      target: target.displayName(),
    }),
    type: MessageType.ATTACK_REQUEST,
    createdAt: game().ticks(),
    focusID: event.targetID,
  });
}

function onEmoji(update: EmojiUpdate, my: Player): void {
  const recipient =
    update.emoji.recipientID === AllPlayers ? AllPlayers : playerBySmall(update.emoji.recipientID);
  const sender = playerBySmall(update.emoji.senderID);
  if (!sender) return;
  if (recipient === my) {
    addEvent({
      description: `${sender.displayName()}: ${update.emoji.message}`,
      type: MessageType.CHAT,
      createdAt: game().ticks(),
      focusID: update.emoji.senderID,
    });
  } else if (sender === my && recipient !== AllPlayers && recipient) {
    addEvent({
      description: t("events_display.sent_emoji", {
        name: (recipient as Player).displayName(),
        emoji: update.emoji.message,
      }),
      type: MessageType.CHAT,
      minor: true,
      createdAt: game().ticks(),
      focusID: (recipient as Player).smallID(),
    });
  } else if (sender === my) {
    addEvent({
      description: t("events_display.sent_emoji_all", { emoji: update.emoji.message }),
      type: MessageType.CHAT,
      minor: true,
      createdAt: game().ticks(),
    });
  }
}

function onUnitIncoming(event: UnitIncomingUpdate, my: Player): void {
  if (my.smallID() !== event.playerID) return;
  const unit = game().unit(event.unitID);
  let description = event.message;
  if (event.message.startsWith("events_display.") && unit) {
    const params: Record<string, string | number> = { name: unit.owner().displayName() };
    if (event.messageType === MessageType.NAVAL_INVASION_INBOUND) {
      params.troops = renderTroops(unit.troops());
    }
    description = t(event.message, params);
  }
  addEvent({
    description,
    type: event.messageType,
    createdAt: game().ticks(),
    unitID: event.unitID,
  });
}

// --- ActionableEvents --------------------------------------------------------

function addActionable(e: Omit<ActionableEvent, "id">): void {
  actionable.push({ ...e, id: nextActionID++ });
}

function removeAllianceRenewalEvents(allianceID: number): void {
  actionable = actionable.filter(
    (e) => !(e.type === MessageType.RENEW_ALLIANCE && e.allianceID === allianceID),
  );
}

function checkForAllianceExpirations(my: Player): void {
  const g = game();
  const offset = g.config().allianceExtensionPromptOffset();
  const current = new Set<number>();
  for (const alliance of my.alliances()) {
    current.add(alliance.id());
    if (alliance.expiresAt() > g.ticks() + offset) continue;
    if ((alliancesCheckedAt.get(alliance.id()) ?? 0) >= g.ticks() - offset) continue;
    alliancesCheckedAt.set(alliance.id(), g.ticks());
    const other = alliance.other(my);
    addActionable({
      description: t("events_display.about_to_expire", { name: other.displayName() }),
      type: MessageType.RENEW_ALLIANCE,
      buttons: [
        { text: t("events_display.focus"), kind: "gray", action: () => {}, preventClose: true },
        {
          text: t("events_display.renew_alliance", { name: other.displayName() }),
          kind: "green",
          action: () => intent({ type: "allianceExtension", recipient: other.id() }),
        },
        { text: t("events_display.ignore"), kind: "blue", action: () => {} },
      ],
      createdAt: g.ticks(),
      focusID: other.smallID(),
      allianceID: alliance.id(),
      requestorID: other.smallID(),
    });
  }
  for (const [id] of alliancesCheckedAt) {
    if (!current.has(id)) {
      removeAllianceRenewalEvents(id);
      alliancesCheckedAt.delete(id);
    }
  }
}

function onAllianceRequest(update: AllianceRequestUpdate, my: Player): void {
  if (update.recipientID !== my.smallID()) return;
  const requestor = playerBySmall(update.requestorID);
  if (!requestor) return;
  if (!requestor.isAlliedWith(my)) sounds.push("alliance-suggested");
  addActionable({
    description: t("events_display.request_alliance", { name: requestor.displayName() }),
    buttons: [
      { text: t("events_display.focus"), kind: "gray", action: () => {}, preventClose: true },
      {
        text: t("events_display.accept_alliance"),
        kind: "green",
        action: () => {
          sounds.push("alliance-accepted");
          intent({ type: "allianceRequest", recipient: requestor.id() });
        },
      },
      {
        text: t("events_display.reject_alliance"),
        kind: "blue",
        action: () => {
          sounds.push("alliance-declined");
          intent({ type: "allianceReject", requestor: requestor.id() });
        },
      },
    ],
    type: MessageType.ALLIANCE_REQUEST,
    createdAt: game().ticks(),
    priority: 0,
    duration: game().config().allianceRequestDuration(),
    focusID: update.requestorID,
    requestorID: update.requestorID,
  });
}

function onAllianceRequestReplyActionable(update: AllianceRequestReplyUpdate, my: Player): void {
  if (update.request.recipientID !== my.smallID()) return;
  const requestorID = update.request.requestorID;
  actionable = actionable.filter(
    (e) => !(e.type === MessageType.ALLIANCE_REQUEST && e.focusID === requestorID),
  );
}

function onAllianceExtension(update: AllianceExtensionUpdate, my: Player): void {
  if (my.smallID() !== update.playerID) return;
  removeAllianceRenewalEvents(update.allianceID);
}

// --- Per tick ---------------------------------------------------------------

export function processUpdates(gu: GameUpdateViewData): void {
  const my = me();
  if (!my || !my.isAlive() || game().inSpawnPhase()) return;
  const u = gu.updates;
  for (const e of u[GameUpdateType.DisplayEvent]) onDisplayMessage(e, my);
  for (const e of u[GameUpdateType.DisplayChatEvent]) onDisplayChat(e, my);
  for (const e of u[GameUpdateType.AllianceRequestReply]) {
    onAllianceRequestReplyFeed(e, my);
    onAllianceRequestReplyActionable(e, my);
  }
  for (const e of u[GameUpdateType.BrokeAlliance]) {
    onBrokeAllianceFeed(e, my);
    removeAllianceRenewalEvents(e.allianceID);
    alliancesCheckedAt.delete(e.allianceID);
  }
  for (const e of u[GameUpdateType.TargetPlayer]) onTargetPlayer(e, my);
  for (const e of u[GameUpdateType.Emoji]) onEmoji(e, my);
  for (const e of u[GameUpdateType.UnitIncoming]) onUnitIncoming(e, my);
  for (const e of u[GameUpdateType.AllianceExpired]) onAllianceExpired(e, my);
  for (const e of u[GameUpdateType.DonateEvent]) onDonate(e, my);
  for (const e of u[GameUpdateType.AllianceRequest]) onAllianceRequest(e, my);
  for (const e of u[GameUpdateType.AllianceExtension]) onAllianceExtension(e, my);
}

/** The feed and prompts, as the renderer draws them. */
export function eventsModel(): Record<string, unknown> | null {
  const g = game();
  const my = me();
  if (!my || !my.isAlive() || g.inSpawnPhase()) return null;
  checkForAllianceExpirations(my);

  // EventsDisplay.tick(): drop events after 8 seconds, inbound warnings
  // once their unit is gone, and keep at most 30.
  events = events.filter((e) => {
    const age = g.ticks() - e.createdAt;
    const inbound =
      e.type === MessageType.NUKE_INBOUND ||
      e.type === MessageType.HYDROGEN_BOMB_INBOUND ||
      e.type === MessageType.MIRV_INBOUND ||
      e.type === MessageType.NAVAL_INVASION_INBOUND;
    const gone = inbound && e.unitID !== undefined && !(g.unit(e.unitID)?.isActive() ?? false);
    return age < 80 && age >= 0 && !gone;
  });
  if (events.length > 30) events = events.slice(-30);

  actionable = actionable.filter((e) => {
    if (e.duration !== undefined && g.ticks() - e.createdAt >= e.duration) return false;
    if (e.type !== MessageType.ALLIANCE_REQUEST) return true;
    const requestor = playerBySmall(e.requestorID);
    return requestor !== null && requestor.isAlive() && isRequestingAllianceWith(requestor, my);
  });

  const tier1: FeedEvent[] = [];
  let tier2: FeedEvent[] = [];
  for (const e of events) (TIER_1_TYPES.has(e.type) && !e.minor ? tier1 : tier2).push(e);
  tier1.sort((a, b) => a.createdAt - b.createdAt);
  tier2.sort((a, b) => a.createdAt - b.createdAt);
  tier2 = tier2.slice(-4);

  const traitorTicks = traitorRemainingTicks(my);
  const row = (e: FeedEvent) => ({
    text: plain(e.description),
    sev: severity(e.type),
    focus: e.focusID ?? -1,
    unit: e.unitID ?? -1,
  });
  const sorted = [...actionable].sort((a, b) => {
    const ap = a.priority ?? 100000;
    const bp = b.priority ?? 100000;
    return ap === bp ? b.createdAt - a.createdAt : bp - ap;
  });
  return {
    minor: tier2.map(row),
    major: tier1.map(row),
    betrayal:
      traitorTicks > 0
        ? t("events_display.betrayal_debuff_ends", { time: Math.ceil(traitorTicks / 10) })
        : null,
    actions: sorted.map((e) => ({
      id: e.id,
      text: plain(e.description),
      sev: severity(e.type),
      focus: e.focusID,
      buttons: e.buttons.map((b) => ({ text: b.text, kind: b.kind })),
    })),
  };
}

/** A click on a prompt button; returns the player to focus, or -1. */
export function actionableClick(id: number, button: number): number {
  const index = actionable.findIndex((e) => e.id === id);
  if (index < 0) return -1;
  const e = actionable[index];
  const b = e.buttons[button];
  if (!b) return -1;
  b.action();
  if (!b.preventClose) actionable.splice(index, 1);
  return b.kind === "gray" ? e.focusID : -1;
}
