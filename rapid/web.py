from __future__ import annotations

from contextlib import asynccontextmanager

from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.responses import HTMLResponse

from .bridge import SharedMemoryReceiver
from .config import Settings
from .models import LiveState
from .motec import MotecRecorder
from .power import PowerLimitMonitor
from .service import AccClient
from .storage import Store
from .upload import ArchiveUploader
from .live import LiveTelemetryBroker, TELEMETRY_PAGE

PAGE = '''<!doctype html>
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<title>raPId</title>
<style>
  * { box-sizing: border-box; cursor: none !important; }
  :root { font-family: Arial, sans-serif; color-scheme: dark; }
  html, body { width: 100%; height: 100%; margin: 0; background: #070a0d; color: #f4f7f9; overflow: hidden; }
  .shell { width: 100vw; height: 100vh; padding: 6px; display: grid; grid-template-rows: 28px 1fr 50px; gap: 6px; }
  header { display: flex; align-items: center; justify-content: space-between; min-width: 0; padding: 0 8px; background: #12181e; border-left: 4px solid #f6b91a; }
  #status { font-weight: 700; font-size: 14px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .header-right { display: flex; align-items: center; gap: 6px; flex: 0 0 auto; }
  #sim { font-size: 11px; color: #8d9ba6; }
  #power-status { min-width: 56px; padding: 3px 5px; border: 1px solid #315944; color: #55dd8f; font-size: 9px; font-weight: 800; text-align: center; letter-spacing: .04em; }
  #power-status.warn { border-color: #e6a619; color: #f6b91a; }
  #power-status.limit { border-color: #d53747; color: #ff6472; background: #291317; }
  #log-status { visibility: hidden; padding: 3px 5px; border: 1px solid #315944; color: #aab7c0; font-size: 9px; font-weight: 800; white-space: nowrap; }
  #log-status.visible { visibility: visible; }
  #log-status.warn { border-color: #e6a619; color: #f6b91a; }
  #log-status.error { border-color: #d53747; color: #ff6472; background: #291317; }
  .dash { min-height: 0; display: grid; grid-template-columns: 140px 190px 1fr; gap: 6px; }
  .card { min-width: 0; min-height: 0; background: #10161b; border: 1px solid #27323a; border-radius: 3px; padding: 7px; }
  .label { color: #8d9ba6; font-size: 10px; font-weight: 700; letter-spacing: .08em; }
  .hero { display: grid; grid-template-columns: 58px 1fr; align-items: end; border-bottom: 1px solid #27323a; padding-bottom: 5px; }
  .gear { font-size: 48px; line-height: .9; font-weight: 800; color: #f6b91a; }
  .speed { text-align: right; font-size: 25px; line-height: 1; font-weight: 700; }
  .unit { color: #8d9ba6; font-size: 9px; font-weight: 400; }
  .rpm-row { margin: 8px 0 7px; }
  .row-head { display: flex; justify-content: space-between; align-items: baseline; }
  .rpm-value { font-size: 16px; font-weight: 700; }
  .rpm-track, .pedal-track { position: relative; height: 7px; overflow: hidden; background: #252d33; border-radius: 8px; margin-top: 3px; }
  .fill { width: 0; height: 100%; transition: width 80ms linear; }
  #rpm-fill { background: #f6b91a; }
  .timing { display: grid; gap: 5px; }
  .timing-row { display: flex; justify-content: space-between; font-size: 12px; }
  .timing-row strong { font-variant-numeric: tabular-nums; }
  .controls { display: grid; grid-template-rows: auto auto 1fr; gap: 10px; }
  .pedal-value { font-size: 16px; font-weight: 800; }
  #throttle-fill { background: #20cf75; }
  #brake-fill { background: #ef4458; }
  .steering { align-self: end; display: grid; grid-template-columns: 1fr 88px; align-items: center; padding: 0 7px 3px; }
  #steering-value { font-size: 19px; font-weight: 800; font-variant-numeric: tabular-nums; }
  #steering-wheel { width: 84px; height: 84px; transform: rotate(0deg); transition: transform 80ms linear; transform-origin: center; }
  .wheel-rim { fill: none; stroke: #090b0d; stroke-width: 14; }
  .wheel-suede { fill: none; stroke: #2b3033; stroke-width: 2; stroke-dasharray: 1.5 2; opacity: .8; }
  .wheel-marker { fill: #f3c51b; }
  .wheel-spoke { fill: #171b1e; stroke: #51595e; stroke-width: 1; }
  .wheel-cutout { fill: #080a0b; stroke: #373e42; stroke-width: .8; }
  .wheel-hub { fill: #0c0f11; stroke: #5c6469; stroke-width: 2; }
  .wheel-bolt { fill: #aeb6ba; stroke: #303538; stroke-width: .8; }
  .wheel-logo { fill: #f3c51b; font: 800 7px Arial, sans-serif; letter-spacing: .4px; text-anchor: middle; }
  .g-card { text-align: center; display: flex; flex-direction: column; align-items: center; }
  .g-pad { position: relative; width: 92px; height: 92px; margin: 8px auto 5px; border: 2px solid #3d4b54; border-radius: 50%; background: radial-gradient(circle, #182128 0 4px, transparent 5px); }
  .g-pad::before, .g-pad::after { content: ''; position: absolute; background: #29343b; }
  .g-pad::before { left: 50%; top: 5px; bottom: 5px; width: 1px; }
  .g-pad::after { top: 50%; left: 5px; right: 5px; height: 1px; }
  #g-dot { position: absolute; z-index: 2; left: 50%; top: 50%; width: 12px; height: 12px; border-radius: 50%; background: #34bdf2; box-shadow: 0 0 7px #34bdf2; transform: translate(-50%, -50%); transition: left 80ms linear, top 80ms linear; }
  #g-value { font-size: 18px; font-weight: 800; }
  .lap-count { margin-top: auto; width: 100%; border-top: 1px solid #27323a; padding-top: 8px; font-size: 12px; }
  .lap-count strong { color: #f6b91a; font-size: 18px; }
  .page { display: none; min-height: 0; }
  .page.active { display: grid; }
  .touch-nav { display: grid; grid-template-columns: repeat(4, 1fr); gap: 5px; }
  .touch-nav button { border: 1px solid #27323a; border-radius: 3px; background: #10161b; color: #aab7c0; font-size: 13px; font-weight: 800; letter-spacing: .05em; touch-action: manipulation; -webkit-tap-highlight-color: transparent; }
  .touch-nav button.active { border-color: #f6b91a; color: #f6b91a; background: #221c0d; }
  .telemetry-page { grid-template-columns: repeat(3, 1fr); gap: 6px; }
  .telemetry-page .card { display: grid; grid-template-rows: auto 1fr; overflow: hidden; }
  .value-grid { display: grid; grid-template-columns: repeat(2, 1fr); gap: 3px; align-content: center; }
  .metric { border-left: 3px solid #27323a; padding: 2px 4px; background: #0c1115; }
  .metric strong { display: block; margin-top: 2px; font-size: 14px; font-variant-numeric: tabular-nums; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .tyre-grid { display: grid; grid-template-columns: repeat(2, 1fr); gap: 6px; align-content: center; }
  .tyre { padding: 7px; background: #0c1115; border-top: 3px solid #27323a; }
  .tyre strong { display: block; margin-top: 4px; font-size: 16px; }
</style>
<main class="shell">
  <header><div id="status">Waiting for daemon connection…</div><div class="header-right"><div id="sim">raPId</div><div id="log-status" role="status" aria-live="polite"></div><div id="power-status">PWR …</div></div></header>
  <section id="drive" class="dash page active">
    <div class="card">
      <div class="hero">
        <div><div class="label">GEAR</div><div class="gear" id="gear">—</div></div>
        <div><div class="speed" id="speed">—</div><div class="unit">KM/H</div></div>
      </div>
      <div class="rpm-row">
        <div class="row-head"><span class="label">RPM</span><span class="rpm-value" id="rpm">—</span></div>
        <div class="rpm-track"><div class="fill" id="rpm-fill"></div></div>
      </div>
      <div class="timing">
        <div class="timing-row"><span class="label">LAP</span><strong id="lap">—</strong></div>
        <div class="timing-row"><span class="label">LAST</span><strong id="last">—</strong></div>
        <div class="timing-row"><span class="label">DELTA</span><strong id="delta">—</strong></div>
      </div>
    </div>
    <div class="card controls">
      <div>
        <div class="row-head"><span class="label">THROTTLE</span><span class="pedal-value" id="throttle-value">—</span></div>
        <div class="pedal-track"><div class="fill" id="throttle-fill"></div></div>
      </div>
      <div>
        <div class="row-head"><span class="label">BRAKE</span><span class="pedal-value" id="brake-value">—</span></div>
        <div class="pedal-track"><div class="fill" id="brake-fill"></div></div>
      </div>
      <div class="steering">
        <div><div class="label">STEERING</div><div id="steering-value">—</div></div>
        <svg id="steering-wheel" viewBox="0 0 100 100" aria-label="Steering wheel">
          <circle class="wheel-rim" cx="50" cy="50" r="40"></circle>
          <circle class="wheel-suede" cx="50" cy="50" r="40"></circle>
          <path class="wheel-marker" d="M45.5 4.5 H54.5 L55.5 16 H44.5 Z"></path>
          <path class="wheel-spoke" d="M40 45 L15 36 L12 46 L39 55 Z"></path>
          <path class="wheel-spoke" d="M60 45 L85 36 L88 46 L61 55 Z"></path>
          <path class="wheel-spoke" d="M43 59 L38 85 L49 90 L50 62 Z"></path>
          <path class="wheel-spoke" d="M57 59 L62 85 L51 90 L50 62 Z"></path>
          <ellipse class="wheel-cutout" cx="26" cy="44" rx="7" ry="2.5" transform="rotate(17 26 44)"></ellipse>
          <ellipse class="wheel-cutout" cx="74" cy="44" rx="7" ry="2.5" transform="rotate(-17 74 44)"></ellipse>
          <ellipse class="wheel-cutout" cx="43" cy="75" rx="2.5" ry="7" transform="rotate(8 43 75)"></ellipse>
          <ellipse class="wheel-cutout" cx="57" cy="75" rx="2.5" ry="7" transform="rotate(-8 57 75)"></ellipse>
          <circle class="wheel-hub" cx="50" cy="53" r="14"></circle>
          <circle class="wheel-bolt" cx="50" cy="42" r="2"></circle><circle class="wheel-bolt" cx="59.5" cy="47.5" r="2"></circle>
          <circle class="wheel-bolt" cx="59.5" cy="58.5" r="2"></circle><circle class="wheel-bolt" cx="50" cy="64" r="2"></circle>
          <circle class="wheel-bolt" cx="40.5" cy="58.5" r="2"></circle><circle class="wheel-bolt" cx="40.5" cy="47.5" r="2"></circle>
          <text class="wheel-logo" x="50" y="55.5">MOMO</text>
        </svg>
      </div>
    </div>
    <div class="card g-card">
      <div class="label">G FORCE</div>
      <div class="g-pad"><div id="g-dot"></div></div>
      <div id="g-value">—</div><div class="unit">LATERAL / LONG.</div>
      <div class="lap-count"><span class="label">LAP </span><strong id="lap-number">—</strong></div>
    </div>
  </section>
  <section id="timing" class="telemetry-page page">
    <div class="card"><span class="label">LIVE TIMING</span><div class="value-grid"><div class="metric"><span class="label">CURRENT</span><strong id="timing-lap">â€”</strong></div><div class="metric"><span class="label">LAST</span><strong id="timing-last">â€”</strong></div><div class="metric"><span class="label">BEST</span><strong id="timing-best">â€”</strong></div><div class="metric"><span class="label">DELTA</span><strong id="timing-delta">â€”</strong></div><div class="metric"><span class="label">S1</span><strong id="sector-1">â€”</strong></div><div class="metric"><span class="label">S2</span><strong id="sector-2">â€”</strong></div><div class="metric"><span class="label">S3</span><strong id="sector-3">â€”</strong></div><div class="metric"><span class="label">LAP</span><strong id="timing-number">â€”</strong></div></div></div>
    <div class="card"><span class="label">SESSION</span><div class="value-grid"><div class="metric"><span class="label">TRACK</span><strong id="track-name">â€”</strong></div><div class="metric"><span class="label">CAR</span><strong id="car-name">â€”</strong></div><div class="metric"><span class="label">DRIVER</span><strong id="driver-name">â€”</strong></div><div class="metric"><span class="label">POSITION</span><strong id="lap-position">â€”</strong></div></div></div>
    <div class="card"><span class="label">RECORDING</span><div class="value-grid"><div class="metric"><span class="label">STATE</span><strong id="recording-state">â€”</strong></div><div class="metric"><span class="label">SAMPLES</span><strong id="sample-count">â€”</strong></div></div></div>
  </section>
  <section id="vehicle" class="telemetry-page page">
    <div class="card"><span class="label">CAR STATE</span><div class="value-grid"><div class="metric"><span class="label">FUEL</span><strong id="fuel">â€”</strong></div><div class="metric"><span class="label">TC</span><strong id="tc">â€”</strong></div><div class="metric"><span class="label">ABS</span><strong id="abs">â€”</strong></div><div class="metric"><span class="label">LIMITER</span><strong id="limiter">â€”</strong></div></div></div>
    <div class="card"><span class="label">WHEEL SPEED</span><div class="value-grid"><div class="metric"><span class="label">FL / FR</span><strong id="wheel-front">â€”</strong></div><div class="metric"><span class="label">RL / RR</span><strong id="wheel-rear">â€”</strong></div></div></div>
    <div class="card"><span class="label">DAMAGE</span><div class="value-grid"><div class="metric"><span class="label">FRONT</span><strong id="damage-front">â€”</strong></div><div class="metric"><span class="label">REAR</span><strong id="damage-rear">â€”</strong></div></div></div>
  </section>
  <section id="tyres" class="telemetry-page page">
    <div class="card"><span class="label">TYRE CORE / PRESSURE</span><div class="tyre-grid"><div class="tyre"><span class="label">FL</span><strong id="tyre-fl">â€”</strong></div><div class="tyre"><span class="label">FR</span><strong id="tyre-fr">â€”</strong></div><div class="tyre"><span class="label">RL</span><strong id="tyre-rl">â€”</strong></div><div class="tyre"><span class="label">RR</span><strong id="tyre-rr">â€”</strong></div></div></div>
    <div class="card"><span class="label">SUSPENSION</span><div class="value-grid"><div class="metric"><span class="label">FRONT</span><strong id="suspension-front">â€”</strong></div><div class="metric"><span class="label">REAR</span><strong id="suspension-rear">â€”</strong></div></div></div>
    <div class="card"><span class="label">STATUS</span><div class="value-grid"><div class="metric"><span class="label">SOURCE</span><strong id="tyre-source">LIVE</strong></div></div></div>
  </section>
  <nav class="touch-nav" aria-label="Dashboard pages"><button class="active" data-page="drive">DRIVE</button><button data-page="timing">TIMING</button><button data-page="vehicle">VEHICLE</button><button data-page="tyres">TYRES</button></nav>
</main>
<script>
const fields = {};
const placeholderText = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT);
for (let node = placeholderText.nextNode(); node; node = placeholderText.nextNode()) node.nodeValue = node.nodeValue.replaceAll('â€”', '--').replaceAll('â€¦', '...');
for (const id of ['status', 'sim', 'gear', 'rpm', 'rpm-fill', 'speed', 'lap', 'last', 'delta',
  'throttle-value', 'throttle-fill', 'brake-value', 'brake-fill', 'steering-value',
  'steering-wheel', 'g-dot', 'g-value', 'lap-number', 'power-status', 'log-status']) {
  fields[id] = document.getElementById(id);
}
const clamp = (value, minimum, maximum) => Math.max(minimum, Math.min(maximum, value));
const formatTime = milliseconds => milliseconds == null ? '—' :
  `${Math.floor(milliseconds / 60000)}:${String(Math.floor(milliseconds / 1000) % 60).padStart(2, '0')}.${String(milliseconds % 1000).padStart(3, '0')}`;

const touchPages = [...document.querySelectorAll('.page')];
const touchButtons = [...document.querySelectorAll('.touch-nav button')];
function showPage(name) { touchPages.forEach(page => page.classList.toggle('active', page.id === name)); touchButtons.forEach(button => button.classList.toggle('active', button.dataset.page === name)); }
touchButtons.forEach(button => button.addEventListener('pointerdown', event => { event.preventDefault(); showPage(button.dataset.page); }));
const setMetric = (id, value) => { const node = document.getElementById(id); if (node) node.textContent = value == null ? '--' : value; };
const pair = (left, right, unit = '') => left == null || right == null ? '--' : `${Math.round(left)} / ${Math.round(right)}${unit}`;

let lastLogSequence = null;
let logNoticeTimer;
function updateLogNotice(state) {
  const sequence = state.log_sequence || 0;
  if (lastLogSequence !== null && sequence > lastLogSequence) {
    const node = fields['log-status'];
    node.textContent = 'Log updated';
    node.className = `visible ${state.log_severity || 'info'}`;
    clearTimeout(logNoticeTimer);
    logNoticeTimer = setTimeout(() => { node.className = ''; node.textContent = ''; }, 3000);
  }
  lastLogSequence = sequence;
}

async function tick() {
  try {
    const response = await fetch('/api/live', {cache: 'no-store'});
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const state = await response.json();
    fields.status.textContent = !state.companion_connected
      ? 'Waiting for daemon connection…'
      : state.companion_daemon_state === 'driving'
        ? `${state.simulator || 'Simulator'} connected`
        : state.simulator
          ? `${state.simulator} detected — waiting for driving`
          : 'Daemon connected — waiting for simulator';
    fields.sim.textContent = state.simulator || 'raPId';
    const power = fields['power-status'];
    power.className = state.power_limited ? 'limit' : state.power_limited_since_boot ? 'warn' : '';
    power.textContent = !state.power_status_available ? 'PWR ?' :
      state.power_limited ? 'PWR LIMIT' : state.power_limited_since_boot ? 'PWR WARN' : 'PWR OK';
    fields.gear.textContent = state.gear === -1 ? 'R' : state.gear === 0 ? 'N' : state.gear ?? '—';
    fields.rpm.textContent = state.rpm ?? '—';
    fields['rpm-fill'].style.width = `${clamp((state.rpm || 0) / 100, 0, 100)}%`;
    fields.speed.textContent = state.speed_kmh == null ? '—' : Math.round(state.speed_kmh);
    fields.lap.textContent = formatTime(state.current_lap_ms);
    fields.last.textContent = formatTime(state.completed_lap_ms);
    fields.delta.textContent = state.delta_ms == null ? '—' :
      `${state.delta_ms > 0 ? '+' : ''}${formatTime(Math.abs(state.delta_ms))}`;
    const throttle = clamp(Number(state.throttle) || 0, 0, 1);
    const brake = clamp(Number(state.brake) || 0, 0, 1);
    fields['throttle-value'].textContent = state.throttle == null ? '—' : `${Math.round(throttle * 100)}%`;
    fields['throttle-fill'].style.width = `${throttle * 100}%`;
    fields['brake-value'].textContent = state.brake == null ? '—' : `${Math.round(brake * 100)}%`;
    fields['brake-fill'].style.width = `${brake * 100}%`;
    const steering = Number(state.steering_angle) || 0;
    fields['steering-value'].textContent = state.steering_angle == null ? '—' : `${steering >= 0 ? '+' : ''}${Math.round(steering * 180 / Math.PI)}°`;
    fields['steering-wheel'].style.transform = `rotate(${steering * 180 / Math.PI}deg)`;
    const gx = Number(state.g_x) || 0;
    const gz = Number(state.g_z) || 0;
    fields['g-dot'].style.left = `${50 + clamp(gx / 2, -1, 1) * 42}%`;
    fields['g-dot'].style.top = `${50 + clamp(gz / 2, -1, 1) * 42}%`;
    fields['g-value'].textContent = state.g_x == null ? '—' : `${Math.hypot(gx, gz).toFixed(2)}g`;
    fields['lap-number'].textContent = state.lap_number ?? '—';
    setMetric('timing-lap', formatTime(state.current_lap_ms)); setMetric('timing-last', formatTime(state.completed_lap_ms));
    setMetric('timing-best', formatTime(state.best_lap_ms));
    setMetric('timing-delta', state.delta_ms == null ? null : `${state.delta_ms > 0 ? '+' : ''}${formatTime(Math.abs(state.delta_ms))}`);
    setMetric('timing-number', state.lap_number); setMetric('track-name', state.track_name); setMetric('car-name', state.car_model); setMetric('driver-name', state.driver_name);
    const sector = number => { const time = state[`sector_${number}_ms`], delta = state[`sector_${number}_delta_ms`]; return time == null ? null : `${formatTime(time)}${delta == null ? '' : ` (${delta > 0 ? '+' : ''}${formatTime(Math.abs(delta))})`}`; };
    setMetric('sector-1', sector(1)); setMetric('sector-2', sector(2)); setMetric('sector-3', sector(3));
    setMetric('lap-position', state.lap_position == null ? null : `${Math.round((state.lap_position > 1 ? state.lap_position / 100 : state.lap_position) * 100)}%`);
    setMetric('recording-state', state.recording ? 'REC' : 'IDLE'); setMetric('sample-count', state.recorded_samples);
    setMetric('fuel', state.fuel == null ? null : `${state.fuel.toFixed(1)} L`); setMetric('tc', state.tc); setMetric('abs', state.abs_activity == null ? null : `${Math.round(state.abs_activity * 100)}%`); setMetric('limiter', state.pit_limiter == null ? null : state.pit_limiter ? 'ON' : 'OFF');
    setMetric('wheel-front', pair(state.wheel_speed_fl, state.wheel_speed_fr, ' km/h')); setMetric('wheel-rear', pair(state.wheel_speed_rl, state.wheel_speed_rr, ' km/h'));
    setMetric('damage-front', state.damage_front == null ? null : `${Math.round(state.damage_front * 100)}%`); setMetric('damage-rear', state.damage_rear == null ? null : `${Math.round(state.damage_rear * 100)}%`);
    const tyre = corner => state[`core_temp_${corner}`] == null || state[`pressure_${corner}`] == null ? null : `${Math.round(state[`core_temp_${corner}`])} C ${state[`pressure_${corner}`].toFixed(1)}`;
    setMetric('tyre-fl', tyre('fl')); setMetric('tyre-fr', tyre('fr')); setMetric('tyre-rl', tyre('rl')); setMetric('tyre-rr', tyre('rr'));
    setMetric('suspension-front', pair(state.suspension_fl, state.suspension_fr)); setMetric('suspension-rear', pair(state.suspension_rl, state.suspension_rr));
  } catch (error) {
    fields.status.textContent = 'Dashboard connection lost';
  }
}
tick();
setInterval(tick, 200);
async function pollLogStatus() {
  try {
    const endpoint = new URL('/api/log-status', window.location.href);
    endpoint.port = '8001';
    const response = await fetch(endpoint, {cache: 'no-store', signal: AbortSignal.timeout(2000)});
    if (response.ok) updateLogNotice(await response.json());
  } catch (_) { /* Telemetry remains usable if the log service is restarting. */ }
  setTimeout(pollLogStatus, 1000);
}
pollLogStatus();
</script>'''


def create_app(settings: Settings) -> FastAPI:
    state, store = LiveState(), Store(settings.database_path)
    client = AccClient(settings, state, store)
    uploader = ArchiveUploader(settings.upload_url, settings.upload_token, settings.upload_queue_path, state)
    recorder = MotecRecorder(settings.telemetry_directory, on_bundle_finalized=uploader.enqueue)
    broker = LiveTelemetryBroker()
    companion = SharedMemoryReceiver(settings.companion_host, settings.companion_port, state, recorder, broker)
    power = PowerLimitMonitor(state)

    @asynccontextmanager
    async def lifespan(_: FastAPI):
        uploader.start()
        client.start()
        companion.start()
        power.start()
        yield
        power.stop()
        companion.stop()
        client.stop()
        store.close()
        uploader.stop()

    app = FastAPI(title="raPId", lifespan=lifespan)

    @app.get("/", response_class=HTMLResponse)
    def dashboard():
        return HTMLResponse(PAGE, headers={"Cache-Control": "no-store, max-age=0"})

    @app.get("/api/live")
    def live():
        return state.snapshot()

    @app.get("/telemetry", response_class=HTMLResponse)
    def telemetry_page():
        return HTMLResponse(TELEMETRY_PAGE, headers={"Cache-Control": "no-store, max-age=0"})

    @app.websocket("/api/v1/live")
    async def telemetry_socket(websocket: WebSocket):
        await websocket.accept()
        try:
            cursor = broker.history_cursor(int(websocket.query_params.get("history", "30")))
            while True:
                cursor, events, dropped = broker.read_after(cursor)
                if events or dropped:
                    await websocket.send_json({"events": events, "dropped": dropped})
                import asyncio
                await asyncio.sleep(0.1)
        except WebSocketDisconnect:
            return

    @app.get("/api/v1/status")
    def recorder_status():
        return {**state.snapshot(), **recorder.status(), "upload_enabled": uploader.enabled}

    @app.put("/api/v1/session/upload")
    def set_session_upload(payload: dict):
        enabled = bool(payload.get("enabled", False))
        recorder.set_upload_after_session(enabled)
        state.update(upload_after_session=enabled)
        return {"enabled": enabled}

    return app
