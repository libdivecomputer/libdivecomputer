FROM debian:stable-slim

RUN apt-get update && apt-get install -y \
    build-essential pkg-config libusb-1.0-0-dev \
    autoconf automake libtool git && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy local source
COPY . .

# build
RUN autoreconf --install && ./configure && make

WORKDIR /app/examples
ENTRYPOINT ["./dctool"]