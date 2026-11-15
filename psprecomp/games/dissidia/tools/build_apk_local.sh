#!/bin/bash
# Build the tag-team APK on this host (no desktop needed): ~/dissidia/tt/dissidia_tt.apk
# usage: build_apk.sh [source psprecomp dir (default ~/code/dissidia-tagteam/psprecomp)] [work folder with output/ + overlays/ (default ~/dissidia/work_tagteam)]
set -eo pipefail
SRC=${1:-$HOME/code/dissidia-tagteam/psprecomp}; W=${2:-$HOME/dissidia/work_tagteam}
V=$HOME/dissidia/android_vk; A=$HOME/dissidia/android_local/tt
mkdir -p $A && cd $A
ln -sfn $V/and_deps29 and_deps29; ln -sfn $V/sdk sdk
export PATH=$HOME/android-build/jdk/bin:$PATH
nice env CMAKE_EXTRA=-DPSPRECOMP_ALLOW_STALE=ON ANDROID_DIR=$A RUNTIME_SRC=$SRC/runtime bash $SRC/android/build_game_and.sh $W 2>&1 | tail -6
python3 -I $SRC/android/pack_shaders.py -o $A/shaders.pack
NDK=$V/sdk/ndk/27.2.12479018
BIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
L=$V/and_deps29/deps/lib
$BIN/llvm-strip -o $A/libmain_stripped.so $A/game_and/libmain.so
python3 -I $SRC/android/apk_pack.py --skeleton $V/skeleton.apk --key $SRC/android/debug_key.json \
  --lib lib/arm64-v8a/libmain.so=$A/libmain_stripped.so --lib lib/arm64-v8a/libSDL2.so=$L/libSDL2.so \
  --lib lib/arm64-v8a/libc++_shared.so=$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so \
  --lib lib/arm64-v8a/libhook_impl.so=$L/libhook_impl.so --lib lib/arm64-v8a/libmain_hook.so=$L/libmain_hook.so \
  --lib lib/arm64-v8a/libfile_redirect_hook.so=$L/libfile_redirect_hook.so --lib lib/arm64-v8a/libgsl_alloc_hook.so=$L/libgsl_alloc_hook.so \
  $([ -f $A/shaders.pack ] && echo --lib assets/shaders.pack=$A/shaders.pack) -o $HOME/dissidia/tt/dissidia_tt.apk
ls -la $HOME/dissidia/tt/dissidia_tt.apk
echo APK_BUILD_DONE
