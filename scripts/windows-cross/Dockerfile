# check=skip=InvalidDefaultArgInFrom
# Cruce a Windows x64 (UCRT, winpthreads estatico) equivalente al mingw-w64 de Homebrew.
# NINGUN pin vive aca: la imagen base, los paquetes y su version salen de
# .github/toolchain-pins.json (clave `_windows_cross.container`) y llegan por
# --build-arg desde build-image.sh. Construirla a mano sin ellos falla a proposito.
ARG BASE_IMAGE
FROM ${BASE_IMAGE}
ARG PACKAGES
ARG TOOLS
ARG CROSS_PREFIX
# Fedora instala el toolchain UCRT con su propio prefijo; el gate lo lee de esta variable.
ENV WMA_CROSS_PREFIX=${CROSS_PREFIX}
RUN test -n "${PACKAGES}" && test -n "${TOOLS}" && test -n "${CROSS_PREFIX}" \
    && dnf install -y --setopt=install_weak_deps=False ${PACKAGES} ${TOOLS} \
    && dnf clean all
