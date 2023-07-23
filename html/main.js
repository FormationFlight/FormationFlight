'use strict';
import { h, render, useState, useEffect, useRef, html, Router } from  './bundle.js';
import { Icons, Login, Setting, Button, Stat, tipColors, Colored, Notification, Pagination, PeerTable, RadioTable } from './components.js';

//const Logo = props => html`<svg class=${props.class} xmlns="http://www.w3.org/2000/svg" viewBox="0 0 12.87 12.85"><defs><style>.ll-cls-1{fill:none;stroke:#000;stroke-miterlimit:10;stroke-width:0.5px;}</style></defs><g id="Layer_2" data-name="Layer 2"><g id="Layer_1-2" data-name="Layer 1"><path class="ll-cls-1" d="M12.62,1.82V8.91A1.58,1.58,0,0,1,11,10.48H4a1.44,1.44,0,0,1-1-.37A.69.69,0,0,1,2.84,10l-.1-.12a.81.81,0,0,1-.15-.48V5.57a.87.87,0,0,1,.86-.86H4.73V7.28a.86.86,0,0,0,.86.85H9.42a.85.85,0,0,0,.85-.85V3.45A.86.86,0,0,0,10.13,3,.76.76,0,0,0,10,2.84a.29.29,0,0,0-.12-.1,1.49,1.49,0,0,0-1-.37H2.39V1.82A1.57,1.57,0,0,1,4,.25H11A1.57,1.57,0,0,1,12.62,1.82Z"/><path class="ll-cls-1" d="M10.48,10.48V11A1.58,1.58,0,0,1,8.9,12.6H1.82A1.57,1.57,0,0,1,.25,11V3.94A1.57,1.57,0,0,1,1.82,2.37H8.9a1.49,1.49,0,0,1,1,.37l.12.1a.76.76,0,0,1,.11.14.86.86,0,0,1,.14.47V7.28a.85.85,0,0,1-.85.85H8.13V5.57a.86.86,0,0,0-.85-.86H3.45a.87.87,0,0,0-.86.86V9.4a.81.81,0,0,0,.15.48l.1.12a.69.69,0,0,0,.13.11,1.44,1.44,0,0,0,1,.37Z"/></g></g></svg>`;
const Logo = props => html`<img class=${props.class} src="/images/logo.png"></img>`
const ENDPOINT_PREFIX = "http://192.168.4.1"

function Header({id, setShowSidebar, showSidebar}) {
  const [rebootResult, setRebootResult] = useState(null);
  const rebootAction = () => {
    fetch(ENDPOINT_PREFIX + "/system/reboot", { method: "POST" })
      .then((r) => {
        setRebootResult({ status: r.status === 200 });
      });
  }
  return html`
<div class="bg-white sticky top-0 z-[48] xw-full border-b py-2 ${showSidebar && 'pl-72'} transition-all duration-300 transform">
  <div class="px-2 w-full py-0 my-0 flex items-center">
    <button type="button" onclick=${ev => setShowSidebar(v => !v)} class="text-slate-400">
      <${Icons.bars3} class="h-6" />
    <//>
    <div class="flex flex-1 gap-x-4 self-stretch lg:gap-x-6">
      <div class="relative flex flex-1"><//>
      <div class="flex items-center gap-x-4 lg:gap-x-6">
        <span class="text-sm text-slate-400">${id}<//>
        <div class="hidden lg:block lg:h-4 lg:w-px lg:bg-gray-200" aria-hidden="true"><//>
        <${Button} title="Reboot" icon=${Icons.refresh} onclick=${rebootAction} />
        ${rebootResult && html`<${Notification} ok=${rebootResult.status} timeout=${rebootResult.status ? 1500 : 5000}
        text="${rebootResult.status ? 'Rebooting...' : 'Failed to send command'}" close=${() => setRebootResult(null)} />`}
      <//>
    <//>
  <//>
<//>`;
};

function Sidebar({url, show, systemStatus}) {
  const NavLink = ({title, icon, href, url}) => html`
  <div>
    <a href="#${href}" class="${href == url ? 'bg-slate-50 text-blue-600 group' : 'text-gray-700 hover:text-blue-600 hover:bg-gray-50 group'} flex gap-x-3 rounded-md p-2 text-sm leading-6 font-semibold">
      <${icon} class="w-6 h-6"/>
      ${title}
    <///>
  <//>`;
  return html`
<div class="bg-violet-100 hs-overlay hs-overlay-open:translate-x-0
            -translate-x-full transition-all duration-300 transform
            fixed top-0 left-0 bottom-0 z-[60] w-72 bg-white border-r
            border-gray-200 overflow-y-auto scrollbar-y
            ${show && 'translate-x-0'} right-auto bottom-0">
  <div class="flex flex-col m-4 gap-y-6">
    <div class="flex h-10 shrink-0 items-center gap-x-4 font-bold text-xl text-slate-500">
      <${Logo} class="h-full"/> FormationFlight
      <div class="text-xs text-slate-500">
      ${systemStatus.version.match(/v\d/) ? systemStatus.version : "dev"}
      <//>
    <//>

    <div class="flex flex-1 flex-col">
      <${NavLink} title="Dashboard" icon=${Icons.home} href="/" url=${url} />
      <${NavLink} title="Settings" icon=${Icons.settings} href="/settings" url=${url} />
      <${NavLink} title="Update" icon=${Icons.upArrowBox} href="/update" url=${url} />
    <//>
  <//>
<//>`;
};

// .map(v => html` <p class="my-2 text-slate-500">${v}<//>`)
function Note({title = "Note", textBlocks}) {
  return html`
<div class="flex p-4 gap-2">
  <${Icons.info} class="self-start basis-[30px] grow-0 shrink-0 text-green-600" />
  <div class="text-sm">
    <div class="font-semibold mt-1">${title}<//>
    ${textBlocks}
  <//>
<//>`;
};


function Main({}) {
  const [systemStatus, setSystemStatus] = useState(null);
  const [peermanagerStatus, setPeermanagerStatus] = useState(null);
  const [gnssmanagerStatus, setGnssmanagerStatus] = useState(null);
  const [cryptomanagerStatus, setCryptomanagerStatus] = useState(null);
  const [radiomanagerStatus, setRadiomanagerStatus] = useState(null);

  const refresh = () => {
    fetch(ENDPOINT_PREFIX + '/system/status').then(r => r.json()).then(r => setSystemStatus(r));
    fetch(ENDPOINT_PREFIX + '/peermanager/status').then(r => r.json()).then(r => setPeermanagerStatus(r));
    fetch(ENDPOINT_PREFIX + '/gnssmanager/status').then(r => r.json()).then(r => setGnssmanagerStatus(r));
    fetch(ENDPOINT_PREFIX + '/cryptomanager/status').then(r => r.json()).then(r => setCryptomanagerStatus(r));
    fetch(ENDPOINT_PREFIX + '/radiomanager/status').then(r => r.json()).then(r => setRadiomanagerStatus(r));
  } 
  useEffect(() => {
    refresh();
    setInterval(refresh, 5000);
  }, []);

  const ellipsizeString = (str, n) => {
    if (str.length <= n) {
      return str;
    }
  
    const ellipsis = '…';
    const truncatedLength = n - ellipsis.length;
    
    // Return the shortened string with an ellipsis
    return str.substring(0, truncatedLength) + ellipsis;
  }

  const fixType = (index) => {
    return ['No', '2D', '3D'][index];
  }

  if (!systemStatus || !peermanagerStatus || !gnssmanagerStatus || !cryptomanagerStatus || !radiomanagerStatus) return '';
  return html`
<div class="p-2">
  <div class="p-4 sm:p-2 mx-auto grid grid-cols-1 lg:grid-cols-4 gap-4 grid-flow-row">
    <${Stat} title="Flight Controller" text="${systemStatus.host}" tipText="${systemStatus.host == 'NoFC' ? 'bad' : 'good'}" tipIcon=${systemStatus.host == 'NoFC' ? Icons.fail : Icons.ok} tipColors=${systemStatus.host == 'NoFC' ? tipColors.red : tipColors.green} />
    <${Stat} title="GPS Fix" text="${gnssmanagerStatus.numSat} Sats / ${fixType(gnssmanagerStatus.fixType)} Fix" tipText="${gnssmanagerStatus.lat == '0' ? 'warning' : 'good'}" tipIcon=${gnssmanagerStatus.lat == '0' ? Icons.warn : Icons.ok} tipColors=${gnssmanagerStatus.lat == '0' ? tipColors.yellow : tipColors.green} />

    <${Stat} title="GPS Location" text="${gnssmanagerStatus.lat}°N ${gnssmanagerStatus.lon}°W" tipText="${gnssmanagerStatus.lat == '0' ? 'warning' : 'good'}" tipIcon=${gnssmanagerStatus.lat == '0' ? Icons.warn : Icons.ok} tipColors=${gnssmanagerStatus.lat == '0' ? tipColors.yellow : tipColors.green} />
    <${Stat} title="Active Peers" text="${peermanagerStatus.countActive}" tipText="${peermanagerStatus.countActive == 0 ? 'warning' : 'good'}" tipIcon=${peermanagerStatus.countActive == 0 ? Icons.warn : Icons.ok} tipColors=${peermanagerStatus.countActive == 0 ? tipColors.yellow : tipColors.green} />
    <${Stat} title="Encryption" text="${cryptomanagerStatus.enabled ? ellipsizeString(cryptomanagerStatus.keyString, 10) : 'Open'}" tipText="${cryptomanagerStatus.enabled ? 'Encrypted' : 'Open'}" tipIcon=${cryptomanagerStatus.enabled ? Icons.shield : Icons.exclamationTriangle} tipColors=${cryptomanagerStatus.enabled ? tipColors.green : tipColors.yellow} />
  <//>
  <div class="p-4 sm:p-2 mx-auto grid grid-cols-1 lg:grid-cols-4 gap-4 grid-flow-row">
    <div class="bg-white col-span-2 border rounded-md shadow-lg" role="alert">
      <${PeerTable} systemStatus=${systemStatus} data=${peermanagerStatus} />
    <//>
    <div class="bg-white col-span-2 border rounded-md shadow-lg" role="alert">
      <${RadioTable} data=${radiomanagerStatus} />
    <//>
  <//>
<//>`;
};

function Settings({}) {
  const [settings, setSettings] = useState(null);
  const [saveResult, setSaveResult] = useState(null);
  const refresh = () => fetch(ENDPOINT_PREFIX + '/system/status')
    .then(r => r.json())
    .then(r => setSettings(r));
  useEffect(refresh, []);

  const mksetfn = k => (v => setSettings(x => Object.assign({}, x, {[k]: v}))); 
  const onsave = ev => fetch(ENDPOINT_PREFIX + '/system/status', {
    method: 'GET'/*, body: JSON.stringify(settings)*/ 
  }).then(r => r.json())
    .then(r => setSaveResult(r))
    .then(refresh);

  if (!settings) return '';
  const logOptions = [[0, 'Disable'], [1, 'Error'], [2, 'Info'], [3, 'Debug']];
  return html`
<div class="m-4 grid grid-cols-1 gap-4 md:grid-cols-2">

  <div class="py-1 divide-y border rounded bg-white flex flex-col">
    <div class="font-light uppercase flex items-center text-gray-600 px-4 py-2">
      Device Settings
    <//>
    <div class="py-2 px-5 flex-1 flex flex-col relative">
      ${saveResult && html`<${Notification} ok=${saveResult.status}
        text=${saveResult.message} close=${() => setSaveResult(null)} />`}

      <${Setting} title="Enable Logs" value=${settings.log_enabled} setfn=${mksetfn('log_enabled')} type="switch" />
      <${Setting} title="Log Level" value=${settings.log_level} setfn=${mksetfn('log_level')} type="select" addonLeft="0-3" disabled=${!settings.log_enabled} options=${logOptions}/>
      <${Setting} title="Brightness" value=${settings.brightness} setfn=${mksetfn('brightness')} type="number" addonRight="%" />
      <${Setting} title="Device Name" value=${settings.device_name} setfn=${mksetfn('device_name')} type="" />
      <div class="mb-1 mt-3 flex place-content-end"><${Button} icon=${Icons.save} onclick=${onsave} title="Save Settings" /><//>
    <//>
  <//>
<//>`;
};

function Update({}) {
  const [updateFile, setUpdateFile] = useState(null);
  const [uploadResult, setUploadResult] = useState(null);

  const onsubmit = ev => fetch(ENDPOINT_PREFIX + '/update', {
    method: 'POST', body: updateFile
  }).then(r => {
    r.text().then((text) => setUploadResult({ok: r.ok, text: text}));
  });

  const onchange = ev => {
    const data = new FormData();
    data.append('file', ev.target.files[0]);
    setUpdateFile(data);
  };

  const onnotificationclose = () => {
    if (uploadResult.ok) {
      setTimeout(() => window.location = '/', 10000);
    }
    setUploadResult(null);
  }

  return html`
<div class="m-4 grid grid-cols-1 gap-4 md:grid-cols-2">

  <div class="py-1 divide-y border rounded bg-white flex flex-col">
    <div class="font-light uppercase flex items-center text-gray-600 px-4 py-2">
      Firmware Update
    <//>
    <div class="py-2 px-5 flex-1 flex flex-col relative">
      ${uploadResult && html`<${Notification} ok=${uploadResult.ok} timeout=${uploadResult.ok ? 3000 : 10000}
        text=${uploadResult.text} close=${onnotificationclose} />`}

      <${Setting} title="Upload Firmware" type="file" onchange=${onchange} />
      <div class="mb-1 mt-3 flex place-content-end"><${Button} icon=${Icons.upArrowBox} onclick=${onsubmit} title="Upload File" /><//>
    <//>
  <//>

  <div class="bg-white border rounded-md text-ellipsis overflow-auto" role="alert">
    <${Note}
        title="Firmware Downloads"
        textBlocks=${[html`<p class='my-2 text-slate-500'>Visit the <a class='text-blue-600 dark:text-blue-500 hover:underline' href='https://github.com/FormationFlight/FormationFlight/releases/latest'>FormationFlight GitHub</a> for firmware downloads</p>`]} />
  <//>

<//>`;
};

const App = function({}) {
  const [loading, setLoading] = useState(true);
  const [url, setUrl] = useState('/');
  const [systemStatus, setSystemStatus] = useState('');
  const [showSidebar, setShowSidebar] = useState(window.innerWidth > 700);

  useEffect(() => fetch(ENDPOINT_PREFIX + '/system/status').then(r => r.json()).then((r) => {
    setLoading(false);
    setSystemStatus(r);
  }), []);

  if (loading) return '';  // Show blank page on initial load


  return html`
<div class="min-h-screen bg-slate-100">
  <${Sidebar} url=${url} show=${showSidebar} systemStatus=${systemStatus} />
  <${Header} id="${systemStatus.target}/${systemStatus.longName} @ ${systemStatus.version}" showSidebar=${showSidebar} setShowSidebar=${setShowSidebar} />
  <div class="${showSidebar && 'pl-72'} transition-all duration-300 transform">
    <${Router} onChange=${ev => setUrl(ev.url)} history=${History.createHashHistory()} >
      <${Main} default=${true} />
      <${Settings} path="settings" />
      <${Update} path="update" />
    <//>
  <//>
<//>`;
};

window.onload = () => render(h(App), document.body);
