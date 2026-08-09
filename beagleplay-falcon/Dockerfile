FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# Yocto Scarthgap (5.0 LTS) host build dependencies, per meta-ti / TI Processor SDK
# for BeaglePlay (AM62x), plus tools meta-ti needs for boot media image creation.
RUN apt-get update && apt-get install -y --no-install-recommends \
    gawk wget git diffstat unzip texinfo gcc build-essential chrpath socat cpio \
    python3 python3-pip python3-pexpect xz-utils debianutils iputils-ping \
    python3-git python3-jinja2 python3-subunit zstd liblz4-tool file locales \
    libacl1 lz4 bc rsync curl ca-certificates sudo vim less \
    dosfstools mtools parted \
    && locale-gen en_US.UTF-8 \
    && rm -rf /var/lib/apt/lists/*

ENV LANG=en_US.UTF-8 LANGUAGE=en_US:en LC_ALL=en_US.UTF-8

# Bitbake refuses to run as root — create a non-root build user.
# GID/UID are left to useradd's own allocation; OrbStack auto-maps bind-mount
# ownership across the VM boundary, so matching the host UID isn't necessary.
ARG USERNAME=builder
RUN useradd -m -U -s /bin/bash ${USERNAME} \
    && echo "${USERNAME} ALL=(ALL) NOPASSWD:ALL" >> /etc/sudoers

USER ${USERNAME}
WORKDIR /home/${USERNAME}/yocto

CMD ["/bin/bash"]
