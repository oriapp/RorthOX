# OCI index digest resolved from Docker Hub registry API (ubuntu:24.04, 2026-11-07).
# Ubuntu archive snapshot: https://snapshot.ubuntu.com/ubuntu/20261101T000000Z/
FROM ubuntu:24.04@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852 AS toolchain
ARG DEBIAN_FRONTEND=noninteractive
RUN printf '%s\n' \
      'deb [check-valid-until=no signed-by=/usr/share/keyrings/ubuntu-archive-keyring.gpg] https://snapshot.ubuntu.com/ubuntu/20261101T000000Z/ noble main universe restricted multiverse' \
      'deb [check-valid-until=no signed-by=/usr/share/keyrings/ubuntu-archive-keyring.gpg] https://snapshot.ubuntu.com/ubuntu/20261101T000000Z/ noble-updates main universe restricted multiverse' \
      'deb [check-valid-until=no signed-by=/usr/share/keyrings/ubuntu-archive-keyring.gpg] https://snapshot.ubuntu.com/ubuntu/20261101T000000Z/ noble-security main universe restricted multiverse' \
      > /etc/apt/sources.list && rm -f /etc/apt/sources.list.d/ubuntu.sources && \
    apt-get -o Acquire::https::Verify-Peer=false update && \
    apt-get -o Acquire::https::Verify-Peer=false install -y --no-install-recommends ca-certificates && \
    apt-get --error-on=any update && apt-get install -y --no-install-recommends \
      build-essential bison flex libgmp3-dev libmpc-dev libmpfr-dev texinfo \
      bash git ca-certificates curl xz-utils tar gzip && \
    rm -rf /var/lib/apt/lists/*
COPY configure.sh /opt/rorthox/configure.sh
RUN PREFIX=/opt/cross SRC_DIR=/opt/toolchain-src JOBS=2 CROSS_TARGET=i686-elf bash /opt/rorthox/configure.sh && \
    PREFIX=/opt/cross SRC_DIR=/opt/toolchain-src JOBS=2 CROSS_TARGET=x86_64-elf bash /opt/rorthox/configure.sh && \
    rm -rf /opt/toolchain-src

FROM ubuntu:24.04@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852
ARG DEBIAN_FRONTEND=noninteractive
RUN printf '%s\n' \
      'deb [check-valid-until=no signed-by=/usr/share/keyrings/ubuntu-archive-keyring.gpg] https://snapshot.ubuntu.com/ubuntu/20261101T000000Z/ noble main universe restricted multiverse' \
      'deb [check-valid-until=no signed-by=/usr/share/keyrings/ubuntu-archive-keyring.gpg] https://snapshot.ubuntu.com/ubuntu/20261101T000000Z/ noble-updates main universe restricted multiverse' \
      'deb [check-valid-until=no signed-by=/usr/share/keyrings/ubuntu-archive-keyring.gpg] https://snapshot.ubuntu.com/ubuntu/20261101T000000Z/ noble-security main universe restricted multiverse' \
      > /etc/apt/sources.list && rm -f /etc/apt/sources.list.d/ubuntu.sources && \
    apt-get -o Acquire::https::Verify-Peer=false update && \
    apt-get -o Acquire::https::Verify-Peer=false install -y --no-install-recommends ca-certificates && \
    apt-get --error-on=any update && apt-get install -y --no-install-recommends \
      build-essential bison flex libgmp3-dev libmpc-dev libmpfr-dev texinfo \
      bash git ca-certificates curl xz-utils tar gzip coreutils findutils \
      diffutils grep sed gawk util-linux procps nasm mtools dosfstools \
      qemu-system-x86 qemu-utils gdb gdb-multiarch python3 \
      grub-common grub-pc-bin grub-efi-amd64-bin grub-efi-ia32-bin xorriso ovmf && \
    rm -rf /var/lib/apt/lists/*
COPY --from=toolchain /opt/cross /opt/cross
ARG RORTHOX_REVISION=""
ARG RORTHOX_INPUT_HASH=""
LABEL org.opencontainers.image.title="RorthOX build" \
      org.opencontainers.image.source="https://github.com/oriapp/RorthOX" \
      org.opencontainers.image.licenses="GPL-2.0" \
      org.opencontainers.image.revision="$RORTHOX_REVISION" \
      org.rorthox.image.input-hash="$RORTHOX_INPUT_HASH"
# Ubuntu reserves uid/gid 1000 for ubuntu; ci/run.sh sets --user to the host uid:gid.
RUN mkdir /work && chown ubuntu:ubuntu /work && touch /opt/rorthox-build-image
ENV PATH="/opt/cross/bin:${PATH}" RORTHOX_IN_IMAGE=1
USER ubuntu
WORKDIR /work
