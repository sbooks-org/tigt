#!/bin/sh
set -eu
# Inputs are read-only bind mounts. All writes/build products stay in container.
mkdir -p /work/tigt /work/tigt-gfxreader
for project in tigt tigt-gfxreader; do
    tar -C "/source/$project" --exclude='./target' --exclude='./build' \
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
cargo fmt --all --check
cmake --install build --prefix /work/installed
cmake -S ci/consumer -B /work/consumer -DCMAKE_PREFIX_PATH=/work/installed
cmake --build /work/consumer --parallel 2
/work/consumer/consumer
printf '\nPASS Linux analyzer, C/Rust snapshots, renderer PTYs and installed C consumer\n'
