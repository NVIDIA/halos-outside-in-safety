# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

FROM nvcr.io/nvidia/base/ubuntu:noble-20260930.1

ARG RUNTIME_DEB
ARG DEV_DEB

ENV PSF_INSTALL_PATH=/opt/nvidia/psf
ENV LD_LIBRARY_PATH=/opt/nvidia/psf/lib:/usr/local/lib
ENV DEBIAN_FRONTEND=noninteractive
# The Safety AI Monitor decodes RTSP with NVDEC, so it needs the video driver
# libraries as well as compute when run with --runtime=nvidia.
ENV NVIDIA_DRIVER_CAPABILITIES=compute,video,utility

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
      cron \
      librdkafka-dev \
      librdkafka1 \
      libprotobuf32t64 \
      libprotobuf-dev \
      logrotate \
      python3 \
      rsyslog \
      sudo \
      tini && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

# The launcher starts rsyslog through systemctl; the container has no init, and
# the entrypoint has already started rsyslogd.
RUN useradd -ms /bin/bash nvidia && \
    echo "nvidia ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/nvidia && \
    chmod 0440 /etc/sudoers.d/nvidia && \
    printf '#!/bin/sh\necho "systemctl $* (no-op in container)"\nexit 0\n' > /usr/bin/systemctl && \
    chmod 0755 /usr/bin/systemctl

COPY ${RUNTIME_DEB} ${DEV_DEB} /tmp/

RUN apt-get update && \
    apt-get install -y --no-install-recommends "/tmp/${RUNTIME_DEB}" "/tmp/${DEV_DEB}" && \
    rm -f "/tmp/${RUNTIME_DEB}" "/tmp/${DEV_DEB}" && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

# Every component log lives under /var/log/psf: rsyslog-routed processes in
# psf.log, which the entrypoint also mirrors to the container's stdout, and the
# SDMs and SAIM in their own files.
RUN sed -i 's/^module(load="imklog")/#module(load="imklog")/' /etc/rsyslog.conf && \
    sed -i 's/^\$ModLoad imklog/#\$ModLoad imklog/' /etc/rsyslog.conf && \
    install -d -o nvidia -g nvidia -m 0755 /var/log/psf && \
    for log in psf saim atl_sdm atl_proximity_sdm pxc_sdm; do \
        install -o nvidia -g nvidia -m 0666 /dev/null "/var/log/psf/${log}.log"; \
    done && \
    printf '*.* action(type="omfile" file="/var/log/psf/psf.log")\n& stop\n' \
        > /etc/rsyslog.d/00-docker.conf

RUN printf '#!/bin/bash\n\
set -e\n\
\n\
cleanup() {\n\
    if pgrep rsyslogd >/dev/null 2>&1; then\n\
        kill -TERM $(pgrep rsyslogd) 2>/dev/null || echo "Warning: failed to stop rsyslog cleanly" >&2\n\
    fi\n\
    if pgrep cron >/dev/null 2>&1; then\n\
        kill -TERM $(pgrep cron) 2>/dev/null || echo "Warning: failed to stop cron cleanly" >&2\n\
    fi\n\
}\n\
trap cleanup EXIT\n\
\n\
if [ -L /var/log/psf ] || [ -L /var/log/psf/psf.log ] || { [ -e /var/log/psf/psf.log ] && [ ! -f /var/log/psf/psf.log ]; }; then\n\
    echo "ERROR: invalid PSF log path" >&2\n\
    exit 1\n\
fi\n\
if [ ! -d /var/log/psf ]; then\n\
    install -d -o nvidia -g nvidia -m 0755 /var/log/psf\n\
fi\n\
if [ ! -e /var/log/psf/psf.log ]; then\n\
    install -o syslog -g adm -m 0640 /dev/null /var/log/psf/psf.log\n\
fi\n\
if ! runuser -u syslog -- sh -c ": >> /var/log/psf/psf.log"; then\n\
    echo "ERROR: /var/log/psf/psf.log is not writable by syslog; check mounted log permissions" >&2\n\
    exit 1\n\
fi\n\
\n\
mkdir -p /run/nvpsf\n\
chmod 1777 /run/nvpsf\n\
\n\
cron\n\
rsyslogd\n\
sleep 1\n\
if ! pgrep rsyslogd >/dev/null 2>&1; then\n\
    echo "ERROR: rsyslog failed to start" >&2\n\
    exit 1\n\
fi\n\
tail -n 0 -F /var/log/psf/psf.log &\n\
\n\
exec runuser -u nvidia -- /opt/nvidia/psf/bin/launch_psf.sh "$@"\n\
' > /usr/local/bin/safety-core-entrypoint.sh && \
    chmod 0755 /usr/local/bin/safety-core-entrypoint.sh

WORKDIR /home/nvidia
ENTRYPOINT ["/usr/bin/tini", "-g", "-s", "--", "/usr/local/bin/safety-core-entrypoint.sh"]
CMD []
