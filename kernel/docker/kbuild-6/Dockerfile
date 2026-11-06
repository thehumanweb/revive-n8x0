# n810-kbuild:6: Ubuntu 24.04 with GCC 13 arm-linux-gnueabi, for 6.x kernels.
FROM ubuntu:24.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential bc bison flex libssl-dev libelf-dev cpio kmod python3 lzop xz-utils \
      gcc-arm-linux-gnueabi binutils-arm-linux-gnueabi ca-certificates \
    && rm -rf /var/lib/apt/lists/*
ARG UID=1000
ARG GID=1000
RUN (getent group $GID || groupadd -g $GID build) && \
    (getent passwd $UID || useradd -m -u $UID -g $GID build)
USER $UID:$GID
WORKDIR /work
