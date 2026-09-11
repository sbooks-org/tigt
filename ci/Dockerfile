FROM rust:1-bookworm
RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake pkg-config libncurses-dev libpng-dev libcaca-dev \
    && rm -rf /var/lib/apt/lists/*
RUN rustup component add rustfmt clippy
ENV CARGO_NET_GIT_FETCH_WITH_CLI=true
COPY run-linux.sh /usr/local/bin/tigt-ci
ENTRYPOINT ["sh", "/usr/local/bin/tigt-ci"]
