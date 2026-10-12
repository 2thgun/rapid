'use strict';
// Source-level contracts, runnable on Linux and Windows without PowerShell/cmd.
// These do not claim to execute the Windows installers.
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const root = path.resolve(__dirname, '../..');
const read = name => fs.readFileSync(path.join(root, name), 'utf8').replace(/\r\n/g, '\n');

test('every portable draft-release input is uploaded by the Release producer', () => {
  const workflow = read('.github/workflows/release.yml');
  const upload = workflow.match(/name: rapid-windows-companion\n\s+path: \|\n([\s\S]*?)\s+if-no-files-found:/);
  assert.ok(upload, 'portable upload block must exist');
  const produced = new Set([...upload[1].matchAll(/companion\/([^\s]+)/g)].map(m => m[1]));
  const consumed = [...workflow.matchAll(/release-assets\/companion\/([^\s\\]+)/g)].map(m => m[1]);
  assert.ok(consumed.length > 0, 'draft release must consume portable files');
  for (const name of consumed) {
    assert.ok(produced.has(name), `Release portable artifact does not produce ${name}`);
    if (!name.endsWith('.exe')) assert.ok(fs.statSync(path.join(root, 'companion', name)).isFile());
  }
});

const psList = (source, variable) => {
  const match = source.match(new RegExp('\\$' + variable + '\\s*=\\s*@\\(([\\s\\S]*?)\\)'));
  assert.ok(match, `${variable} must be a literal PowerShell file list`);
  return [...match[1].matchAll(/'([^']+)'/g)].map(m => m[1]);
};
const guideOutputs = () => {
  const producer = read('companion/package-demo.ps1');
  const mapping = producer.match(/\$guides\s*=\s*\[ordered\]@\{([\s\S]*?)\}/);
  assert.ok(mapping, 'demo guide mapping must exist');
  // The submodule is required by package-demo itself, not by this list check.
  return [...mapping[1].matchAll(/'([^']+)'\s*=\s*'([^']+)'/g)].map(m => m[2]);
};

test('demo installer packages every current guide and retains the legacy guide branch', () => {
  const builder = read('companion/build-demo-installer.ps1');
  const producer = read('companion/package-demo.ps1');
  const guides = guideOutputs();
  const packagedGuides = builder.includes('$guideFiles =')
    ? psList(builder, 'guideFiles') : psList(builder, 'files').filter(name => name.endsWith('.md'));
  assert.deepEqual([...packagedGuides].sort(), [...guides].sort(), 'installer guides must match package-demo outputs');
  assert.match(builder, /if \(-not \(Test-Path[^\n]*'GETTING_STARTED\.md'[^\n]*\)\) \{\n\s*\$guideFiles = @\('AC1_DEMO_GUIDE\.md'\)/);
  assert.match(builder, /\$files = @\([\s\S]*?\) \+ \$guideFiles/);
  const produced = new Set(guides);
  for (const m of producer.matchAll(/Join-Path \$bundle '([^']+)'/g)) produced.add(m[1]);
  for (const m of producer.matchAll(/Copy-Item -LiteralPath \(Join-Path \$PSScriptRoot '([^']+)'\) -Destination \$bundle/g)) {
    assert.ok(fs.statSync(path.join(root, 'companion', m[1])).isFile());
    produced.add(m[1]);
  }
  for (const name of psList(builder, 'files')) assert.ok(produced.has(name), `package-demo does not produce required ${name}`);
});

test('batch installer copies current guides with a legacy-only kit fallback', () => {
  const installer = read('companion/install-demo.cmd');
  const modern = installer.match(/^set "GUIDES=([^"]+)"$/m);
  assert.ok(modern, 'installer must declare its current guide list');
  assert.deepEqual(modern[1].split(/\s+/).sort(), guideOutputs().sort());
  assert.match(installer, /if not exist "%~dp0GETTING_STARTED\.md" set "GUIDES=AC1_DEMO_GUIDE\.md"/);
  assert.match(installer, /for %%F in \([^\n]*%GUIDES%\) do/);
  const copied = installer.match(/for %%F in \(([^\n]+)\) do/)[1].split(/\s+/).filter(name => name !== '%GUIDES%');
  const packaged = psList(read('companion/build-demo-installer.ps1'), 'files').filter(name => name !== 'install-demo.cmd');
  assert.deepEqual(copied.sort(), packaged.sort(), 'every runtime installer file must be staged by IExpress');
});

test('launcher recovery names shipped guides without removing legacy key support', () => {
  const launcher = read('companion/START-RAPID.cmd');
  assert.match(launcher, /if exist "%~dp0TROUBLESHOOTING\.md"/);
  assert.match(launcher, /echo See GETTING_STARTED\.md and TROUBLESHOOTING\.md/);
  assert.match(launcher, /else if exist "%~dp0AC1_DEMO_GUIDE\.md"/);
  assert.match(launcher, /--auth-key-file "%~dp0telemetry\.key"/);
  assert.match(launcher, /--auth-key-dpapi-file/);
  assert.doesNotMatch(launcher, /supported only for migrated demo kits/);
});
