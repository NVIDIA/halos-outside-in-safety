<!--
SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: Apache-2.0
-->

# Safety Core Build Guide

This directory builds the safety-core components, decision-makers, VSS adapters, Debian packages, and a local Docker image archive with CMake.

## Prerequisites

Install the host build tools, runtime package tools, and public development packages:

```sh
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake ninja-build \
  dpkg-dev docker.io \
  librdkafka-dev libprotobuf-dev
```

The build expects CUDA to be installed locally. The default CUDA root is `/usr/local/cuda-13.0`; override it if your installation uses a different path.

```sh
-DSAFETY_CORE_CUDA_TOOLKIT_ROOT=/usr/local/cuda-13.0
```

The build carries cross-architecture dependency link stubs in `safety-core/cmake/stubs`, and the NVDEC headers used by `safety_monitor` in `safety-core/cmake/third-party-headers`. This keeps the x86_64 and aarch64 build flow consistent without requiring target-architecture copies of proprietary runtime libraries at build time.

External headers are still resolved from local system installations where appropriate:

- `librdkafka-dev` provides `rdkafka.h`.
- `libprotobuf-dev` provides Protobuf headers.
- `safety-core/cmake/third-party-headers/nvcuvid/include` provides `nvcuvid.h` and `cuviddec.h`.

If rdkafka or Protobuf headers are installed in non-standard locations, pass `SAFETY_CORE_RDKAFKA_INCLUDE_DIR` or `SAFETY_CORE_PROTOBUF_INCLUDE_DIR`.

## Build For x86_64

Configure and build from the repository root:

```sh
cmake -S safety-core -B safety-core/build-x86_64 \
  -DSAFETY_CORE_TARGET_ARCH=x86_64

cmake --build safety-core/build-x86_64 --parallel
```

## Build For ARM64/Tegra

For a native ARM64 machine:

```sh
cmake -S safety-core -B safety-core/build-aarch64 \
  -DSAFETY_CORE_TARGET_ARCH=aarch64

cmake --build safety-core/build-aarch64 --parallel
```

For cross-compilation from an x86_64 host, use the provided toolchain file:

```sh
cmake -S safety-core -B safety-core/build-aarch64 \
  -DSAFETY_CORE_TARGET_ARCH=aarch64 \
  -DCMAKE_TOOLCHAIN_FILE=safety-core/cmake/toolchains/aarch64-linux-gnu.cmake \
  -DSAFETY_CORE_ARM64_CUDA_TARGET_NAME=sbsa-linux

cmake --build safety-core/build-aarch64 --parallel
```

Use `-DSAFETY_CORE_ARM64_CUDA_TARGET_NAME=aarch64-linux` if your CUDA installation uses that target directory name instead of `sbsa-linux`.

## Build Debian Packages

Build the Debian package target after configuring the desired architecture:

```sh
cmake --build safety-core/build-x86_64 --target safety_core_debian --parallel
cmake --build safety-core/build-aarch64 --target safety_core_debian --parallel
```

Package artifacts are written to `<build-dir>/packages`.

For `x86_64`, the Debian target creates:

```text
safety-core/build-x86_64/packages/psf-desktop.deb
safety-core/build-x86_64/packages/psf-desktop-dev.deb
safety-core/build-x86_64/packages/psf-desktop.tar.gz
safety-core/build-x86_64/packages/psf-desktop-dev.tar.gz
```

For `aarch64`/ARM64, the Debian target creates:

```text
safety-core/build-aarch64/packages/psf-tegra.deb
safety-core/build-aarch64/packages/psf-tegra-dev.deb
safety-core/build-aarch64/packages/psf-tegra.tar.gz
safety-core/build-aarch64/packages/psf-tegra-dev.tar.gz
```

The runtime package installs under `/opt/nvidia/psf` and includes runtime binaries, shared libraries, default configs, launch scripts, event mappings, rsyslog config, and tmpfiles config. The dev package includes public headers and ATL/proximity example sources.

## Build Docker Image Archive

The Docker target depends on the Debian target and uses the generated runtime and dev packages as build inputs:

```sh
cmake --build safety-core/build-x86_64 --target safety_core_docker --parallel
cmake --build safety-core/build-aarch64 --target safety_core_docker --parallel
```

Docker artifacts are written to `<build-dir>/packages`.

For `x86_64`, the Docker target creates:

```text
safety-core/build-x86_64/packages/psf-desktop.docker.tar
```

For `aarch64`/ARM64, the Docker target creates:

```text
safety-core/build-aarch64/packages/psf-tegra.docker.tar
```

The default image tags are:

```text
psf-desktop:latest
psf-tegra:latest
```

Load a generated archive with:

```sh
docker load -i safety-core/build-x86_64/packages/psf-desktop.docker.tar
docker load -i safety-core/build-aarch64/packages/psf-tegra.docker.tar
```

Use the aggregate package target to build Debian packages and the Docker archive together:

```sh
cmake --build safety-core/build-x86_64 --target safety_core_package --parallel
cmake --build safety-core/build-aarch64 --target safety_core_package --parallel
```

## Generated Runtime Targets

The default build produces these executable targets:

```text
safety_monitor
pss_daemon
nvpsd_gateway
mdx_client
atl_sdm
atl_sdm_cmd_receiver
proximity_sdm
proximity_sdm_cmd_receiver
```

The default build produces these shared libraries:

```text
libnvpsb.so
libnvpssd_interface.so
libnvpsd.so
libnvpss_msg_que.so
libnvpss_socket_comms.so
libnvpsf_msgbus.so
libnvpsf_msgcodec.so
```

The build also creates object-library helper targets:

```text
safety_core_msg_validation
safety_core_sensor_config
```

Stub libraries for external dependencies are generated from export files:

```text
SafetyCore::Stub::nvcuvid
SafetyCore::Stub::rdkafka
SafetyCore::Stub::protobuf
```

Headers are used for compilation, while stubs satisfy link-time symbol resolution during the build. The generated dependency stubs are not packaged as runtime libraries; runtime systems must provide the real libraries through the driver or installed packages.

## Packaged Runtime Layout

The runtime Debian package stages the main executables under `/opt/nvidia/psf`:

```text
/opt/nvidia/psf/bin/safety_monitor
/opt/nvidia/psf/bin/nvpss_daemon
/opt/nvidia/psf/bin/nvpsd_gateway
/opt/nvidia/psf/bin/launch_psf.sh
/opt/nvidia/psf/apps/atl/atl_sdm
/opt/nvidia/psf/apps/atl/atl_sdm_cmd_receiver
/opt/nvidia/psf/apps/proximity/proximity_sdm
/opt/nvidia/psf/apps/proximity/proximity_sdm_cmd_receiver
/opt/nvidia/psf/apps/mdx-client/mdx_client
```

Runtime libraries are staged in `/opt/nvidia/psf/lib`, and default configs are staged in `/opt/nvidia/psf/configs`.

