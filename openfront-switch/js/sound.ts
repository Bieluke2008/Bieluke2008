// SoundEffectController: the cues the web client plays for what happens in
// the game, queued on events.ts's `sounds` for the renderer (src/sound.c).
import { MessageType, PlayerType, UnitType } from "@openfront/engine-api/game/GameTypes";
import { GameUpdateType, GameUpdateViewData } from "@openfront/engine-api/game/GameUpdates";
import type { Unit } from "@openfront/engine/game/Game";
import { sounds } from "./events";
import { game, me } from "./state";

// A MIRV rains hundreds of warheads; at most one boom per interval.
const MIRV_HIT_SOUND_INTERVAL_TICKS = 5;
const NUKE_WARNING_SOUND_INTERVAL_TICKS = 10;
const TRAIN_STATION_SOUND_INTERVAL_TICKS = 10;

const STATION_CAPABLE_TYPES = new Set<UnitType>([UnitType.City, UnitType.Factory, UnitType.Port]);
const NUKE_INBOUND_MESSAGES = new Set<MessageType>([
  MessageType.NUKE_INBOUND,
  MessageType.HYDROGEN_BOMB_INBOUND,
  MessageType.MIRV_INBOUND,
]);

let lastMirvHitSoundTick = -Infinity;
let lastNukeWarningSoundTick = -Infinity;
let lastTrainStationSoundTick = -Infinity;
const hadTrainStation = new Map<number, boolean>();
const seenUnits = new Set<number>();

export function resetSounds(): void {
  lastMirvHitSoundTick = lastNukeWarningSoundTick = lastTrainStationSoundTick = -Infinity;
  hadTrainStation.clear();
  seenUnits.clear();
}

export function soundUpdates(gu: GameUpdateViewData): void {
  const g = game();
  const u = gu.updates;
  const tick = g.ticks();
  for (const update of u[GameUpdateType.Unit] ?? []) {
    const unit = g.unit(update.id) as Unit | undefined;
    if (unit === undefined) continue;
    // A unit's first update is the one of the tick it was created on.
    if (!seenUnits.has(update.id)) {
      seenUnits.add(update.id);
      if (unit.isActive()) onCreated(unit);
    }
    if (STATION_CAPABLE_TYPES.has(unit.type())) onTrainStation(unit, tick);
    switch (unit.type()) {
      case UnitType.AtomBomb:
        if (!unit.isActive() && unit.reachedTarget()) sounds.push("atom-hit");
        break;
      case UnitType.HydrogenBomb:
        if (!unit.isActive() && unit.reachedTarget()) sounds.push("hydrogen-hit");
        break;
      case UnitType.MIRVWarhead:
        if (!unit.isActive() && unit.reachedTarget() && tick - lastMirvHitSoundTick >= MIRV_HIT_SOUND_INTERVAL_TICKS) {
          lastMirvHitSoundTick = tick;
          sounds.push("atom-hit");
        }
        break;
    }
    if (!unit.isActive()) seenUnits.delete(update.id);
  }
  if ((u[GameUpdateType.SpawnPhaseEnd] ?? []).length > 0) sounds.push("game-start");
  const my = me();
  if (my === null) return;
  for (const c of u[GameUpdateType.ConquestEvent] ?? []) {
    if (c.conquerorId !== my.id()) continue;
    // Battle cue for a real player; the ka-ching payout for bots and nations.
    const conquered = g.player(c.conqueredId);
    sounds.push(conquered.type() === PlayerType.Human ? "conquered" : "ka-ching");
  }
  for (const inc of u[GameUpdateType.UnitIncoming] ?? []) {
    if (inc.playerID !== my.smallID() || !NUKE_INBOUND_MESSAGES.has(inc.messageType)) continue;
    if (tick - lastNukeWarningSoundTick < NUKE_WARNING_SOUND_INTERVAL_TICKS) continue;
    lastNukeWarningSoundTick = tick;
    sounds.push("nuke-warning");
  }
}

function onCreated(unit: Unit): void {
  const mine = unit.owner() === me();
  switch (unit.type()) {
    case UnitType.AtomBomb:
      sounds.push("atom-launch");
      break;
    case UnitType.HydrogenBomb:
      sounds.push("hydrogen-launch");
      break;
    case UnitType.MIRV:
      sounds.push("mirv-launch");
      break;
    case UnitType.Warship:
      if (mine) sounds.push("build-warship");
      break;
    case UnitType.City:
      if (mine) sounds.push("build-city");
      break;
    case UnitType.Port:
      if (mine) sounds.push("build-port");
      break;
    case UnitType.DefensePost:
      if (mine) sounds.push("build-defense-post");
      break;
    case UnitType.SAMLauncher:
      if (mine) sounds.push("sam-built");
      break;
    case UnitType.MissileSilo:
      if (mine) sounds.push("silo-built");
      break;
    case UnitType.Factory:
      if (mine) sounds.push("build-factory");
      break;
    case UnitType.TransportShip:
      if (mine) sounds.push("transport-ship");
      break;
  }
}

// A train station is a flag on a structure: the cue plays on its false→true edge.
function onTrainStation(unit: Unit, tick: number): void {
  if (!unit.isActive()) {
    hadTrainStation.delete(unit.id());
    return;
  }
  const has = unit.hasTrainStation();
  const prev = hadTrainStation.get(unit.id());
  if (prev === false && has && unit.owner() === me()) {
    if (tick - lastTrainStationSoundTick >= TRAIN_STATION_SOUND_INTERVAL_TICKS) {
      lastTrainStationSoundTick = tick;
      sounds.push("build-train-station");
    }
  }
  hadTrainStation.set(unit.id(), has);
}
