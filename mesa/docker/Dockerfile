# Host-side Mesa build (x86): the mbx Gallium driver against the sim winsys,
# which runs mbx/libmbxemu in-process. meson 1.4 or newer is needed (Mesa 26.1).
FROM ubuntu:24.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential ninja-build python3 python3-pip python3-mako python3-yaml python3-ply \
      python3-packaging bison flex pkg-config libdrm-dev libexpat1-dev zlib1g-dev libzstd-dev \
      libffi-dev ca-certificates git patch xz-utils \
    && pip3 install --break-system-packages 'meson>=1.4' \
    && rm -rf /var/lib/apt/lists/*
ARG UID=1000
ARG GID=1000
RUN (getent group $GID || groupadd -g $GID build) && \
    (getent passwd $UID && usermod -l build -d /home/build -m $(getent passwd $UID | cut -d: -f1) || \
     useradd -m -u $UID -g $GID build)
USER build
WORKDIR /work
