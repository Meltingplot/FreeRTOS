# QEMU runtime test of the FreeRTOS critical-section mask change (Tools/mask-test, see run.sh).
# Adds qemu-system-arm to the firmware build image (Arm GNU toolchain at /opt/arm-gnu-toolchain).
ARG BASE_IMAGE=dsf-firmware-build
FROM ${BASE_IMAGE}

RUN apt-get update \
    && apt-get install -y --no-install-recommends qemu-system-arm \
    && rm -rf /var/lib/apt/lists/*
