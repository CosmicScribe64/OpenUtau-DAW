FROM mcr.microsoft.com/dotnet/sdk:8.0-bookworm-slim

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       build-essential ca-certificates cmake curl git ninja-build pkg-config \
       libasound2-dev libfontconfig1-dev libfreetype6-dev libgl1-mesa-dev \
       libx11-dev libxcomposite-dev libxcursor-dev libxext-dev libxinerama-dev \
       libxrandr-dev libxrender-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
COPY scripts/ci.sh /usr/local/bin/openutau-vst-ci
RUN chmod +x /usr/local/bin/openutau-vst-ci
CMD ["openutau-vst-ci"]
