FROM ubuntu:20.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        bc \
        bison \
        ca-certificates \
        cpio \
        file \
        flex \
        g++ \
        gcc \
        git \
        libncurses-dev \
        libssl-dev \
        locales \
        make \
        patch \
        perl \
        python3 \
        rsync \
        sed \
        unzip \
        wget \
        xxd \
        xz-utils \
    && rm -rf /var/lib/apt/lists/*

RUN locale-gen en_US.UTF-8
ENV LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8

ARG UID=1000
ARG GID=1000
RUN groupadd -g ${GID} builder && \
    useradd -m -u ${UID} -g ${GID} -s /bin/bash builder

USER builder
WORKDIR /work

RUN git config --global url."https://github.com/".insteadOf git://github.com/

CMD ["/bin/bash"]
