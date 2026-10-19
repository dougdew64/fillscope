#!/usr/bin/env bash
# Builds Fillscope twice (normal, and with AddressSanitizer and UndefinedBehaviorSanitizer) and runs
# every test in both. Exits nonzero if anything fails.
#
#   tests/run-tests.sh            from anywhere in the repository
set -euo pipefail

root=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)

run() {  # run BUILD_DIR [CMAKE OPTIONS...]
    local build_dir=$1
    shift
    echo "== $build_dir"
    cmake -S "$root" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Debug "$@" > /dev/null
    cmake --build "$build_dir"
    ctest --test-dir "$build_dir" --output-on-failure
}

run "$root/build/debug"
run "$root/build/asan" -DFILLSCOPE_SANITIZE=ON

# The sanitizer build only means something if the sanitizers are really in it.
for binary in fillscope_tests viewer_ui_tests frame_source viewer; do
    if ! ldd "$root/build/asan/$binary" | grep -q libasan; then
        echo "FAILED: $binary in the sanitizer build is not linked with AddressSanitizer" >&2
        exit 1
    fi
done
echo "== all tests passed in both builds"
