#!/usr/bin/env bash
# Before a commit: builds Fillscope twice (normal, and with AddressSanitizer and
# UndefinedBehaviorSanitizer) and runs every test in both, in parallel. Exits nonzero if anything
# fails. Between edits, tests/scripts/iterate.py runs only the affected fast tests.
#
#   tests/scripts/run-tests.sh            from anywhere in the repository
set -euo pipefail

root=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)

run() {  # run BUILD_DIR [CMAKE OPTIONS...]
    local build_dir=$1
    shift
    echo "== $build_dir"
    cmake -S "$root" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Debug "$@" > /dev/null
    cmake --build "$build_dir"
    local output
    if ! output=$(ctest --test-dir "$build_dir" -j "$(nproc)" --no-tests=error --output-on-failure 2>&1); then
        echo "$output"
        return 1
    fi
    echo "$output" | grep -E 'tests passed'
    echo "slowest:"
    echo "$output" | grep -E 'Test +#' | sed -E 's/.*Test +#[0-9]+: ([^ ]+) .* ([0-9.]+) sec/  \2s \1/' | sort -rn | head -5
}

run "$root/build/debug"
run "$root/build/asan" -DFILLSCOPE_SANITIZE=ON

# The sanitizer build only means something if the sanitizers are really in it: every test program
# (from the manifest, so a new one is covered without editing this) and the two programs.
for binary in $(awk '{print $3}' "$root/build/asan/test-manifest.txt") \
              "$root/build/asan/frame_source" "$root/build/asan/viewer"; do
    if ! ldd "$binary" | grep -q libasan; then
        echo "FAILED: $binary in the sanitizer build is not linked with AddressSanitizer" >&2
        exit 1
    fi
done

# Everything passed, so iterate.py can start from here.
python3 -I "$root/tests/scripts/iterate.py" --record-all
echo "== all tests passed in both builds"
