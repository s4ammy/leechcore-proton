#!/usr/bin/env bash
set -euo pipefail

fail() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

status() {
    printf '==> %s\n' "$*"
}

require_command() {
    command -v "$1" >/dev/null 2>&1 || fail "required command '$1' was not found"
}

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
workspace=$(cd -- "$script_dir/.." && pwd -P)

[[ $# -ge 1 ]] || fail "internal usage: run_fixture_tool.sh <example|benchmark> [arguments]"
tool=$1
shift

case $tool in
    example)
        [[ $# -le 1 ]] || fail "usage: ./example.sh [byte-count]"
        example_bytes=${1:-${EXAMPLE_BYTES:-256}}
        ;;
    benchmark)
        [[ $# -le 2 ]] || fail "usage: ./benchmark.sh [seconds] [batch-pages|sweep]"
        benchmark_seconds=${1:-${BENCHMARK_SECONDS:-5}}
        benchmark_batch_pages=${2:-${BENCHMARK_BATCH_PAGES:-sweep}}
        ;;
    *)
        fail "unknown tool '$tool'"
        ;;
esac

require_command make
require_command install

native_cc=${CC:-cc}
read -r -a native_cc_words <<<"$native_cc"
[[ ${#native_cc_words[@]} -gt 0 ]] || fail "CC is empty"
require_command "${native_cc_words[0]}"

leechcore_dir=${LEECHCORE_DIR:-$workspace/references/LeechCore}
leechcore_so=${LEECHCORE_SO:-$leechcore_dir/files/leechcore.so}
memprocfs_dir=${MEMPROCFS_DIR:-$workspace/references/MemProcFS}
vmm_so=${VMM_SO:-$memprocfs_dir/files/vmm.so}
runtime_dir=$workspace/build
fixture=$workspace/tests/fixtures/proton_fixture.exe
plugin=$workspace/leechcore_device_proton.so

if [[ ! -f $leechcore_so ]]; then
    [[ -z ${LEECHCORE_SO+x} ]] || fail "LEECHCORE_SO does not exist: $leechcore_so"
    [[ -f $leechcore_dir/leechcore/Makefile ]] ||
        fail "LeechCore source is missing: $leechcore_dir/leechcore"
    status "Building LeechCore"
    make -C "$leechcore_dir/leechcore" leechcore "CC=$native_cc" ||
        fail "LeechCore build failed (its Linux build requires pkg-config and libusb-1.0 development files)"
    [[ -f $leechcore_so ]] || fail "LeechCore build did not produce $leechcore_so"
fi

if [[ ! -f $vmm_so ]]; then
    [[ -z ${VMM_SO+x} ]] || fail "VMM_SO does not exist: $vmm_so"
    [[ -f $memprocfs_dir/vmm/Makefile ]] ||
        fail "MemProcFS VMM source is missing: $memprocfs_dir/vmm"
    mkdir -p "$memprocfs_dir/files"
    memprocfs_leechcore=$memprocfs_dir/files/leechcore.so
    if [[ ! -e $memprocfs_leechcore || ! $leechcore_so -ef $memprocfs_leechcore ]]; then
        install -m 0755 "$leechcore_so" "$memprocfs_leechcore"
    fi
    status "Building MemProcFS VMM library"
    make -C "$memprocfs_dir/vmm" vmm "CC=$native_cc" ||
        fail "MemProcFS VMM library build failed"
    [[ -f $vmm_so ]] || fail "MemProcFS build did not produce $vmm_so"
fi

if [[ -n ${MINGW_CC:-} ]]; then
    mingw_cc=$MINGW_CC
elif command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
    mingw_cc=x86_64-w64-mingw32-gcc
elif command -v zig >/dev/null 2>&1; then
    mingw_cc='zig cc -target x86_64-windows-gnu'
else
    fail "a Windows cross-compiler is required (install x86_64-w64-mingw32-gcc or Zig, or set MINGW_CC)"
fi
read -r -a mingw_cc_words <<<"$mingw_cc"
[[ ${#mingw_cc_words[@]} -gt 0 ]] || fail "MINGW_CC is empty"
require_command "${mingw_cc_words[0]}"

status "Building the connector, $tool, and Windows fixture"
make -C "$workspace" "$tool" fixture \
    "CC=$native_cc" \
    "MINGW_CC=$mingw_cc" \
    "LEECHCORE_DIR=$leechcore_dir" \
    "LEECHCORE_SO=$leechcore_so" \
    "MEMPROCFS_DIR=$memprocfs_dir" \
    "VMM_SO=$vmm_so"

tool_binary=$runtime_dir/proton_$tool
[[ -x $tool_binary ]] || fail "build did not produce $tool_binary"
[[ -x $fixture ]] || fail "build did not produce $fixture"
[[ -f $plugin ]] || fail "build did not produce $plugin"

status "Staging the MemProcFS runtime in $runtime_dir"
mkdir -p "$runtime_dir"
if [[ ! -e $runtime_dir/leechcore.so || ! $leechcore_so -ef $runtime_dir/leechcore.so ]]; then
    install -m 0755 "$leechcore_so" "$runtime_dir/leechcore.so"
fi
if [[ ! -e $runtime_dir/vmm.so || ! $vmm_so -ef $runtime_dir/vmm.so ]]; then
    install -m 0755 "$vmm_so" "$runtime_dir/vmm.so"
fi
if [[ ! -e $runtime_dir/leechcore_device_proton.so ||
      ! $plugin -ef $runtime_dir/leechcore_device_proton.so ]]; then
    install -m 0755 "$plugin" "$runtime_dir/leechcore_device_proton.so"
fi

find_proton() {
    local user_home
    local -a candidates=()

    if command -v getent >/dev/null 2>&1; then
        user_home=$(getent passwd "$(id -u)" | awk -F: 'NR == 1 { print $6 }')
    else
        user_home=
    fi
    [[ -n $user_home ]] || return 1

    shopt -s nullglob
    candidates+=(
        "$user_home"/.local/share/Steam/steamapps/common/Proton\ */proton
        "$user_home"/.local/share/Steam/compatibilitytools.d/*/proton
        "$user_home"/.steam/root/steamapps/common/Proton\ */proton
        "$user_home"/.steam/root/compatibilitytools.d/*/proton
        "$user_home"/.steam/steam/steamapps/common/Proton\ */proton
        "$user_home"/.steam/steam/compatibilitytools.d/*/proton
        "$user_home"/.var/app/com.valvesoftware.Steam/data/Steam/steamapps/common/Proton\ */proton
        "$user_home"/.var/app/com.valvesoftware.Steam/data/Steam/compatibilitytools.d/*/proton
    )
    shopt -u nullglob

    [[ ${#candidates[@]} -gt 0 ]] || return 1
    printf '%s\n' "${candidates[@]}" | sort -V | tail -n 1
}

if [[ -n ${PROTON_BIN:-} ]]; then
    [[ -x $PROTON_BIN ]] || fail "PROTON_BIN is not executable: $PROTON_BIN"
elif [[ -n ${WINE_BIN:-} ]]; then
    [[ -x $WINE_BIN ]] || fail "WINE_BIN is not executable: $WINE_BIN"
elif command -v proton >/dev/null 2>&1; then
    PROTON_BIN=$(command -v proton)
    export PROTON_BIN
elif detected_proton=$(find_proton); then
    PROTON_BIN=$detected_proton
    export PROTON_BIN
elif command -v wine64 >/dev/null 2>&1; then
    WINE_BIN=$(command -v wine64)
    export WINE_BIN
elif command -v wine >/dev/null 2>&1; then
    WINE_BIN=$(command -v wine)
    export WINE_BIN
else
    fail "Proton or Wine was not found; install one or set PROTON_BIN/WINE_BIN"
fi

if [[ -n ${PROTON_BIN:-} && -z ${STEAM_COMPAT_CLIENT_INSTALL_PATH:-} ]]; then
    case $PROTON_BIN in
        */steamapps/common/*/proton)
            STEAM_COMPAT_CLIENT_INSTALL_PATH=${PROTON_BIN%%/steamapps/common/*}
            export STEAM_COMPAT_CLIENT_INSTALL_PATH
            ;;
        */compatibilitytools.d/*/proton)
            STEAM_COMPAT_CLIENT_INSTALL_PATH=${PROTON_BIN%%/compatibilitytools.d/*}
            export STEAM_COMPAT_CLIENT_INSTALL_PATH
            ;;
    esac
fi

export LD_LIBRARY_PATH="$runtime_dir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PROTON_FIXTURE_EXE=$fixture

if [[ $tool == example ]]; then
    export PROTON_EXAMPLE_BIN=$tool_binary
    export EXAMPLE_BYTES=$example_bytes
    status "Launching the fixture and C example"
    exec "$workspace/tests/integration/run_proton_fixture.sh" --example
fi

export PROTON_BENCHMARK_BIN=$tool_binary
export BENCHMARK_SECONDS=$benchmark_seconds
export BENCHMARK_BATCH_PAGES=$benchmark_batch_pages
status "Launching the fixture and read benchmark"
exec "$workspace/tests/integration/run_proton_fixture.sh" --benchmark
