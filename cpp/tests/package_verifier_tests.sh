#!/bin/sh
# Exercises packaging/CheckPackage.cmake against a staged package tree served by
# a fake dpkg-deb, so the verifier's own rules are tested without CPack/Qt.
# The baseline must pass; every weakened device-access wiring (#23) must fail.
set -eu
source_root=$(cd "$1" && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/rapid-package-verifier.XXXXXX")
trap 'rm -rf "$work"' EXIT INT TERM

stage_package() {
  stage=$work/stage
  rm -rf "$stage" "$work/control"
  mkdir -p "$stage/etc/rapid" "$stage/usr/lib/rapid" "$stage/usr/share/rapid" \
    "$stage/usr/lib/systemd/system" "$work/control"
  cp "$source_root/packaging/config.toml" "$stage/etc/rapid/config.toml"
  for binary in rapid-pi rapid-qt-display rapid-log-status rapid-setup-server rapid-firstboot \
      rapid-provision rapid-apply rapid-display-recovery rapid-wifi rapid-account rapid-network-mode; do
    : > "$stage/usr/lib/rapid/$binary"
  done
  cp "$source_root/packaging/rapid-panel" "$stage/usr/lib/rapid/rapid-panel"
  cp "$source_root/cpp/assets/setup.html" "$stage/usr/share/rapid/setup.html"
  for unit in "$source_root"/packaging/*.service "$source_root"/packaging/*.path; do
    cp "$unit" "$stage/usr/lib/systemd/system/"
  done
  printf '/etc/rapid/config.toml\n' > "$work/control/conffiles"
  depends="libargon2-1 (>= 0), libcrypt1 (>= 1:4.4), libqt6core6t64, systemd, network-manager, openssh-server, sudo, xinput"
}

write_fake_dpkg() {
  cat > "$work/dpkg-deb" <<EOF
#!/bin/sh
case "\$1" in
  --contents) cd "$work/stage" && find . -mindepth 1 | LC_ALL=C sort | while read -r path; do
      printf -- '-rw-r--r-- root/root 0 2026-09-17 00:00 %s\n' "\$path"; done ;;
  --fsys-tarfile) tar -C "$work/stage" -cf - . ;;
  --ctrl-tarfile) tar -C "$work/control" -cf - ./conffiles ;;
  --field) printf '%s\n' "\$(cat "$work/depends")" ;;
  *) exit 2 ;;
esac
EOF
  chmod +x "$work/dpkg-deb"
}

verify() {
  printf '%s\n' "$depends" > "$work/depends"
  : > "$work/rapid.deb"
  cmake -DPACKAGE="$work/rapid.deb" -DDPKG_DEB="$work/dpkg-deb" \
    -P "$source_root/packaging/CheckPackage.cmake" > "$work/output" 2>&1
}

expect_pass() {
  if ! verify; then
    cat "$work/output"
    echo "FAIL: $1" >&2
    exit 1
  fi
  echo "ok: $1"
}

expect_fail() {
  if verify; then
    echo "FAIL: verifier accepted $1" >&2
    exit 1
  fi
  # CMake wraps long messages; compare on whitespace-normalized output.
  if ! tr -s ' \n' '  ' < "$work/output" | grep -qF "$2"; then
    cat "$work/output"
    echo "FAIL: $1 was rejected for the wrong reason" >&2
    exit 1
  fi
  echo "ok: rejects $1"
}

replace_in() {
  file=$1; from=$2; to=$3
  sed -i "s|$from|$to|" "$file"
  if grep -q "$from" "$file" 2>/dev/null && [ "$from" != "$to" ]; then
    echo "FAIL: test mutation did not apply to $file" >&2
    exit 1
  fi
}

write_fake_dpkg
units=$work/stage/usr/lib/systemd/system

stage_package
expect_pass "packaged units and device access wiring"

stage_package
replace_in "$units/rapid-account.service" '^User=root$' 'User=rapid'
expect_fail "a device access helper that is not root" "rapid-account.service must keep 'User=root'"

stage_package
replace_in "$units/rapid-account.service" ' --user rapid$' ' --user rapid --password-hash x'
expect_fail "a secret on the helper command line" "rapid-account helper from its fixed request file"

stage_package
replace_in "$units/rapid-account.service" '^ProtectHome=read-only$' 'ProtectHome=false'
expect_fail "an unsandboxed home directory" "ProtectHome=read-only"

stage_package
replace_in "$units/rapid-account.service" '^NoNewPrivileges=true$' '#NoNewPrivileges=true'
expect_fail "a helper that may gain privileges" "NoNewPrivileges=true"

stage_package
replace_in "$units/rapid-account.service" '^LimitCORE=0$' '#LimitCORE=0'
expect_fail "helper core dumps" "LimitCORE=0"

stage_package
rm "$units/rapid-account.path"
expect_fail "a missing request watcher" "Package is missing ./usr/lib/systemd/system/rapid-account.path"

stage_package
replace_in "$units/rapid-account.path" 'account-request.json' 'request.json'
expect_fail "a watcher on another queue" "rapid-account helper from its fixed request file"

stage_package
replace_in "$units/rapid-setup.service" ' --account-request-file /run/rapid-apply/account-request.json' ''
expect_fail "a setup server that does not queue device access" "without secrets on a command line"

stage_package
replace_in "$units/rapid-setup.service" '^LimitCORE=0$' ''
expect_fail "setup server core dumps" "without secrets on a command line"

stage_package
mkdir -p "$work/stage/etc/ssh/sshd_config.d"
printf 'PasswordAuthentication yes\n' > "$work/stage/etc/ssh/sshd_config.d/10-rapid-owner.conf"
expect_fail "a packaged SSH configuration" "must not contain account credentials"

stage_package
mkdir -p "$work/stage/etc/sudoers.d"
printf 'rapid ALL=(ALL) NOPASSWD: ALL\n' > "$work/stage/etc/sudoers.d/rapid"
expect_fail "a packaged sudo rule" "must not contain account credentials"

stage_package
depends="libargon2-1 (>= 0), libcrypt1 (>= 1:4.4), libqt6core6t64, systemd, network-manager, sudo, xinput"
expect_fail "a package without the SSH server" "Missing generated runtime library dependencies"

echo "package verifier tests passed"
