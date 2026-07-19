FROM docker.io/alpine:3.20

RUN apk add --no-cache \
    gcc g++ make binutils \
    musl-dev linux-headers \
    nasm \
    bison flex \
    ncurses-dev \
    cpio \
    e2fsprogs \
    xorriso \
    autoconf automake libtool \
    pkgconfig \
    texinfo \
    gawk \
    curl wget tar \
    bash coreutils findutils \
    git

RUN printf '#!/bin/sh\nexec gcc "$@"\n' > /usr/local/bin/musl-gcc \
    && chmod +x /usr/local/bin/musl-gcc \
    && ln -s /usr/bin/aclocal /usr/bin/aclocal-1.18 \
    && ln -s /usr/bin/automake /usr/bin/automake-1.18

WORKDIR /src
