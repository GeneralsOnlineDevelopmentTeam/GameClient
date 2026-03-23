#!/bin/bash
set -euo pipefail
cd /build/cnc

# Use a fresh Wine prefix owned by the current user
export WINEPREFIX="/tmp/wineprefix"
mkdir -p "$WINEPREFIX"

# Initialize Wine prefix
wineboot --init 2>/dev/null || true

# MSVC paths - replicate what msvcenv.sh sets
BASE="Z:\\build\\tools\\msvc"
MSVCVER="14.50.35717"
SDKVER="10.0.26100.0"
ARCH="x86"

MSVCDIR="${BASE}\\VC\\Tools\\MSVC\\${MSVCVER}"
SDKBASE="${BASE}\\Windows Kits\\10"
SDKINCLUDE="${SDKBASE}\\Include\\${SDKVER}"
SDKLIB="${SDKBASE}\\Lib\\${SDKVER}"

# Real Windows executables (not the Unix wrapper scripts)
CL_WIN="${MSVCDIR}\\bin\\Hostx64\\${ARCH}\\cl.exe"
LINK_WIN="${MSVCDIR}\\bin\\Hostx64\\${ARCH}\\link.exe"

# Environment for cl.exe to find headers and libraries
export INCLUDE="${MSVCDIR}\\atlmfc\\include;${MSVCDIR}\\include;${SDKINCLUDE}\\shared;${SDKINCLUDE}\\ucrt;${SDKINCLUDE}\\um;${SDKINCLUDE}\\winrt"
export LIB="${MSVCDIR}\\atlmfc\\lib\\${ARCH};${MSVCDIR}\\lib\\${ARCH};${SDKLIB}\\ucrt\\${ARCH};${SDKLIB}\\um\\${ARCH}"
export LIBPATH="${LIB}"

# Wine needs the Hostx64/x64 dir in PATH for DLLs (vcruntime140.dll etc.)
export WINEPATH="${MSVCDIR}\\bin\\Hostx64\\${ARCH};${MSVCDIR}\\bin\\Hostx64\\x64;${SDKBASE}\\bin\\${SDKVER}\\x64"
export WINEDLLOVERRIDES="vcruntime140=n;vcruntime140_1=n"

BUILD_DIR="/build/cnc/build/${PRESET}"
#
# Symlink "Windows Kits" to a path without spaces so Wine doesn't generate
# 8.3 short names (e.g. WIND~2DP) that can't be resolved on Linux filesystems.
WINKITS="/build/tools/msvc/Windows Kits"
WINKITS_LINK="/build/tools/msvc/WindowsKits"
if [ -d "$WINKITS" ] && [ ! -e "$WINKITS_LINK" ]; then
	ln -s "$WINKITS" "$WINKITS_LINK"
fi

RC_COMPILER="Z:/build/tools/msvc/WindowsKits/10/bin/${SDKVER}/x64/rc.exe"
RC_INCLUDE="Z:/build/tools/msvc/WindowsKits/10/Include/${SDKVER}"
RC_FLAGS="-I \"${RC_INCLUDE}/um\" -I \"${RC_INCLUDE}/shared\""

# Configure if needed
if [ "${FORCE_CMAKE:-}" = "true" ] || [ ! -f "${BUILD_DIR}/build.ninja" ]; then
	rm -f "${BUILD_DIR}/CMakeCache.txt"

	wine /build/tools/cmake/bin/cmake.exe \
		--preset ${PRESET} \
		-DCMAKE_SYSTEM="Windows" \
		-DCMAKE_SYSTEM_NAME="Windows" \
		-DCMAKE_SIZEOF_VOID_P=4 \
		-DCMAKE_MAKE_PROGRAM="Z:/build/tools/ninja.exe" \
		-DCMAKE_C_COMPILER="${CL_WIN}" \
		-DCMAKE_CXX_COMPILER="${CL_WIN}" \
		-DCMAKE_LINKER="${LINK_WIN}" \
		-DCMAKE_C_COMPILER_ID=MSVC \
		-DCMAKE_CXX_COMPILER_ID=MSVC \
		-DCMAKE_C_COMPILER_VERSION=19.50.35726 \
		-DCMAKE_CXX_COMPILER_VERSION=19.50.35726 \
		-DMSVC_VERSION=1950 \
		-DMSVC=1 \
		-DCMAKE_C_STANDARD_COMPUTED_DEFAULT=17 \
		-DCMAKE_C_EXTENSIONS_COMPUTED_DEFAULT=OFF \
		-DCMAKE_CXX_STANDARD_COMPUTED_DEFAULT=20 \
		-DCMAKE_CXX_EXTENSIONS_COMPUTED_DEFAULT=OFF \
		-DCMAKE_SUPPRESS_REGENERATION=ON \
		-DCMAKE_C_COMPILER_WORKS=1 \
		-DCMAKE_CXX_COMPILER_WORKS=1 \
		-DGIT_EXECUTABLE="Z:/build/tools/git/git.exe" \
		-DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
		-DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
		-DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
		-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY \
		-DCMAKE_RC_COMPILER="${RC_COMPILER}" \
		-DCMAKE_RC_FLAGS="${RC_FLAGS}" \
		-B "${BUILD_DIR}"
fi

# Fix PCH paths: CMake generates Unix paths for /FI, /Yc, /Fp flags.
# MSVC under Wine needs Z: drive prefix.
echo "Fixing PCH paths for Wine..."
sed -i \
	-e 's|/Yc/build/cnc/|/YcZ:/build/cnc/|g' \
	-e 's|/Yu/build/cnc/|/YuZ:/build/cnc/|g' \
	-e 's|/Fp/build/cnc/|/FpZ:/build/cnc/|g' \
	-e 's|/FI/build/cnc/|/FIZ:/build/cnc/|g' \
	-e 's| -c /build/cnc/| -c Z:/build/cnc/|g' \
	-e 's| -c \\build\\cnc\\| -c Z:\\build\\cnc\\|g' \
	-e 's| -I \\build\\cnc\\| -I Z:\\build\\cnc\\|g' \
	-e 's| -I /build/cnc/| -I Z:/build/cnc/|g' \
	-e 's|-LIBPATH:\\build\\cnc\\|-LIBPATH:Z:\\build\\cnc\\|g' \
	-e 's|-LIBPATH:/build/cnc/|-LIBPATH:Z:/build/cnc/|g' \
	"${BUILD_DIR}/build.ninja"

# Remove the CMake regeneration rule so Ninja doesn't overwrite our fixes
sed -i '/^build build.ninja:/,/^$/d' "${BUILD_DIR}/build.ninja"

FIXED=$(grep -c 'Z:/build/cnc/' "${BUILD_DIR}/build.ninja" || true)
echo "Fixed paths: ${FIXED} occurrences with Z: prefix"

# Build - pass MSVC environment into the Windows cmd session
cd "${BUILD_DIR}"
wine cmd /c "set TMP=Z:\build\tmp& set TEMP=Z:\build\tmp& set INCLUDE=${INCLUDE}& set LIB=${LIB}& set LIBPATH=${LIBPATH}& set PATH=${WINEPATH};%PATH%& Z:\build\tools\ninja.exe ${MAKE_TARGET:-z_generals}"
