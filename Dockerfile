# Red Daft OS container image
# Build:  docker build -t red-daft-os .
# Run:    docker run -it --rm red-daft-os
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive \
    TERM=xterm-256color

# Base toolchain + AI runtime deps
RUN apt-get update && apt-get install -y --no-install-recommends \
      bash jq python3 python3-pip curl ca-certificates gpg dpkg-dev git \
      tcc locales sudo less procps iproute2 net-tools \
    && rm -rf /var/lib/apt/lists/*

# Red Daft OS components into /opt/daft
COPY packages /opt/daft/packages
COPY ai       /opt/daft/ai
COPY shell    /opt/daft/shell
COPY ux       /opt/daft/ux
COPY kernel   /opt/daft/kernel
COPY docs     /opt/daft/docs

RUN chmod +x /opt/daft/packages/daft-pkg/*.sh \
             /opt/daft/packages/deb-adapter/*.sh \
             /opt/daft/packages/specs/amd/* \
             /opt/daft/shell/*.sh \
             /opt/daft/ux/*.sh

# Branding, MOTD, daft-shell on PATH
RUN bash /opt/daft/ux/setup-motd.sh / && \
    ln -sf /opt/daft/shell/daft-shell.sh /usr/local/bin/daft-shell && \
    ln -sf /opt/daft/packages/daft-pkg/daft-pkg.sh /usr/local/bin/daft-pkg && \
    ln -sf /opt/daft/packages/deb-adapter/daft-deb-adapter.sh /usr/local/bin/daft-deb && \
    ln -sf /opt/daft/ux/firstrun-idcard.sh /usr/local/bin/daft-idcard && \
    ln -sf /usr/bin/python3 /usr/local/bin/python

# Non-root operator account (sudo-capable)
RUN useradd -ms /bin/bash -G sudo agent && \
    echo "agent ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/agent

USER agent
WORKDIR /home/agent

# First-run ID card on interactive shell entry
SHELL ["/bin/bash", "-c"]
CMD ["/bin/bash", "-lc", "/opt/daft/ux/firstrun-idcard.sh; exec bash"]
