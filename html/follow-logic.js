'use strict';

// Pure Follow logic -- no DOM, no Preact, no fetch. Importable both by follow.js
// (the UI) and by a plain Node test (`node --test`), which is why it has no
// imports of its own: adding one would drag the whole bundle into the test.
//
// Everything here mirrors a rule that the firmware also enforces
// (ff::followValidateConfig in lib/ff_core/follow.cpp). Client-side validation
// is a UX nicety -- it lets Save say *which* field is wrong before a round trip
// -- and never a substitute for the server-side check, which a raw REST client
// gets regardless.

// ff::kFollowStackedHorizontalEpsilonM. Below this horizontal magnitude a slot
// counts as "stacked", and the vertical gap rule applies instead.
export const STACKED_HORIZONTAL_EPSILON_M = 0.5;

// ff::kMspMaxRcChannels -- MSP_RC's channel count.
export const MSP_MAX_SUPPORTED_CHANNELS = 16;

// GVAR slots INAV exposes.
const MAX_GVAR_INDEX = 7;

// The AHEAD/BEHIND, LEFT/RIGHT, ABOVE/BELOW grid is purely a client-side view
// over the canonical signed offsets that are actually stored and flown
// (ofsLongM / ofsLatM / ofsVertM, metres in the leader's track-relative frame).
// The firmware only ever sees the signed numbers.
export function slotFromOffset(v, posLabel, negLabel, zeroLabel) {
  return v > 0 ? posLabel : v < 0 ? negLabel : zeroLabel;
}

// The inverse: ('BEHIND', 15) back to -15. Takes the magnitude of gapM so a
// stray minus typed into the gap box can't silently flip the slot.
export function offsetFromSlot(slot, gapM, posLabel, negLabel) {
  const g = Math.abs(+gapM);
  return slot === posLabel ? g : slot === negLabel ? -g : 0;
}

// The geometry half of the check, split out because service() applies the same
// rules to a live RC-derived offset, not just the configured one.
// Returns a message string, or null when the offset is acceptable.
export function offsetGeometryError(offset, minSepM, minVSepM) {
  const long = +offset.longitudinal_m, lat = +offset.lateral_m, vert = +offset.vertical_m;
  const horizontalMag = Math.sqrt(long * long + lat * lat);
  const mag3d = Math.sqrt(horizontalMag * horizontalMag + vert * vert);
  // Minimum 3D separation: forbids the degenerate "fly into the leader" slot.
  if (mag3d < minSepM) return 'slot magnitude is below minSepM (minimum 3D separation)';
  // A stacked slot has to clear the leader by more than GPS vertical error.
  if (horizontalMag < STACKED_HORIZONTAL_EPSILON_M && Math.abs(vert) < minVSepM) {
    return "stacked slot's vertical offset is below minVSepM";
  }
  return null;
}

const GVAR_FIELDS = [
  ['statusGvarIndex', 'Status', 'gvar'],
  ['conditionFlagsGvarIndex', 'Condition Flags', 'gvar'],
  ['targetSpeedGvarIndex', 'Target Speed', 'autothrottle'],
  ['autothrottleEngageGvarIndex', 'Autothrottle Engage', 'autothrottle'],
];

const RC_AXIS_FIELDS = [
  ['rcLongChannel', 'rcLongChannel'],
  ['rcLatChannel', 'rcLatChannel'],
  ['rcVertChannel', 'rcVertChannel'],
];

/**
 * Client-side mirror of ff::followValidateConfig().
 *
 * The rules are checked in the same order the firmware checks them, so a config
 * with two faults reports the same one here as it would over REST -- otherwise
 * fixing the field the UI complained about would just surface a different error
 * from the device, which reads as the UI being wrong.
 *
 * Returns `{ section, message }` -- `section` names the panel that owns the
 * offending field, so the error can be rendered next to the inputs it is about
 * -- or null when the config is valid.
 */
export function validateConfig(cfg) {
  if (!(cfg.emitHz > 0)) return { section: 'trigger', message: 'emitHz must be > 0' };
  if (!(cfg.peerTimeoutMs > 0)) return { section: 'trigger', message: 'peerTimeoutMs must be > 0' };
  if (cfg.minSepM < 0 || cfg.minVSepM < 0 || cfg.minAltM < 0) {
    return { section: 'bounds', message: 'minSepM/minVSepM/minAltM must be >= 0' };
  }
  if (!(cfg.maxTargetDistM > 0)) return { section: 'bounds', message: 'maxTargetDistM must be > 0' };
  if (cfg.minCourseSpeed < 0) return { section: 'bounds', message: 'minCourseSpeed must be >= 0' };

  // targetUid: the firmware has no range rule (all-zero means nearest followable
  // peer, anything else is a valid 32-bit UID). It travels as an 8-char hex
  // string like every other UID, so this only catches what a text box can
  // produce that is not one. Stays quiet when the field is absent, so a partial
  // config still validates.
  if (cfg.targetUid !== undefined && cfg.targetUid !== null) {
    if (!/^[0-9a-fA-F]{1,8}$/.test(String(cfg.targetUid).trim())) {
      return { section: 'trigger', message: 'targetUid must be 1-8 hexadecimal characters' };
    }
  }

  for (const [key, label, section] of GVAR_FIELDS) {
    if (cfg[key] < -1 || cfg[key] > MAX_GVAR_INDEX) {
      return { section, message: `${key} must be -1 (disabled) or 0-${MAX_GVAR_INDEX} (${label})` };
    }
  }
  for (const [key] of RC_AXIS_FIELDS) {
    if (cfg[key] !== -1 && (cfg[key] < 1 || cfg[key] > MSP_MAX_SUPPORTED_CHANNELS)) {
      return { section: 'rc', message: `${key} must be -1 (disabled) or 1-${MSP_MAX_SUPPORTED_CHANNELS}` };
    }
  }
  if (cfg.autothrottleEnableRcChannel !== -1 &&
      (cfg.autothrottleEnableRcChannel < 1 || cfg.autothrottleEnableRcChannel > MSP_MAX_SUPPORTED_CHANNELS)) {
    return { section: 'autothrottle', message: `autothrottleEnableRcChannel must be -1 (disabled) or 1-${MSP_MAX_SUPPORTED_CHANNELS}` };
  }

  // Overlap rules. Two features writing the same GVAR, or two axes reading the
  // same stick, is always a misconfiguration rather than a clever trick.
  const usedGvars = GVAR_FIELDS.filter(([k]) => cfg[k] !== -1);
  for (let i = 0; i < usedGvars.length; i++) {
    for (let j = i + 1; j < usedGvars.length; j++) {
      if (cfg[usedGvars[i][0]] === cfg[usedGvars[j][0]]) {
        return { section: usedGvars[i][2], message: 'GVAR indices must be unique (or -1/disabled)' };
      }
    }
  }
  const rcChannels = RC_AXIS_FIELDS.map(([k]) => cfg[k]).filter(c => c !== -1);
  if (new Set(rcChannels).size !== rcChannels.length) {
    return { section: 'rc', message: 'rcLongChannel/rcLatChannel/rcVertChannel must be unique (or -1/disabled)' };
  }
  if (cfg.autothrottleEnableRcChannel !== -1 && rcChannels.includes(cfg.autothrottleEnableRcChannel)) {
    return { section: 'autothrottle', message: 'autothrottleEnableRcChannel must differ from the RC axis channels (or -1/disabled)' };
  }
  if (cfg.autothrottleEnableMaxThresholdUs <= cfg.autothrottleEnableMinThresholdUs) {
    return { section: 'autothrottle', message: 'autothrottleEnableMaxThresholdUs must be > autothrottleEnableMinThresholdUs' };
  }
  // The speed clamps only matter once a pilot has wired up an arm channel, so
  // the compiled-in 0/0 (an invalid range on its own) can sit there until then.
  if (cfg.autothrottleEnableRcChannel !== -1 &&
      (!(cfg.minTargetSpeedMps > 0) || !(cfg.maxTargetSpeedMps > cfg.minTargetSpeedMps))) {
    return {
      section: 'autothrottle',
      message: 'minTargetSpeedMps must be > 0 and maxTargetSpeedMps must be > minTargetSpeedMps when autothrottleEnableRcChannel is set',
    };
  }
  if (cfg.speedCorrectionAccelCmS2 < 0) {
    // Fed through copysignf() as a magnitude; negative would brake the wrong way.
    return { section: 'autothrottle', message: 'speedCorrectionAccelCmS2 must be >= 0' };
  }

  const geo = offsetGeometryError(
    { longitudinal_m: cfg.ofsLongM, lateral_m: cfg.ofsLatM, vertical_m: cfg.ofsVertM },
    cfg.minSepM, cfg.minVSepM);
  if (geo) return { section: 'bounds', message: geo };

  return null;
}
