#!/bin/bash
# Validate image metadata without building a filesystem or changing the host.
set -euo pipefail
if [[ $# -lt 3 || $# -gt 4 ]]; then
  echo 'Usage: prepare.sh BUILDER_CHECKOUT ARM64_DEB OUTPUT_DIRECTORY [--build]' >&2
  exit 2
fi
builder=$(realpath "$1")
package=$(realpath "$2")
output=$(realpath -m "$3")
source_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
mode=${4:---validate}
[[ "$mode" == --validate || "$mode" == --build ]] || exit 2
[[ $(git -C "$builder" rev-parse HEAD) == "$(cat "$source_root/builder-revision")" ]] || {
  echo 'Image builder revision does not match image/builder-revision.' >&2; exit 1;
}
[[ $(dpkg-deb -f "$package" Package) == rapid && $(dpkg-deb -f "$package" Architecture) == arm64 ]] || {
  echo 'An ARM64 raPId package is required.' >&2; exit 1;
}
for path in "$builder" "$package" "$output" "$source_root"; do
  [[ "$path" != *['"$`\']* && "$path" != *$'\n'* ]] || {
    echo 'Build paths must not contain shell expansion characters or newlines.' >&2; exit 1;
  }
done
if [[ -e "$output" && ! -d "$output" ]]; then
  echo 'Output path must be a directory.' >&2
  exit 1
fi
if [[ -d "$output" && -n $(find "$output" -mindepth 1 -maxdepth 1 -print -quit) ]]; then
  echo 'Output directory must be empty.' >&2
  exit 1
fi
mkdir -p "$output/dynamic/layer"
"$builder/bin/ig" metadata --emit "$builder/registry.defs" > "$output/registry.env"
"$builder/bin/ig" config "$source_root/config/rapid-pi4.yaml" --write-to "$output/user.env"
cat "$output/registry.env" "$output/user.env" > "$output/config.env"
cat >> "$output/config.env" <<ENV
IGTOP="$builder"
IGROOT="$builder"
SRCROOT="$source_root"
RPI_TEMPLATES="$builder/templates/rpi"
LAYER_HOOKS="$builder/layer-hooks"
DYNROOT="$output/dynamic"
IGconf_rapid_package="$package"
IGconf_sys_workroot="$output/work"
DEB_BUILD_ARCH="$(dpkg-architecture -qDEB_BUILD_ARCH)"
DEB_BUILD_GNU_TYPE="$(dpkg-architecture -qDEB_BUILD_GNU_TYPE)"
DEB_HOST_ARCH="arm64"
DEB_HOST_GNU_TYPE="aarch64-linux-gnu"
ENV
"$builder/bin/ig" pipeline --env-in "$output/config.env" \
  --layers essential rpi4 image-rpios rapid-system \
  --path "DYNlayer=$output/dynamic/layer:layer=$builder/layer:device=$builder/device:image=$builder/image:rapid=$source_root/layer" \
  --env-out "$output/resolved.env" --plan-out "$output/layers.plan" > "$output/validation.log" 2>&1 || {
    tail -n 25 "$output/validation.log" >&2; exit 1;
  }
sha256sum "$package" > "$output/package.sha256"
git -C "$builder" rev-parse HEAD > "$output/builder-revision"
dpkg-deb --contents "$package" > "$output/package-contents"
if grep -q '/rapid-firstboot.service$' "$output/package-contents" &&
   grep -q '/rapid-image-ready-v1$' "$output/package-contents"; then
  printf 'ready_for_filesystem_build=yes\n' > "$output/build-readiness.env"
else
  printf 'ready_for_filesystem_build=no\nblocker=firstboot_customer_flow_incomplete\n' \
    > "$output/build-readiness.env"
fi
echo 'Image configuration and layer dependencies validated.'
if [[ "$mode" == --build ]]; then
  if ! grep -q '/rapid-firstboot.service$' "$output/package-contents" ||
     ! grep -q '/rapid-image-ready-v1$' "$output/package-contents"; then
    echo 'Image build unavailable: the package has not declared the complete first-boot customer flow ready.' >&2
    exit 2
  fi
  exec "$builder/rpi-image-gen" build -S "$source_root" -c rapid-pi4.yaml \
    -B "$output/work" -- "IGconf_rapid_package=$package"
fi
