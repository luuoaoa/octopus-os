# octopus-sdk:BOARD: the toolchain to build Octopus apps for a board (an
# x86-64 Linux image, cross-compiling): buildroot's SDK - gcc, the board's
# libraries and headers (libchumby, LVGL, pixman, ALSA, libcurl),
# pkg-config set up for them - and octopus-app. Built by sdk/make-images.sh.
FROM ubuntu:22.04
ARG SDK_DIR
ARG CROSS
ARG BOARD
RUN apt-get update && apt-get install -y --no-install-recommends \
	make ca-certificates file python3 && rm -rf /var/lib/apt/lists/*
ADD sdk.tar.gz /opt/
RUN mv "/opt/$SDK_DIR" /opt/octopus-sdk && /opt/octopus-sdk/relocate-sdk.sh
COPY octopus-app /usr/local/bin/octopus-app
COPY examples /opt/octopus-sdk/examples
# octopus-app build: CC=$OCTOPUS_SDK/bin/${OCTOPUS_CROSS}gcc
ENV OCTOPUS_SDK=/opt/octopus-sdk OCTOPUS_CROSS=$CROSS OCTOPUS_BOARD=$BOARD
WORKDIR /work
