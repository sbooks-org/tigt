#!/bin/sh
set -eu
# Inputs are read-only bind mounts. All writes/build products stay in container.
mkdir -p /work/tigt /work/tigt-gfxreader
for project in tigt tigt-gfxreader; do
    tar -C "/source/$project" --exclude='./target' --exclude='./keyboard/target' --exclude='./build' \
        --exclude='./.git' -cf - . | tar -C "/work/$project" -xf -
done
# Cargo's pinned private dependency comes from the local committed repository,
# never an SSH agent or a credential copied into the container.
git config --global --add safe.directory /source/tigt-gfxreader
git config --global url.file:///source/tigt-gfxreader/.insteadOf \
    ssh://git@github.com/sbooks-org/tigt-gfxreader.git
cd /work/tigt-gfxreader
cargo test --all-targets
cargo fmt --all --check
cargo clippy --all-targets -- -D warnings
cd /work/tigt
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
cargo test --all-features
cargo test --no-default-features
cargo test --manifest-path keyboard/Cargo.toml --locked --offline
cargo fmt --manifest-path keyboard/Cargo.toml --check
cargo fmt --all --check
cmake --install build --prefix /work/installed
cmake -S ci/consumer -B /work/consumer -DCMAKE_PREFIX_PATH=/work/installed
cmake --build /work/consumer --parallel 2
/work/consumer/consumer
cmake -S . -B build-caca -DCMAKE_BUILD_TYPE=Debug -DTIGT_WITH_LIBCACA=ON
cmake --build build-caca --parallel 2
cmake --install build-caca --prefix /work/installed-caca
cmake -S ci/consumer -B /work/consumer-caca -DCMAKE_PREFIX_PATH=/work/installed-caca
cmake --build /work/consumer-caca --parallel 2
/work/consumer-caca/consumer
cmake -S . -B build-keyboard -DCMAKE_BUILD_TYPE=Debug -DTIGT_WITH_KEYBOARD=ON
cmake --build build-keyboard --parallel 2
cmake --install build-keyboard --prefix /work/installed-keyboard
cmake -S ci/consumer -B /work/consumer-keyboard -DCMAKE_PREFIX_PATH=/work/installed-keyboard
cmake --build /work/consumer-keyboard --parallel 2
/work/consumer-keyboard/consumer
/work/consumer-keyboard/keyboard-api
printf '\nPASS Linux analyzer, C/Rust snapshots, renderer/input PTYs and installed C consumers\n'
