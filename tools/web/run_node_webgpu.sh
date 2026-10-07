#!/bin/bash
# Build the Node test configuration of the web build and run the game with
# WebGPU (Dawn's Node bindings), dumping frames. See docs/WEB_BUILDING.md.
#
#   tools/web/run_node_webgpu.sh <name> <seconds> [extra game arguments]
#
# Environment (defaults in brackets):
#   EMSDK_ENV      path to emsdk_env.sh (required unless emcc is on PATH)
#   DAWN_MODULES   node_modules holding the "webgpu" and "ws" packages
#                  (npm install --prefix <dir> webgpu ws) (required)
#   XDG_DATA_HOME  directory containing LibertyRecomp/game (required)
#   NODE           Node 24+ binary [emsdk's node, else node]
#   VK_ICD_FILENAMES  Vulkan driver for Dawn [Chromium's SwiftShader from Playwright]
#   BUILD_DIR      [out/web]   OUT_DIR [/tmp/liberty-webgpu]
#   DUMP_INTERVAL  dump every Nth presented frame [30]
# Writes OUT_DIR/<name>.log and OUT_DIR/frames_<name>/f_NNNNNN.{pam,png}.
set -u
name=${1:?usage: run_node_webgpu.sh <name> <seconds> [args]}
seconds=${2:?usage: run_node_webgpu.sh <name> <seconds> [args]}
shift 2
root=$(cd "$(dirname "$0")/../.." && pwd)
build=${BUILD_DIR:-$root/out/web}
out=${OUT_DIR:-/tmp/liberty-webgpu}
: "${DAWN_MODULES:?set DAWN_MODULES to a node_modules with webgpu and ws}"
: "${XDG_DATA_HOME:?set XDG_DATA_HOME to the directory containing LibertyRecomp/game}"
if [ -n "${EMSDK_ENV:-}" ]; then source "$EMSDK_ENV" >/dev/null 2>&1; fi
if [ -z "${NODE:-}" ]; then
  NODE=$(ls -d "${EMSDK:-/nonexistent}"/node/*/bin/node 2>/dev/null | tail -1)
  NODE=${NODE:-node}
fi
if [ -z "${VK_ICD_FILENAMES:-}" ]; then
  VK_ICD_FILENAMES=$(ls /opt/pw-browsers/chromium-*/chrome-linux/vk_swiftshader_icd.json 2>/dev/null | head -1)
fi

cmake "$build" -DLIBERTY_WEB_NODERAWFS=ON >/dev/null || exit 1
ninja -C "$build" LibertyRecomp | grep -E "error|FAILED" && exit 1

frames="$out/frames_$name"
rm -rf "$frames" && mkdir -p "$frames"
NODE_PATH="$DAWN_MODULES" LIBERTY_DAWN_NODE="$DAWN_MODULES/webgpu" \
VK_ICD_FILENAMES="$VK_ICD_FILENAMES" XDG_DATA_HOME="$XDG_DATA_HOME" \
  timeout "$seconds" "$NODE" "$build/LibertyRecomp/LibertyRecomp.js" --diagnostics=true \
  --webgpu_frame_dump_path="$frames/f" --webgpu_frame_dump_interval="${DUMP_INTERVAL:-30}" \
  "$@" > "$out/$name.log" 2>&1
echo "exit=$? log=$out/$name.log frames=$frames"
ls "$frames"/*.pam >/dev/null 2>&1 && python3 "$root/tools/web/pam_to_png.py" "$frames"/*.pam >/dev/null
echo "GPU errors: $(grep -c 'GPU error' "$out/$name.log")"
grep "gta4-webgpu: frame=[0-9]* draws" "$out/$name.log" | tail -3
