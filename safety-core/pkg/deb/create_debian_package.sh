#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

usage() {
    cat <<'USAGE'
Usage: create_debian_package.sh --source-dir DIR --build-dir DIR --output-dir DIR
                                --version VERSION --arch ARCH
USAGE
}

SOURCE_DIR=""
BUILD_DIR=""
OUTPUT_DIR=""
VERSION="1.2.1"
TARGET_ARCH=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --source-dir) SOURCE_DIR="$2"; shift 2 ;;
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --output-dir) OUTPUT_DIR="$2"; shift 2 ;;
        --version) VERSION="$2"; shift 2 ;;
        --arch) TARGET_ARCH="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; usage >&2; exit 1 ;;
    esac
done

if [[ -z "$SOURCE_DIR" || -z "$BUILD_DIR" || -z "$OUTPUT_DIR" || -z "$TARGET_ARCH" ]]; then
    usage >&2
    exit 1
fi

case "$TARGET_ARCH" in
    x86_64)
        DEB_ARCH="amd64"
        PACKAGE_NAME="psf-desktop"
        ;;
    aarch64|arm64)
        DEB_ARCH="arm64"
        PACKAGE_NAME="psf-tegra"
        ;;
    *) echo "Unsupported package arch: $TARGET_ARCH" >&2; exit 1 ;;
esac

PKG_DEB_DIR="$SOURCE_DIR/pkg/deb"
SAFETY_CORE_CONFIG_DIR="$SOURCE_DIR/configs"
INSTALL_PREFIX="/opt/nvidia/psf"
RUNTIME_ROOT="$(mktemp -d)"
DEV_ROOT="$(mktemp -d)"
trap 'rm -rf "$RUNTIME_ROOT" "$DEV_ROOT"' EXIT
chmod 0755 "$RUNTIME_ROOT" "$DEV_ROOT"

mkdir -p "$OUTPUT_DIR"
mkdir -p \
    "$RUNTIME_ROOT${INSTALL_PREFIX}/bin" \
    "$RUNTIME_ROOT${INSTALL_PREFIX}/configs" \
    "$RUNTIME_ROOT${INSTALL_PREFIX}/lib" \
    "$RUNTIME_ROOT${INSTALL_PREFIX}/apps/atl" \
    "$RUNTIME_ROOT${INSTALL_PREFIX}/apps/proximity" \
    "$RUNTIME_ROOT${INSTALL_PREFIX}/apps/mdx-client" \
    "$RUNTIME_ROOT/etc/ld.so.conf.d" \
    "$RUNTIME_ROOT/etc/rsyslog.d" \
    "$RUNTIME_ROOT/usr/lib/tmpfiles.d"

copy_required() {
    local src="$1"
    local dst="$2"
    if [[ ! -e "$src" ]]; then
        echo "Required package input not found: $src" >&2
        exit 1
    fi
    cp -a "$src" "$dst"
}

copy_optional() {
    local src="$1"
    local dst="$2"
    if [[ -e "$src" ]]; then
        cp -a "$src" "$dst"
    else
        echo "Skipping missing optional package input: $src"
    fi
}

while IFS= read -r lib; do
    case "$(basename "$lib")" in
        libnvcuvid.so*|librdkafka.so*|libprotobuf.so*)
            continue
            ;;
    esac
    cp -a "$lib" "$RUNTIME_ROOT${INSTALL_PREFIX}/lib/"
done < <(find "$BUILD_DIR" -type f \( -name "*.so" -o -name "*.so.*" \) | sort)

copy_required "$BUILD_DIR/components/ai-monitor/safety_monitor" "$RUNTIME_ROOT${INSTALL_PREFIX}/bin/"
copy_required "$BUILD_DIR/components/pss_daemon" "$RUNTIME_ROOT${INSTALL_PREFIX}/bin/nvpss_daemon"
copy_required "$BUILD_DIR/components/protocols/decision-maker-gateway/nvpsd_gateway" "$RUNTIME_ROOT${INSTALL_PREFIX}/bin/"

copy_required "$BUILD_DIR/decision-makers/atl/atl_sdm" "$RUNTIME_ROOT${INSTALL_PREFIX}/apps/atl/"
copy_required "$BUILD_DIR/decision-makers/atl/atl_sdm_cmd_receiver" "$RUNTIME_ROOT${INSTALL_PREFIX}/apps/atl/"
copy_required "$BUILD_DIR/decision-makers/proximity/proximity_sdm" "$RUNTIME_ROOT${INSTALL_PREFIX}/apps/proximity/"
copy_required "$BUILD_DIR/decision-makers/proximity/proximity_sdm_cmd_receiver" "$RUNTIME_ROOT${INSTALL_PREFIX}/apps/proximity/"
copy_required "$BUILD_DIR/adapters/vss/mdx-client/mdx_client" "$RUNTIME_ROOT${INSTALL_PREFIX}/apps/mdx-client/"

copy_required "$SOURCE_DIR/components/event-integrator/daemon/nvpss.conf" "$RUNTIME_ROOT${INSTALL_PREFIX}/bin/"
copy_required "$SAFETY_CORE_CONFIG_DIR/thresholds.cfg" "$RUNTIME_ROOT${INSTALL_PREFIX}/configs/"
copy_required "$SAFETY_CORE_CONFIG_DIR/sensor_config.conf" "$RUNTIME_ROOT${INSTALL_PREFIX}/configs/"
copy_required "$SAFETY_CORE_CONFIG_DIR/thresholds.cfg" "$RUNTIME_ROOT${INSTALL_PREFIX}/bin/"
copy_required "$SAFETY_CORE_CONFIG_DIR/sensor_config.conf" "$RUNTIME_ROOT${INSTALL_PREFIX}/bin/"
copy_required "$PKG_DEB_DIR/launch_safety_core.sh" "$RUNTIME_ROOT${INSTALL_PREFIX}/bin/launch_psf.sh"
chmod 0755 "$RUNTIME_ROOT${INSTALL_PREFIX}/bin/launch_psf.sh"

copy_optional "$SOURCE_DIR/adapters/vss/event-mappings/atl/event_mapping_atl.pb.txt" \
    "$RUNTIME_ROOT${INSTALL_PREFIX}/apps/atl/"
copy_optional "$SOURCE_DIR/adapters/vss/event-mappings/proximity/proximity_event_mapping.pb.txt" \
    "$RUNTIME_ROOT${INSTALL_PREFIX}/apps/proximity/"

copy_required "$PKG_DEB_DIR/systemd/99-safety-core-logs.conf" "$RUNTIME_ROOT/etc/rsyslog.d/"

cat > "$RUNTIME_ROOT/etc/ld.so.conf.d/safety-core.conf" <<EOF
${INSTALL_PREFIX}/lib
EOF

cat > "$RUNTIME_ROOT/usr/lib/tmpfiles.d/safety-core.conf" <<'EOF'
d /run/nvpsf 1777 root root -
EOF

mkdir -p "$RUNTIME_ROOT/DEBIAN"
cat > "$RUNTIME_ROOT/DEBIAN/control" <<EOF
Package: ${PACKAGE_NAME}
Version: ${VERSION}
Architecture: ${DEB_ARCH}
Maintainer: Safety Core Developers <safety-core@example.com>
Depends: libc6, libstdc++6, librdkafka1, libprotobuf32t64
Description: Safety Core runtime package
 Runtime binaries, libraries, launch scripts, and default configuration for safety-core.
EOF

cat > "$RUNTIME_ROOT/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if command -v ldconfig >/dev/null 2>&1; then
    ldconfig
fi
if command -v systemd-tmpfiles >/dev/null 2>&1; then
    systemd-tmpfiles --create /usr/lib/tmpfiles.d/safety-core.conf || true
fi
if [ ! -d /run/nvpsf ]; then
    mkdir -p /run/nvpsf
    chmod 1777 /run/nvpsf
fi
EOF
chmod 0755 "$RUNTIME_ROOT/DEBIAN/postinst"

cat > "$RUNTIME_ROOT/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
if command -v ldconfig >/dev/null 2>&1; then
    ldconfig
fi
if [ "$1" = "purge" ] && [ -d /run/nvpsf ]; then
    rm -f /run/nvpsf/nvpssd /run/nvpsf/nvpssd_to_psd
fi
EOF
chmod 0755 "$RUNTIME_ROOT/DEBIAN/postrm"

RUNTIME_DEB="${PACKAGE_NAME}.deb"
RUNTIME_TARBALL="${PACKAGE_NAME}.tar.gz"
tar -czf "$OUTPUT_DIR/${RUNTIME_TARBALL}" -C "$RUNTIME_ROOT" .
dpkg-deb --build "$RUNTIME_ROOT" "$OUTPUT_DIR/${RUNTIME_DEB}"

mkdir -p \
    "$DEV_ROOT${INSTALL_PREFIX}/include" \
    "$DEV_ROOT${INSTALL_PREFIX}/src" \
    "$DEV_ROOT${INSTALL_PREFIX}/examples/apps/metropolis/atl/sdm/ccplex" \
    "$DEV_ROOT${INSTALL_PREFIX}/examples/apps/metropolis/atl/udp_cmd_receiver" \
    "$DEV_ROOT${INSTALL_PREFIX}/examples/apps/metropolis/atl/include" \
    "$DEV_ROOT${INSTALL_PREFIX}/examples/apps/metropolis/proximity/sdm/ccplex" \
    "$DEV_ROOT${INSTALL_PREFIX}/examples/apps/metropolis/proximity/udp_cmd_receiver" \
    "$DEV_ROOT${INSTALL_PREFIX}/examples/apps/metropolis/proximity/include" \
    "$DEV_ROOT${INSTALL_PREFIX}/examples/samples"

for dir in \
    "$SOURCE_DIR/components/black-box/include" \
    "$SOURCE_DIR/components/event-integrator/daemon/include" \
    "$SOURCE_DIR/components/protocols/decision-maker-gateway/include" \
    "$SOURCE_DIR/components/safecomm/validation/include" \
    "$SOURCE_DIR/components/safecomm/posix_msg_que/include" \
    "$SOURCE_DIR/components/safecomm/posix_sockets/include" \
    "$SOURCE_DIR/adapters/vss/mdx-msg-bus/include" \
    "$SOURCE_DIR/adapters/vss/mdx-msg-codec/include" \
    "$SOURCE_DIR/adapters/vss/mdx-client/include"; do
    if [[ -d "$dir" ]]; then
        cp -a "$dir"/. "$DEV_ROOT${INSTALL_PREFIX}/include/"
    fi
done

cp -a "$SOURCE_DIR/decision-makers/atl/sdm"/. \
    "$DEV_ROOT${INSTALL_PREFIX}/examples/apps/metropolis/atl/sdm/ccplex/"
cp -a "$SOURCE_DIR/components/safecomm/validation/src/pss_message_validate.c" \
    "$DEV_ROOT${INSTALL_PREFIX}/examples/apps/metropolis/atl/sdm/ccplex/"
cp -a "$SOURCE_DIR/decision-makers/atl/udp_cmd_receiver"/. \
    "$DEV_ROOT${INSTALL_PREFIX}/examples/apps/metropolis/atl/udp_cmd_receiver/"
cp -a "$SOURCE_DIR/decision-makers/atl/include"/. \
    "$DEV_ROOT${INSTALL_PREFIX}/examples/apps/metropolis/atl/include/"
copy_optional "$SOURCE_DIR/adapters/vss/event-mappings/atl/event_mapping_atl.pb.txt" \
    "$DEV_ROOT${INSTALL_PREFIX}/examples/apps/metropolis/atl/"

cp -a "$SOURCE_DIR/decision-makers/proximity/sdm/ccplex"/. \
    "$DEV_ROOT${INSTALL_PREFIX}/examples/apps/metropolis/proximity/sdm/ccplex/"
cp -a "$SOURCE_DIR/components/safecomm/validation/src/pss_message_validate.c" \
    "$DEV_ROOT${INSTALL_PREFIX}/examples/apps/metropolis/proximity/sdm/ccplex/"
cp -a "$SOURCE_DIR/decision-makers/proximity/udp_cmd_receiver"/. \
    "$DEV_ROOT${INSTALL_PREFIX}/examples/apps/metropolis/proximity/udp_cmd_receiver/"
cp -a "$SOURCE_DIR/decision-makers/proximity/include"/. \
    "$DEV_ROOT${INSTALL_PREFIX}/examples/apps/metropolis/proximity/include/"
copy_optional "$SOURCE_DIR/adapters/vss/event-mappings/proximity/proximity_event_mapping.pb.txt" \
    "$DEV_ROOT${INSTALL_PREFIX}/examples/apps/metropolis/proximity/"

mkdir -p "$DEV_ROOT/DEBIAN"
cat > "$DEV_ROOT/DEBIAN/control" <<EOF
Package: ${PACKAGE_NAME}-dev
Version: ${VERSION}
Architecture: ${DEB_ARCH}
Maintainer: Safety Core Developers <safety-core@example.com>
Depends: ${PACKAGE_NAME} (= ${VERSION})
Description: Safety Core development package
 Headers and example application sources for safety-core integrations.
EOF

DEV_DEB="${PACKAGE_NAME}-dev.deb"
DEV_TARBALL="${PACKAGE_NAME}-dev.tar.gz"
tar -czf "$OUTPUT_DIR/${DEV_TARBALL}" -C "$DEV_ROOT" .
dpkg-deb --build "$DEV_ROOT" "$OUTPUT_DIR/${DEV_DEB}"

echo "Created:"
echo "  $OUTPUT_DIR/${RUNTIME_DEB}"
echo "  $OUTPUT_DIR/${RUNTIME_TARBALL}"
echo "  $OUTPUT_DIR/${DEV_DEB}"
echo "  $OUTPUT_DIR/${DEV_TARBALL}"
