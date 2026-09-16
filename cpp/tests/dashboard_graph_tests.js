// Run with Node in CI, or supply dashboardSource when embedding in a browser.
const embedded = typeof dashboardSource === 'string';
const fs = embedded ? null : require('fs');
const path = embedded ? null : require('path');
const source = embedded ? dashboardSource :
  fs.readFileSync(path.join(__dirname, '../assets/dashboard.html'), 'utf8');
const qmlSource = embedded ? null : fs.readFileSync(path.join(__dirname, '../display/Main.qml'), 'utf8');
const wheelPair = /pair\(state\.wheel_speed_fl, state\.wheel_speed_fr, '([^']+)'\)/.exec(source);
if (!wheelPair || wheelPair[1] !== ' rad/s' ||
    source.includes("wheel_speed_fl, state.wheel_speed_fr, ' km/h'") ||
    (!embedded && (!qmlSource.includes('root.pair("wheel_speed_fl","wheel_speed_fr"," rad/s")') ||
                   qmlSource.includes('root.pair("wheel_speed_fl","wheel_speed_fr"," km/h")')))) {
  throw new Error('Wheel angular-speed displays must use rad/s');
}
const sampleFunction = source.match(/function addGraphSample\(state\) \{[\s\S]*?\n\}/);
if (!sampleFunction) throw new Error('Dashboard graph function missing');
const run = new Function(`
  let clock = 1000;
  const performance = {now: () => clock};
  const graphHistory = [], graphWindowMs = 30000;
  let lastGraphSample = null;
  const clamp = (x, lo, hi) => Math.max(lo, Math.min(hi, x));
  const setMetric = () => {}, redrawGraphs = () => {};
  ${sampleFunction[0]}
  const check = (ok, message) => { if (!ok) throw new Error(message); };
  const state = {companion_connected: true, companion_daemon_state: 'driving',
    telemetry_fresh: true, telemetry_age_ms: 100, session_id: 'run-a',
    samples_received: 1, throttle: .8, brake: .2, g_x: 1, g_z: -.5};
  addGraphSample(state);
  check(graphHistory.length === 1 && graphHistory[0].time === 900,
    'Use receipt time rather than poll time');
  clock += 200;
  addGraphSample(state);
  check(graphHistory.length === 1, 'Do not append repeated telemetry');
  clock += 800;
  addGraphSample({...state, samples_received: 2, telemetry_age_ms: 0});
  check(graphHistory.length === 2 && graphHistory[1].throttle === 80,
    'Slow but fresh telemetry still produces a continuous trace');
  clock += 1600;
  addGraphSample({...state, samples_received: 2, telemetry_fresh: false});
  check(graphHistory.at(-1).throttle === null, 'Heartbeat-only periods are gaps');
  addGraphSample({});
  check(graphHistory.at(-1).lateral === null, 'Fetch failure is a gap');
  addGraphSample({...state, session_id: 'run-b'});
  check(graphHistory.at(-1).throttle === 80, 'Receiver/session reset can resume');
  clock += 31000;
  addGraphSample({});
  check(graphHistory.length === 1 && graphHistory[0].throttle === null,
    'Inactive history continues aging');
`);
run();

// Live push (#17): pushed frames are coalesced to only the newest before
// being rendered, and the HTTP poll is the fallback used only while no push
// is active.
const pushFunctions = source.match(
  /function queuePush\(raw\) \{[\s\S]*?\n\}\nfunction flushPending\(\) \{[\s\S]*?\n\}/);
if (!pushFunctions) throw new Error('Live push coalescing functions missing');
const runPush = new Function('check', `
  let livePushActive = false, pendingState = null;
  const applied = [];
  const applyState = state => applied.push(state);
  ${pushFunctions[0]}
  check(livePushActive === false, 'Push starts inactive until the socket delivers a frame');
  queuePush(JSON.stringify({rpm: 1000}));
  check(livePushActive === true, 'A pushed frame marks the live socket active');
  queuePush(JSON.stringify({rpm: 2000}));
  queuePush(JSON.stringify({rpm: 3000}));
  check(applied.length === 0, 'Pushed frames wait for the next render frame');
  flushPending();
  check(applied.length === 1 && applied[0].rpm === 3000,
    'Several pushes before one frame coalesce to only the newest');
  flushPending();
  check(applied.length === 1, 'A frame with no new push renders nothing extra');
  queuePush('not valid json');
  check(pendingState === null, 'A malformed push frame is dropped, not queued or thrown');
`);
runPush((ok, message) => { if (!ok) throw new Error(message); });

const tickFunction = source.match(/async function tick\(\) \{[\s\S]*?\n\}/);
if (!tickFunction) throw new Error('Dashboard fallback poll function missing');
function runTick(livePushActiveInitial, fetchImpl) {
  const calls = {fetch: 0, applyState: [], applyFailure: 0};
  const harness = new Function('livePushActiveInitial', 'fetchImpl', 'calls', `
    return (async () => {
      let livePushActive = livePushActiveInitial;
      const applyState = state => calls.applyState.push(state);
      const applyFailure = () => { calls.applyFailure += 1; };
      const setTimeout = () => {};
      const AbortSignal = {timeout: () => undefined};
      const fetch = async (...args) => { calls.fetch += 1; return fetchImpl(...args); };
      ${tickFunction[0]}
      await tick();
    })();
  `);
  return harness(livePushActiveInitial, fetchImpl, calls).then(() => calls);
}
(async () => {
  const check = (ok, message) => { if (!ok) throw new Error(message); };
  const polled = await runTick(false, async () => ({ok: true, json: async () => ({rpm: 42})}));
  check(polled.fetch === 1 && polled.applyState.length === 1 && polled.applyState[0].rpm === 42,
    'Fallback polling fetches and applies state while the push socket is down');
  const suppressed = await runTick(true, async () => ({ok: true, json: async () => ({rpm: 42})}));
  check(suppressed.fetch === 0,
    'Fallback polling is suppressed while the live push socket is active');
  const failed = await runTick(false, async () => { throw new Error('offline'); });
  check(failed.applyFailure === 1 && failed.applyState.length === 0,
    'A failed fallback fetch reports a connection failure rather than a partial state');
  if (typeof document !== 'undefined') document.body.textContent = 'GRAPH_TESTS_PASSED';
  else console.log('Graph freshness, duplicate polling, gaps, restart, aging, push coalescing and fallback-poll checks passed');
})().catch(error => {
  if (typeof document !== 'undefined') document.body.textContent = 'GRAPH_TESTS_FAILED: ' + error.message;
  else { console.error(error); process.exitCode = 1; }
});
