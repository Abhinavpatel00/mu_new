#!/bin/sh
# Install / remove the Khronos validation layer for the demo APK on a device.
#
#   sh vvl_install.sh /path/to/arm64-v8a/libVkLayer_khronos_validation.so
#   sh vvl_install.sh --off
#   sh vvl_install.sh --status
#
# The layer is NOT bundled in the APK: 27 MB per ABI is not worth it for a demo,
# and Android 9+ can load a layer from the app's own data directory. This is the
# procedure from developer.android.com/ndk/guides/graphics/validation-layer
# ("Use external validation layers"), with the four `settings` that make the
# loader look there:
#
#   enable_gpu_debug_layers=1      turn the mechanism on
#   gpu_debug_app=<pkg>            which app gets the layers
#   gpu_debug_layer_app=<pkg>      which app's data dir to search for them
#   gpu_debug_layers=<layer>       which layer to load (colon-separated list)
#
# Requirements: the app is installed and debuggable (the manifest sets
# android:debuggable="true"), and the APK is built with `.validate = true`
# (mobile_main.c) -- the app must request the layer in vkCreateInstance.
#
# Get the binary from the official release (one zip, four ABIs):
#   curl -LO https://github.com/KhronosGroup/Vulkan-ValidationLayers/releases/download/vulkan-sdk-1.4.363.1/android-binaries-1.4.363.1.zip
#   unzip -q android-binaries-1.4.363.1.zip
#
set -e

APP=${APP:-com.example.mgdemo}
LAYER=VK_LAYER_KHRONOS_validation
LIB=libVkLayer_khronos_validation.so
TMP=/data/local/tmp

need_device() {
    adb get-state 2>/dev/null | grep -q device || {
        echo "no device: check 'adb devices' (USB debugging on, prompt accepted)" >&2
        exit 1
    }
}

case "$1" in
--status)
    need_device
    adb shell settings list global | grep gpu || echo "(no gpu_debug_* settings: external layers are off)"
    adb shell run-as "$APP" ls -l "$LIB" 2>/dev/null || echo "(no $LIB in the app's data dir)"
    adb shell getprop debug.vulkan.layers | sed 's/^/debug.vulkan.layers=/'
    ;;
--off)
    need_device
    adb shell settings delete global enable_gpu_debug_layers >/dev/null
    adb shell settings delete global gpu_debug_app            >/dev/null
    adb shell settings delete global gpu_debug_layers         >/dev/null
    adb shell settings delete global gpu_debug_layer_app      >/dev/null
    adb shell run-as "$APP" rm -f "$LIB" 2>/dev/null || true
    adb shell rm -f "$TMP/$LIB" 2>/dev/null || true
    # A forced layer that cannot be loaded makes vkCreateInstance fail for every
    # Vulkan app on the device, so clear the global switch too (it is not
    # persistent across reboots, but it is easy to leave behind from a test).
    adb shell setprop debug.vulkan.layers '\"\"' 2>/dev/null || true
    echo "external layers disabled and $LIB removed"
    ;;
"")
    sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
    ;;
*)
    need_device
    SRC=$1
    [ -f "$SRC" ] || { echo "no such file: $SRC" >&2; exit 1; }
    # 1. staged where adb can write, 2. copied into the app's data dir with the
    #    app's own uid (run-as), so no root and no unreadable paths.
    adb push "$SRC" "$TMP/$LIB" | tail -1
    adb shell run-as "$APP" cp "$TMP/$LIB" .
    adb shell run-as "$APP" ls -l "$LIB"
    # 3. tell the loader where to look. These persist across reboots.
    adb shell settings put global enable_gpu_debug_layers 1
    adb shell settings put global gpu_debug_app "$APP"
    adb shell settings put global gpu_debug_layers "$LAYER"
    adb shell settings put global gpu_debug_layer_app "$APP"
    adb shell settings list global | grep gpu
    echo
    echo "installed. Run the app, then:"
    echo "  adb logcat -s mg | grep -E 'validation|val [0-9]+e'"
    echo "Turn it off again with: sh vvl_install.sh --off"
    ;;
esac
