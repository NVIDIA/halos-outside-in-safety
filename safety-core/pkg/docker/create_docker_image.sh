#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

usage() {
    cat <<'USAGE'
Usage: create_docker_image.sh --package-dir DIR --output-dir DIR --dockerfile FILE
                              --image-tag TAG --platform PLATFORM
                              --version VERSION --arch ARCH
USAGE
}

PACKAGE_DIR=""
OUTPUT_DIR=""
DOCKERFILE=""
IMAGE_TAG=""
PLATFORM=""
VERSION="1.2.1"
TARGET_ARCH=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --package-dir) PACKAGE_DIR="$2"; shift 2 ;;
        --output-dir) OUTPUT_DIR="$2"; shift 2 ;;
        --dockerfile) DOCKERFILE="$2"; shift 2 ;;
        --image-tag) IMAGE_TAG="$2"; shift 2 ;;
        --platform) PLATFORM="$2"; shift 2 ;;
        --version) VERSION="$2"; shift 2 ;;
        --arch) TARGET_ARCH="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; usage >&2; exit 1 ;;
    esac
done

if [[ -z "$PACKAGE_DIR" || -z "$OUTPUT_DIR" || -z "$DOCKERFILE" || -z "$IMAGE_TAG" || -z "$PLATFORM" || -z "$TARGET_ARCH" ]]; then
    usage >&2
    exit 1
fi

case "$TARGET_ARCH" in
    x86_64)
        PACKAGE_NAME="psf-desktop"
        ;;
    aarch64|arm64)
        PACKAGE_NAME="psf-tegra"
        ;;
    *) echo "Unsupported Docker package arch: $TARGET_ARCH" >&2; exit 1 ;;
esac

RUNTIME_DEB="$PACKAGE_DIR/${PACKAGE_NAME}.deb"
DEV_DEB="$PACKAGE_DIR/${PACKAGE_NAME}-dev.deb"

if [[ ! -f "$RUNTIME_DEB" || ! -f "$DEV_DEB" ]]; then
    echo "Missing Debian package inputs for Docker build:" >&2
    echo "  $RUNTIME_DEB" >&2
    echo "  $DEV_DEB" >&2
    exit 1
fi

if ! command -v docker >/dev/null 2>&1; then
    echo "docker command not found; install Docker to build the container image." >&2
    exit 1
fi

mkdir -p "$OUTPUT_DIR"
CONTEXT_DIR="$(mktemp -d)"
trap 'rm -rf "$CONTEXT_DIR"' EXIT

cp "$DOCKERFILE" "$CONTEXT_DIR/Dockerfile"
cp "$RUNTIME_DEB" "$CONTEXT_DIR/"
cp "$DEV_DEB" "$CONTEXT_DIR/"

OUTPUT_TAR="$OUTPUT_DIR/${PACKAGE_NAME}.docker.tar"

if docker buildx version >/dev/null 2>&1; then
    docker buildx build \
        --platform "$PLATFORM" \
        --build-arg "RUNTIME_DEB=$(basename "$RUNTIME_DEB")" \
        --build-arg "DEV_DEB=$(basename "$DEV_DEB")" \
        --tag "$IMAGE_TAG" \
        --load \
        "$CONTEXT_DIR"
    docker save "$IMAGE_TAG" -o "$OUTPUT_TAR"
else
    docker build \
        --platform "$PLATFORM" \
        --build-arg "RUNTIME_DEB=$(basename "$RUNTIME_DEB")" \
        --build-arg "DEV_DEB=$(basename "$DEV_DEB")" \
        --tag "$IMAGE_TAG" \
        "$CONTEXT_DIR"
    docker save "$IMAGE_TAG" -o "$OUTPUT_TAR"
fi

echo "Created Docker image archive:"
echo "  $OUTPUT_TAR"
echo "Image tag:"
echo "  $IMAGE_TAG"
