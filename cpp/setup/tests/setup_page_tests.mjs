// Setup-page structure and copy checks (setup-page UX rework, 2026-09-21).
//
// There is no browser harness for setup.html, so this test pins the shape the
// owner asked for: clearly separated categories, one Save action per category
// with its own status line, the "Save home Wi-Fi credentials" wording, the
// minimum-required vs recommended distinction, and the shortened TLS
// fingerprint. It loads the pure helper block straight out of
// cpp/assets/setup.html (between the #setup-page-pure markers) and also asserts
// the static markup, so a later edit cannot silently undo the rework.
//
// Usage: node setup_page_tests.mjs [path/to/setup.html]
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import {fileURLToPath} from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const setupPath = process.argv[2] || path.resolve(here, '../../assets/setup.html');

function fail(message) {
  console.error('FAIL: ' + message);
  process.exit(1);
}
function require(condition, message) {
  if (!condition) fail(message);
}
function equal(actual, expected, what) {
  if (actual !== expected) fail(what + '\n  expected: ' + JSON.stringify(expected) +
                                '\n  actual:   ' + JSON.stringify(actual));
}

const html = fs.readFileSync(setupPath, 'utf8');

// 1. The shortened fingerprint helper: 16 hex characters (64 bits) in four
//    readable groups, matching the first 16-character block the panel prints.
{
  const start = html.indexOf('/* #setup-page-pure:start */');
  const end = html.indexOf('/* #setup-page-pure:end */');
  require(start >= 0 && end > start, 'setup.html must contain the #setup-page-pure block');
  const block = html.slice(start + '/* #setup-page-pure:start */'.length, end);
  const context = {console};
  vm.createContext(context);
  try {
    vm.runInContext(block, context);
  } catch (error) {
    fail('the #setup-page-pure block did not evaluate: ' + error.message);
  }
  require(typeof context.formatFingerprint === 'function', 'setup.html must define formatFingerprint');
  const full = 'AABBCCDDEEFF00112233445566778899AABBCCDDEEFF00112233445566778899';
  equal(context.formatFingerprint(full), 'aabb ccdd eeff 0011',
        'the panel fingerprint is shortened to its first 64 bits, grouped in fours');
  equal(context.formatFingerprint(full).replace(/ /g, '').length, 16,
        'the shortened fingerprint keeps 64 bits (16 hex characters)');
  equal(context.formatFingerprint(full), context.formatFingerprint(full.toLowerCase()),
        'the fingerprint comparison is case-insensitive');
  equal(context.formatFingerprint(''), 'unavailable', 'a missing fingerprint reads as unavailable');
}

// 2. The #28 key-format block the OpenSSH key test loads must survive.
require(html.includes('/* #28-key-format:start */') && html.includes('/* #28-key-format:end */'),
        'the #28-key-format block must remain in setup.html');

// 3. The owner-facing wording and per-category Save actions.
require(html.includes('Save home Wi-Fi credentials'), 'the Wi-Fi action must read "Save home Wi-Fi credentials"');
require(!html.includes('Join Home Wi-Fi'), 'the misleading "Join Home Wi-Fi" wording must be gone');
for (const id of ['save-settings', 'save-wifi', 'access-save'])
  require(html.includes('id="' + id + '"'), 'each category must have its own Save action: ' + id);
for (const id of ['settings-status', 'wifi-status', 'access-status', 'pairing-status', 'calibration-status'])
  require(html.includes('id="' + id + '"') && html.includes('id="' + id + '" class="status"'),
          'each category must have its own status line: ' + id);
require(/class="req">Required</.test(html) && /class="rec">Recommended</.test(html),
        'required and recommended fields must be labelled distinctly');

// 4. Categories are separated: the authenticated area holds the named cards.
for (const heading of ['Device profile', 'Home Wi-Fi', 'Touchscreen', 'Paired PCs'])
  require(html.includes('<h3>' + heading + '</h3>'), 'missing category card: ' + heading);
require((html.match(/class="category"/g) || []).length >= 4, 'categories must be separate blocks');

// 5. Credential policy in the markup: the device-access field takes a 4-digit
//    PIN while the owner setup login keeps its 12-character minimum.
require(/id="access-password"[^>]*minlength="4"/.test(html),
        'the device password/PIN field must accept 4 characters');
require(/id="access-confirm"[^>]*minlength="4"/.test(html),
        'the device confirmation field must match the 4-character minimum');
require(!/id="access-password"[^>]*minlength="12"/.test(html),
        'the device password/PIN field must not still require 12 characters');
require(/id="new-password"[^>]*minlength="12"/.test(html),
        'the owner setup password keeps its 12-character minimum');

// 6. SSH-key management (#28) is its own category, separate from the device
//    password, and only ever deals in public material.
require(html.includes('<h3>Device password or PIN</h3>') && html.includes('<h3>SSH keys</h3>'),
        'the device password and SSH keys must be separate categories');
for (const id of ['category-password', 'category-ssh-keys'])
  require(html.includes('id="' + id + '"'), 'missing category: ' + id);
for (const id of ['ssh-keys', 'ssh-key-form', 'ssh-key-input', 'ssh-key-save', 'ssh-key-cancel'])
  require(html.includes('id="' + id + '"'), 'missing SSH-key element: ' + id);
require(html.includes('id="ssh-key-status" class="status"'),
        'the SSH-key category must have its own status line');
require(html.includes('action: \'remove_key\'') && html.includes('remove_key'),
        'the page must be able to remove an enrolled key');
require(html.includes('remove_fingerprint'),
        'an edit must replace the identified key through the same endpoint');
require(!html.includes('id="access-key"'),
        'the old combined password+key field must be gone');
// The browser-generated key download (#28) legitimately builds a private-key
// PEM in memory, so assert only that no private-key *field* can be posted.
require(!/ssh_private_key|private_key_pem|private_key:/.test(html),
        'the page must never post a private-key field');

console.log('ok: setup page categories, per-category Save actions, wording, the ' +
            'shortened fingerprint and the separate SSH-key category match the reworked policy');
