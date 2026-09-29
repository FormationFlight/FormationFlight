'use strict';
import { h, render, useRef, useState, useEffect, html, Router } from './bundle.js';
import LoadingSpinner, {
  Icons, tipColors, Button, Colored, Stat, Setting, Notification, Banner, Card, Note,
  SectionTitle, ConfigActions, PeerTable, RadarScope, RadioCard, WifiCard, FrameLogView, Sparkline,
  PowerCard, NoPowerCard, SystemCard, LoopCard, LogView, LOG_FILTERS, GnssCard, GnssLinkCard,
  Th, Td, peerPartial, present, num, age, uptime, latLon, speedMs, courseDeg, DASH,
} from './components.js';
import FollowPage from './follow.js';

const Logo = props => html`<img class=${props.class} src="images/logo.svg"></img>`;

// Served from the node itself, the UI is on port 80 and location.port is empty,
// so every fetch is same-origin. Anything else is a development server, and the
// API then lives on the node's SoftAP address - except under
// scripts/mock_server.py, which rewrites this whole statement to "" on the way
// out so its own fake API answers instead. Keep it a single `const
// ENDPOINT_PREFIX = ...;` line for that reason.
const ENDPOINT_PREFIX = window.location.port === '' ? '' : 'http://192.168.4.1';

/**
 * One fetch wrapper for the whole UI.
 *
 * Errors are `4xx` with a `text/plain` body naming the offending field, and the
 * contract says the UI shows that string verbatim - so the body is read on
 * every response, not just the happy path, and becomes the Error message.
 * Success bodies are JSON for the GETs and plain text ("saved", "ok") for the
 * commands, hence the tolerant parse.
 */
function api(path, opts) {
  return fetch(ENDPOINT_PREFIX + path, opts).then(r => r.text().then(body => {
    if (!r.ok) throw new Error(body || ('HTTP ' + r.status));
    if (!body) return null;
    try { return JSON.parse(body); } catch (e) { return body; }
  }));
}

const postJson = (path, obj) => api(path, {
  method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(obj),
});
const postEmpty = path => api(path, { method: 'POST' });

// A number typed into a text box arrives as '' while it is being cleared, and
// as NaN if it never parsed. Posting either turns a uint32 field into 0 on the
// device, so every numeric field falls back to what the node last told us.
function coerceNumbers(edited, base) {
  const out = {};
  Object.keys(edited).forEach(k => {
    const e = edited[k], b = base ? base[k] : undefined;
    if (e && typeof e === 'object' && !Array.isArray(e)) out[k] = coerceNumbers(e, b || {});
    else if (typeof b === 'number' && (e === '' || e === null || !isFinite(+e))) out[k] = b;
    else if (typeof b === 'number') out[k] = +e;
    else out[k] = e;
  });
  return out;
}

// ---- Chrome ------------------------------------------------------------------

function Header({ status, online, setShowSidebar, showSidebar }) {
  const [rebootResult, setRebootResult] = useState(null);
  // index.html has already seeded the class from localStorage / the OS
  // preference before first paint, so read the truth off the element rather than
  // recomputing it. Toggling records an explicit choice, which then wins over
  // the OS preference on later loads.
  const [dark, setDark] = useState(() => document.documentElement.classList.contains('dark'));
  const toggleTheme = () => {
    const next = !dark;
    setDark(next);
    document.documentElement.classList.toggle('dark', next);
    try { localStorage.setItem('theme', next ? 'dark' : 'light'); } catch (e) { /* no storage */ }
  };
  const rebootAction = () => postEmpty('/api/system/reboot')
    .then(() => setRebootResult({ ok: true, text: 'Rebooting…' }))
    .catch(e => setRebootResult({ ok: false, text: e.message }));

  const node = (status && status.node) || {};
  return html`
<div class="bg-white dark:bg-slate-800 border-b border-gray-200 dark:border-slate-700 sticky top-0 z-[48] py-2 ${showSidebar && 'pl-72'} transition-all duration-300 transform">
  <div class="px-2 w-full flex items-center">
    <button type="button" onclick=${() => setShowSidebar(v => !v)} class="text-slate-400">
      <${Icons.bars3} class="h-6" />
    <//>
    <div class="flex flex-1 gap-x-4 self-stretch lg:gap-x-6">
      <div class="relative flex flex-1"><//>
      <div class="flex items-center gap-x-3 lg:gap-x-4">
        <span class="text-sm text-slate-400 font-mono">${node.uid || DASH}<//>
        <span class="text-sm text-slate-400">${node.name || ''}<//>
        <div class="hidden lg:block lg:h-4 lg:w-px lg:bg-gray-200" aria-hidden="true"><//>
        <span class="text-sm text-slate-400">${uptime(node.uptime_ms)}<//>
        <div class="hidden lg:block lg:h-4 lg:w-px lg:bg-gray-200" aria-hidden="true"><//>
        <${Colored} text="" icon=${Icons.link} colors=${online ? tipColors.green : tipColors.red}
          title=${online ? 'Device responding' : 'No response from the device - still retrying'} />
        <button type="button" onclick=${toggleTheme} class="text-slate-400 hover:text-blue-600"
          title=${dark ? 'Switch to light mode' : 'Switch to night mode'}>
          <${dark ? Icons.sun : Icons.moon} class="h-5 w-5" />
        <//>
        <${Button} title="Reboot" icon=${Icons.refresh} onclick=${rebootAction} />
        ${rebootResult && html`<${Notification} ok=${rebootResult.ok} timeout=${rebootResult.ok ? 1500 : 6000}
          text=${rebootResult.text} close=${() => setRebootResult(null)} />`}
      <//>
    <//>
  <//>
<//>`;
}

// Hoisted out of Sidebar: a component declared inside another is a new function
// identity on every render, which makes preact tear down and rebuild its DOM
// instead of patching it.
const NavLink = ({ title, icon, href, url, badge }) => html`
  <div>
    <a href="#${href}" class="${href == url
      ? 'bg-slate-50 dark:bg-slate-700 text-blue-600 dark:text-blue-300'
      : 'text-gray-700 dark:text-slate-300 hover:text-blue-600 dark:hover:text-blue-300 hover:bg-gray-50 dark:hover:bg-slate-700'} flex items-center gap-x-3 rounded-md p-2 text-sm leading-6 font-semibold">
      <${icon} class="w-6 h-6"/>
      ${title}
      ${badge}
    <///>
  <//>`;

function Sidebar({ url, show, status }) {
  const version = (status && status.node && status.node.version) || '';
  const simOn = !!(status && status.sim && status.sim.enabled);
  // Counted since boot, not held in the ring, so a node that logged an error an
  // hour ago still wears the badge after the ring has scrolled past it.
  const errors = (status && status.log && status.log.errors) || 0;
  const warnings = (status && status.log && status.log.warnings) || 0;
  return html`
<div class="-translate-x-full transition-all duration-300 transform
            fixed top-0 left-0 bottom-0 z-[60] w-72 bg-white dark:bg-slate-800 border-r
            border-gray-200 dark:border-slate-700 overflow-y-auto
            ${show && 'translate-x-0'} right-auto">
  <div class="flex flex-col m-4 gap-y-6">
    <div class="flex h-10 shrink-0 items-center gap-x-3 font-bold text-xl text-slate-500 dark:text-slate-400">
      <${Logo} class="h-full logo"/> FormationFlight
    <//>
    <div class="text-xs text-slate-400 font-mono" style="margin-top:-1rem">${version.match(/v\d/) ? version : 'dev build'}<//>
    <div class="flex flex-1 flex-col">
      <${NavLink} title="Dashboard" icon=${Icons.home} href="/" url=${url} />
      <${NavLink} title="Radios" icon=${Icons.antenna} href="/radios" url=${url} />
      <${NavLink} title="Follow" icon=${Icons.scan} href="/follow" url=${url} />
      <${NavLink} title="Settings" icon=${Icons.settings} href="/settings" url=${url} />
      <${NavLink} title="Simulator" icon=${Icons.beaker} href="/sim" url=${url}
        badge=${simOn ? html`<${Colored} text="ON" colors="bg-violet-600 text-white" />` : null} />
      <${NavLink} title="System" icon=${Icons.cpu} href="/system" url=${url}
        badge=${errors ? html`<${Colored} text=${errors} colors=${tipColors.red} />`
          : warnings ? html`<${Colored} text=${warnings} colors=${tipColors.yellow} />` : null} />
      <${NavLink} title="Update" icon=${Icons.upArrowBox} href="/update" url=${url} />
    <//>
  <//>
<//>`;
}

// ---- Dashboard ---------------------------------------------------------------

function Dashboard({ status }) {
  if (!status) return '';
  const node = status.node || {};
  const loc = status.location || {};
  const peers = status.peers || [];
  const radios = status.radios || [];
  const crypto = status.crypto || {};
  const follow = status.follow || {};
  const lockedUid = follow.locked_uid && follow.locked_uid !== '00000000' ? follow.locked_uid : null;

  const fixOk = !!loc.valid;
  const power = status.power;
  // 3.5 V on a single cell. Not a percentage anyone should trust, but the
  // voltage is measured and the threshold is the one the PMIC cares about.
  const cellLow = !!(power && power.battery_present && power.battery_v > 0 && power.battery_v < 3.5);
  const cryptoOpen = crypto.mode === 'none';
  const badCrypto = (crypto.bad_tag || 0) + (crypto.replay || 0);

  // "Heard on one radio but not the others" is the single most useful thing a
  // multi-radio node can tell you, so it gets counted up here and not just
  // coloured in the table.
  const realRadios = radios.filter(r => r.enabled && !r.sim);
  const partial = peers.filter(p => peerPartial(p, radios)).length;

  return html`
<div class="p-2">
  <div class="p-4 sm:p-2 mx-auto grid grid-cols-1 md:grid-cols-2 lg:grid-cols-4 xl:grid-cols-5 gap-4">
    <${Stat} title="Node" icon=${Icons.home} text=${node.name || node.uid || DASH}
      tipText=${node.listen_only ? 'listen only' : ''} tipColors=${tipColors.yellow}
      tipIcon=${node.listen_only ? Icons.warn : null}
      subText=${html`${node.uid} · up ${uptime(node.uptime_ms)} · ${num(node.free_heap)} B heap free`} />
    <${Stat} title="Position" icon=${Icons.scan}
      text=${fixOk ? latLon(loc.lat, loc.lon) : 'No fix'}
      tipText=${fixOk ? (loc.source || 'fix') : 'no fix'}
      tipIcon=${fixOk ? Icons.ok : Icons.warn}
      tipColors=${fixOk ? tipColors.green : tipColors.yellow}
      subText=${fixOk
        ? html`${num(loc.alt_m, 0, ' m')} · ${speedMs(loc.speed_cms)} · ${courseDeg(loc.course_ddeg)}${present(loc.sats) ? ' · ' + loc.sats + ' sats' : ''}${loc.armed ? ' · ARMED' : ''}`
        : 'Nothing is beaconed and Follow cannot run without a position.'} />
    <${Stat} title="Peers" icon=${Icons.antenna} text=${peers.length}
      tipText=${partial ? partial + ' partial' : ''}
      tipIcon=${partial ? Icons.warn : null} tipColors=${tipColors.yellow}
      subText=${partial
        ? html`${partial} of them are not being heard on every enabled radio`
        : html`heard on ${realRadios.map(r => r.name).join(' + ') || 'no enabled radio'}`} />
    <${Stat} title="Encryption" icon=${Icons.shield}
      text=${cryptoOpen ? 'Off' : (crypto.mode || DASH).toUpperCase()}
      tipText=${cryptoOpen ? 'open' : badCrypto ? badCrypto + ' rejected' : 'ok'}
      tipIcon=${cryptoOpen ? Icons.warn : badCrypto ? Icons.warn : Icons.ok}
      tipColors=${cryptoOpen ? tipColors.yellow : badCrypto ? tipColors.yellow : tipColors.green}
      subText=${cryptoOpen
        ? 'No cipher. Frames go out in the clear and anything can be injected.'
        : html`${num(crypto.bad_tag)} bad tag · ${num(crypto.replay)} replay · tx counter ${num(crypto.tx_counter)}`} />
    ${power && html`
    <${Stat} title="Power" icon=${Icons.battery}
      text=${power.battery_present ? num(power.battery_v, 2, ' V') : 'USB'}
      tipText=${power.battery_present ? (power.charging ? 'charging' : cellLow ? 'low' : 'on battery') : 'no cell'}
      tipIcon=${cellLow ? Icons.warn : null}
      tipColors=${cellLow ? tipColors.yellow : power.charging ? tipColors.green : tipColors.gray}
      subText=${power.battery_present
        ? html`${num(power.discharge_ma, 0, ' mA')} out · ${num(power.charge_ma, 0, ' mA')} in · ${num(power.pmic_temp_c, 0, ' °C')}`
        : html`Running off USB at ${num(power.supply_v, 2, ' V')}. It stops the instant that is unplugged.`} />`}
  <//>

  <div class="p-4 sm:p-2 mx-auto grid grid-cols-1 lg:grid-cols-3 gap-4">
    <div class="lg:col-span-2">
      <${Card} title="Peers" icon=${Icons.list}
        right=${html`<span class="text-xs text-slate-400">${peers.length} tracked<//>`} cls="overflow-hidden">
        <${PeerTable} peers=${peers} radios=${radios} selfLocation=${loc} lockedUid=${lockedUid} />
      <//>
    <//>
    <${Card} title="Radar" icon=${Icons.scan}
      right=${html`<span class="text-xs text-slate-400">north up<//>`}>
      <${RadarScope} peers=${peers} radios=${radios} location=${loc} lockedUid=${lockedUid} />
    <//>
  <//>

  <div class="p-4 sm:p-2 mx-auto grid grid-cols-1 ${power ? 'lg:grid-cols-2' : ''} gap-4">
    <${GnssCard} location=${loc} />
    ${power && html`<${PowerCard} power=${power} />`}
  <//>
<//>`;
}

// ---- Radios ------------------------------------------------------------------

// How many rows of frame history to keep client-side. The device ring is 32
// entries; this is the scrollback the UI accumulates across polls on top of it.
const FRAME_SCROLLBACK = 250;

// Rolling history for the radio cards. Sampled off the status the App already
// polls rather than polling anything itself, so it costs the device nothing and
// stops the moment this page unmounts. It lives only as long as the tab is open,
// which is all a tuning view needs.
const HIST_N = 120; // ~2 min at the 1 s status cadence

function HistoryCard({ status }) {
  const [hist, setHist] = useState({ rx: [], peers: [], beacon: {} });
  const prev = useRef(null);
  useEffect(() => {
    if (!status) return;
    const now = Date.now();
    const totalRx = (status.radios || []).reduce((a, r) => a + (r.rx_ok || 0), 0);
    let rate = null;
    if (prev.current) {
      const dt = (now - prev.current.t) / 1000;
      if (dt < 0.25) return;  // same sample arriving twice; nothing new to plot
      rate = Math.max(0, (totalRx - prev.current.rx) / dt);
    }
    prev.current = { t: now, rx: totalRx };
    const push = (arr, v) => {
      const a = (arr || []).concat(v);
      return a.length > HIST_N ? a.slice(-HIST_N) : a;
    };
    setHist(h => {
      const beacon = {};
      (status.radios || []).forEach(r => { beacon[r.index] = push(h.beacon[r.index], r.beacon_interval_ms); });
      return { rx: push(h.rx, rate), peers: push(h.peers, (status.peers || []).length), beacon };
    });
  }, [status]);

  const radios = (status && status.radios) || [];
  return html`
<${Card} title="History" icon=${Icons.bolt}
  right=${html`<span class="text-xs text-slate-400">last ~2 min<//>`}>
  <${Sparkline} series=${hist.rx} label="Accepted frames" unit="/s" digits=1 color="stroke-blue-500" />
  <${Sparkline} series=${hist.peers} label="Peers" color="stroke-slate-300" />
  ${radios.filter(r => r.enabled).map(r => html`
    <${Sparkline} key=${r.index} series=${hist.beacon[r.index]} label=${r.name + ' beacon interval'} unit="ms" color="stroke-blue-500" />`)}
  <p class="text-xs text-gray-400">
    The beacon interval is the rate controller reacting to the peer count. It should climb as aircraft join and
    fall again as they leave; a LoRa interval that pins to the ceiling means the channel is as full as the
    target load allows.
  <//>
<//>`;
}

function Radios({ status }) {
  const [log, setLog] = useState({ frames: [], missed: 0, total: null, capacity: null });
  const [logError, setLogError] = useState(null);
  const [paused, setPaused] = useState(false);
  const pausedRef = useRef(false);
  const sinceRef = useRef(0);
  pausedRef.current = paused;

  // The frame log poller belongs to this page, so navigating away really stops
  // it. `since` is the total counter of the newest frame we already have: the
  // device returns only what was logged after it, which keeps the response tiny
  // once the log is warm.
  useEffect(() => {
    let stopped = false, timer = null;
    const tick = () => {
      if (stopped) return;
      if (pausedRef.current) { timer = setTimeout(tick, 500); return; }
      api('/api/frames' + (sinceRef.current ? '?since=' + sinceRef.current : ''))
        .then(r => {
          if (stopped || !r) return;
          setLogError(null);
          const fresh = (r.frames || []).map((f, i) => ({ ...f, seq: r.total - 1 - i }));
          // Anything between what we had and the oldest frame the device still
          // holds is gone for good: the ring wrapped before we asked. Say so
          // rather than presenting the list as complete.
          let missed = 0;
          if (sinceRef.current && r.total > sinceRef.current) {
            missed = (r.total - sinceRef.current) - fresh.length;
            if (missed < 0) missed = 0;
          }
          sinceRef.current = r.total;
          setLog(prev => ({
            total: r.total,
            capacity: r.capacity,
            missed: prev.missed + missed,
            frames: fresh.concat(prev.frames).slice(0, FRAME_SCROLLBACK),
          }));
        })
        .catch(e => { if (!stopped) setLogError(e.message); })
        .then(() => { if (!stopped) timer = setTimeout(tick, 750); });
    };
    tick();
    return () => { stopped = true; clearTimeout(timer); };
  }, []);

  if (!status) return '';
  const radios = status.radios || [];
  const stats = status.stats || {};
  const newest = log.frames.length ? log.frames[0].ms : null;
  const withRel = log.frames.map(f => ({ ...f, rel_ms: present(newest) ? newest - f.ms : null }));

  return html`
<div class="p-2">
  <div class="p-4 sm:p-2 mx-auto grid grid-cols-1 md:grid-cols-2 lg:grid-cols-3 gap-4">
    ${radios.map(r => html`<${RadioCard} key=${r.index} radio=${r} />`)}
    ${!radios.length && html`<p class="text-sm text-slate-400">No radios reported.<//>`}
    ${status.wifi && html`<${WifiCard} wifi=${status.wifi} />`}
  <//>
  <div class="p-4 sm:p-2 mx-auto grid grid-cols-1 lg:grid-cols-3 gap-4">
    <div class="lg:col-span-2">
    <${Card} title="Frame log" icon=${Icons.list} cls="overflow-hidden"
      right=${html`
      <div class="flex items-center gap-3">
        <span class="text-xs text-slate-400">${num(stats.rx_ok)} ok · ${num(stats.rx_rejected)} rejected · ${num(stats.rx_self)} self · ${num(stats.beacons_sent)} beacons<//>
        <button type="button" onclick=${() => setPaused(v => !v)}
          class="px-2 py-0.5 text-xs font-medium rounded ${paused ? 'bg-yellow-100 text-yellow-900 dark:bg-yellow-800 dark:text-yellow-100' : 'bg-slate-100 dark:bg-slate-700 text-slate-600 dark:text-slate-300'}">
          ${paused ? 'paused' : 'live'}
        <//>
      <//>`}>
      <${FrameLogView} frames=${withRel} radios=${radios} missed=${log.missed}
        capacity=${log.capacity} total=${log.total} error=${logError} />
    <//>
    <//>
    <${HistoryCard} status=${status} />
  <//>
<//>`;
}

// ---- Settings ----------------------------------------------------------------

const boolOptions = [[1, 'Yes'], [0, 'No']];

function Settings() {
  const [cfg, setCfg] = useState(null);
  const [base, setBase] = useState(null);
  const [result, setResult] = useState(null);
  const [unsaved, setUnsaved] = useState(false);

  const load = () => api('/api/config').then(r => { setCfg(r); setBase(r); });
  useEffect(() => { load().catch(e => setResult({ ok: false, text: e.message })); }, []);

  const mk = (sec, key) => v => setCfg(c => ({ ...c, [sec]: { ...c[sec], [key]: v } }));

  // Only this page's sections are posted. A partial body merges, so Settings
  // never has to carry - and therefore never has to clobber - the Follow block
  // that the Follow page owns.
  const payload = () => {
    const picked = {};
    ['node', 'security', 'rate', 'peers', 'msp', 'gnss', 'radios', 'wifi', 'sim']
      .forEach(k => { if (cfg[k]) picked[k] = cfg[k]; });
    return coerceNumbers(picked, base);
  };

  const apply = () => postJson('/api/config', payload())
    .then(r => { setCfg(r); setBase(r); setUnsaved(true); setResult({ ok: true, text: 'Applied to the running node' }); })
    .catch(e => setResult({ ok: false, text: e.message }));

  const save = () => postJson('/api/config', payload())
    .then(r => { setCfg(r); setBase(r); return postEmpty('/api/config/save'); })
    .then(() => { setUnsaved(false); setResult({ ok: true, text: 'Saved to flash' }); })
    .catch(e => setResult({ ok: false, text: e.message }));

  const factoryReset = () => postEmpty('/api/config/reset')
    .then(() => load())
    .then(() => { setUnsaved(false); setResult({ ok: true, text: 'Defaults restored and saved. Reboot to bring the radios up on them.' }); })
    .catch(e => setResult({ ok: false, text: e.message }));

  if (!cfg) return html`<div class="m-4 text-sm text-slate-400">Loading configuration…<//>`;
  const radios = cfg.radios || {}, wifi = cfg.wifi || {}, rate = cfg.rate || {};
  const noRadio = !radios.espnow_enabled && !radios.lora_enabled && !(cfg.sim && cfg.sim.enabled);

  return html`
<div class="m-4 grid grid-cols-1 gap-4 lg:grid-cols-2">
  <${Card} title="Node" icon=${Icons.settings}>
    <${Setting} title="Name" value=${cfg.node.name} setfn=${mk('node', 'name')}
      tip="What other aircraft show for this node, up to 15 characters. Leave it empty and the node derives a name from its UID at boot, so it is never nameless." />
    <${Setting} title="Listen only" value=${cfg.node.listen_only} setfn=${mk('node', 'listen_only')} type="switch"
      tip="Track peers but never transmit. For a ground station or a receiver used purely as a display - nobody else will see this node at all." />

    <${SectionTitle} title="Security"
      note="The passphrase is stretched to the 128-bit AES-CCM group key; everyone who should see each other sets the same one." />
    <${Setting} title="Group passphrase" value=${cfg.security.passphrase} setfn=${mk('security', 'passphrase')}
      placeholder="(default key)"
      tip="Empty is NOT 'no encryption': it means the default key, which is public, so traffic is still encrypted and authenticated but anyone with stock firmware can read it. The literal word 'none' disables the cipher entirely - bench use only. A stored passphrase reads back as dots, and posting the dots back leaves it alone. Changing it only takes effect at the next boot." />
    <p class="text-xs text-gray-400 mt-1">
      ${cfg.security.passphrase === 'none'
        ? 'Cipher disabled. Frames go out in the clear and anything can be injected.'
        : cfg.security.passphrase
          ? 'A group key is set. Only nodes with the same passphrase will decode this traffic.'
          : 'Using the default key. Interoperable out of the box, no secrecy.'}
    <//>

    <${SectionTitle} title="Radios" note=${noRadio ? '' : 'Radio changes take effect when the node next boots.'} />
    ${noRadio && html`<p class="text-xs text-red-600 mb-2">At least one radio (or the simulator) must be enabled - the device will reject this.<//>`}
    <${Setting} title="ESP-NOW" value=${radios.espnow_enabled} setfn=${mk('radios', 'espnow_enabled')} type="switch"
      tip="2.4 GHz, short range, almost no airtime per frame. This is what makes two nodes on a bench see each other instantly, and what carries the formation at close range." />
    <${Setting} title="LoRa" value=${radios.lora_enabled} setfn=${mk('radios', 'lora_enabled')} type="switch"
      tip="Long range, but each frame occupies the channel for milliseconds rather than microseconds, so the rate controller backs its beacon off much harder as the group grows." />
    <${Setting} title="LoRa TX power" value=${radios.lora_power_dbm} setfn=${mk('radios', 'lora_power_dbm')} type="number" addonRight="dBm"
      tip="0 leaves the target's compiled-in default alone; 1-30 overrides it, clamped by the driver to what the part can actually do. Check your local limit before raising it." />

    <${SectionTitle} title="WiFi" note="Getting this wrong is how you lose the web UI. Apply it first, confirm the node is still reachable, and only then Save." />
    <${Setting} title="Run own AP" value=${wifi.ap} setfn=${mk('wifi', 'ap')} type="switch"
      tip="On, the node runs its own access point and serves this page at 192.168.4.1. Off, it joins the network named below instead." />
    <${Setting} title="AP password" value=${wifi.ap_psk} setfn=${mk('wifi', 'ap_psk')} disabled=${!wifi.ap}
      placeholder="(open)"
      tip="Password for the node's own AP. Empty leaves it open, which is the v1 behaviour and fine on a field bench. WPA2 will not accept fewer than 8 characters, and the device refuses a shorter one rather than silently coming up open." />
    <${Setting} title="Join SSID" value=${wifi.ssid} setfn=${mk('wifi', 'ssid')} disabled=${wifi.ap}
      tip="Network to join when 'Run own AP' is off. Required in that mode." />
    <${Setting} title="Join password" value=${wifi.psk} setfn=${mk('wifi', 'psk')} disabled=${wifi.ap}
      tip="Password for that network. Reads back as dots once set; posting the dots back leaves it alone." />
    <${Setting} title="Channel" value=${wifi.channel} setfn=${mk('wifi', 'channel')} type="number"
      tip="Channel for the node's own AP, 1-13, and therefore for ESP-NOW: the two share one radio. Every node that should hear every other over ESP-NOW must agree on it, and 1, 6 and 11 are the non-overlapping choices. Joining an external network hands the choice to that router instead - the Radios page shows the channel actually in use." />
    <p class="text-xs text-gray-400 mt-1">
      ${wifi.ap
        ? 'ESP-NOW runs on this channel. Nodes set to a different one will not hear this node at all.'
        : 'Ignored while joining a network: the router picks the channel, and ESP-NOW follows it.'}
    <//>

  <//>

  <div class="flex flex-col gap-4">
    <${Card} title="Rate control" icon=${Icons.bolt}>
      <p class="text-xs text-gray-400 mb-3">
        There are no timeslots. Every node scales its own beacon interval by the number of peers it hears,
        aiming at a fixed share of the channel - so without talking to each other they converge on the same rate.
      <//>
      <${Setting} title="Target channel load" value=${rate.target_load} setfn=${mk('rate', 'target_load')} type="number"
        tip="Share of the channel the whole group should occupy. Pure-ALOHA throughput peaks near 0.5; the default 0.15 deliberately runs well below that, trading throughput for a low collision probability - the beacon stream is already redundant, a collided one is not." />
      <${Setting} title="Fastest beacon" value=${rate.min_interval_ms} setfn=${mk('rate', 'min_interval_ms')} type="number" addonRight="ms"
        tip="Floor on the interval, used when the node is alone or hears only a peer or two." />
      <${Setting} title="Slowest beacon" value=${rate.max_interval_ms} setfn=${mk('rate', 'max_interval_ms')} type="number" addonRight="ms"
        tip="Ceiling on the interval - the worst update rate you are willing to accept on a crowded channel. Must be at least the fastest interval." />
      <${Setting} title="Jitter" value=${rate.jitter_frac} setfn=${mk('rate', 'jitter_frac')} type="number"
        tip="Random spread applied to every transmission, as a fraction of the interval. It is what stops nodes that computed the same interval from colliding on the same schedule forever. 0 to 0.99." />
    <//>

    <${Card} title="Peers, MSP and GNSS" icon=${Icons.list}>
      <${Setting} title="Peer timeout" value=${cfg.peers.timeout_ms} setfn=${mk('peers', 'timeout_ms')} type="number" addonRight="ms"
        tip="A peer not heard from within this goes from active to lost. It also shrinks the peer count the rate controller scales the beacon by, so setting it very long keeps the channel slow after aircraft have landed." />
      <${Setting} title="Announce interval" value=${cfg.peers.announce_interval_ms} setfn=${mk('peers', 'announce_interval_ms')} type="number" addonRight="ms"
        tip="How often the announce packet carrying this node's name and capabilities goes out. Position beacons are separate and far more frequent; this only needs to be often enough for a newly-arrived peer to learn the name." />
      <${Setting} title="MSP radar interval" value=${cfg.msp.radar_interval_ms} setfn=${mk('msp', 'radar_interval_ms')} type="number" addonRight="ms"
        tip="How often peer positions are pushed to the flight controller as MSP radar targets, which is what puts them on the OSD." />
      <${Setting} title="GNSS rate" value=${cfg.gnss.rate_hz} setfn=${mk('gnss', 'rate_hz')} type="number" addonRight="Hz"
        tip="Update rate requested from a directly attached GNSS receiver, 1-25 Hz. Irrelevant when position comes from the flight controller over MSP." />
    <//>

    <${Card} title="Factory reset" icon=${Icons.warn}>
      <p class="text-sm text-slate-600 dark:text-slate-300">
        Restores compile-time defaults and writes them straight to flash. The node keeps running on the new
        config rather than rebooting itself, so you can set a few fields before the radios come back up on a
        different group.
      <//>
      <div class="flex justify-end mt-3">
        <${Button} title="Reset to defaults" icon=${Icons.warn} onclick=${factoryReset}
          colors="bg-red-600 hover:bg-red-500 disabled:bg-red-400" />
      <//>
    <//>
  <//>

  <div class="lg:col-span-2">
    <${Card}>
      ${result && html`<${Notification} ok=${result.ok} timeout=${result.ok ? 2500 : 9000}
        text=${result.text} close=${() => setResult(null)} />`}
      <${ConfigActions} onApply=${apply} onSave=${save} unsaved=${unsaved} />
    <//>
  <//>
<//>`;
}

// ---- Simulator ---------------------------------------------------------------

const SIM_MODES = [
  ['static', 'Static - sits still, reports course and zero speed'],
  ['line', 'Line - departs on the course and never turns'],
  ['circle', 'Circle - orbits the point at the radius'],
  ['hex', 'Hexagon - closed circuit, stepped turns, altitude ramp'],
];

// The device holds four at once and answers a fifth with a 409.
const MAX_SIM_PEERS = 4;

const blankSimPeer = () => ({
  uid: '', name: 'SIM1', mode: 'hex', lat: 0, lon: 0, alt_m: 120,
  speed_ms: 15, course_deg: 90, radius_m: 150,
});

function Simulator({ status, sim, refreshSim }) {
  const [draft, setDraft] = useState(blankSimPeer);
  const [result, setResult] = useState(null);
  const [busy, setBusy] = useState(false);
  const enabled = !!(status && status.sim && status.sim.enabled);
  const seeded = useRef(false);

  // Seed the form from this node's own position the first time we have one -
  // simulated traffic on the other side of the planet is useless, and typing a
  // lat/lon by hand to eight digits is how you get it.
  useEffect(() => {
    if (seeded.current || !status || !status.location || !status.location.valid) return;
    seeded.current = true;
    setDraft(d => ({ ...d, lat: +(status.location.lat / 1e7).toFixed(6), lon: +(status.location.lon / 1e7).toFixed(6) }));
  }, [status && status.location && status.location.valid]);

  const mk = k => v => setDraft(d => ({ ...d, [k]: v }));

  const setEnabled = on => {
    setBusy(true);
    return postJson('/api/config', { sim: { enabled: on } })
      .then(() => postEmpty('/api/config/save')
        // The apply already took effect; only the flash write can be refused
        // here (saves are rate-limited to one every two seconds), and saying so
        // is better than implying the toggle did not work.
        .catch(e => { throw new Error(e.message + ' - the change is live, but has not been written to flash.'); }))
      .then(() => setResult({ ok: true, text: on ? 'Simulator enabled and saved' : 'Simulator disabled and saved' }))
      .catch(e => setResult({ ok: false, text: e.message }))
      .then(() => setBusy(false));
  };

  const addPeer = () => {
    const body = {
      name: draft.name, mode: draft.mode,
      lat: +draft.lat, lon: +draft.lon, alt_m: +draft.alt_m,
      speed_ms: +draft.speed_ms, course_deg: +draft.course_deg, radius_m: +draft.radius_m,
    };
    if (draft.uid) body.uid = draft.uid;
    return postJson('/api/sim/peer', body)
      .then(() => { setResult({ ok: true, text: 'Simulated peer added' }); return refreshSim(); })
      .catch(e => setResult({ ok: false, text: e.message }));
  };

  const removePeer = uid => api('/api/sim/peer?uid=' + encodeURIComponent(uid), { method: 'DELETE' })
    .then(() => refreshSim())
    .catch(e => setResult({ ok: false, text: e.message }));

  const clearAll = () => postEmpty('/api/sim/clear')
    .then(() => refreshSim())
    .catch(e => setResult({ ok: false, text: e.message }));

  const peers = (sim && sim.peers) || [];
  const needsRadius = draft.mode === 'circle' || draft.mode === 'hex';
  const full = peers.length >= MAX_SIM_PEERS;

  return html`
<div class="m-4 grid grid-cols-1 gap-4 lg:grid-cols-2">
  <${Card} title="Simulated traffic" icon=${Icons.beaker}>
    ${result && html`<${Notification} ok=${result.ok} timeout=${result.ok ? 2500 : 9000}
      text=${result.text} close=${() => setResult(null)} />`}
    <p class="text-sm text-slate-600 dark:text-slate-300 mb-3">
      Simulated peers are injected into a virtual radio and travel the same decrypt and decode path real RF
      takes, so the peer table, Follow and the MSP radar output cannot tell them apart. That is the point -
      it exercises the real code. It is also why the banner is unmissable: nothing downstream will warn you.
    <//>
    <${Setting} title="Simulator enabled" value=${enabled} setfn=${setEnabled} type="switch" disabled=${busy}
      tip="Saved to flash immediately, because a node that boots with the simulator on and no banner to say so is exactly the trap this setting is guarding. Peers can only be added while it is on." />

    <${SectionTitle} title="Add a peer" />
    <${Setting} title="Name" value=${draft.name} setfn=${mk('name')} tip="Up to 15 characters, shown in the peer table just like a real aircraft's." />
    <${Setting} title="UID" value=${draft.uid} setfn=${mk('uid')} placeholder="(generated)"
      tip="8 hex characters. Leave it empty and the device generates one in the 5eed0000 range, so a simulated node is recognisable as one even in a raw packet capture." />
    <${Setting} title="Mode" value=${draft.mode} setfn=${mk('mode')} type="select" options=${SIM_MODES}
      tip="Hexagon is the awkward one and therefore the useful one: heading changes in steps, altitude ramps to a peak at the half-way vertex and back, and the loop closes - so a follower has to cope with all three." />
    <${Setting} title="Latitude" value=${draft.lat} setfn=${mk('lat')} type="number" addonRight="°"
      tip="Decimal degrees, not the 1e7 integers the wire protocol uses - this one is typed by a human." />
    <${Setting} title="Longitude" value=${draft.lon} setfn=${mk('lon')} type="number" addonRight="°" />
    <${Setting} title="Altitude" value=${draft.alt_m} setfn=${mk('alt_m')} type="number" addonRight="m" />
    <${Setting} title="Speed" value=${draft.speed_ms} setfn=${mk('speed_ms')} type="number" addonRight="m/s"
      tip="0 to 200 m/s. Static mode reports zero regardless." />
    <${Setting} title="Course" value=${draft.course_deg} setfn=${mk('course_deg')} type="number" addonRight="°"
      tip="Direction of travel for line mode, and the reported heading for static mode." />
    <${Setting} title="Radius" value=${draft.radius_m} setfn=${mk('radius_m')} type="number" addonRight="m"
      disabled=${!needsRadius}
      tip="Orbit radius for circle mode, hexagon side length for hex mode. Must be greater than zero for both." />
    <div class="flex justify-end mt-4">
      <${Button} title="Add peer" icon=${Icons.plus} onclick=${addPeer} disabled=${!enabled || full} />
    <//>
    ${!enabled
      ? html`<p class="text-xs text-gray-400 mt-2 text-right">Enable the simulator first - the device refuses peers while it is off.<//>`
      : full
        ? html`<p class="text-xs text-gray-400 mt-2 text-right">${MAX_SIM_PEERS} simulated peers is the limit. Remove one to add another.<//>`
        : html`<p class="text-xs text-gray-400 mt-2 text-right">Posting a UID that already exists replaces that peer and restarts its path.<//>`}
  <//>

  <${Card} title="Running simulated peers" icon=${Icons.list}
    right=${peers.length ? html`<${Button} title="Clear all" icon=${Icons.trash} onclick=${clearAll}
      colors="bg-red-600 hover:bg-red-500" />` : null}>
    ${!peers.length
      ? html`<p class="text-sm text-slate-400">None. Whatever is in the peer table is real traffic.<//>`
      : html`
      <div class="overflow-auto">
        <table class="min-w-full border-separate border-spacing-0">
          <thead><tr>
            <${Th} title="UID" /><${Th} title="Name" /><${Th} title="Mode" />
            <${Th} title="Position" /><${Th} title="Speed" /><${Th} title="Elapsed" />
            <${Th} title="State" /><${Th} title="" />
          </tr></thead>
          <tbody>
            ${peers.map(p => html`
            <tr key=${p.uid}>
              <${Td} cls="font-mono" text=${p.uid} />
              <${Td} text=${p.name} />
              <${Td} text=${p.mode} />
              <${Td} cls="font-mono" text=${present(p.lat) ? (+p.lat).toFixed(5) + ', ' + (+p.lon).toFixed(5) : DASH} />
              <${Td} cls="font-mono" text=${num(p.speed_ms, 1, ' m/s')} />
              <${Td} cls="font-mono text-slate-500 dark:text-slate-400" text=${present(p.elapsed_ms) ? age(p.elapsed_ms) : DASH}
                title="How long this peer has been flying its path. It restarts whenever the peer is posted again." />
              <${Td}><${Colored} text=${p.running ? 'running' : 'stopped'}
                colors=${p.running ? tipColors.green : tipColors.gray}
                title=${p.running ? 'Transmitting on the virtual radio.' : 'Defined, but the simulator is off.'} /><//>
              <${Td}><button type="button" onclick=${() => removePeer(p.uid)}
                class="text-slate-400 hover:text-red-600" title="Remove this peer"><${Icons.trash} class="w-4 h-4" /><//><//>
            </tr>`)}
          </tbody>
        </table>
      <//>`}
  <//>
<//>`;
}

// ---- Update ------------------------------------------------------------------

function Update() {
  const [file, setFile] = useState(null);
  const [progress, setProgress] = useState(null);
  const [uploadResult, setUploadResult] = useState(null);

  const onchange = ev => setFile(ev.target.files[0] || null);

  // XHR rather than fetch: fetch has no upload-progress event, and a firmware
  // image over a 2.4 GHz link from a phone is exactly the upload that needs a
  // bar. The node reboots on success, so there is nothing to poll afterwards.
  const onsubmit = () => new Promise(resolve => {
    if (!file) { setUploadResult({ ok: false, text: 'Choose a firmware image first' }); return resolve(); }
    const form = new FormData();
    form.append('file', file);
    const xhr = new XMLHttpRequest();
    xhr.open('POST', ENDPOINT_PREFIX + '/update');
    setProgress(0);
    xhr.upload.onprogress = e => {
      if (e.lengthComputable) setProgress(Math.round(e.loaded / e.total * 100));
    };
    xhr.onload = () => {
      setProgress(null);
      setUploadResult({ ok: xhr.status === 200, text: xhr.responseText || ('HTTP ' + xhr.status) });
      resolve();
    };
    xhr.onerror = () => {
      setProgress(null);
      setUploadResult({ ok: false, text: 'Connection lost during the upload. The node has not been flashed.' });
      resolve();
    };
    xhr.send(form);
  });

  const onnotificationclose = () => {
    if (uploadResult.ok) setTimeout(() => { window.location.hash = '#/'; window.location.reload(); }, 8000);
    setUploadResult(null);
  };

  return html`
<div class="m-4 grid grid-cols-1 gap-4 lg:grid-cols-2">
  <${Card} title="Firmware update" icon=${Icons.upArrowBox}>
    ${uploadResult && html`<${Notification} ok=${uploadResult.ok} timeout=${uploadResult.ok ? 4000 : 12000}
      text=${uploadResult.text} close=${onnotificationclose} />`}
    <${Setting} title="Firmware image" type="file" onchange=${onchange}
      tip="A .bin built for this exact target. ESP8266 targets also accept .bin.gz." />
    ${file && html`<p class="text-xs text-gray-400 mt-1">${file.name} · ${Math.round(file.size / 1024)} KB<//>`}
    ${present(progress) && html`
    <div class="mt-4">
      <div class="flex justify-between text-xs text-slate-500 dark:text-slate-400 mb-1">
        <span>Uploading<//><span class="font-mono">${progress}%<//>
      <//>
      <div class="w-full rounded-full bg-slate-200 dark:bg-slate-700" style="height:8px">
        <div class="rounded-full bg-blue-600" style="height:8px;width:${progress}%;transition:width .2s"><//>
      <//>
      <p class="text-xs text-gray-400 mt-2">Do not power the node down. It reboots on its own when the write completes.<//>
    <//>`}
    <div class="mt-4 flex place-content-end">
      <${Button} icon=${Icons.upArrowBox} onclick=${onsubmit} title="Upload & install" disabled=${!file || present(progress)} />
    <//>
  <//>

  <${Card}>
    <${Note} title="Firmware downloads">
      <p class="my-2 text-slate-500 dark:text-slate-400">
        Builds live on the <a class="text-blue-600 dark:text-blue-500 hover:underline"
        href="https://github.com/FormationFlight/FormationFlight/releases/latest">FormationFlight releases page</a>.
        Flash the image for your target - the node does not check, and a mismatched image bricks it back to USB.
      <//>
    <//>
  <//>
<//>`;
}

// ---- App ---------------------------------------------------------------------

// ---- System ------------------------------------------------------------------

// Rolling history for the power and loop plots, sampled off the status the App
// already polls. Same deal as the radio history: it costs the device nothing
// and lives only as long as the tab is open.
const SYS_HIST_N = 240; // ~4 min at the 1 s status cadence

function SystemHistory({ status }) {
  const [hist, setHist] = useState({ batt: [], draw: [], heap: [], loop: [] });
  const last = useRef(0);
  useEffect(() => {
    if (!status) return;
    const now = Date.now();
    if (now - last.current < 250) return; // same sample arriving twice
    last.current = now;
    const p = status.power || {};
    const l = status.loop || {};
    const sys = status.system || {};
    const push = (arr, v) => {
      const a = (arr || []).concat(present(v) ? +v : null);
      return a.length > SYS_HIST_N ? a.slice(-SYS_HIST_N) : a;
    };
    setHist(h => ({
      batt: push(h.batt, p.battery_present ? p.battery_v : null),
      draw: push(h.draw, p.battery_present ? (p.discharge_ma || 0) - (p.charge_ma || 0) : null),
      heap: push(h.heap, present(sys.free_heap) ? sys.free_heap / 1024 : null),
      loop: push(h.loop, present(l.max_us) ? l.max_us / 1000 : null),
    }));
  }, [status]);

  const hasPower = !!(status && status.power && status.power.battery_present);
  return html`
<${Card} title="History" icon=${Icons.bolt}
  right=${html`<span class="text-xs text-slate-400">last ~4 min<//>`}>
  ${hasPower && html`
    <${Sparkline} series=${hist.batt} label="Cell voltage" unit=" V" digits=2 color="stroke-green-500" />
    <${Sparkline} series=${hist.draw} label="Cell current, + out / - in" unit=" mA" color="stroke-blue-500" />`}
  <${Sparkline} series=${hist.heap} label="Free heap" unit=" KB" color="stroke-slate-400" />
  <${Sparkline} series=${hist.loop} label="Worst loop" unit=" ms" digits=1 color="stroke-violet-500" />
  <p class="text-xs text-gray-400">
    Free heap that only ever falls is a leak. The worst-loop trace is the running maximum, so it is a staircase
    by construction: what matters is whether it takes a step while you are doing something in particular.
  <//>
<//>`;
}

const LOG_POLL_MS = 1000;
const LOG_SCROLLBACK = 400;

function System({ status }) {
  const [gnss, setGnss] = useState(null);
  const [log, setLog] = useState({ entries: [], total: null, capacity: null, warnings: 0, errors: 0 });
  const [logError, setLogError] = useState(null);
  const [filter, setFilter] = useState('all');
  const [paused, setPaused] = useState(false);
  const [clearResult, setClearResult] = useState(null);
  const pausedRef = useRef(false);
  const sinceRef = useRef(0);
  pausedRef.current = paused;

  // Owned by this page, so navigating away really stops it. `since` is the
  // sequence of the newest line we already hold; the device returns only what
  // was logged after it.
  useEffect(() => {
    let stopped = false, timer = null;
    const tick = () => {
      if (stopped) return;
      if (pausedRef.current) { timer = setTimeout(tick, LOG_POLL_MS); return; }
      api('/api/log' + (sinceRef.current ? '?since=' + sinceRef.current : ''))
        .then(r => {
          if (stopped || !r) return;
          setLogError(null);
          sinceRef.current = r.total;
          setLog(prev => ({
            total: r.total,
            capacity: r.capacity,
            warnings: r.warnings,
            errors: r.errors,
            entries: (r.entries || []).concat(prev.entries).slice(0, LOG_SCROLLBACK),
          }));
        })
        .catch(e => { if (!stopped) setLogError(e.message); })
        .then(() => { if (!stopped) timer = setTimeout(tick, LOG_POLL_MS); });
    };
    tick();
    return () => { stopped = true; clearTimeout(timer); };
  }, []);

  // Slower than the log: these counters move in the thousands and nothing about
  // them needs to be watched at a second's resolution.
  useEffect(() => {
    let stopped = false, timer = null;
    const tick = () => {
      if (stopped) return;
      api('/api/gnss')
        .then(r => { if (!stopped && r) setGnss(r); })
        .catch(() => { /* absent on a build with no GPS; the card says so */ })
        .then(() => { if (!stopped) timer = setTimeout(tick, 2000); });
    };
    tick();
    return () => { stopped = true; clearTimeout(timer); };
  }, []);

  // The window only. A node that stalled an hour ago has still stalled, so the
  // count and the accumulated web time are left alone by the device.
  const resetLoop = () => postEmpty('/api/loop/reset')
    .then(() => setClearResult({ ok: true, text: 'Loop window reset' }))
    .catch(e => setClearResult({ ok: false, text: e.message }));

  const clearLog = () => api('/api/log', { method: 'DELETE' })
    .then(() => {
      // The device keeps its running total across a clear so a client cursor
      // never walks backwards. Drop what we are holding, keep the cursor.
      setLog(p => ({ ...p, entries: [] }));
      setClearResult({ ok: true, text: 'Log cleared' });
    })
    .catch(e => setClearResult({ ok: false, text: e.message }));

  if (!status) return '';
  const power = status.power;
  // The firmware omits the whole power object when the PMIC did not answer, so
  // status alone cannot tell "this board has none" from "this board's died".
  // The log can: BoardPower logs an error naming the chip when it is missing.
  const pmicMissing = log.entries.some(e => e.level === 'error' && /axp/i.test(e.text));

  return html`
<div class="p-2">
  <div class="p-4 sm:p-2 mx-auto grid grid-cols-1 md:grid-cols-2 lg:grid-cols-3 gap-4">
    ${power ? html`<${PowerCard} power=${power} />` : html`<${NoPowerCard} expected=${pmicMissing} />`}
    <${SystemCard} system=${status.system} node=${status.node} />
    <${LoopCard} loop=${status.loop} onReset=${resetLoop} />
  <//>
  <div class="p-4 sm:p-2 mx-auto grid grid-cols-1 lg:grid-cols-2 gap-4">
    <${GnssCard} location=${status.location} />
    <${GnssLinkCard} link=${gnss} />
  <//>
  <div class="p-4 sm:p-2 mx-auto grid grid-cols-1 lg:grid-cols-3 gap-4">
    <div class="lg:col-span-2">
      <${Card} title="Device log" icon=${Icons.document} cls="overflow-hidden"
        right=${html`
        <div class="flex items-center gap-2">
          ${LOG_FILTERS.map(([k, label]) => html`
            <button key=${k} type="button" onclick=${() => setFilter(k)}
              class="px-2 py-0.5 text-xs font-medium rounded ${filter === k
                ? 'bg-blue-600 text-white'
                : 'bg-slate-100 dark:bg-slate-700 text-slate-600 dark:text-slate-300'}">${label}<//>`)}
          <button type="button" onclick=${() => setPaused(v => !v)}
            class="px-2 py-0.5 text-xs font-medium rounded ${paused
              ? 'bg-yellow-100 text-yellow-900 dark:bg-yellow-800 dark:text-yellow-100'
              : 'bg-slate-100 dark:bg-slate-700 text-slate-600 dark:text-slate-300'}">
            ${paused ? 'paused' : 'live'}
          <//>
          <button type="button" onclick=${clearLog}
            class="px-2 py-0.5 text-xs font-medium rounded bg-slate-100 dark:bg-slate-700 text-slate-600 dark:text-slate-300"
            title="Empty the device's ring. The warning and error counts survive: a node that logged an error an hour ago has still logged one.">clear<//>
        <//>`}>
        <${LogView} ...${log} error=${logError} filter=${filter} />
      <//>
      ${clearResult && html`<${Notification} ok=${clearResult.ok} timeout=${clearResult.ok ? 1500 : 6000}
        text=${clearResult.text} close=${() => setClearResult(null)} />`}
    <//>
    <${SystemHistory} status=${status} />
  <//>
  <div class="p-4 sm:p-2 mx-auto">
    <${Note} title="Why the log lives here and not on a serial console">
      On an ESP8266 target the console UART <em>is<//> the MSP UART. Anything printed for a human goes straight
      into the flight controller's serial link, so the node keeps its diagnostics in RAM and serves them over
      HTTP instead. The ring holds ${log.capacity || 'a few dozen'} lines; everything older is gone, which is
      what the "logged since boot" count is there to tell you.
    <//>
  <//>
<//>`;
}

// One tick per entry, round-robin, so each request stays small and the device is
// never asked for two things at once. /api/sim is only worth a tick when the
// simulator is on or the user is looking at its page.
const POLL_MS = 500;
const FIRST_LOAD_MS = 20;
const OFFLINE_AFTER = 3;

const App = function () {
  const [url, setUrl] = useState('/');
  const [status, setStatus] = useState(null);
  const [sim, setSim] = useState(null);
  const [online, setOnline] = useState(true);
  const [loaded, setLoaded] = useState(false);
  const [showSidebar, setShowSidebar] = useState(true);

  // Read by the poller without re-arming it: making the effect depend on these
  // would tear the timer down and build a new one on every status update.
  const wantSim = useRef(false);
  wantSim.current = url === '/sim' || !!(status && status.sim && status.sim.enabled);
  const simRefresh = useRef(() => Promise.resolve());

  // The single owned-by-effect poller. Everything in the cleanup matters: the
  // old code left a setTimeout chain running past unmount, so a second chain
  // stacked on the first every time you came back to a page.
  useEffect(() => {
    // `warm` lives in the closure rather than in state: the effect deliberately
    // has no dependencies, so it must not read anything that re-arms it.
    let stopped = false, timer = null, idx = 0, misses = 0, warm = false;
    const fetchStatus = () => api('/api/status').then(r => { if (!stopped) setStatus(r); });
    const fetchSim = () => (wantSim.current
      ? api('/api/sim').then(r => { if (!stopped) setSim(r); })
      : Promise.resolve());
    simRefresh.current = () => (stopped ? Promise.resolve() : fetchSim());

    const tick = () => {
      if (stopped) return;
      const job = (idx++ % 2 === 0) ? fetchStatus : fetchSim;
      job()
        .then(() => { misses = 0; warm = true; if (!stopped) { setOnline(true); setLoaded(true); } })
        // Unreachable is a state, not a crash: keep the loop alive, say so in
        // the header, and let the next tick find the device again. Every
        // rejection is handled here, so nothing reaches unhandledrejection.
        .catch(() => { if (!stopped && ++misses >= OFFLINE_AFTER) setOnline(false); })
        .then(() => { if (!stopped) timer = setTimeout(tick, warm ? POLL_MS : FIRST_LOAD_MS); });
    };
    tick();
    return () => { stopped = true; clearTimeout(timer); };
  }, []);

  useEffect(() => {
    const onResize = () => {
      if (window.innerWidth < 1200) setShowSidebar(false);
      if (window.innerWidth > 1600) setShowSidebar(true);
    };
    window.addEventListener('resize', onResize);
    onResize();
    return () => window.removeEventListener('resize', onResize);
  }, []);

  if (!loaded && !status) return LoadingSpinner();

  const simOn = !!(status && status.sim && status.sim.enabled);
  const simCount = (status && status.sim && status.sim.peers) || 0;

  return html`
<div class="min-h-screen page-bg">
  <${Sidebar} url=${url} show=${showSidebar} status=${status} />
  <${Header} status=${status} online=${online} showSidebar=${showSidebar} setShowSidebar=${setShowSidebar} />
  <div class="${showSidebar && 'pl-72'} transition-all duration-300 transform">
    ${!online && html`
    <${Banner} tone="bad" icon=${Icons.fail} title="Device not responding.">
      Still retrying every ${POLL_MS} ms - the figures below are the last ones it sent.
    <//>`}
    ${simOn && html`
    <${Banner} tone="sim" icon=${Icons.beaker} title="Simulated traffic is switched on.">
      ${simCount} simulated ${simCount === 1 ? 'peer is' : 'peers are'} being injected on a virtual radio. Anything in the
      peer table, on the radar or in Follow may not be a real aircraft.
    <//>`}
    ${status && status.config_corrupt && html`
    <${Banner} tone="warn" title="/config.json could not be parsed.">
      The node is running compile-time defaults for this boot and has left the file alone so it can be recovered.
      Saving from here overwrites it.
    <//>`}
    ${status && status.reboot_required && html`
    <${Banner} tone="warn" title="Reboot required.">
      A setting that only takes effect at boot has changed.
    <//>`}
    ${status && status.log && status.log.errors > 0 && url !== '/system' && html`
    <${Banner} tone="bad" icon=${Icons.warn}
      title=${status.log.errors + ' error' + (status.log.errors === 1 ? '' : 's') + ' logged since boot.'}>
      <a href="#/system" class="underline">Read the device log<//> - on this hardware the console UART is the
      flight controller's link, so this is the only place those lines go.
    <//>`}
    <${Router} onChange=${ev => { setUrl(ev.url); setShowSidebar(window.innerWidth > 1200); }} history=${History.createHashHistory()}>
      <${Dashboard} default=${true} status=${status} />
      <${Radios} path="/radios" status=${status} />
      <${FollowPage} path="/follow" status=${status} />
      <${Settings} path="/settings" />
      <${Simulator} path="/sim" status=${status} sim=${sim} refreshSim=${() => simRefresh.current()} />
      <${System} path="/system" status=${status} />
      <${Update} path="/update" />
    <//>
  <//>
<//>`;
};

window.onload = () => render(h(App), document.body);
