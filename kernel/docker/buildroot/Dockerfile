# Buildroot build host for the N810 rootfs.
FROM ubuntu:24.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential file wget cpio unzip rsync bc python3 perl git ca-certificates \
      xz-utils bzip2 patch debianutils libncurses-dev \
    && rm -rf /var/lib/apt/lists/*
ARG UID=1000
ARG GID=1000
RUN (getent group $GID || groupadd -g $GID build) && \
    (getent passwd $UID && usermod -l build -d /home/build -m $(getent passwd $UID | cut -d: -f1) || \
     useradd -m -u $UID -g $GID build)
USER build
WORKDIR /work
