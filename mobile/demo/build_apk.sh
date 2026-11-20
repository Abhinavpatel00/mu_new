#!/bin/sh
# Build mg-demo.apk without Gradle.
#
#   sh build_apk.sh            -> build/mg-demo.apk   (arm64-v8a + x86_64)
#   ABI=arm64-v8a sh build_apk.sh
#
# Needs: Android SDK build-tools + platform + NDK, and a JDK (only for
# apksigner/keytool). Everything below is the same pipeline Gradle runs.
set -e
cd "$(dirname "$0")"

ANDROID_HOME=${ANDROID_HOME:-$HOME/.local/android}
NDK=$(ls -d "$ANDROID_HOME"/ndk/* | head -1)
BT=$(ls -d "$ANDROID_HOME"/build-tools/* | sort -V | tail -1)
PLATFORM="$ANDROID_HOME/platforms/android-34/android.jar"
TC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
GLUE="$NDK/sources/android/native_app_glue/android_native_app_glue.c"
OUT=build/apk
ABIS=${ABI:-"arm64-v8a x86_64"}

echo "== 1/6 shaders"
ANDROID_HOME="$ANDROID_HOME" sh shaders/gen_spirv.sh

echo "== 2/6 native library"
# -Wl,--no-undefined matters more than any of these: without it the linker
# happily emits "U cosf" with no DT_NEEDED for libm, and the failure shows up
# as a dlopen error on the device instead of an error here.
CFLAGS="-std=c11 -Wall -Wextra -Werror -O2 -fPIC -fvisibility=hidden"
# Android 15+ ships 16 KB page kernels: align the loadable segments to 16 KB or
# the loader maps them at 4 KB and mmap of a 16 KB-aligned offset fails. Costs
# ~1 page of file size per segment and nothing at runtime.
LDFLAGS="-Wl,--no-undefined -Wl,--fatal-warnings -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384"
# the whole API is ../mg.c (collector + Vulkan backend, one implementation file)
SRC="mobile_main.c eco.c hud.c ../mg.c"
INC="-I. -I.. -I$(dirname "$GLUE")"
rm -rf $OUT build/obj
mkdir -p $OUT/lib
for ABI in $ABIS; do
    case $ABI in
        arm64-v8a) CC=$TC/aarch64-linux-android24-clang ;;
        x86_64)    CC=$TC/x86_64-linux-android24-clang ;;
        *) echo "unknown ABI $ABI"; exit 1 ;;
    esac
    mkdir -p $OUT/lib/$ABI
    # ours is -Werror clean, the NDK's glue is not, so they are compiled apart
    rm -f build/obj_*.o
    for f in $SRC; do
        $CC $CFLAGS $INC -c -o build/obj_$(basename "$f" .c).o "$f"
    done
    $CC -std=c11 -O2 -fPIC $INC -c -o build/obj_glue.o "$GLUE"
    $CC -shared $LDFLAGS -o $OUT/lib/$ABI/libmobile.so build/obj_*.o \
        -lm -landroid -llog -lvulkan -u ANativeActivity_onCreate
    echo "   $ABI: $(du -h $OUT/lib/$ABI/libmobile.so | cut -f1)"
done

echo "== 3/6 manifest + resources (aapt2)"
"$BT/aapt2" link -o $OUT/base.apk -I "$PLATFORM" \
    --manifest AndroidManifest.xml \
    --min-sdk-version 24 --target-sdk-version 34 \
    --version-code 1 --version-name 1.0

echo "== 4/6 package the .so files"
# -0 (store), not -r alone: with android:extractNativeLibs="false" the loader
# mmaps the .so straight out of the APK, which requires the entry to be
# UNCOMPRESSED and page-aligned. Compressing it is a classic silent crash.
( cd $OUT && zip -q -0 -r base.apk lib )

echo "== 5/6 align"
"$BT/zipalign" -f -p 4 $OUT/base.apk $OUT/aligned.apk
"$BT/zipalign" -c -p 4 $OUT/aligned.apk >/dev/null && echo "   .so entries page-aligned and stored"
# keep a copy with symbols next to the APK, for ndk-stack
cp $OUT/lib/arm64-v8a/libmobile.so libmobile-arm64-v8a.so

echo "== 6/6 sign"
# outside $OUT: the step above wipes $OUT, and a new key every build means
# every rebuild needs an uninstall (INSTALL_FAILED_UPDATE_INCOMPATIBLE).
KS=build/debug.keystore
if [ ! -f "$KS" ]; then
    keytool -genkeypair -keystore "$KS" -storepass android -keypass android \
        -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 \
        -dname "CN=Android Debug,O=Android,C=US" >/dev/null 2>&1
fi
"$BT/apksigner" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android \
    --ks-key-alias androiddebugkey --out mg-demo.apk $OUT/aligned.apk
"$BT/apksigner" verify --print-certs mg-demo.apk | head -4

echo "== 7/7 dlopen audit"
# Every undefined symbol must be provided by a library in DT_NEEDED, or the
# device's linker fails at load time (exactly how "cannot locate symbol cosf"
# happened). --no-undefined catches it at link; this catches what it cannot see.
SYS=$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot
NM=$TC/llvm-nm
for ABI in $ABIS; do
    case $ABI in
        arm64-v8a) DIR=aarch64-linux-android ;;
        x86_64)    DIR=x86_64-linux-android ;;
    esac
    SO=$OUT/lib/$ABI/libmobile.so
    for L in libc.so libm.so libdl.so liblog.so libandroid.so libvulkan.so; do
        $NM -D --defined-only "$SYS/usr/lib/$DIR/24/$L" 2>/dev/null | awk '{print $NF}' | sed 's/@.*//'
    done | sort -u > build/provided.txt
    $NM -D --undefined-only "$SO" | awk '{print $NF}' | sed 's/@.*//' | sort -u > build/undef.txt
    MISSING=$(comm -23 build/undef.txt build/provided.txt)
    if [ -n "$MISSING" ]; then
        echo "   $ABI: UNRESOLVED SYMBOLS -- this .so would fail to load:"
        echo "$MISSING" | sed 's/^/      /'
        exit 1
    fi
    echo "   $ABI: $(wc -l < build/undef.txt) imports, all resolved"
done

echo
echo "built: $(pwd)/mg-demo.apk  ($(du -h mg-demo.apk | cut -f1))"
