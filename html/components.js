'use strict';
import { h, useState, useEffect, useRef, html } from './bundle.js';

// Shared presentational pieces for the v2 UI. Nothing here fetches; every
// component is handed the data it renders, so pages own their own polling and
// a component can be dropped anywhere without starting a timer behind your back.

export const Icons = {
  upArrowBox: props => html`<svg class=${props.class} xmlns="http://www.w3.org/2000/svg" fill="none" viewBox="0 0 24 24" stroke-width="1.5" stroke="currentColor"> <path stroke-linecap="round" stroke-linejoin="round" d="M9 8.25H7.5a2.25 2.25 0 00-2.25 2.25v9a2.25 2.25 0 002.25 2.25h9a2.25 2.25 0 002.25-2.25v-9a2.25 2.25 0 00-2.25-2.25H15m0-3l-3-3m0 0l-3 3m3-3V15" /> </svg>`,
  settings: props => html`<svg class=${props.class} xmlns="http://www.w3.org/2000/svg" fill="none" viewBox="0 0 24 24" stroke-width="1.5" stroke="currentColor"> <path stroke-linecap="round" stroke-linejoin="round" d="M9.594 3.94c.09-.542.56-.94 1.11-.94h2.593c.55 0 1.02.398 1.11.94l.213 1.281c.063.374.313.686.645.87.074.04.147.083.22.127.324.196.72.257 1.075.124l1.217-.456a1.125 1.125 0 011.37.49l1.296 2.247a1.125 1.125 0 01-.26 1.431l-1.003.827c-.293.24-.438.613-.431.992a6.759 6.759 0 010 .255c-.007.378.138.75.43.99l1.005.828c.424.35.534.954.26 1.43l-1.298 2.247a1.125 1.125 0 01-1.369.491l-1.217-.456c-.355-.133-.75-.072-1.076.124a6.57 6.57 0 01-.22.128c-.331.183-.581.495-.644.869l-.213 1.28c-.09.543-.56.941-1.11.941h-2.594c-.55 0-1.02-.398-1.11-.94l-.213-1.281c-.062-.374-.312-.686-.644-.87a6.52 6.52 0 01-.22-.127c-.325-.196-.72-.257-1.076-.124l-1.217.456a1.125 1.125 0 01-1.369-.49l-1.297-2.247a1.125 1.125 0 01.26-1.431l1.004-.827c.292-.24.437-.613.43-.992a6.932 6.932 0 010-.255c.007-.378-.138-.75-.43-.99l-1.004-.828a1.125 1.125 0 01-.26-1.43l1.297-2.247a1.125 1.125 0 011.37-.491l1.216.456c.356.133.751.072 1.076-.124.072-.044.146-.087.22-.128.332-.183.582-.495.644-.869l.214-1.281z" /> <path stroke-linecap="round" stroke-linejoin="round" d="M15 12a3 3 0 11-6 0 3 3 0 016 0z" /> </svg>`,
  scan: props => html`<svg class=${props.class} xmlns="http://www.w3.org/2000/svg" fill="none" viewBox="0 0 24 24" stroke-width="1.5" stroke="currentColor"> <path stroke-linecap="round" stroke-linejoin="round" d="M7.5 3.75H6A2.25 2.25 0 003.75 6v1.5M16.5 3.75H18A2.25 2.25 0 0120.25 6v1.5m0 9V18A2.25 2.25 0 0118 20.25h-1.5m-9 0H6A2.25 2.25 0 013.75 18v-1.5M15 12a3 3 0 11-6 0 3 3 0 016 0z" /> </svg>`,
  refresh: props => html`<svg class=${props.class} xmlns="http://www.w3.org/2000/svg" fill="none" viewBox="0 0 24 24" stroke-width="1.5" stroke="currentColor"> <path stroke-linecap="round" stroke-linejoin="round" d="M16.023 9.348h4.992v-.001M2.985 19.644v-4.992m0 0h4.992m-4.993 0l3.181 3.183a8.25 8.25 0 0013.803-3.7M4.031 9.865a8.25 8.25 0 0113.803-3.7l3.181 3.182m0-4.991v4.99" /> </svg>`,
  bars3: props => html`<svg class=${props.class} xmlns="http://www.w3.org/2000/svg" fill="none" viewBox="0 0 24 24" stroke-width="1.5" stroke="currentColor"> <path stroke-linecap="round" stroke-linejoin="round" d="M3.75 6.75h16.5M3.75 12h16.5m-16.5 5.25h16.5" /> </svg>`,
  save: props => html`<svg class=${props.class} xmlns="http://www.w3.org/2000/svg" fill="none" viewBox="0 0 24 24" stroke-width="1.5" stroke="currentColor"> <path stroke-linecap="round" stroke-linejoin="round" d="M16.5 3.75V16.5L12 14.25 7.5 16.5V3.75m9 0H18A2.25 2.25 0 0120.25 6v12A2.25 2.25 0 0118 20.25H6A2.25 2.25 0 013.75 18V6A2.25 2.25 0 016 3.75h1.5m9 0h-9" /> </svg>`,
  ok: props => html`<svg class=${props.class} fill="none" viewBox="0 0 24 24" stroke-width="1.5" stroke="currentColor" aria-hidden="true"> <path stroke-linecap="round" stroke-linejoin="round" d="M9 12.75L11.25 15 15 9.75M21 12a9 9 0 11-18 0 9 9 0 0118 0z" /> </svg>`,
  fail: props => html`<svg class=${props.class} xmlns="http://www.w3.org/2000/svg" fill="none" viewBox="0 0 24 24" stroke-width="1.5" stroke="currentColor"> <path stroke-linecap="round" stroke-linejoin="round" d="M9.75 9.75l4.5 4.5m0-4.5l-4.5 4.5M21 12a9 9 0 11-18 0 9 9 0 0118 0z" /> </svg>`,
  bolt: props => html`<svg class=${props.class} xmlns="http://www.w3.org/2000/svg" fill="none" viewBox="0 0 24 24" stroke-width="1.5" stroke="currentColor"> <path stroke-linecap="round" stroke-linejoin="round" d="M3.75 13.5l10.5-11.25L12 10.5h8.25L9.75 21.75 12 13.5H3.75z" /> </svg>`,
  home: props => html`<svg class=${props.class} xmlns="http://www.w3.org/2000/svg" fill="none" viewBox="0 0 24 24" stroke-width="1.5" stroke="currentColor"> <path stroke-linecap="round" stroke-linejoin="round" d="M2.25 12l8.954-8.955c.44-.439 1.152-.439 1.591 0L21.75 12M4.5 9.75v10.125c0 .621.504 1.125 1.125 1.125H9.75v-4.875c0-.621.504-1.125 1.125-1.125h2.25c.621 0 1.125.504 1.125 1.125V21h4.125c.621 0 1.125-.504 1.125-1.125V9.75M8.25 21h8.25" /> </svg>`,
  shield: props => html`<svg class=${props.class} xmlns="http://www.w3.org/2000/svg" fill="none" viewBox="0 0 24 24" stroke-width="1.5" stroke="currentColor"> <path stroke-linecap="round" stroke-linejoin="round" d="M9 12.75L11.25 15 15 9.75m-3-7.036A11.959 11.959 0 013.598 6 11.99 11.99 0 003 9.749c0 5.592 3.824 10.29 9 11.623 5.176-1.332 9-6.03 9-11.622 0-1.31-.21-2.571-.598-3.751h-.152c-3.196 0-6.1-1.248-8.25-3.285z" /> </svg>`,
  warn: props => html`<svg class=${props.class} xmlns="http://www.w3.org/2000/svg" fill="none" viewBox="0 0 24 24" stroke-width="1.5" stroke="currentColor"> <path stroke-linecap="round" stroke-linejoin="round" d="M12 9v3.75m-9.303 3.376c-.866 1.5.217 3.374 1.948 3.374h14.71c1.73 0 2.813-1.874 1.948-3.374L13.949 3.378c-.866-1.5-3.032-1.5-3.898 0L2.697 16.126zM12 15.75h.007v.008H12v-.008z" /> </svg>`,
  info: props => html`<svg class=${props.class} xmlns="http://www.w3.org/2000/svg" fill="none" viewBox="0 0 24 24" stroke-width="1.5" stroke="currentColor"> <path stroke-linecap="round" stroke-linejoin="round" d="M11.25 11.25l.041-.02a.75.75 0 011.063.852l-.708 2.836a.75.75 0 001.063.853l.041-.021M21 12a9 9 0 11-18 0 9 9 0 0118 0zm-9-3.75h.008v.008H12V8.25z" /> </svg>`,
  link: props => html`<svg class=${props.class} xmlns="http://www.w3.org/2000/svg" fill="none" viewBox="0 0 24 24" stroke-width="1.5" stroke="currentColor"> <path stroke-linecap="round" stroke-linejoin="round" d="M13.19 8.688a4.5 4.5 0 011.242 7.244l-4.5 4.5a4.5 4.5 0 01-6.364-6.364l1.757-1.757m13.35-.622l1.757-1.757a4.5 4.5 0 00-6.364-6.364l-4.5 4.5a4.5 4.5 0 001.242 7.244" /> </svg>`,
  antenna: props => html`<svg class=${props.class} xmlns="http://www.w3.org/2000/svg" fill="none" viewBox="0 0 24 24" stroke-width="1.5" stroke="currentColor"> <path stroke-linecap="round" stroke-linejoin="round" d="M9.348 14.652a3.75 3.75 0 010-5.304m5.304 0a3.75 3.75 0 010 5.304m-7.425 2.121a6.75 6.75 0 010-9.546m9.546 0a6.75 6.75 0 010 9.546M5.106 18.894c-3.808-3.807-3.808-9.98 0-13.788m13.788 0c3.808 3.807 3.808 9.98 0 13.788M12 12h.008v.008H12V12z" /> </svg>`,
  list: props => html`<svg class=${props.class} xmlns="http://www.w3.org/2000/svg" fill="none" viewBox="0 0 24 24" stroke-width="1.5" stroke="currentColor"> <path stroke-linecap="round" stroke-linejoin="round" d="M8.25 6.75h12M8.25 12h12m-12 5.25h12M3.75 6.75h.007v.008H3.75V6.75zm.375 0a.375.375 0 11-.75 0 .375.375 0 01.75 0zM3.75 12h.007v.008H3.75V12zm.375 0a.375.375 0 11-.75 0 .375.375 0 01.75 0zm-.375 5.25h.007v.008H3.75v-.008zm.375 0a.375.375 0 11-.75 0 .375.375 0 01.75 0z" /> </svg>`,
  beaker: props => html`<svg class=${props.class} xmlns="http://www.w3.org/2000/svg" fill="none" viewBox="0 0 24 24" stroke-width="1.5" stroke="currentColor"> <path stroke-linecap="round" stroke-linejoin="round" d="M9.75 3.104v5.714a2.25 2.25 0 01-.659 1.591L5 14.5M9.75 3.104c-.251.023-.501.05-.75.082m.75-.082a24.301 24.301 0 014.5 0m0 0v5.714c0 .597.237 1.17.659 1.591L19.8 15.3M14.25 3.104c.251.023.501.05.75.082M19.8 15.3l-1.57.393A9.065 9.065 0 0112 15a9.065 9.065 0 00-6.23-.693L5 14.5m14.8.8l1.402 1.402c1.232 1.232.65 3.318-1.067 3.611A48.309 48.309 0 0112 21c-2.773 0-5.491-.235-8.135-.687-1.718-.293-2.3-2.379-1.067-3.61L5 14.5" /> </svg>`,
  trash: props => html`<svg class=${props.class} xmlns="http://www.w3.org/2000/svg" fill="none" viewBox="0 0 24 24" stroke-width="1.5" stroke="currentColor"> <path stroke-linecap="round" stroke-linejoin="round" d="M14.74 9l-.346 9m-4.788 0L9.26 9m9.968-3.21c.342.052.682.107 1.022.166m-1.022-.165L18.16 19.673a2.25 2.25 0 01-2.244 2.077H8.084a2.25 2.25 0 01-2.244-2.077L4.772 5.79m14.456 0a48.108 48.108 0 00-3.478-.397m-12 .562c.34-.059.68-.114 1.022-.165m0 0a48.11 48.11 0 013.478-.397m7.5 0v-.916c0-1.18-.91-2.164-2.09-2.201a51.964 51.964 0 00-3.32 0c-1.18.037-2.09 1.022-2.09 2.201v.916m7.5 0a48.667 48.667 0 00-7.5 0" /> </svg>`,
  plus: props => html`<svg class=${props.class} xmlns="http://www.w3.org/2000/svg" fill="none" viewBox="0 0 24 24" stroke-width="1.5" stroke="currentColor"> <path stroke-linecap="round" stroke-linejoin="round" d="M12 4.5v15m7.5-7.5h-15" /> </svg>`,
  sun: props => html`<svg class=${props.class} xmlns="http://www.w3.org/2000/svg" fill="none" viewBox="0 0 24 24" stroke-width="1.5" stroke="currentColor"> <path stroke-linecap="round" stroke-linejoin="round" d="M12 3v2.25m6.364.386l-1.591 1.591M21 12h-2.25m-.386 6.364l-1.591-1.591M12 18.75V21m-4.773-4.227l-1.591 1.591M5.25 12H3m4.227-4.773L5.636 5.636M15.75 12a3.75 3.75 0 11-7.5 0 3.75 3.75 0 017.5 0z" /> </svg>`,
  moon: props => html`<svg class=${props.class} xmlns="http://www.w3.org/2000/svg" fill="none" viewBox="0 0 24 24" stroke-width="1.5" stroke="currentColor"> <path stroke-linecap="round" stroke-linejoin="round" d="M21.752 15.002A9.718 9.718 0 0118 15.75c-5.385 0-9.75-4.365-9.75-9.75 0-1.33.266-2.597.748-3.752A9.753 9.753 0 003 11.25C3 16.635 7.365 21 12.75 21a9.753 9.753 0 009.002-5.998z" /> </svg>`,
};

// Light mode: pale tint + dark ink. Dark mode flips it - a deep tint with light
// ink - rather than leaving a glaring pale chip on a dark card.
export const tipColors = {
  green: 'bg-green-100 text-green-900 dark:bg-green-900 dark:text-green-200',
  yellow: 'bg-yellow-100 text-yellow-900 dark:bg-yellow-800 dark:text-yellow-100',
  red: 'bg-red-100 text-red-900 dark:bg-red-900 dark:text-red-200',
  blue: 'bg-blue-600 text-white',
  gray: 'bg-gray-200 text-gray-800 dark:bg-slate-700 dark:text-slate-200',
};

// ---- Formatting --------------------------------------------------------------
//
// The API omits a field that has no value rather than sending zero, because
// zero is a legitimate reading for most of them. Everything below keeps that
// distinction: `present()` is the only test used, and absent renders as a dash,
// never as 0.

export const present = v => v !== undefined && v !== null && !(typeof v === 'number' && isNaN(v));

export const DASH = '–';

/** A number, or a dash when the field was absent. */
export function num(v, digits = 0, unit = '') {
  if (!present(v)) return DASH;
  return (+v).toFixed(digits) + unit;
}

/** deg * 1e7 (the wire/MSP format) as plain degrees. */
export const deg1e7 = v => (present(v) ? v / 1e7 : null);

export function latLon(lat, lon) {
  if (!present(lat) || !present(lon)) return DASH;
  return `${deg1e7(lat).toFixed(5)}, ${deg1e7(lon).toFixed(5)}`;
}

/** "how long ago", short enough for a table cell. */
export function age(ms) {
  if (!present(ms)) return DASH;
  if (ms < 1000) return ms + 'ms';
  if (ms < 10000) return (ms / 1000).toFixed(1) + 's';
  if (ms < 60000) return Math.round(ms / 1000) + 's';
  if (ms < 3600000) return Math.floor(ms / 60000) + 'm' + String(Math.floor(ms / 1000) % 60).padStart(2, '0') + 's';
  return Math.floor(ms / 3600000) + 'h' + String(Math.floor(ms / 60000) % 60).padStart(2, '0') + 'm';
}

export function uptime(ms) {
  if (!present(ms)) return DASH;
  const s = Math.floor(ms / 1000);
  const d = Math.floor(s / 86400), hh = Math.floor(s / 3600) % 24;
  const mm = Math.floor(s / 60) % 60, ss = s % 60;
  if (d) return `${d}d ${hh}h ${String(mm).padStart(2, '0')}m`;
  if (hh) return `${hh}h ${String(mm).padStart(2, '0')}m`;
  return `${mm}m ${String(ss).padStart(2, '0')}s`;
}

// UIDs travel as 8-char lower-case hex in /api/status and /api/frames, but the
// config carries follow.targetUid as a plain JSON number. These two convert
// between the forms so only one of them ever reaches the screen.
export const uidToHex = n => (present(n) ? (n >>> 0).toString(16).padStart(8, '0') : '');
export const hexToUid = s => {
  const v = parseInt(String(s || '').trim().replace(/^0x/i, ''), 16);
  return isNaN(v) ? 0 : v >>> 0;
};
export const isHexUid = s => /^[0-9a-fA-F]{1,8}$/.test(String(s || '').trim());

/** Peer status bits from ff::PositionFlags. */
export const PEER_FLAG_ARMED = 1;
export const PEER_FLAG_HAS_FIX = 2;

/** Speed is cm/s on the wire everywhere; nobody thinks in cm/s. */
export const speedMs = cms => (present(cms) ? (cms / 100).toFixed(1) + ' m/s' : DASH);
/** Course is decidegrees on the wire. */
export const courseDeg = ddeg => (present(ddeg) ? Math.round(ddeg / 10) + '°' : DASH);

const COMPASS = ['N', 'NE', 'E', 'SE', 'S', 'SW', 'W', 'NW'];
export const compass = deg => (present(deg) ? COMPASS[Math.round(((deg % 360) + 360) % 360 / 45) % 8] : DASH);

// ---- Primitives --------------------------------------------------------------

export function Button({ title, onclick, disabled, cls, icon, ref, colors }) {
  const [spin, setSpin] = useState(false);
  const cb = () => {
    const res = onclick ? onclick() : null;
    if (res && typeof (res.catch) === 'function') {
      setSpin(true);
      res.catch(() => false).then(() => setSpin(false));
    }
  };
  if (!colors) colors = 'bg-blue-600 hover:bg-blue-500 disabled:bg-blue-400';
  return html`
  <button type="button" class="inline-flex justify-center items-center gap-1 rounded px-2.5 py-1.5 text-sm font-semibold text-white shadow-sm ${colors} ${cls}"
  ref=${ref} onclick=${cb} disabled=${disabled || spin} >
  ${title}
  <${spin ? Icons.refresh : icon} class="w-4 ${spin ? 'animate-spin' : ''}" />
<//>`;
}

export function Notification({ ok, text, close, timeout = 2500 }) {
  const closebtn = useRef(null);
  const from = 'translate-y-2 opacity-0 sm:translate-y-0 sm:translate-x-2';
  const to = 'translate-y-0 opacity-100 sm:translate-x-0';
  const [tr, setTr] = useState(from);
  // The dismiss timer is owned by the effect: without the cleanup a toast that
  // unmounts early (page change) still fires its click into a dead ref.
  useEffect(() => {
    setTr(to);
    const t = setTimeout(() => {
      if (closebtn.current && closebtn.current.click) closebtn.current.click();
    }, timeout);
    return () => clearTimeout(t);
  }, []);
  const onclose = () => { setTr(from); setTimeout(close, 300); };
  return html`
<div aria-live="assertive" class="z-10 pointer-events-none absolute inset-0 flex items-end px-4 py-6 sm:items-start sm:p-6">
  <div class="flex w-full flex-col items-end space-y-4">
    <div class="pointer-events-auto w-full max-w-sm overflow-hidden rounded-lg bg-white dark:bg-slate-700 shadow-lg ring-1 ring-black ring-opacity-5 transform ease-out duration-300 transition ${tr}">
      <div class="p-4">
        <div class="flex items-start">
          <div class="flex-shrink-0">
            <${ok === true ? Icons.ok : Icons.fail} class="h-6 w-6 ${ok === true ? 'text-green-400' : 'text-red-400'}" />
          <//>
          <div class="ml-3 w-0 flex-1 pt-0.5">
            <p class="text-sm font-medium text-gray-900 dark:text-slate-100">${text}<//>
          <//>
          <div class="ml-4 flex flex-shrink-0">
            <button type="button" ref=${closebtn} onclick=${onclose} class="inline-flex rounded-md text-gray-400 hover:text-gray-500">
              <span class="sr-only">Close<//>
              <svg class="h-5 w-5" viewBox="0 0 20 20" fill="currentColor" aria-hidden="true">
                <path d="M6.28 5.22a.75.75 0 00-1.06 1.06L8.94 10l-3.72 3.72a.75.75 0 101.06 1.06L10 11.06l3.72 3.72a.75.75 0 101.06-1.06L11.06 10l3.72-3.72a.75.75 0 00-1.06-1.06L10 8.94 6.28 5.22z" />
              <//>
            <//>
          <//>
        <//>
      <//>
    <//>
  <//>
<//>`;
}

export function Colored({ icon, text, colors, title }) {
  return html`
<span title=${title} class="inline-flex items-center gap-1.5 py-0.5 px-2 rounded-full ${colors || 'bg-slate-100 text-slate-900 dark:bg-slate-700 dark:text-slate-100'}">
  ${icon && html`<${icon} class="w-4 h-4" />`}
  ${text !== undefined && text !== '' && html`<span class="inline-block text-xs font-medium">${text}<//>`}
<//>`;
}

export function Stat({ title, text, tipText, tipIcon, tipColors, subText, icon }) {
  return html`
<div class="flex flex-col bg-white dark:bg-slate-800 border dark:border-slate-700 shadow-sm rounded-xl">
  <div class="p-4 md:p-5">
    <p class="text-xs uppercase tracking-wide text-gray-500 dark:text-slate-400">
      <span class="inline-flex gap-1.5 items-center py-0.5 pr-2">${icon && html`<${icon} class="w-4 h-4" />`}${title}<//>
    <//>
    <div class="mt-1 flex items-center gap-x-2">
      <h3 class="text-xl font-medium text-gray-800 dark:text-slate-100">${text}<//>
      ${(tipText || tipIcon) && html`<span class="flex items-center"><${Colored} text=${tipText} icon=${tipIcon} colors=${tipColors} /><//>`}
    <//>
    ${!!subText && html`
    <div class="mt-1 text-xs text-gray-500 dark:text-slate-400">${subText}<//>`}
  <//>
<//>`;
}

export function TextValue({ value, setfn, disabled, placeholder, type, addonRight, addonLeft, attr }) {
  const f = type === 'number' ? x => setfn(x === '' ? '' : +x) : setfn;
  return html`
<div class="flex w-full items-center rounded border border-gray-300 dark:border-slate-600 shadow-sm">
  ${addonLeft && html`<span class="inline-flex font-normal truncate py-1 border-r border-gray-300 dark:border-slate-600 bg-slate-100 dark:bg-slate-700 items-center px-2 text-gray-500 dark:text-slate-400 text-xs">${addonLeft}<//>`}
  <input type=${type || 'text'} disabled=${disabled}
    oninput=${ev => f(ev.target.value)} ...${attr}
    class="font-normal text-sm rounded w-full flex-1 py-0.5 px-2 field-bg text-gray-700 dark:text-slate-200 placeholder:text-gray-400 focus:outline-none disabled:cursor-not-allowed disabled:bg-gray-100 dark:disabled:bg-slate-800 disabled:text-gray-500" placeholder=${placeholder} value=${value} />
  ${addonRight && html`<span class="inline-flex font-normal truncate py-1 border-l border-gray-300 dark:border-slate-600 bg-slate-100 dark:bg-slate-700 items-center px-2 text-gray-500 dark:text-slate-400 text-xs">${addonRight}<//>`}
<//>`;
}

export function SelectValue({ value, setfn, options, disabled }) {
  // Numeric option values come back from the DOM as strings; anything that
  // round-trips through parseInt unchanged is handed back as a number, so a
  // select over enum indices doesn't quietly turn the config into strings.
  const toInt = x => (x == parseInt(x) ? parseInt(x) : x);
  return html`
<select onchange=${ev => setfn(toInt(ev.target.value))} disabled=${disabled}
  class="w-full rounded font-normal border border-gray-300 dark:border-slate-600 field-bg py-0.5 px-1 text-gray-600 dark:text-slate-200 focus:outline-none text-sm disabled:cursor-not-allowed">
  ${options.map(v => html`<option key=${v[0]} value=${v[0]} selected=${v[0] == value}>${v[1]}<//>`)}
<//>`;
}

export function SwitchValue({ value, setfn, disabled }) {
  const bg = value ? 'bg-blue-600' : 'bg-gray-200 dark:bg-slate-600';
  const tr = value ? 'translate-x-5' : 'translate-x-0';
  return html`
<button type="button" onclick=${() => !disabled && setfn(!value)} disabled=${disabled}
  class="${bg} inline-flex h-6 w-11 flex-shrink-0 cursor-pointer rounded-full border-2 border-transparent transition-colors duration-200 ease-in-out focus:outline-none focus:ring-0 ring-0 disabled:cursor-not-allowed" role="switch" aria-checked=${!!value}>
  <span aria-hidden="true" class="${tr} pointer-events-none inline-block h-5 w-5 transform rounded-full bg-white shadow ring-0 transition duration-200 ease-in-out"><//>
<//>`;
}

// A stepper built from plain buttons + a display div rather than a native
// <input type=number>: it lets a sentinel like -1 read as "Disabled" instead of
// a number, and keeps clear of the native control's mobile-Firefox quirks.
export function SpinnerValue({ value, setfn, min, max, step, disabled, labelFn }) {
  const s = step || 1;
  const lo = min === undefined ? -Infinity : min;
  const hi = max === undefined ? Infinity : max;
  const clamp = v => Math.min(hi, Math.max(lo, v));
  const label = labelFn ? labelFn(value) : undefined;
  const btn = 'px-2.5 py-0.5 text-gray-500 dark:text-slate-300 disabled:cursor-not-allowed disabled:opacity-40 hover:enabled:bg-slate-200 dark:hover:enabled:bg-slate-600';
  return html`
<div class="flex w-full items-stretch rounded border border-gray-300 dark:border-slate-600 shadow-sm overflow-hidden bg-slate-100 dark:bg-slate-700">
  <button type="button" onclick=${() => setfn(clamp(value - s))} disabled=${disabled || value <= lo} class="${btn} border-r border-gray-300 dark:border-slate-600">−<//>
  <div class="flex-1 flex items-center justify-center text-sm field-bg select-none ${disabled ? 'text-gray-400' : 'text-gray-700 dark:text-slate-200'}">${label !== undefined ? label : value}<//>
  <button type="button" onclick=${() => setfn(clamp(value + s))} disabled=${disabled || value >= hi} class="${btn} border-l border-gray-300 dark:border-slate-600">+<//>
<//>`;
}

export function FileValue({ onchange }) {
  return html`<input type="file" class="block w-full text-sm text-gray-500 dark:text-slate-400" onchange=${onchange} />`;
}

export function Tooltip({ text }) {
  if (!text) return '';
  return html`
<span class="tooltip-wrap" tabindex="0">
  <${Icons.info} class="w-4 h-4 tooltip-icon" />
  <span class="tooltip-bubble" role="tooltip">${text}<//>
<//>`;
}

export function Setting(props) {
  return html`
<div class=${props.cls || 'grid grid-cols-2 gap-2 my-1'}>
  <label class="flex items-center text-sm text-gray-700 dark:text-slate-300 mr-2 font-medium">${props.title}<${Tooltip} text=${props.tip} /><//>
  <div class="flex flex-col">
    <div class="flex items-center">
      ${props.type === 'switch' ? h(SwitchValue, props) :
        props.type === 'select' ? h(SelectValue, props) :
          props.type === 'spinner' ? h(SpinnerValue, props) :
            props.type === 'file' ? h(FileValue, props) :
              props.type === 'static' ? html`<span class="text-sm text-gray-500 dark:text-slate-400 font-mono">${props.value}<//>` :
                h(TextValue, props)}
    <//>
    ${props.hint && html`<span class="text-xs text-gray-400 mt-1">${props.hint}<//>`}
  <//>
<//>`;
}

// Hoisted rather than declared inside their pages: a component defined inside
// another is a fresh function identity every render, which makes preact tear
// the DOM down and rebuild it instead of patching.
export const SectionTitle = ({ title, note }) => html`
<div class="mb-2 mt-5">
  <div class="text-sm text-slate-500 dark:text-slate-400 font-semibold">${title}<//>
  ${note && html`<p class="text-xs text-gray-400 mt-1">${note}<//>`}
<//>`;

export const Card = ({ title, icon, right, children, cls }) => html`
<div class="border border-gray-200 dark:border-slate-700 rounded-xl bg-white dark:bg-slate-800 shadow-sm flex flex-col ${cls}">
  ${title && html`
  <div class="flex items-center justify-between gap-2 px-4 py-2 border-b border-gray-200 dark:border-slate-700">
    <div class="font-light uppercase flex items-center gap-2 text-gray-600 dark:text-slate-300 text-sm">
      ${icon && html`<${icon} class="w-5 h-5" />`}${title}
    <//>
    ${right}
  <//>`}
  <div class="p-4 flex-1 relative">${children}<//>
<//>`;

export const Note = ({ title = 'Note', children }) => html`
<div class="flex p-4 gap-2">
  <${Icons.info} class="self-start basis-[30px] grow-0 shrink-0 text-green-600" />
  <div class="text-sm text-slate-600 dark:text-slate-300">
    <div class="font-semibold mt-1">${title}<//>
    ${children}
  <//>
<//>`;

/** Full-width page banner. `tone` is one of ok / warn / bad / sim. */
export const Banner = ({ tone = 'warn', icon, title, children }) => {
  const tones = {
    ok: 'bg-green-100 text-green-900 dark:bg-green-900 dark:text-green-200 border-green-600',
    warn: 'bg-yellow-100 text-yellow-900 dark:bg-yellow-800 dark:text-yellow-100 border-yellow-600',
    bad: 'bg-red-100 text-red-900 dark:bg-red-900 dark:text-red-200 border-red-600',
    sim: 'bg-violet-100 text-violet-900 dark:bg-violet-900 dark:text-violet-100 border-violet-600',
  };
  return html`
<div class="flex items-center gap-3 px-4 py-2 border-b-2 ${tones[tone]}">
  <${icon || Icons.warn} class="w-5 h-5 shrink-0" />
  <div class="text-sm">
    <span class="font-semibold">${title}<//>${children && html` <span>${children}<//>`}
  <//>
<//>`;
};

/**
 * The Apply / Save pair, which is the same on every config page.
 *
 * Presentational on purpose: the page supplies the two callbacks, because the
 * two actions really are different requests and the difference is the whole
 * point. Apply is POST /api/config - RAM only, live immediately, and undone by a
 * power cycle, which is the way back from a setting that takes the node off the
 * air. Save additionally writes /config.json, at which point a power cycle
 * brings the bad setting back with it.
 */
export function ConfigActions({ onApply, onSave, disabled, blockedReason, unsaved }) {
  return html`
<div class="mt-5 pt-4 border-t border-gray-200 dark:border-slate-700">
  ${blockedReason && html`
  <div class="mb-3 flex items-start gap-2 text-sm text-red-900 dark:text-red-200 bg-red-100 dark:bg-red-900 rounded px-3 py-2">
    <${Icons.warn} class="w-5 h-5 shrink-0" /><span>${blockedReason}<//>
  <//>`}
  <div class="flex flex-wrap items-center gap-2 justify-end">
    ${unsaved && html`<${Colored} text="applied, not yet written to flash" colors=${tipColors.yellow} icon=${Icons.warn} />`}
    <div class="flex-1"><//>
    <${Button} title="Apply" icon=${Icons.bolt} onclick=${onApply} disabled=${disabled}
      colors="bg-blue-600 hover:bg-blue-500 disabled:bg-blue-400" />
    <${Button} title="Save to flash" icon=${Icons.save} onclick=${onSave} disabled=${disabled}
      colors="bg-green-600 hover:bg-green-500 disabled:bg-green-400" />
  <//>
  <p class="text-xs text-gray-400 mt-2 text-right">
    <span class="font-semibold">Apply<//> changes the running node only - a power cycle puts it back.
    <span class="font-semibold">Save<//> also writes it to flash, so it survives the reboot.
  <//>
<//>`;
}

export const Th = ({ title, cls, tip }) => html`
<th scope="col" title=${tip} class="sticky top-0 z-10 border-b border-slate-300 dark:border-slate-600 bg-white dark:bg-slate-800 bg-opacity-75 py-1.5 px-2 text-left text-xs font-semibold uppercase tracking-wide text-slate-500 dark:text-slate-400 backdrop-blur backdrop-filter ${cls}">${title}</th>`;

export const Td = ({ text, cls, title, onclick, children }) => html`
<td title=${title} onclick=${onclick} class="whitespace-nowrap border-b border-slate-200 dark:border-slate-700 py-1.5 px-2 text-sm text-slate-900 dark:text-slate-100 ${cls}">${text}${children}</td>`;

export default function LoadingSpinner() {
  return html`
  <div id="root">
    <div class="loader-container">
      <div class="loader">
        <div class="sk-chase">
          <div class="sk-chase-dot"><//><div class="sk-chase-dot"><//><div class="sk-chase-dot"><//>
          <div class="sk-chase-dot"><//><div class="sk-chase-dot"><//><div class="sk-chase-dot"><//>
        <//>
      <//>
    <//>
  <//>`;
}

// ---- Sparkline ---------------------------------------------------------------

/**
 * Tiny self-contained line chart for a rolling series of numbers (oldest first,
 * nulls allowed for gaps). No chart library: one SVG path, auto-scaled, themed
 * with a stroke-* class so it follows light/dark like everything else.
 */
export function Sparkline({ series, label, unit = '', digits = 0, color = 'stroke-blue-500' }) {
  const vals = (series || []).filter(v => v != null && !isNaN(v));
  const W = 240, H = 40, pad = 3;
  const head = html`<div class="flex justify-between items-baseline text-xs">
    <span class="text-slate-500 dark:text-slate-400">${label}<//>
    <span class="font-mono text-slate-700 dark:text-slate-200">${vals.length ? vals[vals.length - 1].toFixed(digits) + unit : DASH}<//>
  <//>`;
  if (vals.length < 2) {
    return html`<div class="mb-3">${head}<div class="text-xs text-slate-400" style="height:40px;line-height:40px">collecting…<//><//>`;
  }
  const min = Math.min(...vals), max = Math.max(...vals), span = (max - min) || 1, n = series.length;
  const x = i => pad + (i / (n - 1)) * (W - 2 * pad);
  const y = v => H - pad - ((v - min) / span) * (H - 2 * pad);
  let d = '', started = false;
  series.forEach((v, i) => {
    if (v == null || isNaN(v)) { started = false; return; } // break the line across gaps
    d += (started ? 'L' : 'M') + x(i).toFixed(1) + ',' + y(v).toFixed(1) + ' ';
    started = true;
  });
  return html`
  <div class="mb-3">
    ${head}
    <svg viewBox="0 0 ${W} ${H}" preserveAspectRatio="none" class="w-full" style="height:40px">
      <path d=${d} fill="none" stroke-width="1.5" class=${color} vector-effect="non-scaling-stroke" />
    <//>
    <div class="flex justify-between text-slate-400" style="font-size:10px">
      <span>${min.toFixed(digits)}${unit}<//><span>${max.toFixed(digits)}${unit}<//>
    <//>
  <//>`;
}

// ---- Radios ------------------------------------------------------------------

/**
 * Which radios this node is running, and which of them a given peer has been
 * heard on.
 *
 * A chip that is present-but-hollow is the whole point: "heard on ESP-NOW,
 * silent on LoRa" is the diagnostic a multi-radio node exists to give you, and
 * it only reads at a glance if the missing radio still occupies its space in
 * the row instead of vanishing.
 *
 * The virtual simulator radio is the exception. No real aircraft is ever heard
 * on it and no simulated one is ever heard anywhere else, so it only appears
 * for the peers it actually carries - otherwise every row would wear a
 * permanent, meaningless "not on SIM".
 */
export function RadioChips({ radios, heardOn }) {
  const heard = heardOn || [];
  const shown = (radios || []).filter(r => r.enabled && (!r.sim || heard.indexOf(r.index) >= 0));
  return html`
<span class="inline-flex gap-1">
  ${shown.map(r => {
    const on = heard.indexOf(r.index) >= 0;
    const cls = on
      ? (r.sim ? 'bg-violet-100 text-violet-900 dark:bg-violet-900 dark:text-violet-100' : tipColors.green)
      : 'text-slate-400 dark:text-slate-500 radio-chip-off';
    return html`<span key=${r.index} title=${on ? 'heard on ' + r.name : 'NOT heard on ' + r.name}
      class="inline-block rounded px-1.5 py-0.5 text-xs font-medium ${cls}">${r.name}<//>`;
  })}
<//>`;
}

/**
 * True when a peer is missing from a radio it ought to be audible on.
 * Simulated peers are excluded: they only ever arrive on the virtual radio, so
 * counting them as "not heard on LoRa" would be noise, not a finding.
 */
export function peerPartial(peer, radios) {
  const real = (radios || []).filter(r => r.enabled && !r.sim);
  const heard = peer.radios || [];
  const simOnly = heard.length > 0 && heard.every(i => {
    const r = (radios || []).filter(x => x.index === i)[0];
    return r && r.sim;
  });
  if (simOnly || real.length < 2) return false;
  return real.some(r => heard.indexOf(r.index) < 0);
}

/** One radio's counters. The rejection counters are the interesting half. */
export function RadioCard({ radio }) {
  const r = radio;
  const rejected = (r.rx_crypto_fail || 0) + (r.rx_replay || 0) + (r.rx_decode_fail || 0);
  const state = !r.enabled ? ['disabled', tipColors.gray]
    : r.sim ? ['simulated', 'bg-violet-100 text-violet-900 dark:bg-violet-900 dark:text-violet-100']
      : rejected > 0 ? ['rejecting', tipColors.yellow]
        : ['enabled', tipColors.green];
  const Counter = ({ label, value, tone, tip }) => html`
  <div title=${tip} class="flex flex-col">
    <span class="text-xs uppercase tracking-wide text-gray-400">${label}<//>
    <span class="font-mono text-sm ${tone || 'text-slate-800 dark:text-slate-100'}">${num(value)}<//>
  <//>`;
  return html`
<${Card} title=${r.name} icon=${Icons.antenna}
  right=${html`<${Colored} text=${state[0]} colors=${state[1]} />`}>
  <div class="grid grid-cols-3 gap-3">
    <${Counter} label="TX" value=${r.tx} tip="Frames this node has transmitted on this radio." />
    <${Counter} label="RX ok" value=${r.rx_ok} tone="text-green-600" tip="Frames that decrypted, passed the replay check and decoded." />
    <${Counter} label="Self" value=${r.rx_self} tip="Our own frames heard back - normal on ESP-NOW broadcast, and not an error." />
    <${Counter} label="Crypto fail" value=${r.rx_crypto_fail} tone=${r.rx_crypto_fail ? 'text-red-600' : ''} tip="Authentication tag did not verify: a different group passphrase, corruption, or someone else's traffic." />
    <${Counter} label="Replay" value=${r.rx_replay} tone=${r.rx_replay ? 'text-yellow-600' : ''} tip="Frame counter was not above the last one accepted from that sender - a recorded frame re-injected, or a peer that rebooted inside the 10 s resync window." />
    <${Counter} label="Decode fail" value=${r.rx_decode_fail} tone=${r.rx_decode_fail ? 'text-red-600' : ''} tip="Decrypted cleanly but the payload was not a packet this firmware understands." />
  <//>
  <div class="grid grid-cols-2 gap-3 mt-4 pt-3 border-t border-gray-200 dark:border-slate-700">
    <div class="flex flex-col" title="Signal strength of the most recent frame received on this radio.">
      <span class="text-xs uppercase tracking-wide text-gray-400">Last RSSI<//>
      <span class="font-mono text-sm text-slate-800 dark:text-slate-100">${present(r.last_rssi) && r.last_rssi !== 0 ? r.last_rssi + ' dBm' : DASH}<//>
    <//>
    <div class="flex flex-col" title="Time since the last frame arrived on this radio.">
      <span class="text-xs uppercase tracking-wide text-gray-400">Last RX<//>
      <span class="font-mono text-sm text-slate-800 dark:text-slate-100">${r.last_rx_age_ms ? age(r.last_rx_age_ms) + ' ago' : 'never'}<//>
    <//>
  <//>
  <div class="grid grid-cols-3 gap-3 mt-4 pt-3 border-t border-gray-200 dark:border-slate-700">
    <div class="flex flex-col" title="Current ALOHA beacon interval. It scales with the number of peers this radio can hear, so every node converges on the same value without coordinating.">
      <span class="text-xs uppercase tracking-wide text-gray-400">Beacon<//>
      <span class="font-mono text-sm text-slate-800 dark:text-slate-100">${num(r.beacon_interval_ms, 0, ' ms')}<//>
    <//>
    <div class="flex flex-col" title="Time one beacon occupies the channel at this radio's current mode. It is the input to the rate controller: a slow LoRa mode costs more airtime and therefore backs the beacon off further.">
      <span class="text-xs uppercase tracking-wide text-gray-400">Airtime<//>
      <span class="font-mono text-sm text-slate-800 dark:text-slate-100">${num(r.airtime_ms, 2, ' ms')}<//>
    <//>
    <div class="flex flex-col" title="Peers currently active on this radio specifically.">
      <span class="text-xs uppercase tracking-wide text-gray-400">Peers<//>
      <span class="font-mono text-sm text-slate-800 dark:text-slate-100">${num(r.peers)}<//>
    <//>
  <//>
<//>`;
}

// ---- Frame log ---------------------------------------------------------------

const FRAME_RESULTS = {
  tx: ['TX', 'bg-blue-600 text-white', 'Transmitted by this node.'],
  ok: ['OK', tipColors.green, 'Accepted: tag verified, counter fresh, payload decoded.'],
  self: ['SELF', tipColors.gray, 'Our own frame heard back.'],
  crypto_fail: ['CRYPTO', tipColors.red, 'Authentication tag did not verify - wrong group passphrase, or not our traffic.'],
  replay_fail: ['REPLAY', tipColors.yellow, 'Frame counter was not above the last accepted one from that sender.'],
  decode_fail: ['DECODE', tipColors.red, 'Decrypted but the payload was not a packet we understand.'],
  oversize: ['OVERSIZE', tipColors.yellow, 'Longer than the largest packet this build accepts.'],
};

const FRAME_TYPES = { 0: '?', 1: 'position', 2: 'announce' };

export function FrameLogView({ frames, radios, missed, capacity, total, error }) {
  const radioName = i => {
    const r = (radios || []).filter(x => x.index === i)[0];
    return r ? r.name : '#' + i;
  };
  return html`
<div class="overflow-auto" style="max-height:60vh">
  <table class="min-w-full border-separate border-spacing-0">
    <thead>
      <tr>
        <${Th} title="Age" tip="How long ago the frame was logged, relative to the newest one." />
        <${Th} title="Radio" />
        <${Th} title="Result" />
        <${Th} title="UID" />
        <${Th} title="Type" />
        <${Th} title="Len" />
        <${Th} title="RSSI" />
      </tr>
    </thead>
    <tbody>
      ${error && html`
      <tr><td colspan="7" class="border-b border-slate-200 dark:border-slate-700 px-2 py-2 text-sm text-red-600">${error}</td></tr>`}
      ${!error && missed > 0 && html`
      <tr><td colspan="7" class="border-b border-slate-200 dark:border-slate-700 px-2 py-2 text-sm text-yellow-900 dark:text-yellow-100 bg-yellow-100 dark:bg-yellow-800">
        ${missed} ${missed === 1 ? 'frame' : 'frames'} missed - the ${capacity}-entry log wrapped between polls, so they were overwritten before the UI asked for them.
      </td></tr>`}
      ${(frames || []).map(f => {
        const res = FRAME_RESULTS[f.result] || [f.result, tipColors.gray, ''];
        return html`
        <tr key=${f.seq}>
          <${Td} cls="font-mono text-slate-400"
            text=${!present(f.rel_ms) ? num(f.ms, 0, ' ms') : f.rel_ms === 0 ? 'newest' : '-' + age(f.rel_ms)}
            title=${'device uptime ' + f.ms + ' ms'} />
          <${Td} text=${radioName(f.radio)} />
          <${Td}><${Colored} text=${res[0]} colors=${res[1]} title=${res[2]} /><//>
          <${Td} cls="font-mono" text=${f.uid === '00000000' ? DASH : f.uid} />
          <${Td} cls="text-slate-500 dark:text-slate-400" text=${FRAME_TYPES[f.type] || f.type} />
          <${Td} cls="font-mono" text=${num(f.len, 0, ' B')} />
          <${Td} cls="font-mono" text=${f.rssi ? f.rssi + ' dBm' : DASH} />
        </tr>`;
      })}
      ${!error && (!frames || !frames.length) && html`
      <tr><td colspan="7" class="px-3 py-6 text-sm text-slate-400 text-center">
        Nothing logged yet. ${present(total) ? `${total} frames since boot.` : ''}
      </td></tr>`}
    </tbody>
  </table>
<//>`;
}

// ---- Peers -------------------------------------------------------------------

export function PeerTable({ peers, radios, selfLocation, lockedUid }) {
  const list = peers || [];
  return html`
<div class="overflow-auto" style="max-height:70vh">
  <table class="min-w-full border-separate border-spacing-0">
    <thead>
      <tr>
        <${Th} title="UID" tip="The peer's 32-bit node identity, as hex. Stable across reboots." />
        <${Th} title="Name" />
        <${Th} title="Dist" tip="Ground distance from this node. Absent until both ends have a position." />
        <${Th} title="Brg" tip="True bearing from this node to the peer." />
        <${Th} title="Rel alt" tip="Peer altitude minus ours. Positive means the peer is above." />
        <${Th} title="Speed" />
        <${Th} title="RSSI" />
        <${Th} title="Age" tip="Time since any packet from this peer, on any radio." />
        <${Th} title="Heard on" tip="Which radios this peer has been heard on. A dim chip is a radio that is running but has never heard them." />
        <${Th} title="State" />
      </tr>
    </thead>
    <tbody>
      ${list.map(p => {
        const locked = lockedUid && p.uid === lockedUid;
        const partial = peerPartial(p, radios);
        return html`
        <tr key=${p.uid} class=${locked ? 'bg-blue-50 dark:bg-slate-700' : ''}>
          <${Td} cls="font-mono" text=${p.uid} />
          <${Td} cls="font-medium" text=${p.name || DASH} />
          <${Td} cls="font-mono" text=${present(p.distance_m) ? num(p.distance_m, 1, ' m') : DASH} />
          <${Td} cls="font-mono" text=${present(p.bearing_deg) ? `${Math.round(p.bearing_deg)}° ${compass(p.bearing_deg)}` : DASH} />
          <${Td} cls="font-mono" text=${present(p.rel_alt_m) ? (p.rel_alt_m > 0 ? '+' : '') + num(p.rel_alt_m, 0, ' m') : DASH} />
          <${Td} cls="font-mono" text=${speedMs(p.speed_cms)} />
          <${Td} cls="font-mono" text=${present(p.rssi) && p.rssi !== 0 ? p.rssi + ' dBm' : DASH} />
          <${Td} cls="font-mono ${p.age_ms > 5000 ? 'text-yellow-600' : ''}" text=${age(p.age_ms)} />
          <${Td} title=${partial ? 'Not heard on every enabled radio.' : ''}><${RadioChips} radios=${radios} heardOn=${p.radios} /><//>
          <${Td}>
            <span class="inline-flex gap-1">
              ${!(p.flags & PEER_FLAG_HAS_FIX) && html`<${Colored} text="no fix" colors=${tipColors.yellow} title="Beaconing, but without a GPS fix - Follow will not chase it." />`}
              ${(p.flags & PEER_FLAG_ARMED) ? html`<${Colored} text="armed" colors=${tipColors.red} />` : ''}
              ${locked ? html`<${Colored} text="locked" colors=${tipColors.blue} title="Follow is locked onto this peer." />` : ''}
            <//>
          <//>
        </tr>`;
      })}
      ${!list.length && html`
      <tr><td colspan="10" class="px-3 py-6 text-sm text-slate-400 text-center">
        No peers heard. ${selfLocation && !selfLocation.valid ? 'This node has no position fix either.' : ''}
      </td></tr>`}
    </tbody>
  </table>
<//>`;
}

// ---- Radar scope -------------------------------------------------------------

const RADAR_RANGES = [50, 100, 250, 500, 1000, 2500]; // metres

/**
 * Polar scope of the peer table, centred on this node, north up. Self-contained
 * SVG: no tiles, no library, nothing to fetch, which matters on a node serving
 * its own AP with no internet behind it.
 *
 * It plots distance_m/bearing_deg straight from /api/status rather than
 * recomputing from lat/lon - the firmware already did that geodesy with the same
 * ff::geo code Follow steers with, and doing it twice is how the two disagree.
 */
export function RadarScope({ peers, radios, location, lockedUid }) {
  const [rangeIdx, setRangeIdx] = useState(2);
  const range = RADAR_RANGES[rangeIdx];
  const R = 170, CX = 200, CY = 200;
  const haveLoc = location && location.valid;

  const blips = [];
  let outOfRange = 0;
  if (haveLoc) {
    (peers || []).forEach(p => {
      if (!present(p.distance_m) || !present(p.bearing_deg)) return;
      if (p.distance_m > range) { outOfRange++; return; }
      const rad = (p.bearing_deg - 90) * Math.PI / 180;
      const rr = (p.distance_m / range) * R;
      blips.push({ p, px: CX + rr * Math.cos(rad), py: CY + rr * Math.sin(rad) });
    });
  }

  return html`
<div>
  <div class="flex flex-wrap items-center justify-center gap-1 mb-3">
    ${RADAR_RANGES.map((r, i) => html`<button key=${r} onclick=${() => setRangeIdx(i)}
      class="px-1.5 py-0.5 text-xs font-medium rounded transition-colors ${i === rangeIdx ? 'bg-blue-600 text-white' : 'bg-slate-100 dark:bg-slate-700 text-slate-600 dark:text-slate-300 hover:bg-slate-200'}">${r >= 1000 ? (r / 1000) + 'km' : r + 'm'}<//>`)}
  <//>
  ${!haveLoc ? html`
    <p class="text-xs text-slate-400 py-6 text-center">No position for this node, so there is nothing to plot peers relative to. Connect a GNSS receiver or a flight controller.<//>
  ` : html`
    <svg viewBox="0 0 400 400" class="w-full mx-auto">
      <rect x="0" y="0" width="400" height="400" rx="8" class="fill-slate-50 dark:fill-slate-900" />
      ${[0.25, 0.5, 0.75, 1].map(f => html`<circle key=${'c' + f} cx=${CX} cy=${CY} r=${R * f} fill="none" stroke-width="1" class="stroke-slate-300 dark:stroke-slate-700" />`)}
      ${[0.5, 1].map(f => html`<text key=${'t' + f} x=${CX + 4} y=${CY - R * f + 12} font-size="9" class="fill-slate-400 dark:fill-slate-500">${Math.round(range * f)}m<//>`)}
      <line x1=${CX} y1="22" x2=${CX} y2="378" stroke-width="1" class="stroke-slate-300 dark:stroke-slate-700" />
      <line x1="22" y1=${CY} x2="378" y2=${CY} stroke-width="1" class="stroke-slate-300 dark:stroke-slate-700" />
      <text x=${CX - 4} y="16" font-size="10" class="fill-slate-400 dark:fill-slate-500">N<//>
      <g transform="translate(${CX},${CY})">
        <g transform="rotate(${present(location.course_ddeg) ? location.course_ddeg / 10 : 0})">
          <path d="M0,-7 L5,7 L0,4 L-5,7 Z" class="fill-blue-600" />
        <//>
      <//>
      ${blips.map(({ p, px, py }) => {
        const partial = peerPartial(p, radios);
        const locked = lockedUid && p.uid === lockedUid;
        const fill = locked ? 'fill-blue-600' : partial ? 'fill-yellow-500' : 'fill-slate-700 dark:fill-slate-200';
        const lines = [p.name || p.uid];
        if (present(p.rel_alt_m)) lines.push((p.rel_alt_m > 0 ? '+' : '') + Math.round(p.rel_alt_m) + 'm');
        if (present(p.distance_m)) lines.push(Math.round(p.distance_m) + 'm');
        return html`
        <g key=${p.uid} transform="translate(${px.toFixed(1)},${py.toFixed(1)})">
          <g transform="rotate(${present(p.course_ddeg) ? p.course_ddeg / 10 : 0})"><path d="M0,-6 L4,6 L0,3 L-4,6 Z" class=${fill} /><//>
          <text x="7" y="-1" font-size="8" font-family="monospace">
            ${lines.map((t, i) => html`<tspan key=${i} x="7" dy=${i === 0 ? 0 : 9}
              class=${i === 0 ? 'fill-slate-700 dark:fill-slate-200' : 'fill-slate-500 dark:fill-slate-400'}>${t}<//>`)}
          <//>
        <//>`;
      })}
    <//>
    <div class="flex justify-between text-xs text-slate-400 mt-2">
      <span>${blips.length} in range<//>
      ${outOfRange > 0 && html`<span>${outOfRange} beyond ${range >= 1000 ? (range / 1000) + 'km' : range + 'm'}<//>`}
    <//>
  `}
<//>`;
}
