// Port of the web client's radial menu (src/client/hud/layers/
// RadialMenuElements.ts and MainRadialMenu.ts): the same items, colours,
// enable rules, tooltips and actions, resolved for one clicked tile. The
// renderer draws the tree and sends back the path of the item picked.
import { TileRef } from "@openfront/engine-api/game/GameMap";
import {
  BuildableAttacks,
  BuildableUnit,
  bulkCost,
  maxBulkAmount,
  NUKE_BULK_STEPS,
  PlayerActions,
  PlayerBuildableUnitType,
  STRUCTURE_BULK_STEPS,
  Structures,
  UnitType,
} from "@openfront/engine-api/game/GameTypes";
import { renderNumber } from "@openfront/engine-lib/Format";
import { Player } from "@openfront/engine/game/Game";
import { addFeedEvent } from "./events";
import { t } from "./i18n";
import { MessageType } from "@openfront/engine-api/game/GameTypes";
import {
  deleteUnitCooldown,
  game,
  intent,
  isFriendly,
  isRequestingAllianceWith,
  me,
  readyMissileCount,
  S,
  totalUnitLevels,
} from "./state";

export const COLORS = {
  build: "#e6c74a",
  building: "#1e3a5f",
  boat: "#2a82c9",
  disabled: "#94a3b8",
  ally: "#4ade80",
  breakAlly: "#dc2626",
  breakAllyNoDebuff: "#d97706",
  delete: "#ef4444",
  info: "#475569",
  target: "#ef4444",
  attack: "#ef4444",
  infoEmoji: "#fbbf24",
};

interface Params {
  myPlayer: Player;
  selected: Player | null;
  tile: TileRef;
  playerActions: PlayerActions;
}

/** What a picked item asks the renderer to do next (open a panel...). */
export interface ActionResult {
  close?: boolean;
  open?: "panel" | "sendGold" | "sendTroops" | "emoji";
  player?: number;
}

interface MenuElement {
  id: string;
  name: string;
  displayed?: (p: Params) => boolean;
  color?: string | ((p: Params) => string);
  icon?: string;
  text?: string;
  fontSize?: number;
  tooltip?: (p: Params) => { text: string; cls: string }[];
  cooldown?: (p: Params) => number;
  disabled: (p: Params) => boolean;
  action?: (p: Params) => ActionResult;
  subMenu?: (p: Params) => MenuElement[];
  timerFraction?: (p: Params) => number;
}

interface BuildItem {
  unitType: PlayerBuildableUnitType;
  icon: string;
  description: string;
  key: string;
  countable: boolean;
}

// BuildMenu.ts flattenedBuildTable, in its order.
const flattenedBuildTable: BuildItem[] = [
  { unitType: UnitType.AtomBomb, icon: "NukeIconWhite", description: "build_menu.desc.atom_bomb", key: "unit_type.atom_bomb", countable: false },
  { unitType: UnitType.MIRV, icon: "MIRVIcon", description: "build_menu.desc.mirv", key: "unit_type.mirv", countable: false },
  { unitType: UnitType.HydrogenBomb, icon: "MushroomCloudIconWhite", description: "build_menu.desc.hydrogen_bomb", key: "unit_type.hydrogen_bomb", countable: false },
  { unitType: UnitType.Warship, icon: "BattleshipIconWhite", description: "build_menu.desc.warship", key: "unit_type.warship", countable: true },
  { unitType: UnitType.Port, icon: "PortIcon", description: "build_menu.desc.port", key: "unit_type.port", countable: true },
  { unitType: UnitType.MissileSilo, icon: "MissileSiloIconWhite", description: "build_menu.desc.missile_silo", key: "unit_type.missile_silo", countable: true },
  { unitType: UnitType.SAMLauncher, icon: "SamLauncherIconWhite", description: "build_menu.desc.sam_launcher", key: "unit_type.sam_launcher", countable: true },
  { unitType: UnitType.DefensePost, icon: "ShieldIconWhite", description: "build_menu.desc.defense_post", key: "unit_type.defense_post", countable: true },
  { unitType: UnitType.City, icon: "CityIconWhite", description: "build_menu.desc.city", key: "unit_type.city", countable: true },
  { unitType: UnitType.Factory, icon: "FactoryIconWhite", description: "build_menu.desc.factory", key: "unit_type.factory", countable: true },
];

function buildable(p: Params, type: PlayerBuildableUnitType): BuildableUnit | undefined {
  return p.playerActions.buildableUnits.find((b) => b.type === type);
}
function canBuildOrUpgrade(p: Params, item: BuildItem): boolean {
  const u = buildable(p, item.unitType);
  return u ? u.canBuild !== false || u.canUpgrade !== false : false;
}
function cost(p: Params, item: BuildItem): bigint {
  return buildable(p, item.unitType)?.cost ?? 0n;
}

function isFriendlyTarget(p: Params): boolean {
  return p.selected !== null && isFriendly(p.selected, p.myPlayer);
}
function isDisconnectedTarget(p: Params): boolean {
  return p.selected !== null && p.selected.isDisconnected();
}

// PlayerActionHandler + the Transport intents behind it.
function sendAttack(p: Params, targetID: string | null): void {
  const troops = S.ui.attackRatio * p.myPlayer.troops();
  if (Number.isFinite(troops) && troops >= 0) intent({ type: "attack", targetID, troops });
}
function sendBoat(p: Params, tile: TileRef): void {
  const troops = S.ui.attackRatio * p.myPlayer.troops();
  if (Number.isFinite(troops) && troops >= 0) intent({ type: "boat", troops, dst: tile });
}
export function sendAllianceRequest(my: Player, recipient: Player): void {
  intent({ type: "allianceRequest", recipient: recipient.id() });
  // EventsDisplay.onAllianceRequestSentConfirmation()
  if (!isRequestingAllianceWith(recipient, my)) {
    addFeedEvent({
      description: t("events_display.alliance_request_sent", { name: recipient.displayName() }),
      type: MessageType.ALLIANCE_REQUEST,
      createdAt: game().ticks(),
    });
  }
}

const close: ActionResult = { close: true };

const allyRequestElement: MenuElement = {
  id: "ally_request",
  name: "request",
  disabled: (p) => !p.playerActions?.interaction?.canSendAllianceRequest,
  displayed: (p) => !p.playerActions?.interaction?.canBreakAlliance,
  color: COLORS.ally,
  icon: "AllianceIconWhite",
  action: (p) => {
    sendAllianceRequest(p.myPlayer, p.selected!);
    return close;
  },
};

const allyExtendElement: MenuElement = {
  id: "ally_extend",
  name: "extend",
  displayed: (p) => !!p.playerActions?.interaction?.allianceInfo?.inExtensionWindow,
  disabled: (p) => !p.playerActions?.interaction?.allianceInfo?.canExtend,
  color: COLORS.ally,
  icon: "AllianceIconWhite",
  action: (p) => {
    if (!p.playerActions?.interaction?.allianceInfo?.canExtend) return {};
    intent({ type: "allianceExtension", recipient: p.selected!.id() });
    return close;
  },
  timerFraction: (p) => {
    const info = p.playerActions?.interaction?.allianceInfo;
    if (!info) return 1;
    const remaining = Math.max(0, info.expiresAt - game().ticks());
    const window = Math.max(1, game().config().allianceExtensionPromptOffset());
    return Math.max(0, Math.min(1, remaining / window));
  },
};

const allyBreakElement: MenuElement = {
  id: "ally_break",
  name: "break",
  disabled: (p) => !p.playerActions?.interaction?.canBreakAlliance,
  displayed: (p) => !!p.playerActions?.interaction?.canBreakAlliance,
  color: (p) =>
    p.selected?.isTraitor() || p.selected?.isDisconnected() ? COLORS.breakAllyNoDebuff : COLORS.breakAlly,
  icon: "TraitorIconWhite",
  action: (p) => {
    intent({ type: "breakAlliance", recipient: p.selected!.id() });
    return close;
  },
};

const infoMenuElement: MenuElement = {
  id: "info",
  name: "info",
  disabled: (p) => !p.selected || game().inSpawnPhase(),
  icon: "InfoIcon",
  color: COLORS.info,
  action: (p) => ({ open: "panel", player: p.selected!.smallID() }),
};

function enabledUnits(myTerritory: boolean): Set<PlayerBuildableUnitType> {
  const cfg = game().config();
  const units = new Set<PlayerBuildableUnitType>();
  const add = (u: PlayerBuildableUnitType) => {
    if (!cfg.isUnitDisabled(u)) units.add(u);
  };
  if (myTerritory) Structures.types.forEach(add);
  else BuildableAttacks.types.forEach(add);
  return units;
}

function createMenuElements(params: Params, filterType: "attack" | "build", prefix: string): MenuElement[] {
  const unitTypes = enabledUnits(params.selected === params.myPlayer);
  return flattenedBuildTable
    .filter(
      (item) =>
        unitTypes.has(item.unitType) &&
        (filterType === "attack" ? BuildableAttacks.has(item.unitType) : !BuildableAttacks.has(item.unitType)),
    )
    .map((item) => ({
      id: `${prefix}_${item.unitType}`,
      name: item.key.replace("unit_type.", ""),
      disabled: (p: Params) => !canBuildOrUpgrade(p, item),
      color: (p: Params) =>
        canBuildOrUpgrade(p, item) ? (filterType === "attack" ? COLORS.attack : COLORS.building) : COLORS.building,
      icon: item.icon,
      tooltip: (p: Params) =>
        [
          { text: t(item.key), cls: "title" },
          { text: t(item.description), cls: "description" },
          { text: `${renderNumber(cost(p, item))} ${t("player_panel.gold")}`, cls: "cost" },
          item.countable ? { text: `${totalUnitLevels(p.myPlayer, item.unitType)}x`, cls: "count" } : null,
        ].filter((x): x is { text: string; cls: string } => x !== null),
      subMenu: (p: Params) => {
        const bu = buildable(p, item.unitType);
        if (!bu) return [];
        const isStackableNuke = item.unitType === UnitType.AtomBomb && bu.canBuild !== false;
        if ((bu.canUpgrade === false && !isStackableNuke) || !canBuildOrUpgrade(p, item)) return [];
        let maxAmount = maxBulkAmount(bu, p.myPlayer.gold());
        if (isStackableNuke) maxAmount = Math.min(maxAmount, readyMissileCount(p.myPlayer));
        if (maxAmount <= 1) return [];
        const steps = isStackableNuke ? NUKE_BULK_STEPS : STRUCTURE_BULK_STEPS;
        const slots = [1, ...steps, maxAmount];
        return slots.map((amount, i) => {
          const isMax = i === slots.length - 1;
          const executable = amount <= maxAmount;
          const c = bulkCost(bu, amount);
          const label = t("build_menu.upgrade_amount", { amount: amount.toString() });
          return {
            id: isMax ? `upgrade_${item.unitType}_max` : `upgrade_${item.unitType}_${amount}`,
            name: label,
            text: label,
            fontSize: 20,
            color: (pp: Params) => (executable && pp.myPlayer.gold() >= c ? COLORS.building : COLORS.disabled),
            icon: "",
            tooltip: () => [
              { text: t("radial_menu.upgrade_x", { amount: amount.toString() }), cls: "title" },
              { text: `${renderNumber(c)} ${t("player_panel.gold")}`, cls: "cost" },
            ],
            disabled: (pp: Params) => !executable || pp.myPlayer.gold() < c,
            action: (pp: Params) => {
              if (isStackableNuke) {
                intent({ type: "build_unit", unit: bu.type, tile: pp.tile, rocketDirectionUp: S.ui.rocketDirectionUp, amount });
              } else {
                intent({ type: "upgrade_structure", unit: bu.type, unitId: bu.canUpgrade as number, amount });
              }
              return close;
            },
          } as MenuElement;
        });
      },
      action: (p: Params) => {
        const bu = buildable(p, item.unitType);
        if (bu === undefined) return {};
        if (canBuildOrUpgrade(p, item)) {
          if (bu.canUpgrade !== false) {
            intent({ type: "upgrade_structure", unit: bu.type, unitId: bu.canUpgrade });
          } else if (bu.canBuild !== false) {
            const rocketDirectionUp =
              item.unitType === UnitType.AtomBomb || item.unitType === UnitType.HydrogenBomb
                ? S.ui.rocketDirectionUp
                : undefined;
            intent({ type: "build_unit", unit: bu.type, tile: p.tile, rocketDirectionUp });
          }
        }
        return close;
      },
    }));
}

const attackMenuElement: MenuElement = {
  id: "attack",
  name: "radial_attack",
  disabled: () => game().inSpawnPhase(),
  icon: "SwordIconWhite",
  color: COLORS.attack,
  subMenu: (p) => createMenuElements(p, "attack", "attack"),
};

const donateGoldRadialElement: MenuElement = {
  id: "attack",
  name: "radial_donate_gold",
  disabled: (p) => game().inSpawnPhase() || !p.playerActions?.interaction?.canDonateGold,
  icon: "DonateGoldIconWhite",
  color: "#f59e0b",
  action: (p) => (p.selected ? { open: "sendGold", player: p.selected.smallID() } : {}),
};

const DELETE_SELECTION_RADIUS = 5;
function deletableUnits(p: Params) {
  const g = game();
  return p.myPlayer
    .units()
    .filter(
      (u) =>
        !u.isUnderConstruction() &&
        u.isMarkedForDeletion() === false &&
        Structures.has(u.type()) &&
        g.manhattanDist(u.tile(), p.tile) <= DELETE_SELECTION_RADIUS,
    );
}

const deleteUnitElement: MenuElement = {
  id: "delete",
  name: "delete",
  cooldown: (p) => deleteUnitCooldown(p.myPlayer),
  disabled: (p) => {
    const g = game();
    const owner = g.owner(p.tile);
    if (!owner.isPlayer() || owner.id() !== p.myPlayer.id()) return true;
    if (!g.isLand(p.tile)) return true;
    if (g.inSpawnPhase()) return true;
    if (deleteUnitCooldown(p.myPlayer) > 0) return true;
    return deletableUnits(p).length === 0;
  },
  icon: "XIcon",
  color: COLORS.delete,
  tooltip: () => [
    { text: t("radial_menu.delete_unit_title"), cls: "title" },
    { text: t("radial_menu.delete_unit_description"), cls: "description" },
  ],
  action: (p) => {
    const g = game();
    let best: ReturnType<typeof deletableUnits>[number] | null = null;
    let bestDist = Infinity;
    for (const u of deletableUnits(p)) {
      const d = g.manhattanDist(u.tile(), p.tile);
      if (d < bestDist) {
        best = u;
        bestDist = d;
      }
    }
    if (best) intent({ type: "delete_unit", unitId: best.id() });
    return close;
  },
};

const buildMenuElement: MenuElement = {
  id: "build",
  name: "build",
  disabled: () => game().inSpawnPhase(),
  icon: "BuildIconWhite",
  color: COLORS.build,
  subMenu: (p) => createMenuElements(p, "build", "build"),
};

const boatMenuElement: MenuElement = {
  id: "boat",
  name: "boat",
  disabled: (p) =>
    !p.playerActions.buildableUnits.some((u) => u.type === UnitType.TransportShip && u.canBuild),
  icon: "BoatIconWhite",
  color: COLORS.boat,
  action: (p) => {
    sendBoat(p, p.tile);
    return close;
  },
};

function centerDisabled(p: Params): boolean {
  const g = game();
  const owner = g.owner(p.tile);
  if (!g.isLand(p.tile)) return true;
  if (g.inSpawnPhase()) {
    if (g.config().isRandomSpawn()) return true;
    return owner.isPlayer();
  }
  if (isFriendlyTarget(p) && !isDisconnectedTarget(p)) {
    return !p.playerActions.interaction?.canDonateTroops;
  }
  return !p.playerActions.canAttack;
}

function centerAction(p: Params): ActionResult {
  const g = game();
  if (g.inSpawnPhase()) {
    intent({ type: "spawn", tile: p.tile });
  } else if (isFriendlyTarget(p) && !isDisconnectedTarget(p)) {
    const troops = Math.floor(S.ui.attackRatio * p.myPlayer.troops());
    if (troops > 0) intent({ type: "donate_troops", recipient: p.selected!.id(), troops });
  } else {
    sendAttack(p, p.selected?.id() ?? null);
  }
  return close;
}

function rootItems(p: Params): MenuElement[] {
  const g = game();
  const isAllied = p.selected?.isAlliedWith(p.myPlayer) ?? false;
  const isDisconnected = isDisconnectedTarget(p);
  const owner = g.owner(p.tile);
  const isOwnTerritory = owner.isPlayer() && owner.id() === p.myPlayer.id();
  const inExtensionWindow = p.playerActions.interaction?.allianceInfo?.inExtensionWindow;
  const hasBuildableAttacks =
    p.playerActions.buildableUnits?.some((bu) => BuildableAttacks.has(bu.type) && bu.canBuild !== false) ?? false;
  const showDonateInsteadOfAttack = isFriendlyTarget(p) && !isDisconnected && !hasBuildableAttacks;
  return [
    infoMenuElement,
    ...(isOwnTerritory
      ? [deleteUnitElement, allyRequestElement, buildMenuElement]
      : [
          isAllied && !isDisconnected ? allyBreakElement : boatMenuElement,
          inExtensionWindow ? allyExtendElement : allyRequestElement,
          showDonateInsteadOfAttack ? donateGoldRadialElement : attackMenuElement,
        ]),
  ];
}

// --- Serialization for the renderer -------------------------------------------

function resolveColor(e: MenuElement, p: Params): string {
  if (typeof e.color === "function") return e.color(p);
  return e.color ?? "#1e3a5f";
}

function serialize(e: MenuElement, p: Params, depth: number): Record<string, unknown> {
  const sub = depth < 3 && e.subMenu ? e.subMenu(p) : null;
  return {
    id: e.id,
    icon: e.icon ?? "",
    text: e.text ?? "",
    font: e.fontSize ?? 0,
    color: resolveColor(e, p),
    disabled: e.disabled(p),
    displayed: e.displayed ? e.displayed(p) : true,
    cooldown: e.cooldown ? e.cooldown(p) : 0,
    timer: e.timerFraction ? e.timerFraction(p) : -1,
    tooltip: e.tooltip ? e.tooltip(p) : [],
    sub: sub && sub.length > 0 ? sub.map((s) => serialize(s, p, depth + 1)) : null,
    action: e.action !== undefined,
  };
}

let lastTile: TileRef | null = null;

function paramsFor(x: number, y: number): Params | null {
  const g = game();
  const my = me();
  if (!my || !g.isValidCoord(x, y)) return null;
  const tile = g.ref(x, y);
  const owner = g.owner(tile);
  return {
    myPlayer: my,
    selected: owner.isPlayer() ? owner : null,
    tile,
    playerActions: S.runner!.playerActions(my.id(), x, y),
  };
}

/** MainRadialMenu's ContextMenuEvent handler: the menu for a tile. */
export function radialMenu(x: number, y: number): Record<string, unknown> | null {
  const g = game();
  const my = me();
  if (!my || !g.isValidCoord(x, y)) return null;
  // Spectators (dead or not spawned yet) get the player panel instead.
  if (!my.isAlive() && my.hasSpawned()) {
    const owner = g.owner(g.ref(x, y));
    return owner.isPlayer() ? { spectate: owner.smallID() } : null;
  }
  const p = paramsFor(x, y);
  if (!p) return null;
  lastTile = p.tile;
  const friendly = p.selected !== null && isFriendly(p.selected, my) && !p.selected.isDisconnected();
  return {
    x,
    y,
    center: {
      icon: friendly ? "DonateTroopIconWhite" : "SwordIconWhite",
      color: friendly ? "#22d3ee" : "#0f2744",
      size: friendly ? 0.75 : 1,
      disabled: centerDisabled(p),
    },
    // RadialMenu draws every root item; `displayed` is not consulted.
    items: rootItems(p).map((e) => serialize(e, p, 1)),
  };
}

/** Runs the item at `path` (indices into items/sub) of the menu at x,y. */
export function radialSelect(x: number, y: number, path: number[]): ActionResult {
  const p = paramsFor(x, y);
  if (!p) return close;
  let list = rootItems(p);
  let e: MenuElement | undefined;
  for (let i = 0; i < path.length; i++) {
    e = list[path[i]];
    if (!e) return close;
    if (i < path.length - 1) list = e.subMenu ? e.subMenu(p) : [];
  }
  if (!e || e.disabled(p) || !e.action) return {};
  return e.action(p);
}

export function radialCenter(x: number, y: number): ActionResult {
  const p = paramsFor(x, y);
  if (!p || centerDisabled(p)) return {};
  return centerAction(p);
}

export { lastTile };
