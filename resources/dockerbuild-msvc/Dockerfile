FROM debian:12-slim

# Build arguments
ARG CMAKE_VERSION="3.31.6"
ARG GIT_VERSION="2.49.0"
ARG UID=1000
ARG GID=1000

# Install system dependencies (Wine, Python for msvc-wine, and build tools)
RUN apt-get update \
    && dpkg --add-architecture i386 \
    && apt-get install -y --no-install-recommends \
        wget gpg unzip git ca-certificates \
        python3 msitools \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

# Install Wine from WineHQ
RUN mkdir -pm755 /etc/apt/keyrings \
    && wget -O - https://dl.winehq.org/wine-builds/winehq.key | gpg --dearmor -o /etc/apt/keyrings/winehq-archive.key - \
    && wget -NP /etc/apt/sources.list.d/ https://dl.winehq.org/wine-builds/debian/dists/bookworm/winehq-bookworm.sources \
    && apt-get update \
    && apt-get install -y --no-install-recommends winehq-stable \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

# Setup build folder
RUN mkdir -p /build/tools /build/tmp /build/tmp/home \
    && chown -R ${UID}:${GID} /build

USER ${UID}:${GID}
WORKDIR /build/tools

# Install Windows CMake
RUN wget -q https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-windows-x86_64.zip \
    && unzip -q cmake-${CMAKE_VERSION}-windows-x86_64.zip -d /build/tools/ \
    && mv /build/tools/cmake-${CMAKE_VERSION}-windows-x86_64 /build/tools/cmake \
    && rm -f *.zip

# Install Windows Git
RUN wget -q https://github.com/git-for-windows/git/releases/download/v${GIT_VERSION}.windows.1/MinGit-${GIT_VERSION}-64-bit.zip \
    && unzip -q MinGit-${GIT_VERSION}-64-bit.zip -d /build/tools/ \
    && mv /build/tools/cmd /build/tools/git \
    && rm -f *.zip

# Install Windows Ninja
RUN wget -q https://github.com/ninja-build/ninja/releases/download/v1.13.1/ninja-win.zip \
    && unzip -q ninja-win.zip -d /build/tools/ \
    && rm -f *.zip

# Install MSVC Build Tools via msvc-wine
RUN git clone --depth 1 https://github.com/mstorsjo/msvc-wine.git /build/tools/msvc-wine

# Download MSVC 2022 and Windows SDK (x86 target for 32-bit game)
# --msvc-version 17 = VS 2022, host x64, target x86
ENV WINEDEBUG=-all
ENV WINEARCH=win64
ENV WINEPREFIX=/build/prefix64
ENV HOME=/build/tmp/home

RUN /build/tools/msvc-wine/vsdownload.py \
        --dest /build/tools/msvc \
        --accept-license \
    && /build/tools/msvc-wine/install.sh /build/tools/msvc

# Setup environment for MSVC 2022
# The install.sh creates wrapper scripts in msvc/bin/x86/ and msvc/bin/x64/
# We use x86 for 32-bit compilation
ENV CC="Z:\\build\\tools\\msvc\\bin\\x86\\cl.exe"
ENV CXX="Z:\\build\\tools\\msvc\\bin\\x86\\cl.exe"
ENV PRESET=msvc-wine

# Create empty TEMP folder for linking
ENV TMP="Z:\\build\\tmp"
ENV TEMP="Z:\\build\\tmp"
ENV TEMPDIR="Z:\\build\\tmp"

WORKDIR /build/cnc

USER root
COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh
CMD ["/entrypoint.sh"]
