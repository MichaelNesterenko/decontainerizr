#!/usr/bin/env bash

set -eEuo pipefail
shopt -s inherit_errexit

cat <<EOF | docker buildx build -t decontainerizr-build -
from archlinux

run pacman --noconfirm -Syu base-devel cmake
EOF

selfdir="$(dirname "$(readlink -f "$0")")"
tar -C "$selfdir" -c . | docker run -i --rm decontainerizr-build bash -c '{ mkdir /build /build-result && tar -C /build -x && cd /build-result && cmake /build && cpack; } 1>&2 && cd packages && tar -c *.tar.gz'