FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    build-essential \
    gcc-aarch64-linux-gnu \
    g++-aarch64-linux-gnu \
    linux-libc-dev-arm64-cross \
    && rm -rf /var/lib/apt/lists/*

ENV CC=aarch64-linux-gnu-gcc
ENV CXX=aarch64-linux-gnu-g++

WORKDIR /build

COPY src/ src/
COPY Makefile .

CMD ["make"]
