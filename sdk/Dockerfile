# chumby-sdk:BOARD: the toolchain to build Chumby apps for a board (an
# x86-64 Linux image, cross-compiling): buildroot's SDK - gcc, the board's
# libraries and headers (libchumby, LVGL, pixman, ALSA, libcurl),
# pkg-config set up for them - and chumby-app. Built by sdk/make-images.sh.
FROM ubuntu:22.04
ARG SDK_DIR
ARG CROSS
ARG BOARD
RUN apt-get update && apt-get install -y --no-install-recommends \
	make ca-certificates file python3 && rm -rf /var/lib/apt/lists/*
ADD sdk.tar.gz /opt/
RUN mv "/opt/$SDK_DIR" /opt/chumby-sdk && /opt/chumby-sdk/relocate-sdk.sh
COPY chumby-app /usr/local/bin/chumby-app
COPY examples /opt/chumby-sdk/examples
# chumby-app build: CC=$CHUMBY_SDK/bin/${CHUMBY_CROSS}gcc
ENV CHUMBY_SDK=/opt/chumby-sdk CHUMBY_CROSS=$CROSS CHUMBY_BOARD=$BOARD
WORKDIR /work
