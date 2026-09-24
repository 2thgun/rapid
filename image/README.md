# Pi image

This directory holds the input for building the Raspberry Pi 4 + PiScreen disk
image (`rapid-pi4.img.xz`) with
[rpi-image-gen](https://github.com/raspberrypi/rpi-image-gen).

| File | Purpose |
| --- | --- |
| `builder-revision` | The pinned `rpi-image-gen` commit. |
| `config/rapid-pi4.yaml` | Device profile: Pi 4, hostname `rapid`, a locked `rapid` user, 4 GB root partition. |
| `layer/rapid-system.yaml` | Installs the raPId `.deb`, configures the PiScreen overlay and Xorg, enables the services, and enforces the security checks below. |
| `prepare.sh` | Validates the inputs and assembles the image. |

## Usage

```sh
bash image/prepare.sh BUILDER_CHECKOUT ARM64_DEB OUTPUT_DIRECTORY            # validate only
bash image/prepare.sh BUILDER_CHECKOUT ARM64_DEB NEW_OUTPUT_DIRECTORY --build
```

Run it on an ARM64 Linux host. Each run needs a new, empty output directory.
Validation writes `builder-revision`, `package.sha256`, `resolved.env`,
`layers.plan`, `validation.log` and `build-readiness.env` there.

`--build` accepts only a package built with `-DRAPID_IMAGE_READY=ON`, which
installs `/usr/share/rapid/rapid-image-ready-v1`. Only release builds set it.
Normally the release workflow builds the image when a `v*` tag is pushed. See
[Releasing](https://github.com/2thgun/rapid/wiki/Releasing).

## What the image guarantees

The layer fails the build unless all of these hold:

- no `runtime.env`, setup database, owner, key or recording is present, so each
  device creates its own identity on first boot;
- every account password is locked, and there are no `authorized_keys`;
- there are no SSH host keys, password login is off, and `ssh` is masked until
  the owner turns on device access on the setup page;
- Samba is installed but disabled.
