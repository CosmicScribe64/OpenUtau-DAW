FROM mcr.microsoft.com/dotnet/sdk:8.0.424-bookworm-slim@sha256:306301580fcaa5b445180e759db59309979002d1000669cb4cf58a567d0014bc

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       build-essential ca-certificates cmake curl git ninja-build pkg-config \
       libasound2-dev libfontconfig1-dev libfreetype6-dev libgl1-mesa-dev \
       libx11-dev libxcomposite-dev libxcursor-dev libxext-dev libxinerama-dev \
       libxrandr-dev libxrender-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
# GitHub's Linux checkout is bind-mounted from runner uid 1001 while this
# disposable build container runs as root. Trust only the expected mounted
# repositories and the submodule's separate metadata directory so Git can
# inspect and locally clone it without changing host configuration.
RUN git config --system --add safe.directory /workspace \
    && git config --system --add safe.directory /workspace/upstream \
    && git config --system --add safe.directory /workspace/.git/modules/upstream
COPY scripts/ci.sh /usr/local/bin/openutau-vst-ci
RUN chmod +x /usr/local/bin/openutau-vst-ci
CMD ["openutau-vst-ci"]
