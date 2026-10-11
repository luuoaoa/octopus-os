# chumby-sdk: the toolchain to build Chumby apps (an x86-64 Linux image,
# cross-compiling for the Chumby's ARMv5): buildroot's SDK - gcc, the
# target's libraries and headers (libchumby, LVGL, pixman, ALSA, libcurl),
# pkg-config set up for them - and chumby-app. Built by sdk/make-images.sh.
FROM ubuntu:22.04
RUN apt-get update && apt-get install -y --no-install-recommends \
	make ca-certificates file python3 && rm -rf /var/lib/apt/lists/*
ADD sdk.tar.gz /opt/
RUN mv /opt/arm-buildroot-linux-gnueabi_sdk-buildroot /opt/chumby-sdk && \
	/opt/chumby-sdk/relocate-sdk.sh
COPY chumby-app /usr/local/bin/chumby-app
COPY examples /opt/chumby-sdk/examples
ENV CHUMBY_SDK=/opt/chumby-sdk
WORKDIR /work
