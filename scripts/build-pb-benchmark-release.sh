#!/usr/bin/env bash

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
image="${PB_BENCHMARK_MANYLINUX_IMAGE:-quay.io/pypa/manylinux_2_28_x86_64}"
container_engine="${PB_BENCHMARK_CONTAINER_ENGINE:-}"
version="${PB_BENCHMARK_VERSION:-}"

if [ -z "$version" ]; then
  version="$(git -C "$root" describe --tags --always --dirty)"
fi

if [ "${PB_BENCHMARK_IN_CONTAINER:-0}" != "1" ]; then
  if [ -z "$container_engine" ]; then
    if command -v podman >/dev/null 2>&1; then
      container_engine=podman
    elif command -v docker >/dev/null 2>&1; then
      container_engine=docker
    else
      echo "podman or docker is required for the portable release build" >&2
      exit 1
    fi
  fi

  volume_arg="$root:$root"
  if [ "$container_engine" = "podman" ]; then
    volume_arg="$volume_arg:Z"
  fi

  exec "$container_engine" run --rm \
    -v "$volume_arg" \
    -w "$root" \
    -e PB_BENCHMARK_IN_CONTAINER=1 \
    -e PB_BENCHMARK_VERSION="$version" \
    -e PB_BENCHMARK_MANYLINUX_IMAGE="$image" \
    "$image" \
    bash "$root/scripts/build-pb-benchmark-release.sh"
fi

if command -v dnf >/dev/null 2>&1; then
  dnf install -y \
    cmake \
    file \
    git \
    libatomic-static \
    libstdc++-static \
    ninja-build \
    tar \
    vulkan-loader-devel \
    which
elif command -v yum >/dev/null 2>&1; then
  yum install -y \
    cmake \
    file \
    git \
    libatomic-static \
    libstdc++-static \
    ninja-build \
    tar \
    vulkan-loader-devel \
    which
fi

git config --global --add safe.directory "$root" || true

build_dir="$root/build-pb-release-manylinux"
dist_dir="$root/dist/pb-benchmark"
package_name="q2rtx-penguinburner-${version}-linux-x86_64"
stage="$dist_dir/$package_name"

rm -rf "$build_dir" "$stage"
mkdir -p "$stage/baseq2" "$dist_dir"

cmake -S "$root" -B "$build_dir" -GNinja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_SKIP_RPATH=ON \
  -DCONFIG_PB_BENCHMARK=ON
cmake --build "$build_dir" --target q2rtx game

install -m 0755 "$root/q2rtx" "$stage/q2rtx"
install -m 0755 "$root/baseq2/gamex86_64.so" "$stage/baseq2/gamex86_64.so"
cp -a "$root/baseq2/maps" "$stage/baseq2/"
cp -a "$root/baseq2/materials" "$stage/baseq2/"
cp -a "$root/baseq2/pics" "$stage/baseq2/"
cp -a "$root/baseq2/shader_vkpt" "$stage/baseq2/"
install -m 0644 "$root/baseq2/prefetch.txt" "$stage/baseq2/prefetch.txt"
install -m 0644 "$root/baseq2/pt_toggles.cfg" "$stage/baseq2/pt_toggles.cfg"
install -m 0644 "$root/baseq2/q2rtx.cfg" "$stage/baseq2/q2rtx.cfg"
install -m 0644 "$root/baseq2/q2rtx.menu" "$stage/baseq2/q2rtx.menu"
install -m 0644 "$root/readme.md" "$stage/readme.md"
install -m 0644 "$root/license.txt" "$stage/license.txt"
install -m 0644 "$root/notice.txt" "$stage/notice.txt"
install -m 0644 "$root/changelog.md" "$stage/changelog.md"

strip --strip-unneeded "$stage/q2rtx" "$stage/baseq2/gamex86_64.so"

{
  echo "name=$package_name"
  echo "version=$version"
  echo "commit=$(git -C "$root" rev-parse HEAD)"
  echo "source=$(git -C "$root" config --get remote.origin.url)"
  echo "build_image=$image"
  echo "arch=x86_64"
  echo
  echo "q2rtx file:"
  file "$stage/q2rtx"
  echo
  echo "q2rtx dynamic dependencies:"
  readelf -d "$stage/q2rtx" | grep -E 'NEEDED|RUNPATH|RPATH' || true
  echo
  echo "gamex86_64.so dynamic dependencies:"
  readelf -d "$stage/baseq2/gamex86_64.so" | grep -E 'NEEDED|RUNPATH|RPATH' || true
  echo
  echo "sha256:"
  (cd "$stage" && find . -type f -print0 | sort -z | xargs -0 sha256sum)
} > "$stage/BUILD-INFO.txt"

dynamic_deps="$(readelf -d "$stage/q2rtx" "$stage/baseq2/gamex86_64.so" | grep -E 'NEEDED|RUNPATH|RPATH' || true)"
if printf '%s\n' "$dynamic_deps" | grep -Ei 'Shared library: \[lib(curl|ssl|crypto|idn2|psl|SDL|openal|X11|wayland)[^]]*\]|RUNPATH|RPATH'; then
  echo "release artifact has forbidden dynamic dependencies" >&2
  exit 1
fi

tarball="$dist_dir/$package_name.tar.gz"
rm -f "$tarball" "$tarball.sha256"
tar -C "$dist_dir" -czf "$tarball" "$package_name"
sha256sum "$tarball" > "$tarball.sha256"

echo "$tarball"
