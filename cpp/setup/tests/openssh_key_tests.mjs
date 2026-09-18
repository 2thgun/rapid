// #28: unit tests for the OpenSSH key encoding the setup page uses to turn a
// browser WebCrypto keypair into an authorized_keys line and a private-key
// download. There is no browser test harness for setup.html, so this test
// loads the pure-function block straight out of cpp/assets/setup.html (between
// the #28-key-format markers) and checks it against fixed key material.
//
// Usage: node openssh_key_tests.mjs [path/to/setup.html]
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import vm from 'node:vm';
import {execFileSync} from 'node:child_process';
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
const start = html.indexOf('/* #28-key-format:start */');
const end = html.indexOf('/* #28-key-format:end */');
require(start >= 0 && end > start, 'setup.html must contain the #28-key-format block');
const block = html.slice(start + '/* #28-key-format:start */'.length, end);

const context = {btoa, atob, TextEncoder, console};
vm.createContext(context);
try {
  vm.runInContext(block, context);
} catch (error) {
  fail('the #28-key-format block did not evaluate: ' + error.message);
}
for (const name of ['sshBytes', 'sshField', 'sshMpint', 'base64FromBytes', 'bytesFromBase64Url',
                    'opensshPublicKeyLine', 'opensshPrivateKeyPem', 'sshEd25519', 'sshEcdsaP256'])
  require(typeof context[name] === 'function', 'setup.html must define ' + name);

const hex = (text) => Uint8Array.from(Buffer.from(text, 'hex'));
const base64Url = (text) => Uint8Array.from(Buffer.from(text, 'base64url'));
const checkint = new Uint8Array([1, 2, 3, 4]);
const comment = 'rapid@laptop';

// RFC 8032 section 7.1 TEST 1: the seed and public key are fixed and
// independent of this implementation.
const ed = context.sshEd25519(
    hex('9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60'),
    hex('d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a'), comment, checkint);
equal(ed.publicLine,
      'ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAINdamAGCsQq31Uv+08lkBzoO4XLz2qYjJa8CGmj3B1Ea rapid@laptop',
      'Ed25519 authorized_keys line');
equal(ed.privateKeyPem,
      '-----BEGIN OPENSSH PRIVATE KEY-----\n' +
      'b3BlbnNzaC1rZXktdjEAAAAABG5vbmUAAAAEbm9uZQAAAAAAAAABAAAAMwAAAAtzc2gtZW\n' +
      'QyNTUxOQAAACDXWpgBgrEKt9VL/tPJZAc6DuFy89qmIyWvAhpo9wdRGgAAAJABAgMEAQID\n' +
      'BAAAAAtzc2gtZWQyNTUxOQAAACDXWpgBgrEKt9VL/tPJZAc6DuFy89qmIyWvAhpo9wdRGg\n' +
      'AAAECdYbGd7/1aYLqESvSS7CzEREnFaXsyaRlwO6wDHK5/YNdamAGCsQq31Uv+08lkBzoO\n' +
      '4XLz2qYjJa8CGmj3B1EaAAAADHJhcGlkQGxhcHRvcAE=\n' +
      '-----END OPENSSH PRIVATE KEY-----\n',
      'Ed25519 private-key encoding');

// Pinned P-256 material (generated once, then frozen here).
const ec = context.sshEcdsaP256(
    base64Url('ARhWPTGGvwzVFS6Aq-HTbbOQ144Ps9O3OxQ73YMv7KE'),
    base64Url('e81G3B4lue03I0HHmFBZlKBlZSiSpfFSbbDbcwMElkk'),
    base64Url('3yyNNDyNZD8k00vIBC8f2_SUqSJ_Ttm_eBqWnRLxYKA'), comment, checkint);
equal(ec.publicLine,
      'ecdsa-sha2-nistp256 AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTYAAABBBHvNRtweJbntNyNBx5hQWZSgZWUokqXxUm2w23MDBJZJ3yyNNDyNZD8k00vIBC8f2/SUqSJ/Ttm/eBqWnRLxYKA= rapid@laptop',
      'ECDSA P-256 authorized_keys line');
equal(ec.privateKeyPem,
      '-----BEGIN OPENSSH PRIVATE KEY-----\n' +
      'b3BlbnNzaC1rZXktdjEAAAAABG5vbmUAAAAEbm9uZQAAAAAAAAABAAAAaAAAABNlY2RzYS\n' +
      '1zaGEyLW5pc3RwMjU2AAAACG5pc3RwMjU2AAAAQQR7zUbcHiW57TcjQceYUFmUoGVlKJKl\n' +
      '8VJtsNtzAwSWSd8sjTQ8jWQ/JNNLyAQvH9v0lKkif07Zv3galp0S8WCgAAAAqAECAwQBAg\n' +
      'MEAAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTYAAABBBHvNRtweJbntNyNB\n' +
      'x5hQWZSgZWUokqXxUm2w23MDBJZJ3yyNNDyNZD8k00vIBC8f2/SUqSJ/Ttm/eBqWnRLxYK\n' +
      'AAAAAgARhWPTGGvwzVFS6Aq+HTbbOQ144Ps9O3OxQ73YMv7KEAAAAMcmFwaWRAbGFwdG9w\n' +
      'AQIDBA==\n' +
      '-----END OPENSSH PRIVATE KEY-----\n',
      'ECDSA P-256 private-key encoding');

// Independent check: real OpenSSH must derive exactly the shown public key
// from the private-key file. This is the same check the manual round-trip does.
const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'rapid-openssh-key-'));
try {
  for (const [name, built] of [['ed25519', ed], ['ecdsa', ec]]) {
    const file = path.join(directory, 'id_' + name);
    fs.writeFileSync(file, built.privateKeyPem);
    fs.chmodSync(file, 0o600);
    let derived;
    try {
      derived = execFileSync('ssh-keygen', ['-y', '-f', file], {encoding: 'utf8'}).trim();
    } catch (error) {
      if (error.code === 'ENOENT') {
        console.log('SKIP ssh-keygen round-trip: ssh-keygen is not installed');
        break;
      }
      fail('ssh-keygen rejected the generated ' + name + ' key: ' + error.message);
    }
    equal(derived, built.publicLine, 'ssh-keygen -y output for the ' + name + ' key');
  }
} finally {
  fs.rmSync(directory, {recursive: true, force: true});
}

console.log('ok: OpenSSH key encoding matches fixed Ed25519 and ECDSA P-256 vectors' +
            (existsSshKeygen() ? ' and ssh-keygen -y derives the shown public key' : ''));
function existsSshKeygen() {
  try {
    execFileSync('ssh-keygen', ['-V'], {stdio: 'ignore'});
  } catch (error) {
    return error.code !== 'ENOENT';
  }
  return true;
}
