#!/usr/bin/env bash
# Builds the lockstep experiment twice (normal, and with AddressSanitizer and
# UndefinedBehaviorSanitizer) and runs every test in both. Exits nonzero if anything fails.
#
#   experiments/lockstep/run-tests.sh            from the repository root
set -euo pipefail

root=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
source_dir="$root/experiments/lockstep"

run() {  # run BUILD_DIR [CMAKE OPTIONS...]
    local build_dir=$1
    shift
    echo "== $build_dir"
    cmake -S "$source_dir" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Debug "$@" > /dev/null
    cmake --build "$build_dir"
    ctest --test-dir "$build_dir" --output-on-failure
}

run "$root/build/lockstep"
run "$root/build/lockstep-asan" -DLOCKSTEP_SANITIZE=ON

# The sanitizer build only means something if the sanitizers are really in it.
for binary in lockstep_tests viewer_ui_tests engine; do
    if ! ldd "$root/build/lockstep-asan/$binary" | grep -q libasan; then
        echo "FAILED: $binary in the sanitizer build is not linked with AddressSanitizer" >&2
        exit 1
    fi
done
echo "== all tests passed in both builds"
