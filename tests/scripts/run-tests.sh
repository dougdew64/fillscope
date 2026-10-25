#!/usr/bin/env bash
# Before a commit: builds Fillscope twice (normal, and with AddressSanitizer and
# UndefinedBehaviorSanitizer) and runs every test in both, in parallel. Exits nonzero if anything
# fails. Between edits, tests/scripts/iterate.py runs only the affected fast tests.
#
#   tests/scripts/run-tests.sh            from anywhere in the repository
#
# A warning in our own code fails both builds, as in CI (FILLSCOPE_WERROR=OFF turns that off).
set -euo pipefail

root=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
iterate=(python3 -I "$root/tests/scripts/iterate.py")
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT

# Holds a build directory's lock, the one iterate.py takes, until this script ends: two runs never
# build or test one build directory at once.
lock() {  # lock BUILD_DIR
    mkdir -p "$1"
    local fd
    exec {fd}>"$1/.fillscope-lock"
    if ! flock --nonblock "$fd"; then
        echo "waiting for another run using $1"
        flock "$fd"
    fi
}
lock "$root/build/debug"
lock "$root/build/asan"

build() {  # build BUILD_DIR [CMAKE OPTIONS...]
    local build_dir=$1
    shift
    echo "== $build_dir"
    cmake -S "$root" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
          -DFILLSCOPE_WERROR="${FILLSCOPE_WERROR:-ON}" "$@" > /dev/null
    cmake --build "$build_dir"
}

run_tests() {  # run_tests BUILD_DIR LOG: runs every test, keeping CTest's output in LOG
    local build_dir=$1 log=$2
    if ! ctest --test-dir "$build_dir" -j "$(nproc)" --no-tests=error --output-on-failure > "$log" 2>&1; then
        cat "$log"
        return 1
    fi
    grep -E 'tests passed' "$log"
    echo "slowest:"
    # sed -n, not head: head stops reading after five lines, and with pipefail the SIGPIPE that
    # gives sort would fail the whole run.
    grep -E 'Test +#' "$log" | sed -E 's/.*Test +#[0-9]+: ([^ ]+) .* ([0-9.]+) sec/  \2s \1/' | sort -rn | sed -n 1,5p
}

build "$root/build/debug"
# What the debug build's tests are about to run on, taken now: a file edited during the minutes
# that follow is then seen as changed by iterate.py, not recorded as tested.
"${iterate[@]}" --snapshot "$scratch/snapshot.json"
run_tests "$root/build/debug" "$scratch/debug.log"
build "$root/build/asan" -DFILLSCOPE_SANITIZE=ON
run_tests "$root/build/asan" "$scratch/asan.log"

# Every test must be registered through fillscope_test_program, or the selection and the checks
# below wouldn't know it.
for build_dir in "$root/build/debug" "$root/build/asan"; do
    FILLSCOPE_ITERATE_BUILD="$build_dir" "${iterate[@]}" --check-registration
done

# The sanitizer build only means something if both sanitizers are really in it, and the normal
# build only means something if neither is.
FILLSCOPE_ITERATE_BUILD="$root/build/asan" "${iterate[@]}" --check-sanitizers yes
FILLSCOPE_ITERATE_BUILD="$root/build/debug" "${iterate[@]}" --check-sanitizers no

# Everything passed, so iterate.py can start from here: from the files as they were tested, and
# without the programs whose tests were all skipped (they tested nothing).
"${iterate[@]}" --record-snapshot "$scratch/snapshot.json" "$scratch/debug.log"
echo "== all tests passed in both builds"
