#!/usr/bin/env bash
set -u

skip() {
    printf 'SKIP: %s\n' "$*" >&2
    exit 77
}

fail() {
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
workspace=$(cd -- "$script_dir/../.." && pwd -P)
fixture=${PROTON_FIXTURE_EXE:-$workspace/tests/fixtures/proton_fixture.exe}
accessHelper=${PROTON_FIXTURE_ACCESS_HELPER:-$workspace/tests/fixtures/proton_fixture_access.so}
python_bin=${PYTHON_BIN:-python3}
mode=test
case ${1:-} in
    --benchmark)
        mode=benchmark
        shift
        ;;
    --example)
        mode=example
        shift
        ;;
esac
[[ $# -eq 0 ]] || fail "unknown argument: $1"

if [[ $mode == benchmark && ${BENCHMARK_BATCH_PAGES:-sweep} == sweep ]]; then
    result=0
    for batch_pages in 1 16 256 4096; do
        BENCHMARK_BATCH_PAGES=$batch_pages \
            "$script_dir/run_proton_fixture.sh" --benchmark || {
                result=$?
                break
            }
    done
    exit "$result"
fi

temporary=
launcher_pid=
target_pid=

RunnerProcessIds() {
    local environmentFile
    local environmentMatch
    local processId

    [[ -n ${temporary:-} ]] || return

    if [[ $runner == proton ]]; then
        environmentMatch="STEAM_COMPAT_DATA_PATH=$temporary/compat"
    else
        environmentMatch="WINEPREFIX=$temporary/wineprefix"
    fi

    for environmentFile in /proc/[0-9]*/environ; do
        [[ -r $environmentFile ]] || continue
        if tr '\0' '\n' 2>/dev/null <"$environmentFile" |
            grep -Fqx -- "$environmentMatch"; then
            processId=${environmentFile#/proc/}
            processId=${processId%/environ}
            printf '%s\n' "$processId"
        fi
    done
}

TerminateRunnerProcesses() {
    local processId
    local attempt
    local alive
    local -a processIds=()

    mapfile -t processIds < <(RunnerProcessIds)
    if [[ ${#processIds[@]} -eq 0 ]]; then
        return
    fi

    kill -TERM "${processIds[@]}" 2>/dev/null || true
    for attempt in $(seq 1 20); do
        alive=false
        for processId in "${processIds[@]}"; do
            if kill -0 "$processId" 2>/dev/null; then
                alive=true
                break
            fi
        done
        [[ $alive == false ]] && break
        sleep 0.1
    done

    mapfile -t processIds < <(RunnerProcessIds)
    if [[ ${#processIds[@]} -gt 0 ]]; then
        kill -KILL "${processIds[@]}" 2>/dev/null || true
    fi
}

cleanup() {
    if [[ -n ${target_pid:-} && -d /proc/$target_pid ]]; then
        kill -TERM "$target_pid" 2>/dev/null || true
    fi
    if [[ -n ${launcher_pid:-} && -d /proc/$launcher_pid ]]; then
        kill -TERM "$launcher_pid" 2>/dev/null || true
    fi
    TerminateRunnerProcesses
    if [[ -n ${temporary:-} && -d $temporary && $temporary == /tmp/proton-connector.* ]]; then
        rm -rf -- "$temporary"
    fi
}
trap cleanup EXIT INT TERM

RunWithCleanup() {
    local result

    "$@"
    result=$?
    exit "$result"
}

[[ -f $fixture ]] || skip "fixture is missing; run 'make fixture' with MinGW-w64 installed"
[[ -f $accessHelper ]] || skip "fixture access helper is missing; run 'make fixture'"
case $mode in
    test)
        command -v "$python_bin" >/dev/null 2>&1 || skip "Python executable '$python_bin' is unavailable"
        "$python_bin" -c 'import memprocfs' >/dev/null 2>&1 || skip "memprocfs Python module is unavailable"
        ;;
    benchmark)
        benchmark_bin=${PROTON_BENCHMARK_BIN:-$workspace/build/proton_benchmark}
        [[ -x $benchmark_bin ]] || skip "benchmark executable is missing; run 'make benchmark'"
        ;;
    example)
        example_bin=${PROTON_EXAMPLE_BIN:-$workspace/build/proton_example}
        [[ -x $example_bin ]] || skip "example executable is missing; run 'make example'"
        ;;
esac

if [[ -n ${PROTON_BIN:-} ]]; then
    [[ -x $PROTON_BIN ]] || skip "PROTON_BIN is not executable: $PROTON_BIN"
    runner=proton
elif [[ -n ${WINE_BIN:-} ]]; then
    [[ -x $WINE_BIN ]] || skip "WINE_BIN is not executable: $WINE_BIN"
    runner=wine
else
    skip "set PROTON_BIN to a Proton launcher (or WINE_BIN for development)"
fi

temporary=$(mktemp -d /tmp/proton-connector.XXXXXXXX) || fail "mktemp failed"
[[ -n $temporary && -d $temporary && $temporary == /tmp/proton-connector.* ]] || fail "unsafe temporary directory"
mkdir -p "$temporary/compat" "$temporary/steam"
log=$temporary/fixture.log
readyFile=$temporary/fixture.ready
readyWindowsPath="Z:$readyFile"
fixtureLdPreload=$accessHelper
if [[ -n ${LD_PRELOAD:-} ]]; then
    fixtureLdPreload=$accessHelper:$LD_PRELOAD
fi
fixture_abs=$(cd -- "$(dirname -- "$fixture")" && pwd -P)/$(basename -- "$fixture")

if [[ $runner == proton ]]; then
    LD_PRELOAD=$fixtureLdPreload \
    STEAM_COMPAT_DATA_PATH=$temporary/compat \
    STEAM_COMPAT_CLIENT_INSTALL_PATH=${STEAM_COMPAT_CLIENT_INSTALL_PATH:-$temporary/steam} \
        "$PROTON_BIN" run "$fixture_abs" "$readyWindowsPath" >"$log" 2>&1 &
else
    LD_PRELOAD=$fixtureLdPreload WINEPREFIX=$temporary/wineprefix \
        "$WINE_BIN" "$fixture_abs" \
        "$readyWindowsPath" >"$log" 2>&1 &
fi
launcher_pid=$!

ready=
for _ in $(seq 1 300); do
    ready=$(sed -n '/^READY .* benchmark_size=/{p;q;}' "$readyFile" 2>/dev/null || true)
    [[ -n $ready ]] && break
    kill -0 "$launcher_pid" 2>/dev/null || {
        sed -n '1,120p' "$log" >&2
        fail "Proton/Wine fixture exited before READY"
    }
    sleep 0.1
done
[[ -n $ready ]] || {
    sed -n '1,120p' "$log" >&2
    fail "timed out waiting for fixture READY line"
}

candidates=()
mapfile -t runnerProcessIds < <(RunnerProcessIds)
for pid in "${runnerProcessIds[@]}"; do
    [[ -r /proc/$pid/maps ]] || continue
    if awk -v fixturePath="$fixture_abs" '
        {
            $1 = $2 = $3 = $4 = $5 = ""
            sub(/^ +/, "")
            if ($0 == fixturePath || $0 == fixturePath " (deleted)") {
                found = 1
                exit
            }
        }
        END { exit found ? 0 : 1 }
    ' "/proc/$pid/maps" 2>/dev/null; then
        candidates+=("$pid")
    fi
done

mapfile -t candidates < <(printf '%s\n' "${candidates[@]}" | sed '/^$/d' | sort -nu)
if [[ ${#candidates[@]} -eq 0 ]]; then
    skip "no same-user host process maps the fixture; check Flatpak/PID namespaces and /proc access"
fi
[[ ${#candidates[@]} -eq 1 ]] || fail "expected one fixture process, found ${#candidates[@]} (${candidates[*]-})"
target_pid=${candidates[0]}
sleep 0.2

declare -A metadata
for field in $ready; do
    if [[ $field == *=* ]]; then
        metadata[${field%%=*}]=${field#*=}
    fi
done
required_fields=(address size ro rw rx)
if [[ $mode == benchmark ]]; then
    required_fields+=(benchmark benchmark_size)
fi
for required in "${required_fields[@]}"; do
    [[ -n ${metadata[$required]:-} ]] || fail "READY line lacks '$required'"
done

if [[ $mode == example ]]; then
    RunWithCleanup "$example_bin" "$target_pid" "${metadata[address]}" \
        "${EXAMPLE_BYTES:-256}"
fi

if [[ $mode == benchmark ]]; then
    RunWithCleanup "$benchmark_bin" "$target_pid" "${metadata[benchmark]}" \
        "${metadata[benchmark_size]}" "${BENCHMARK_SECONDS:-5}" \
        "${BENCHMARK_BATCH_PAGES:-256}"
fi

test_args=(
    --pid "$target_pid"
    --address "${metadata[address]}"
    --size "${metadata[size]}"
    --ro "${metadata[ro]}"
    --rw "${metadata[rw]}"
    --rx "${metadata[rx]}"
)
if [[ ${PROTON_WRITE_TEST:-0} == 1 ]]; then
    test_args+=(--test-write)
fi

RunWithCleanup "$python_bin" "$script_dir/test_memprocfs_proton.py" \
    "${test_args[@]}"
