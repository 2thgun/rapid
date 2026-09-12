# Pi image build contract

This directory defines the reproducible image input for the supported Raspberry
Pi 4 and PiScreen GPIO display. It is deliberately **not** a released image.
The profile may be validated before first-boot provisioning is available, but
`--build` refuses to assemble a filesystem until the application package
contains `rapid-firstboot.service` and the deliberately absent
`rapid-image-ready-v1` release marker. Add that marker only after the complete
customer flow below is implemented and reviewed.

That guard prevents a card that requires SSH, a manually created telemetry key,
or manual Wi-Fi configuration from being presented as a fresh installation.

## Inputs

`prepare.sh` requires three inputs:

1. an exact checkout of `rpi-image-gen` at the revision in `builder-revision`;
2. an ARM64 `rapid` Debian package built from the same release candidate; and
3. an empty output directory outside the source tree.

The validation phase resolves the official image layers and writes a traceable
record to the output directory:

- `builder-revision` and `package.sha256` identify the inputs;
- `resolved.env`, `layers.plan` and `validation.log` record the resolved image
  configuration; and
- `build-readiness.env` gives the one-line build decision.

Run validation on a Linux ARM64 builder:

```sh
bash image/prepare.sh /path/to/rpi-image-gen /path/to/rapid_arm64.deb /path/to/output
```

After first-boot provisioning has been packaged, use `--build` with the same
inputs. Image assembly requires the dependencies documented by upstream
`rpi-image-gen`; it must run in a disposable build workspace and must never use
the development Pi's data partition as an input.

## Required package behavior before assembly

`rapid-firstboot.service` now initializes private identity and reports missing
capabilities. It is a foundation, not proof of readiness. Before the release
marker can be packaged and an image assembled, first boot must make a
fresh card safe and usable without a terminal:

- generate device-specific identity and telemetry credentials on the Pi;
- start the protected setup AP before Wi-Fi has been enrolled;
- keep the runtime disabled until its required private configuration exists;
- retain a keyboardless recovery route after failed Wi-Fi, rotation or touch
  changes; and
- leave no owner password, telemetry key, database, recording or runtime
  environment file in the image artifact.

The image layer checks the last condition during assembly. First-boot AP and
owner enrollment are tracked in [issue #8](https://github.com/2thgun/rapid/issues/8);
the browser workflow is [issue #9](https://github.com/2thgun/rapid/issues/9).

## Release acceptance

Treat an assembled `.img.xz` as a candidate until a clean SD card completes the
full customer path: flash, boot, join the displayed AP, enroll an owner, join
Wi-Fi, calibrate display/touch, install and pair a Windows companion, then
record a real AC1 or ACC session. Record the image SHA-256, package SHA-256,
builder revision and observed boot/fallback timings with that run.

The full clean-device acceptance is tracked in
[issue #12](https://github.com/2thgun/rapid/issues/12).
