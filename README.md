# Waydroid hardware: NVIDIA desktop integration

This fork tracks Waydroid's `lineage-20` branch for Android 13. The downstream
hwcomposer changes are ordinary Git commits on top of upstream
`a575cfcc12ab79a122844218f2c6df5bd208cca0` (2026-11-01).

The 29-patch series from [waydroid-nvidia](https://github.com/yeoj34760/waydroid-nvidia)
was migrated as 28 source commits. Each commit retains the original patch header,
including its source, attribution and licensing notes. Patch 0009 was omitted:
upstream `4aee4c8` already fixes the binder threadpool race by leaving the service's
configuration alone. The upstream `5af0c11` invalid-size check is retained for
wl_shell; resize handling uses the downstream reconnect worker and never sends a
hotplug from the presentation path. Existing upstream audio changes are retained.

The fork includes NVIDIA buffer handling, cross-GPU presentation, native window
decorations, display/task resizing, clipboard fixes and input-method routing.
The source files retain their existing copyright and licence notices; historical
patch-header licence descriptions are retained as provenance, not replacements
for those notices.

## Build and test

Android builds continue to use the existing `Android.bp` files. The standalone
NDK build and pinned Android 13 headers remain in the companion repository:

```sh
cd /path/to/waydroid-nvidia
hwcomposer/provision.sh
hwcomposer/build.sh
# To build local edits instead of the pinned commit:
HWC_SRC=/path/to/android_hardware_waydroid hwcomposer/build.sh
```

Run the host regression checks here with Python 3 and a C++17 compiler:

```sh
sh tests/hwcomposer/run.sh
```

The checks compile the production functions in small host harnesses. They cover
background opacity, task geometry, logical display resize, child/IME routing,
RGB565 read-back and partial resource cleanup. They do not replace a live Android
and Wayland test. Restart the entire container after deploying a guest library.

## Updating

Merge new `waydroid/android_hardware_waydroid` `lineage-20` commits, resolve the
source conflicts, run the host checks and NDK build, then update `HWC_SHA` in the
companion repository. Keep that SHA fixed for reproducible package builds; do not
reintroduce a second exported patch series.
