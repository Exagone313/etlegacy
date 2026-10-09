#!/bin/sh
#
# Compiles the Vulkan renderer shaders into spirv/shader_data.c
# Linux and macOS counterpart of compile.bat
#
# Requires glslangValidator (Vulkan SDK or the glslang-tools package)
# and a C compiler to build bin2hex.
#

set -e

cd "$(dirname "$0")"

CL="${GLSLANG:-glslangValidator}"
CC="${CC:-cc}"
OUTF=spirv/shader_data.c

if ! command -v "$CL" >/dev/null 2>&1; then
	echo "$CL not found, set GLSLANG to the glslangValidator path" >&2
	exit 1
fi

TMPDIR_SPV=$(mktemp -d)
trap 'rm -rf "$TMPDIR_SPV"' EXIT INT TERM

BH="$TMPDIR_SPV/bin2hex"
TMPF="$TMPDIR_SPV/data.spv"

"$CC" -O2 -o "$BH" bin2hex.c

mkdir -p spirv
rm -f "$OUTF"

# build <stage> <source> <array name> [defines...]
build() {
	stage=$1
	src=$2
	name=$3
	shift 3
	"$CL" -S "$stage" -V -o "$TMPF" "$src" "$@" >/dev/null
	"$BH" "$TMPF" "+$OUTF" "$name"
	rm -f "$TMPF"
}

# compile individual shaders

for f in *.vert; do
	build vert "$f" "${f%.vert}_vert_spv"
done

for f in *.frag; do
	build frag "$f" "${f%.frag}_frag_spv"
done

build vert light_vert.tmpl vert_light
build vert light_vert.tmpl vert_light_fog -DUSE_FOG
build frag light_frag.tmpl frag_light
build frag light_frag.tmpl frag_light_fog -DUSE_FOG 
build frag light_frag.tmpl frag_light_line -DUSE_LINE
build frag light_frag.tmpl frag_light_line_fog -DUSE_LINE -DUSE_FOG
build vert gen_vert.tmpl vert_tx0
build vert gen_vert.tmpl vert_tx0_fog -DUSE_FOG
build vert gen_vert.tmpl vert_tx0_env -DUSE_ENV
build vert gen_vert.tmpl vert_tx0_env_fog -DUSE_FOG -DUSE_ENV
build vert gen_vert.tmpl vert_tx0_ident1 -DUSE_CLX_IDENT
build vert gen_vert.tmpl vert_tx0_ident1_fog -DUSE_CLX_IDENT -DUSE_FOG
build vert gen_vert.tmpl vert_tx0_ident1_env -DUSE_CLX_IDENT -DUSE_ENV
build vert gen_vert.tmpl vert_tx0_ident1_env_fog -DUSE_CLX_IDENT -DUSE_FOG -DUSE_ENV
build vert gen_vert.tmpl vert_tx0_fixed -DUSE_FIXED_COLOR
build vert gen_vert.tmpl vert_tx0_fixed_fog -DUSE_FIXED_COLOR -DUSE_FOG
build vert gen_vert.tmpl vert_tx0_fixed_env -DUSE_FIXED_COLOR -DUSE_ENV
build vert gen_vert.tmpl vert_tx0_fixed_env_fog -DUSE_FIXED_COLOR -DUSE_FOG -DUSE_ENV
build vert gen_vert.tmpl vert_tx1 -DUSE_TX1
build vert gen_vert.tmpl vert_tx1_fog -DUSE_TX1 -DUSE_FOG
build vert gen_vert.tmpl vert_tx1_env -DUSE_TX1 -DUSE_ENV
build vert gen_vert.tmpl vert_tx1_env_fog -DUSE_TX1 -DUSE_FOG -DUSE_ENV
build vert gen_vert.tmpl vert_tx1_ident1 -DUSE_CLX_IDENT -DUSE_TX1
build vert gen_vert.tmpl vert_tx1_ident1_fog -DUSE_CLX_IDENT -DUSE_TX1 -DUSE_FOG
build vert gen_vert.tmpl vert_tx1_ident1_env -DUSE_CLX_IDENT -DUSE_TX1 -DUSE_ENV
build vert gen_vert.tmpl vert_tx1_ident1_env_fog -DUSE_CLX_IDENT -DUSE_TX1 -DUSE_FOG -DUSE_ENV
build vert gen_vert.tmpl vert_tx1_fixed -DUSE_FIXED_COLOR -DUSE_TX1
build vert gen_vert.tmpl vert_tx1_fixed_fog -DUSE_FIXED_COLOR -DUSE_TX1 -DUSE_FOG
build vert gen_vert.tmpl vert_tx1_fixed_env -DUSE_FIXED_COLOR -DUSE_TX1 -DUSE_ENV
build vert gen_vert.tmpl vert_tx1_fixed_env_fog -DUSE_FIXED_COLOR -DUSE_TX1 -DUSE_FOG -DUSE_ENV
build vert gen_vert.tmpl vert_tx1_cl -DUSE_CL1 -DUSE_TX1
build vert gen_vert.tmpl vert_tx1_cl_fog -DUSE_CL1 -DUSE_TX1 -DUSE_FOG
build vert gen_vert.tmpl vert_tx1_cl_env -DUSE_CL1 -DUSE_TX1 -DUSE_ENV
build vert gen_vert.tmpl vert_tx1_cl_env_fog -DUSE_CL1 -DUSE_TX1 -DUSE_ENV -DUSE_FOG
build vert gen_vert.tmpl vert_tx2 -DUSE_TX2
build vert gen_vert.tmpl vert_tx2_fog -DUSE_TX2 -DUSE_FOG
build vert gen_vert.tmpl vert_tx2_env -DUSE_TX2 -DUSE_ENV
build vert gen_vert.tmpl vert_tx2_env_fog -DUSE_TX2 -DUSE_ENV -DUSE_FOG
build vert gen_vert.tmpl vert_tx2_cl -DUSE_CL2 -DUSE_TX2
build vert gen_vert.tmpl vert_tx2_cl_fog -DUSE_CL2 -DUSE_TX2 -DUSE_FOG
build vert gen_vert.tmpl vert_tx2_cl_env -DUSE_CL2 -DUSE_TX2 -DUSE_ENV
build vert gen_vert.tmpl vert_tx2_cl_env_fog -DUSE_CL2 -DUSE_TX2 -DUSE_ENV -DUSE_FOG
build frag gen_frag.tmpl frag_tx0 -DUSE_ATEST
build frag gen_frag.tmpl frag_tx0_fog -DUSE_ATEST -DUSE_FOG
build frag gen_frag.tmpl frag_tx0_ident1 -DUSE_CLX_IDENT -DUSE_ATEST
build frag gen_frag.tmpl frag_tx0_ident1_fog -DUSE_CLX_IDENT -DUSE_ATEST -DUSE_FOG
build frag gen_frag.tmpl frag_tx0_fixed -DUSE_FIXED_COLOR -DUSE_ATEST
build frag gen_frag.tmpl frag_tx0_fixed_fog -DUSE_FIXED_COLOR -DUSE_ATEST -DUSE_FOG
build frag gen_frag.tmpl frag_tx0_ent -DUSE_ENT_COLOR -DUSE_ATEST
build frag gen_frag.tmpl frag_tx0_ent_fog -DUSE_ENT_COLOR -DUSE_ATEST -DUSE_FOG
build frag gen_frag.tmpl frag_tx0_df -DUSE_CLX_IDENT -DUSE_ATEST -DUSE_DF
build frag gen_frag.tmpl frag_tx1 -DUSE_TX1
build frag gen_frag.tmpl frag_tx1_fog -DUSE_TX1 -DUSE_FOG
build frag gen_frag.tmpl frag_tx1_ident1 -DUSE_CLX_IDENT -DUSE_TX1
build frag gen_frag.tmpl frag_tx1_ident1_fog -DUSE_CLX_IDENT -DUSE_TX1 -DUSE_FOG
build frag gen_frag.tmpl frag_tx1_fixed -DUSE_FIXED_COLOR -DUSE_TX1
build frag gen_frag.tmpl frag_tx1_fixed_fog -DUSE_FIXED_COLOR -DUSE_TX1 -DUSE_FOG
build frag gen_frag.tmpl frag_tx1_cl -DUSE_CL1 -DUSE_TX1
build frag gen_frag.tmpl frag_tx1_cl_fog -DUSE_CL1 -DUSE_TX1 -DUSE_FOG
build frag gen_frag.tmpl frag_tx2 -DUSE_TX2
build frag gen_frag.tmpl frag_tx2_fog -DUSE_TX2 -DUSE_FOG
build frag gen_frag.tmpl frag_tx2_cl -DUSE_CL2 -DUSE_TX2
build frag gen_frag.tmpl frag_tx2_cl_fog -DUSE_CL2 -DUSE_TX2 -DUSE_FOG

# ET global distance fog variants

build vert gen_vert.tmpl vert_tx0_gfog -DUSE_GLOBAL_FOG
build vert gen_vert.tmpl vert_tx1_gfog -DUSE_TX1 -DUSE_GLOBAL_FOG
build frag gen_frag.tmpl frag_tx0_gfog -DUSE_ATEST -DUSE_GLOBAL_FOG
build frag gen_frag.tmpl frag_tx1_gfog -DUSE_TX1 -DUSE_GLOBAL_FOG

echo "Generated $OUTF"
