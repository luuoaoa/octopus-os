# octopus-sim: the device's root filesystem (ARMv5 for the Chumby One) to run
# its UI on a Linux PC. The host runs the ARM programs through QEMU user
# emulation (binfmt_misc with the F flag: qemu-user-static, or
# "docker run --privileged --rm tonistiigi/binfmt --install arm").
# Built by sdk/make-images.sh: nothing ARM runs while building.
FROM scratch
ADD rootfs.tar /
COPY octopus-sim-session /usr/libexec/octopus-sim-session
COPY asound.conf /etc/asound.conf
ENTRYPOINT ["/usr/libexec/octopus-sim-session"]
