// Execute the shipped page script and WebSocket handler, not a copied reducer.
// DOM/canvas/WebSocket are deterministic Node stand-ins; this is not visual QA.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const {test} = require('node:test');
const html = fs.readFileSync(path.join(__dirname, '../telemetry.html'), 'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];

test('Verify runs the viewer handler regression suite', () => {
  const workflow = fs.readFileSync(path.join(__dirname, '../../../.github/workflows/verify.yml'), 'utf8');
  assert.match(workflow, /^\s*- run: node --test cpp\/assets\/tests\/telemetry_history_tests\.js\s*$/m);
});

function page() {
  const elements = new Map(), sockets = [], timers = [], paths = [];
  let currentPath;
  const canvas = {
    scale() {}, fillRect() {}, stroke() {},
    beginPath() {currentPath = []; paths.push(currentPath);},
    moveTo(x, y) {currentPath.push(['move', x, y]);},
    lineTo(x, y) {currentPath.push(['line', x, y]);}
  };
  function element() {
    return {value: '', checked: false, textContent: '', innerHTML: '',
      classList: {toggle() {}}, append() {}, add() {},
      getBoundingClientRect: () => ({width: 900, height: 500}),
      getContext: () => canvas};
  }
  const document = {
    getElementById(id) {
      if (!elements.has(id)) elements.set(id, element());
      return elements.get(id);
    },
    createElement: element, createTextNode: text => text
  };
  document.getElementById('window').value = '30';
  const context = vm.createContext({document, console, devicePixelRatio: 1,
    location: {protocol: 'http:', host: 'test.invalid'},
    Option: function() {}, addEventListener() {}, setInterval() {},
    setTimeout(fn) {timers.push(fn);},
    fetch: async () => ({ok: true, json: async () => ({})}),
    WebSocket: class {constructor() {sockets.push(this);}}
  });
  vm.runInContext(script, context, {filename: 'telemetry.html'});
  return {
    send(events, dropped = 0, receiver_stream_id) {
      paths.length = 0;
      sockets.at(-1).onmessage({data: JSON.stringify({events, dropped, receiver_stream_id})});
    },
    sockets,
    samples: () => JSON.parse(vm.runInContext('JSON.stringify(samples)', context)),
    paths: () => paths,
    pause: () => elements.get('pause').onclick(),
    window(seconds) {
      elements.get('window').value = String(seconds);
      elements.get('window').onchange?.();
    },
    reconnect() {sockets.at(-1).onclose(); timers.shift()(); sockets.at(-1).onopen();},
    connection: () => elements.get('connection').textContent,
    evaluate: source => vm.runInContext(source, context)
  };
}
const epochA = 'a'.repeat(32), epochB = 'b'.repeat(32);

test('receiver reboot resets history and axis only on changed server stream identity', () => {
  const p = page();
  p.send([sample(1000, 0), sample(1001, 1)], 0, epochA);
  p.reconnect();
  p.send([sample(1, 0), sample(2, 1)], 0, epochB);
  assert.deepEqual(p.samples().map(s => s.received_monotonic), [1, 2]);
  assert.equal(p.evaluate('lastReceived'), 2);
  assert.equal(p.evaluate('sampleKeys.size'), 2);
  assert.deepEqual(p.paths().at(-1).map(point => point[1]), [870, 900]);
  for (let i = 3; i <= 40; i++) p.send([sample(i, i)], 0, epochB);
  assert.equal(p.samples().length, 31);
  assert.equal(p.samples()[0].received_monotonic, 10);
  assert.equal(p.evaluate('lastReceived'), 40);
});

test('broker event identity keeps coincident sparse samples distinct and replay idempotent', () => {
  const p = page();
  const first = {...sample(100, 1), event_id: '1'};
  const second = {...sample(100, 2), event_id: '2', values: {throttle: .9}};
  delete first.sequence; delete second.sequence;
  p.send([first, second], 0, epochA);
  assert.equal(p.samples().length, 2);
  p.reconnect();
  p.send([first, second], 0, epochA);
  assert.equal(p.samples().length, 2);
  assert.equal(p.evaluate('sampleKeys.size'), 2);
  const identical = sample(101, 3);
  delete identical.sequence;
  const twins = ['9007199254740992', '9007199254740993'].map(event_id => ({...identical, event_id}));
  p.send(twins, 0, epochA); p.send(twins, 0, epochA);
  assert.equal(p.samples().length, 4, 'decimal IDs are not rounded or derived from sample contents');
  p.send([{...first, event_id: '1'}], 0, epochB);
  assert.equal(p.samples().length, 1, 'event counter is scoped to its broker');
});

test('superseded socket cannot restore an old receiver epoch or schedule another reconnect', () => {
  const p = page();
  p.send([sample(1000, 0)], 0, epochA);
  const old = p.sockets.at(-1);
  p.reconnect();
  p.send([sample(1, 0), sample(2, 1)], 0, epochB);
  old.onmessage({data: JSON.stringify({receiver_stream_id: epochA, events: [sample(1001, 1)]})});
  assert.deepEqual(p.samples().map(s => s.received_monotonic), [1, 2]);
  assert.equal(p.evaluate('receiverStream'), epochB);
  old.onclose();
  assert.notEqual(p.connection(), 'reconnecting…');
});

test('same receiver epoch replays out of order without clearing or rewinding', () => {
  const p = page();
  const first = {...sample(100, 1), event_id: '1'};
  const last = {...sample(102, 3), event_id: '3'};
  p.send([first, last], 0, epochA);
  p.reconnect();
  p.send([{...sample(101, 2, 'another-session'), event_id: '2'}, first], 0, epochA);
  assert.deepEqual(p.samples().map(s => s.received_monotonic), [100, 101, 102]);
  assert.equal(p.evaluate('lastReceived'), 102);
  for (const id of [undefined, null, '', 'not-an-epoch', 123]) {
    p.send([sample(99, 0, 'older-session')], 0, id);
    assert.equal(p.evaluate('receiverStream'), epochA);
    assert.equal(p.evaluate('lastReceived'), 102);
    assert.equal(p.samples().length, 4);
  }
});

test('unidentified packets do not borrow cached broker identity', () => {
  for (const missingStream of [undefined, null, '', 'invalid-stream', 123]) {
    const p = page();
    const identified = {...sample(100, 1), event_id: '1'};
    const legacy = {...sample(101, 2, 'legacy'), event_id: '1'};
    p.send([identified], 0, epochA);
    p.send([legacy], 0, missingStream);
    assert.equal(p.samples().length, 2, 'a missing packet stream uses legacy identity');
    assert.equal(p.evaluate('lastReceived'), 101);
    p.send([legacy], 0, missingStream);
    p.send([identified], 0, epochA);
    assert.equal(p.samples().length, 2, 'each identity still deduplicates its replay');
    p.send([{...sample(132, 3), event_id: '2'}], 0, epochA);
    assert.equal(p.samples().length, 1);
    assert.equal(p.evaluate('sampleKeys.size'), 1, 'eviction removes the originally chosen keys');
    p.send([legacy], 0, missingStream);
    assert.equal(p.evaluate('sampleKeys.size'), 1, 'expired legacy replay leaves no stale key');
  }
});

test('pause freezes epoch too; resumed empty packet switches to current broker', () => {
  const p = page();
  p.send([sample(1000, 0), sample(1001, 1)], 0, epochA);
  p.pause(); p.reconnect();
  p.send([sample(1, 0)], 0, epochB);
  assert.equal(p.evaluate('receiverStream'), epochA);
  assert.equal(p.evaluate('lastReceived'), 1001);
  assert.equal(p.samples().length, 2);
  p.pause();
  p.send([], 0, epochB);
  assert.equal(p.evaluate('receiverStream'), epochB);
  assert.equal(p.evaluate('lastReceived'), null);
  assert.equal(p.evaluate('sampleKeys.size'), 0);
  assert.equal(p.samples().length, 0);
  p.send([sample(2, 1), sample(3, 2)], 0, epochB);
  assert.deepEqual(p.paths().at(-1).map(point => point[1]), [870, 900]);
});

test('legacy packets keep best-effort sparse dedup but do not infer receiver reboot', () => {
  const p = page();
  const first = sample(1000, 1), second = sample(1000, 2);
  delete first.sequence; delete second.sequence;
  second.values.throttle = .9;
  p.send([first, second]); p.send([first, second]);
  assert.equal(p.samples().length, 2);
  p.reconnect(); p.send([sample(1, 0, 'new-session')]);
  assert.equal(p.evaluate('lastReceived'), 1000);
  assert.equal(p.samples().length, 2, 'legacy timestamps cannot prove clock reset');
});

function sample(received, mono, session = 'run', sequence = mono) {
  return {type: 'sample', received_monotonic: received, monotonic_us: mono,
    session_id: session, sequence, values: {throttle: sequence % 100 / 100}};
}

test('1000 short sender-clock epochs retain only the receiver-time window', () => {
  const p = page();
  // Isolate the retention stress from trace rendering; plots have their own checks.
  p.evaluate('selected=[]');
  let peak = 0;
  for (let run = 0; run < 1000; run++) {
    const events = Array.from({length: 100}, (_, i) =>
      sample((run * 100 + i) / 100, i * 10000, `run-${run}`, i));
    p.send(events);
    const count = p.evaluate('samples.length');
    peak = Math.max(peak, count);
  }
  const kept = p.samples();
  console.log(`short epochs: delivered=100000 retained=${kept.length} peak=${peak}`);
  assert.ok(peak <= 3001, `30s at 100Hz must retain <=3001 samples; peak=${peak}`);
  assert.ok(kept.length >= 3000);
  assert.ok(kept.every(s => s.received_monotonic >= 969.99));
});

test('plots use the same rolling receiver-time axis across sender epochs', () => {
  const p = page();
  p.send([sample(100, 4000000000, 'old', 1), sample(101, 4001000000, 'old', 2)]);
  p.send([sample(102, 0, 'new', 0), sample(103, 1000000, 'new', 1)]);
  const trace = p.paths().at(-1);
  assert.deepEqual(trace.map(point => point[1]), [810, 840, 870, 900]);
  assert.ok(trace.every(point => point.slice(1).every(Number.isFinite)));
  assert.equal(p.samples().length, 4, 'a sender reset does not erase in-window history');
  console.log(`epoch plot: x=${trace.map(point => point[1]).join(',')}`);
});

test('same-session reconnect merges overlapping history without clearing or duplicating it', () => {
  const p = page();
  p.window(120);
  const history = Array.from({length: 101}, (_, i) => sample(i, i * 1e6, 'same', i));
  p.send(history);
  const replay = [...history.slice(80), sample(101, 101000000, 'same', 101)];
  for (let i = 0; i < 100; i++) {
    p.reconnect();
    p.send(replay);
  }
  const kept = p.samples();
  console.log(`reconnect overlap: reconnects=100 retained=${kept.length}`);
  assert.equal(kept.length, 102, 'overlap is not new telemetry');
  assert.deepEqual(kept.map(s => s.sequence), Array.from({length: 102}, (_, i) => i));
  assert.equal(kept[0].received_monotonic, 0, 'older local history survives a 30s reconnect snapshot');
});

test('window changes trim and redraw immediately, including while viewer-paused', () => {
  const p = page();
  p.window(120);
  p.send(Array.from({length: 101}, (_, i) => sample(i, i * 1e6, 'run', i)));
  p.pause();
  p.window(10);
  assert.equal(p.samples().length, 11, 'a window change must not wait for another live packet');
  assert.deepEqual(p.samples().map(s => s.received_monotonic), Array.from({length: 11}, (_, i) => i + 90));
  const trace = p.paths().at(-1);
  assert.equal(trace.at(-1)[1], 900);
  assert.ok(trace.every(point => point[1] >= 0 && point[1] <= 900));
  p.window(120);
  assert.equal(p.samples().length, 11, 'expansion cannot resurrect evicted history');
  p.pause();
  p.send([sample(101, 101000000, 'run', 101)]);
  assert.equal(p.samples().length, 12, 'the expanded window grows with new telemetry');
  console.log('window changes: 120s -> 10s retained=11; expand/resume retained=12');
});

// The following are characterization/acceptance checks, not additional RED cycles.
test('receiver and sender zero timestamps are valid, plotted, and eventually evicted', () => {
  const p = page();
  p.send([sample(0, 0, 'zero', 0), sample(1, 0, 'zero', 1)]);
  assert.deepEqual(p.paths().at(-1).map(point => point[1]), [870, 900]);
  assert.equal(p.samples()[0].received_monotonic, 0);
  p.send([sample(31, 0, 'zero', 2)]);
  assert.deepEqual(p.samples().map(s => s.received_monotonic), [1, 31]);
  assert.equal(p.evaluate('sampleKeys.size'), 2, 'eviction also bounds duplicate tracking');
  console.log('zero timestamps: x=870,900; after 31s retained receiver times=1,31');
});

test('viewer pause freezes history and axis; resume rolls only on accepted telemetry', () => {
  const p = page();
  p.send([sample(10, 0, 'run', 0), sample(11, 1000000, 'run', 1)]);
  const before = p.samples();
  p.pause();
  p.send([sample(50, 0, 'next', 0)], 17);
  p.reconnect();
  p.send([sample(100, 0, 'third', 0)]);
  assert.deepEqual(p.samples(), before);
  assert.equal(p.evaluate('lastReceived'), 11);
  assert.equal(p.evaluate('sampleKeys.size'), 2);
  p.pause();
  p.send([]);
  assert.deepEqual(p.samples(), before, 'idle packets do not erase ordinary history');
  p.send([sample(12, 2000000, 'run', 2)]);
  assert.equal(p.samples().length, 3);
  p.send([sample(100, 0, 'later', 0)], 7);
  assert.deepEqual(p.samples().map(s => s.received_monotonic), [100]);
  assert.equal(p.connection(), 'live (viewer skipped 7)');
  console.log('pause: retained=2 axis=11; resume retained=3; later gap retained=1');
});

test('reconnect fills older unseen history in order without rewinding the axis', () => {
  const p = page();
  p.send([sample(100, 10000000, 'same', 10), sample(102, 12000000, 'same', 12)]);
  p.reconnect();
  p.send([sample(99, 9000000, 'same', 9), sample(100, 10000000, 'same', 10),
    sample(101, 11000000, 'same', 11)]);
  assert.deepEqual(p.samples().map(s => s.received_monotonic), [99, 100, 101, 102]);
  assert.equal(p.evaluate('lastReceived'), 102);
  assert.deepEqual(p.paths().at(-1).map(point => point[1]), [810, 840, 870, 900]);
  p.send([sample(102, 12000000, 'different-session', 12), sample(102, 12000000, 'same', 13)]);
  assert.equal(p.samples().length, 6, 'equal receiver times alone are not duplicate identities');
});

test('10 long sender-clock epochs bound samples and dedup keys at the 120s maximum', () => {
  const p = page();
  p.evaluate('selected=[]');
  p.window(120);
  let peak = 0;
  for (let run = 0; run < 10; run++) {
    for (let second = 0; second < 180; second++) {
      p.send(Array.from({length: 100}, (_, i) => ({...sample(
        run * 180 + second + i / 100, (second * 100 + i) * 10000, `long-${run}`, second * 100 + i),
        event_id: String(run * 18000 + second * 100 + i + 1)})), 0, epochA);
      const count = p.evaluate('samples.length');
      peak = Math.max(peak, count);
      assert.equal(p.evaluate('sampleKeys.size'), count);
      assert.ok(count <= 12001, `120s at 100Hz: count=${count}`);
    }
  }
  const kept = p.samples();
  assert.ok(kept.length >= 12000);
  assert.ok(kept.every(s => s.received_monotonic >= 1679.99));
  p.evaluate("selected=['throttle'];draw()");
  const trace = p.paths().at(-1);
  assert.equal(trace.length, kept.length);
  assert.equal(trace.at(-1)[1], 900);
  assert.ok(trace.every(point => point.slice(1).every(Number.isFinite) && point[1] >= 0 && point[1] <= 900));
  console.log(`long epochs: delivered=180000 retained=${kept.length} peak=${peak} keys=${p.evaluate('sampleKeys.size')} plotted=${trace.length}`);
});

test('all selectable rolling windows age ordinary same-session history', () => {
  for (const seconds of [10, 30, 60, 120]) {
    const p = page();
    p.window(seconds);
    p.send(Array.from({length: 251}, (_, i) => sample(i, i * 1e6, 'ordinary', i)));
    assert.equal(p.samples().length, seconds + 1);
    assert.equal(p.samples()[0].received_monotonic, 250 - seconds);
    assert.equal(p.evaluate('sampleKeys.size'), seconds + 1);
    assert.equal(p.paths().at(-1)[0][1], 0);
    assert.equal(p.paths().at(-1).at(-1)[1], 900);
  }
  console.log('ordinary history: 10/30/60/120s retain=11/31/61/121');
});
