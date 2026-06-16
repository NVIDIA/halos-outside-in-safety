# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

FROM nvcr.io/nvidia/base/ubuntu:noble-20260217

ARG RUNTIME_DEB
ARG DEV_DEB

ENV PSF_INSTALL_PATH=/opt/nvidia/psf
ENV LD_LIBRARY_PATH=/opt/nvidia/psf/lib:/usr/local/lib
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
      cron \
      librdkafka-dev \
      librdkafka1 \
      libprotobuf32t64 \
      libprotobuf-dev \
      logrotate \
      rsyslog \
      sudo \
      tini && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

RUN useradd -ms /bin/bash nvidia && \
    echo "nvidia ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/nvidia && \
    chmod 0440 /etc/sudoers.d/nvidia

COPY ${RUNTIME_DEB} ${DEV_DEB} /tmp/

RUN apt-get update && \
    apt-get install -y --no-install-recommends "/tmp/${RUNTIME_DEB}" "/tmp/${DEV_DEB}" && \
    rm -f "/tmp/${RUNTIME_DEB}" "/tmp/${DEV_DEB}" && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

RUN sed -i 's/^module(load="imklog")/#module(load="imklog")/' /etc/rsyslog.conf 2>/dev/null || true && \
    sed -i 's/^\$ModLoad imklog/#\$ModLoad imklog/' /etc/rsyslog.conf 2>/dev/null || true && \
    touch /var/log/safety-core.log && \
    chown nvidia:nvidia /var/log/safety-core.log && \
    chmod 666 /var/log/safety-core.log && \
    printf '/var/log/safety-core.log {\n\
    daily\n\
    rotate 7\n\
    maxsize 100M\n\
    missingok\n\
    notifempty\n\
    compress\n\
    delaycompress\n\
    copytruncate\n\
    create 0666 nvidia nvidia\n\
}\n' > /etc/logrotate.d/safety-core

RUN printf '#!/usr/bin/env bash\n\
set -euo pipefail\n\
\n\
cleanup() {\n\
    if pgrep rsyslogd >/dev/null 2>&1; then\n\
        kill -TERM "$(pgrep rsyslogd)" 2>/dev/null || true\n\
    fi\n\
    if pgrep cron >/dev/null 2>&1; then\n\
        kill -TERM "$(pgrep cron)" 2>/dev/null || true\n\
    fi\n\
}\n\
trap cleanup EXIT\n\
\n\
mkdir -p /run/nvpsf\n\
chmod 1777 /run/nvpsf\n\
\n\
cron\n\
rsyslogd 2>&1 | grep -v "imklog" | grep -v "kmsg" || true\n\
\n\
exec runuser -u nvidia -- /opt/nvidia/psf/bin/launch_psf.sh "$@"\n\
' > /usr/local/bin/safety-core-entrypoint.sh && \
    chmod 0755 /usr/local/bin/safety-core-entrypoint.sh

WORKDIR /home/nvidia
ENTRYPOINT ["/usr/bin/tini", "-g", "-s", "--", "/usr/local/bin/safety-core-entrypoint.sh"]
CMD []
