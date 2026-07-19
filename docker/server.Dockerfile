# Runtime image for the dedicated servers.
#
# The binaries are built on the host and bind-mounted in, so this image carries
# no toolchain -- only the shared libraries they link against. It's based on
# Arch to match the host's glibc: a binary built against a newer glibc than the
# runtime provides will not start.
FROM archlinux:base

RUN pacman -Sy --noconfirm --needed \
        postgresql-libs \
        libsodium \
        gcc-libs \
    && pacman -Scc --noconfirm

# Nothing is copied in: /srv/bin is a bind mount of the host's build output.
WORKDIR /srv
