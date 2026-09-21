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
    "$stage/usr/lib/systemd/system" "$stage/usr/lib/tmpfiles.d" \
    "$stage/usr/lib/sysusers.d" "$work/control"
  cp "$source_root/packaging/config.toml" "$stage/etc/rapid/config.toml"
  cp "$source_root/packaging/rapid.tmpfiles" "$stage/usr/lib/tmpfiles.d/rapid.conf"
  cp "$source_root/packaging/rapid.sysusers" "$stage/usr/lib/sysusers.d/rapid.conf"
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
  set -- -DPACKAGE="$work/rapid.deb" -DDPKG_DEB="$work/dpkg-deb"
  if [ "${EXPECT_IMAGE_READY:-}" = ON ]; then
    set -- "$@" -DEXPECT_IMAGE_READY=ON
  fi
  cmake "$@" -P "$source_root/packaging/CheckPackage.cmake" > "$work/output" 2>&1
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
tmpfiles=$work/stage/usr/lib/tmpfiles.d/rapid.conf

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
replace_in "$units/rapid-setup.service" ' --ssid-file /run/rapid/network-ssid' ''
expect_fail "a setup server that cannot show the AP name" "publish the setup AP name"

# #25: WSL has no systemd, so nothing else in the gate exercises unit
# sandboxing. These mutations reproduce the exact class of bug found on a
# real Pi: a unit references a path its binary writes that ProtectSystem=strict
# then silently makes read-only.
stage_package
replace_in "$units/rapid-setup.service" '^ReadWritePaths=/run/rapid$' 'ReadWritePaths=/var/lib/rapid-setup'
expect_fail "a setup server that cannot write /run/rapid" "does not grant write access to /run/rapid/calibration-request.json"

stage_package
replace_in "$units/rapid-wifi.service" '^ReadWritePaths=/run/rapid-apply$' ''
expect_fail "a Wi-Fi helper that cannot write its shared queue result" "does not grant write access to /run/rapid-apply/wifi-result.json"

stage_package
replace_in "$units/rapid-wifi.service" '^ProtectSystem=strict$' ''
expect_fail "a Wi-Fi helper with a widened sandbox" "rapid-wifi.service must keep ProtectSystem=strict"

stage_package
replace_in "$units/rapid-display-recovery.service" '^ReadWritePaths=/var/lib/rapid$' ''
expect_fail "a display recovery helper that cannot write its state file" "does not grant write access to /var/lib/rapid/display-recovery.json"

stage_package
replace_in "$units/rapid-display-recovery.service" '^ProtectSystem=strict$' ''
expect_fail "a display recovery helper with a widened sandbox" "rapid-display-recovery.service must keep ProtectSystem=strict and grant ReadWritePaths=/var/lib/rapid"

# #32: systemd accumulates repeated ReadWritePaths=/RuntimeDirectory= directives,
# so the generic cross-check must consider every line, not only the first. This
# unit's required result path is moved to a second ReadWritePaths line: it must
# still pass, where reading only the first line would reject it by accident.
stage_package
replace_in "$units/rapid-setup.service" '^ReadWritePaths=/run/rapid$' 'ReadWritePaths=/var/lib/rapid-setup'
printf '\nReadWritePaths=/run/rapid\n' >> "$units/rapid-setup.service"
expect_pass "a result path granted only on a later ReadWritePaths line"

# The same required path listed on no ReadWritePaths line must still fail.
stage_package
replace_in "$units/rapid-setup.service" '^ReadWritePaths=/run/rapid$' 'ReadWritePaths=/var/lib/rapid-setup'
expect_fail "a setup server whose runtime path is granted nowhere" "does not grant write access to /run/rapid/calibration-request.json"

# RuntimeDirectory= is gathered the same way: a grant moved to a later line is
# still a grant.
stage_package
replace_in "$units/rapid-firstboot.service" '^RuntimeDirectory=rapid$' 'RuntimeDirectory=unused'
printf '\nRuntimeDirectory=rapid\n' >> "$units/rapid-firstboot.service"
expect_pass "a required path covered only by a later RuntimeDirectory line"

stage_package
replace_in "$units/rapid-provision.service" '^Group=rapid$' ''
expect_fail "a provisioner that leaves /run/rapid unreadable by the rapid group" "share /run/rapid with the rapid group"

stage_package
replace_in "$units/rapid-firstboot.service" '^RuntimeDirectoryMode=0770$' 'RuntimeDirectoryMode=0750'
expect_fail "a first-boot /run/rapid mode too narrow for the files written into it" "share /run/rapid at mode 0770"

stage_package
replace_in "$units/rapid-provision.service" '^RuntimeDirectoryMode=0770$' 'RuntimeDirectoryMode=0750'
expect_fail "a provisioner whose /run/rapid mode disagrees with first boot's" "must declare the exact same RuntimeDirectoryMode"

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

stage_package
replace_in "$units/rapid-wifi.service" '^ReadWritePaths=/etc/NetworkManager/system-connections$' ''
expect_fail "a Wi-Fi applicator that cannot write its NetworkManager keyfile" "write access to NetworkManager's connection directory"

stage_package
printf 'v1\n' > "$work/stage/usr/share/rapid/rapid-image-ready-v1"
expect_fail "an ordinary package that declares the image ready" "must not contain the image-ready marker"

stage_package
EXPECT_IMAGE_READY=ON expect_fail "a release package missing the image-ready marker" "must contain the image-ready marker"

stage_package
printf 'v1\n' > "$work/stage/usr/share/rapid/rapid-image-ready-v1"
EXPECT_IMAGE_READY=ON expect_pass "a release package that declares the image ready"

# /run/rapid-apply lifecycle (#25 follow-up): the shared queue has exactly one
# lifecycle owner, rapid-setup.service, through RuntimeDirectory=. A shared
# RuntimeDirectory is not reference counted: systemd removes it whenever any
# declaring unit stops, so a transient consumer that also declared it deleted the
# queue while rapid-setup was still active. The consumers must not declare it;
# they must instead be ordered after the owner and granted ReadWritePaths.
stage_package
replace_in "$units/rapid-setup.service" '^RuntimeDirectory=rapid-apply$' ''
expect_fail "a setup server that does not own the shared queue" "must declare RuntimeDirectory=rapid-apply as the single lifecycle owner"

stage_package
replace_in "$units/rapid-setup.service" '^RuntimeDirectoryMode=0770$' 'RuntimeDirectoryMode=0750'
expect_fail "a setup server whose shared queue mode is too narrow" "must declare RuntimeDirectoryMode=0770 for the shared"

stage_package
printf '\nRuntimeDirectory=rapid-apply\n' >> "$units/rapid-wifi.service"
expect_fail "a transient helper that also claims the shared queue directory" "must not declare RuntimeDirectory=rapid-apply"

stage_package
printf '\nRuntimeDirectory=rapid-apply\nRuntimeDirectoryMode=0770\n' >> "$units/rapid-apply.service"
expect_fail "a second queue owner among the transient helpers" "must not declare RuntimeDirectory=rapid-apply"

stage_package
replace_in "$units/rapid-wifi.service" '^Requires=rapid-setup[.]service$' ''
expect_fail "a helper with no dependency on the queue owner" "must require rapid-setup.service"

stage_package
replace_in "$units/rapid-apply.service" '^After=rapid-setup[.]service$' ''
expect_fail "a helper not ordered after the queue owner" "must be ordered After=rapid-setup.service"

stage_package
replace_in "$units/rapid-account.service" '^ReadWritePaths=/run/rapid-apply$' ''
expect_fail "a helper that cannot write the shared queue" "does not grant write access to /run/rapid-apply/account-request.json"

stage_package
replace_in "$units/rapid-account.service" '^Group=rapid$' ''
expect_fail "a helper outside the shared queue group" "must run in the rapid group"

stage_package
printf 'd /run/rapid-apply 0770 root rapid -\n' >> "$tmpfiles"
expect_fail "a tmpfiles rule that reclaims the queue directory" "must not create /run/rapid-apply"

stage_package
rm "$tmpfiles"
expect_fail "a package without the tmpfiles rule" "Package is missing ./usr/lib/tmpfiles.d/rapid.conf"

echo "package verifier tests passed"
