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
if (typeof document !== 'undefined') document.body.textContent = 'GRAPH_TESTS_PASSED';
else console.log('Graph freshness, duplicate polling, gaps, restart and aging passed');
