'use strict';
import { h, useState, useEffect, useRef, html } from './bundle.js';
import {
  Icons, tipColors, Card, Colored, Setting, SectionTitle, Notification, ConfigActions,
  present, num, age, DASH, normUid, isHexUid, ZERO_UID, latLon,
} from './components.js';
import { validateConfig, slotFromOffset, offsetFromSlot, offsetGeometryError } from './follow-logic.js';

// Duplicated deliberately, one line per module: scripts/mock_server.py rewrites
// this exact statement in every .js it serves, and an imported constant would
// survive the rewrite pointing at the wrong host. See main.js.
const ENDPOINT_PREFIX = window.location.port === '' ? '' : 'http://192.168.4.1';

function api(path, opts) {
  return fetch(ENDPOINT_PREFIX + path, opts).then(r => r.text().then(body => {
    if (!r.ok) throw new Error(body || ('HTTP ' + r.status));
    if (!body) return null;
    try { return JSON.parse(body); } catch (e) { return body; }
  }));
}

const HEADING_MODES = [
  ['OFF', "Off - don't touch heading"],
  ['COURSE', "Course - match the leader's direction of travel"],
  ['POINT_LEADER', "Point at leader - aim the nose at their live position"],
  ['FIXED', 'Fixed - hold the absolute heading below'],
  ['COURSE_RELATIVE', "Course relative - the leader's course plus the offset below"],
];

const LOCK_STATES = {
  IDLE: ['Idle', tipColors.gray, 'The trigger is not active, so nothing is being commanded.'],
  ACQUIRING: ['Acquiring', tipColors.yellow, 'Triggered, but no followable peer yet - no fix, too stale, or nothing in range.'],
  LOCKED: ['Locked', tipColors.green, 'Following a live peer. Waypoints are going to the flight controller.'],
  LOCKED_HOLDING: ['Holding', tipColors.yellow, 'Locked, but the leader has gone quiet - flying the last good solution.'],
};

// FollowConditionCode, reported through the condition-flags GVAR. Sequential,
// not a bitmask: when several are true in one cycle the highest value wins.
const CONDITION_CODES = {
  0: ['None', 'Nothing to report this cycle.'],
  1: ['Altitude floor clamped', 'The commanded altitude hit minAltM and was raised to it.'],
  2: ['Target too far', 'The solved slot was further than maxTargetDistM, so it was refused.'],
  3: ['RC gap settings invalid', 'The RC-derived slot failed the geometry rules and the last good one is frozen in.'],
};

const STATUS_GVAR_VALUES = { 0: 'IDLE', 1: 'ACQUIRING', 2: 'LOCKED', 3: 'HOLDING' };

// /api/status omits a GVAR value it has never published. That covers both "the
// slot is switched off" and "the controller has not run a cycle yet", which are
// very different things to be looking at - and the config right next to it is
// what tells them apart.
const gvarAbsentLabel = (cfg, key) =>
  (cfg && cfg[key] === -1 ? 'disabled' : 'not published yet');

// GVAR / RC pickers. -1 is a real, meaningful value in both - it means the
// feature writes nothing at all - so it gets a name rather than a number.
const gvarOptions = [[-1, 'Disabled']].concat([0, 1, 2, 3, 4, 5, 6, 7].map(i => [i, 'GVAR ' + i]));
const rcOptions = [[-1, 'Disabled']].concat(
  Array.apply(null, { length: 16 }).map((_, i) => [i + 1, 'Channel ' + (i + 1)]));

// ---- The friendly slot grid --------------------------------------------------
//
// AHEAD/BEHIND, LEFT/RIGHT, ABOVE/BELOW is a view, not a storage format. What is
// stored and flown is the signed track-relative metres underneath, and they stay
// on screen so the two can never drift apart in someone's head.

const AXES = [
  { key: 'ofsLongM', label: 'Fore / aft', pos: 'AHEAD', neg: 'BEHIND', zero: 'IN LINE',
    tip: 'Along the leader\'s track. Behind is the conventional chase slot; ahead means they are closing on you.' },
  { key: 'ofsLatM', label: 'Left / right', pos: 'RIGHT', neg: 'LEFT', zero: 'CENTRED',
    tip: 'Across the leader\'s track, from their point of view.' },
  { key: 'ofsVertM', label: 'Up / down', pos: 'ABOVE', neg: 'BELOW', zero: 'LEVEL',
    tip: 'Vertical separation. A slot that is directly above or below with no horizontal offset has to clear the minimum vertical separation instead of the 3D one, because GPS altitude error is the thing being absorbed.' },
];

function OffsetEditor({ cfg, setField }) {
  return html`
<div>
  ${AXES.map(a => {
    const v = +cfg[a.key] || 0;
    const slot = slotFromOffset(v, a.pos, a.neg, a.zero);
    const gap = Math.abs(v);
    const opts = [[a.pos, a.pos], [a.zero, a.zero], [a.neg, a.neg]];
    return html`
    <div key=${a.key} class="grid grid-cols-2 gap-2 my-1">
      <label class="flex items-center text-sm text-gray-700 dark:text-slate-300 mr-2 font-medium">
        ${a.label}<${Tip} text=${a.tip} />
      <//>
      <div class="flex items-center gap-2">
        <div class="flex-1">
          ${h(SlotSelect, { value: slot, options: opts, setfn: s => setField(a.key, offsetFromSlot(s, gap, a.pos, a.neg)) })}
        <//>
        <div class="flex-1">
          ${h(GapInput, {
            value: gap, disabled: slot === a.zero,
            setfn: g => setField(a.key, offsetFromSlot(slot, g, a.pos, a.neg)),
          })}
        <//>
      <//>
    <//>`;
  })}
  <p class="text-xs text-gray-400 mt-2 font-mono">
    stored as ofsLongM ${signed(cfg.ofsLongM)} · ofsLatM ${signed(cfg.ofsLatM)} · ofsVertM ${signed(cfg.ofsVertM)} (metres)
  <//>
<//>`;
}

const signed = v => (present(v) ? (v > 0 ? '+' : '') + (+v) : DASH);

const Tip = ({ text }) => (text ? html`
<span class="tooltip-wrap" tabindex="0">
  <${Icons.info} class="w-4 h-4 tooltip-icon" />
  <span class="tooltip-bubble" role="tooltip">${text}<//>
<//>` : '');

const SlotSelect = ({ value, options, setfn }) => html`
<select onchange=${ev => setfn(ev.target.value)}
  class="w-full rounded font-normal border border-gray-300 dark:border-slate-600 field-bg py-0.5 px-1 text-gray-600 dark:text-slate-200 focus:outline-none text-sm">
  ${options.map(o => html`<option key=${o[0]} value=${o[0]} selected=${o[0] === value}>${o[1]}<//>`)}
<//>`;

const GapInput = ({ value, setfn, disabled }) => html`
<div class="flex w-full items-center rounded border border-gray-300 dark:border-slate-600 shadow-sm">
  <input type="number" value=${value} disabled=${disabled} oninput=${ev => setfn(ev.target.value)}
    class="font-normal text-sm rounded w-full flex-1 py-0.5 px-2 field-bg text-gray-700 dark:text-slate-200 focus:outline-none disabled:cursor-not-allowed disabled:bg-gray-100 dark:disabled:bg-slate-800 disabled:text-gray-500" />
  <span class="inline-flex font-normal py-1 border-l border-gray-300 dark:border-slate-600 bg-slate-100 dark:bg-slate-700 items-center px-2 text-gray-500 dark:text-slate-400 text-xs">m<//>
<//>`;

/** The same friendly grid, read-only, over a live signed offset from the API. */
function OffsetReadout({ offset, title, note }) {
  if (!offset) {
    return html`
    <div>
      <div class="text-xs uppercase tracking-wide text-gray-400 mb-1">${title}<//>
      <p class="text-sm text-slate-400">none yet<//>
    <//>`;
  }
  const rows = [
    [offset.long_m, 'AHEAD', 'BEHIND', 'IN LINE'],
    [offset.lat_m, 'RIGHT', 'LEFT', 'CENTRED'],
    [offset.vert_m, 'ABOVE', 'BELOW', 'LEVEL'],
  ];
  return html`
<div>
  <div class="text-xs uppercase tracking-wide text-gray-400 mb-1">${title}<//>
  <div class="grid grid-cols-3 gap-2">
    ${rows.map((r, i) => html`
    <div key=${i} class="rounded bg-slate-100 dark:bg-slate-700 px-2 py-1">
      <div class="text-xs font-semibold text-slate-600 dark:text-slate-200">${slotFromOffset(r[0], r[1], r[2], r[3])}<//>
      <div class="font-mono text-sm text-slate-800 dark:text-slate-100">${num(Math.abs(r[0]), 1, ' m')}<//>
    <//>`)}
  <//>
  <p class="text-xs text-gray-400 mt-1 font-mono">${signed(round1(offset.long_m))} / ${signed(round1(offset.lat_m))} / ${signed(round1(offset.vert_m))} m</p>
  ${note && html`<p class="text-xs text-gray-400 mt-1">${note}<//>`}
<//>`;
}

const round1 = v => (present(v) ? Math.round(v * 10) / 10 : v);

// ---- Live status -------------------------------------------------------------

const StatRow = ({ label, value, tip }) => html`
<div class="flex items-baseline justify-between gap-2 py-0.5">
  <span class="text-sm text-gray-500 dark:text-slate-400">${label}<${Tip} text=${tip} /><//>
  <span class="text-sm font-mono text-slate-800 dark:text-slate-100">${value}<//>
<//>`;

function FollowStatusPanel({ status, cfg }) {
  const f = (status && status.follow) || null;
  if (!f) {
    return html`<${Card} title="Live" icon=${Icons.scan}>
      <p class="text-sm text-slate-400">This build reports no Follow controller.<//>
    <//>`;
  }
  const st = LOCK_STATES[f.state] || [f.state, tipColors.gray, ''];
  const lockedUid = f.locked_uid && f.locked_uid !== '00000000' ? f.locked_uid : null;
  const fc = (status && status.fc) || {};
  const platform = { 0: 'multirotor', 1: 'airplane', 255: 'not answered yet' }[f.platform];

  // Pre-arm warnings. These are the things that will stop Follow doing anything
  // useful the moment the trigger goes active, and they are worth surfacing on
  // the ground rather than discovering in the air.
  const warnings = [];
  if (!fc.connected) warnings.push('No flight controller on MSP. Nothing can be commanded.');
  if (status && status.location && !status.location.valid) warnings.push('This node has no position fix.');
  if (f.prearm_failed) {
    warnings.push('The RC-derived slot failed its pre-arm check, so the last known good offset is frozen in. Compare the two offsets below.');
  }
  if (f.state === 'ACQUIRING') warnings.push('Triggered but unlocked: no peer currently has a fresh position fix within range.');
  if (status && status.node && status.node.listen_only) warnings.push('Listen-only is on, so this node is invisible to the aircraft it is chasing.');

  return html`
<${Card} title="Live" icon=${Icons.scan}
  right=${html`<${Colored} text=${st[0]} colors=${st[1]} title=${st[2]} />`}>
  ${warnings.length > 0 && html`
  <div class="mb-3 rounded bg-yellow-100 dark:bg-yellow-800 px-3 py-2">
    <div class="flex items-center gap-2 text-sm font-semibold text-yellow-900 dark:text-yellow-100">
      <${Icons.warn} class="w-4 h-4" /> Pre-arm
    <//>
    <ul class="mt-1">
      ${warnings.map((w, i) => html`<li key=${i} class="text-xs text-yellow-900 dark:text-yellow-100">· ${w}<//>`)}
    <//>
  <//>`}

  <${StatRow} label="Trigger gate" value=${f.gate_active ? 'active' : 'inactive'}
    tip="Whether the configured trigger (GCS-NAV mode, or the AUX switch) is currently asserted." />
  <${StatRow} label="Locked peer" value=${lockedUid ? `${f.locked_name || ''} ${lockedUid}` : 'none'} />
  <${StatRow} label="FC" value=${fc.connected ? `${fc.variant || '?'} ${fc.version || ''}` : 'not connected'} />
  <${StatRow} label="Platform" value=${platform || DASH}
    tip="INAV's mixer platform type, as the FC reported it. Autothrottle only applies to fixed wing." />

  <${SectionTitle} title="Target" />
  ${f.target ? html`
    <${StatRow} label="Position" value=${latLon(f.target.lat, f.target.lon)} />
    <${StatRow} label="Altitude" value=${num(f.target.alt_cm / 100, 1, ' m')}
      tip="Home-relative, as sent to the flight controller in waypoint 255." />
    <${StatRow} label="Heading" value=${present(f.target.heading_deg) && f.target.heading_deg ? f.target.heading_deg + '°' : 'not commanded'} />
    <${StatRow} label="Solved" value=${age(f.target.age_ms) + ' ago'} />
  ` : html`<p class="text-sm text-slate-400">No target solved yet.<//>`}

  <${SectionTitle} title="Offsets" />
  <div class="flex flex-col gap-3">
    <${OffsetReadout} offset=${f.live_offset} title="Live offset"
      note="Where the slot actually is right now, including any RC axis trim." />
    ${f.prearm_offset && html`
      <${OffsetReadout} offset=${f.prearm_offset} title="Pre-arm candidate"
        note=${f.prearm_failed
          ? 'This is what the sticks were asking for when the pre-arm check refused it.'
          : 'The offset the RC channels produced at the pre-arm check.'} />`}
  <//>
  ${f.rc_slot_frozen && html`
  <p class="text-xs text-yellow-700 dark:text-yellow-200 mt-2">
    RC slot frozen: the live stick positions are being ignored and the last valid offset is held.
  <//>`}

  <${SectionTitle} title="Reported to the FC" />
  <${StatRow} label="Status GVAR"
    value=${present(f.status_gvar)
      ? `${f.status_gvar} (${STATUS_GVAR_VALUES[f.status_gvar] || '?'})`
      : gvarAbsentLabel(cfg, 'statusGvarIndex')}
    tip="The API omits this while no value has been published, which is not the same as it reading 0 - 0 is a real value meaning IDLE." />
  <${StatRow} label="Condition GVAR"
    value=${present(f.condition_gvar)
      ? `${f.condition_gvar} (${(CONDITION_CODES[f.condition_gvar] || ['?'])[0]})`
      : gvarAbsentLabel(cfg, 'conditionFlagsGvarIndex')}
    tip=${present(f.condition_gvar)
      ? (CONDITION_CODES[f.condition_gvar] || ['', ''])[1]
      : 'No value published yet. 0 (none) is a real value, so this is not it.'} />
  <${StatRow} label="Autothrottle"
    value=${f.autothrottle_armed ? (f.autothrottle_engaged ? 'engaged' : 'armed') : 'not armed'} />
  ${f.autothrottle_engaged && html`
  <${StatRow} label="Target speed" value=${num((f.target_speed_cms || 0) / 100, 1, ' m/s')} />`}
<//>`;
}

// ---- The page ----------------------------------------------------------------

export default function FollowPage({ status }) {
  const [cfg, setCfg] = useState(null);
  const [base, setBase] = useState(null);
  const [result, setResult] = useState(null);
  const [unsaved, setUnsaved] = useState(false);
  const [uidText, setUidText] = useState('');
  const loaded = useRef(false);

  const load = () => api('/api/config').then(r => {
    setCfg(r.follow);
    setBase(r.follow);
    if (!loaded.current) {
      loaded.current = true;
      const t = r.follow.targetUid;
      setUidText(t && t !== ZERO_UID ? t : '');
    }
  });
  useEffect(() => { load().catch(e => setResult({ ok: false, text: e.message })); }, []);

  const setField = (k, v) => setCfg(c => ({ ...c, [k]: v }));
  const mk = k => v => setField(k, v);
  // Numeric fields come out of the inputs as strings; the validator and the
  // firmware both want numbers, so coerce once, here, rather than everywhere.
  const mkNum = k => v => setField(k, v === '' ? '' : +v);

  const uidBad = uidText !== '' && !isHexUid(uidText);
  const candidate = cfg ? { ...cfg, targetUid: uidText === '' ? ZERO_UID : normUid(uidText) } : null;
  const numericCandidate = candidate ? coerceFollowNumbers(candidate, base) : null;
  const invalid = numericCandidate ? validateConfig(numericCandidate) : null;
  const blocked = uidBad
    ? 'Target UID must be 1-8 hexadecimal characters, or empty for "nearest followable peer".'
    : invalid ? invalid.message : null;

  const post = () => api('/api/config', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ follow: numericCandidate }),
  }).then(r => { setCfg(r.follow); setBase(r.follow); return r; });

  const apply = () => post()
    .then(() => { setUnsaved(true); setResult({ ok: true, text: 'Applied to the running node' }); })
    .catch(e => setResult({ ok: false, text: e.message }));
  const save = () => post()
    .then(() => api('/api/config/save', { method: 'POST' }))
    .then(() => { setUnsaved(false); setResult({ ok: true, text: 'Saved to flash' }); })
    .catch(e => setResult({ ok: false, text: e.message }));

  if (!cfg) return html`<div class="m-4 text-sm text-slate-400">Loading Follow configuration…<//>`;

  const peers = (status && status.peers) || [];
  const err = sec => (invalid && invalid.section === sec
    ? html`<p class="text-xs text-red-600 mt-1">${invalid.message}<//>` : '');
  // The slot card only owns the geometry rules. Everything else that reports
  // itself as a bounds problem (a bad maxTargetDistM, say) belongs under the
  // bounds inputs, not under the offsets it has nothing to do with.
  const geoErr = offsetGeometryError(
    { longitudinal_m: cfg.ofsLongM, lateral_m: cfg.ofsLatM, vertical_m: cfg.ofsVertM },
    cfg.minSepM, cfg.minVSepM);
  const rcInUse = cfg.rcLongChannel !== -1 || cfg.rcLatChannel !== -1 || cfg.rcVertChannel !== -1;
  const atInUse = cfg.autothrottleEnableRcChannel !== -1;

  return html`
<div class="m-4 grid grid-cols-1 gap-4 lg:grid-cols-3">
  <div class="lg:col-span-2 flex flex-col gap-4">
    <${Card} title="Slot geometry" icon=${Icons.scan}>
      <p class="text-xs text-gray-400 mb-3">
        Where to sit relative to the leader, in their own track-relative frame - so the slot rotates with them
        rather than staying pinned to a compass direction.
      <//>
      <${OffsetEditor} cfg=${cfg} setField=${setField} />
      ${geoErr && html`<p class="text-xs text-red-600 mt-1">${geoErr}<//>`}
    <//>

    <${Card} title="Trigger and target" icon=${Icons.bolt}>
      <${Setting} title="Trigger" value=${cfg.triggerMode || 'GCSNAV'} type="static"
        tip="Compiled in at build time (FOLLOW_TRIGGER_MODE), not editable here. GCSNAV follows while INAV's GCS-NAV mode is on; AUX follows a switch." />
      <div class="grid grid-cols-2 gap-2 my-1">
        <label class="flex items-center text-sm text-gray-700 dark:text-slate-300 mr-2 font-medium">
          Target peer<${Tip} text="Empty locks onto the nearest followable peer at acquire time. A UID pins it to one specific aircraft - and because a UID is a stable identity, it cannot be inherited by a different aircraft the way a v1 slot id could." />
        <//>
        <div class="flex flex-col">
          <div class="flex w-full items-center rounded border ${uidBad ? 'border-red-500' : 'border-gray-300 dark:border-slate-600'} shadow-sm">
            <input type="text" value=${uidText} placeholder="(nearest peer)" spellcheck="false"
              oninput=${ev => setUidText(ev.target.value.trim())}
              class="font-normal font-mono text-sm rounded w-full flex-1 py-0.5 px-2 field-bg text-gray-700 dark:text-slate-200 focus:outline-none" />
          <//>
          ${peers.length > 0 && html`
          <div class="flex flex-wrap gap-1 mt-1">
            <button type="button" onclick=${() => setUidText('')}
              class="px-1.5 py-0.5 text-xs rounded bg-slate-100 dark:bg-slate-700 text-slate-600 dark:text-slate-300 hover:bg-slate-200">nearest<//>
            ${peers.map(p => html`<button key=${p.uid} type="button" onclick=${() => setUidText(p.uid)}
              class="px-1.5 py-0.5 text-xs font-mono rounded ${uidText === p.uid ? 'bg-blue-600 text-white' : 'bg-slate-100 dark:bg-slate-700 text-slate-600 dark:text-slate-300 hover:bg-slate-200'}">${p.name || p.uid}<//>`)}
          <//>`}
        <//>
      <//>
      <${Setting} title="Update rate" value=${cfg.emitHz} setfn=${mkNum('emitHz')} type="number" addonRight="Hz"
        tip="How often the control cycle runs and a fresh waypoint goes to the flight controller. Faster tracks better but costs MSP bandwidth on the same link the rest of the telemetry uses." />
      <${Setting} title="Peer timeout" value=${cfg.peerTimeoutMs} setfn=${mkNum('peerTimeoutMs')} type="number" addonRight="ms"
        tip="How stale the leader's last POSITION may be before the lock goes to holding. An announce alone does not count - a peer beaconing without a fix is not followable." />
      ${err('trigger')}
    <//>

    <${Card} title="Safety bounds" icon=${Icons.shield}>
      <p class="text-xs text-gray-400 mb-3">
        These are refusals, not suggestions: a slot that fails them is rejected by the firmware as well, so a
        config accepted here can never be turned down later.
      <//>
      <${Setting} title="Min separation" value=${cfg.minSepM} setfn=${mkNum('minSepM')} type="number" addonRight="m"
        tip="Minimum 3D magnitude of the slot. It forbids the degenerate 'fly into the leader' offset outright." />
      <${Setting} title="Min vertical separation" value=${cfg.minVSepM} setfn=${mkNum('minVSepM')} type="number" addonRight="m"
        tip="Applies only to stacked slots - directly above or below with essentially no horizontal offset. It is sized to absorb GPS vertical error, which is far worse than horizontal, not just physical clearance." />
      <${Setting} title="Max target distance" value=${cfg.maxTargetDistM} setfn=${mkNum('maxTargetDistM')} type="number" addonRight="m"
        tip="Runtime sanity bound on how far the solved target may be from us. A solution beyond it is refused rather than chased, which is what stops a stale or spoofed position dragging the aircraft away." />
      <${Setting} title="Altitude floor" value=${cfg.minAltM} setfn=${mkNum('minAltM')} type="number" addonRight="m"
        tip="Absolute floor on the commanded home-relative altitude. A clamp rather than a refusal: the target is raised to it and the condition GVAR says so." />
      <${Setting} title="Min course speed" value=${cfg.minCourseSpeed} setfn=${mkNum('minCourseSpeed')} type="number" addonRight="m/s"
        tip="Below this leader ground speed their reported course is noise, so the last valid course is held instead - otherwise a hovering leader would spin the whole slot geometry around." />
      ${err('bounds')}
    <//>

    <${Card} title="Heading" icon=${Icons.scan}>
      <${Setting} title="Heading mode" value=${cfg.headingMode} setfn=${mk('headingMode')} type="select" options=${HEADING_MODES}
        tip="What to command as nose heading, sent through waypoint 255's p1 field and MSP_SET_HEAD. Off leaves the FC's own heading logic alone." />
      <${Setting} title="Heading angle" value=${cfg.headingDeg} setfn=${mkNum('headingDeg')} type="number" addonRight="°"
        disabled=${cfg.headingMode !== 'FIXED' && cfg.headingMode !== 'COURSE_RELATIVE'}
        tip="An absolute compass heading in Fixed mode, or an offset added to the leader's course in Course-relative mode." />
    <//>

    <${Card} title="RC axis control" icon=${Icons.bolt}>
      <p class="text-xs text-gray-400 mb-3">
        Optionally drive one or more slot axes from a stick or knob, so the slot can be trimmed in flight.
        An axis left disabled uses its configured offset above. Each axis needs its own channel.
      <//>
      <${Setting} title="Fore / aft channel" value=${cfg.rcLongChannel} setfn=${mk('rcLongChannel')} type="select" options=${rcOptions} />
      <${Setting} title="Left / right channel" value=${cfg.rcLatChannel} setfn=${mk('rcLatChannel')} type="select" options=${rcOptions} />
      <${Setting} title="Up / down channel" value=${cfg.rcVertChannel} setfn=${mk('rcVertChannel')} type="select" options=${rcOptions} />
      ${rcInUse && html`<p class="text-xs text-gray-400 mt-2">
        A live RC slot still has to pass the same geometry rules. When it does not, the last valid offset is
        frozen in and the condition GVAR reports it, rather than the aircraft flying the bad slot.
      <//>`}
      ${err('rc')}
    <//>
  <//>

  <div class="flex flex-col gap-4">
    <${FollowStatusPanel} status=${status} cfg=${cfg} />

    <${Card} title="GVAR reporting" icon=${Icons.list}>
      <p class="text-xs text-gray-400 mb-3">
        INAV global variables Follow writes, so logic conditions and the OSD can react to lock state.
        Disabled means nothing is sent at all - zero MSP traffic until a pilot opts in.
      <//>
      <${Setting} title="Status GVAR" value=${cfg.statusGvarIndex} setfn=${mk('statusGvarIndex')} type="select" options=${gvarOptions}
        tip="Receives 0 idle, 1 acquiring, 2 locked, 3 holding. Never renumber these in a logic condition." />
      <${Setting} title="Condition GVAR" value=${cfg.conditionFlagsGvarIndex} setfn=${mk('conditionFlagsGvarIndex')} type="select" options=${gvarOptions}
        tip="Receives a condition code: 0 none, 1 altitude floor clamped, 2 target too far, 3 RC gap settings invalid. Sequential rather than a bitmask - if several are true in one cycle the highest wins." />
      <${Setting} title="Debug GVARs" value=${cfg.debug} setfn=${mk('debug')} type="switch"
        tip="Writes the raw north/east/altitude/heading solution into GVARs 0-3 for bench work. RAM only: it is never persisted and is off again after a reboot." />
      ${err('gvar')}
    <//>

    <${Card} title="Autothrottle" icon=${Icons.bolt}>
      <p class="text-xs text-gray-400 mb-3">
        Fixed-wing only. Follow publishes a target speed that an INAV logic condition can feed to cruise
        throttle, so the follower closes or opens the slot gap instead of only steering at it.
      <//>
      <${Setting} title="Arm channel" value=${cfg.autothrottleEnableRcChannel} setfn=${mk('autothrottleEnableRcChannel')} type="select" options=${rcOptions}
        tip="The switch that arms autothrottle. Until this is set, none of the speed settings below matter and their defaults are allowed to sit un-configured." />
      <${Setting} title="Arm range min" value=${cfg.autothrottleEnableMinThresholdUs} setfn=${mkNum('autothrottleEnableMinThresholdUs')} type="number" addonRight="µs"
        disabled=${!atInUse} tip="Autothrottle arms while that channel reads between these two pulse widths." />
      <${Setting} title="Arm range max" value=${cfg.autothrottleEnableMaxThresholdUs} setfn=${mkNum('autothrottleEnableMaxThresholdUs')} type="number" addonRight="µs"
        disabled=${!atInUse} />
      <${Setting} title="Min speed" value=${cfg.minTargetSpeedMps} setfn=${mkNum('minTargetSpeedMps')} type="number" addonRight="m/s"
        disabled=${!atInUse} tip="Lower clamp on the published target speed. Keep it above the follower's stall speed - this number is the only thing between a slot-lag correction and a spin." />
      <${Setting} title="Max speed" value=${cfg.maxTargetSpeedMps} setfn=${mkNum('maxTargetSpeedMps')} type="number" addonRight="m/s"
        disabled=${!atInUse} tip="Upper clamp. Must be greater than the minimum." />
      <${Setting} title="Slot-lag accel" value=${cfg.speedCorrectionAccelCmS2} setfn=${mkNum('speedCorrectionAccelCmS2')} type="number" addonRight="cm/s²"
        disabled=${!atInUse}
        tip="How hard to correct along-track position error, as a kinematic braking law. 0 is pure feedforward - match the leader's speed and let the gap be whatever it is." />
      <${Setting} title="Target speed GVAR" value=${cfg.targetSpeedGvarIndex} setfn=${mk('targetSpeedGvarIndex')} type="select" options=${gvarOptions}
        tip="Receives the target speed in cm/s while autothrottle is engaged." />
      <${Setting} title="Engage GVAR" value=${cfg.autothrottleEngageGvarIndex} setfn=${mk('autothrottleEngageGvarIndex')} type="select" options=${gvarOptions}
        tip="Receives 1 while autothrottle is engaged and 0 otherwise, so a logic condition can gate the throttle override on it." />
      ${err('autothrottle')}
    <//>

    <${Card}>
      ${result && html`<${Notification} ok=${result.ok} timeout=${result.ok ? 2500 : 9000}
        text=${result.text} close=${() => setResult(null)} />`}
      <${ConfigActions} onApply=${apply} onSave=${save} unsaved=${unsaved}
        disabled=${!!blocked} blockedReason=${blocked} />
    <//>
  <//>
<//>`;
}

// Same job as main.js's coerceNumbers, scoped to the flat Follow block: a
// half-typed number must not be posted as 0.
function coerceFollowNumbers(edited, base) {
  const out = {};
  Object.keys(edited).forEach(k => {
    const e = edited[k], b = base ? base[k] : undefined;
    if (typeof b === 'number' && (e === '' || e === null || !isFinite(+e))) out[k] = b;
    else if (typeof b === 'number') out[k] = +e;
    else out[k] = e;
  });
  return out;
}
