FROM zephyrprojectrtos/zephyr-build:v0.18.3

MAINTAINER m.niestroj@golioth.io

USER root

### Gitpod user ###
# https://community.gitpod.io/t/how-to-resolve-password-issue-in-sudo-mode-for-custom-image/2395
#
# '-l': see https://docs.docker.com/develop/develop-images/dockerfile_best-practices/#user
RUN useradd -l -u 33333 -G sudo -md /home/gitpod -s /bin/bash -p gitpod gitpod \
    # passwordless sudo for users in the 'sudo' group
    && sed -i.bkp -e 's/%sudo\s\+ALL=(ALL\(:ALL\)\?)\s\+ALL/%sudo ALL=NOPASSWD:ALL/g' /etc/sudoers

# Zephyr SDK
ENV ZEPHYR_TOOLCHAIN_VARIANT zephyr
ENV ZEPHYR_SDK_INSTALL_DIR /opt/toolchains/zephyr-sdk-0.13.0

# Install goliothctl
RUN echo "deb [trusted=yes] https://repos.golioth.io/apt/ /" | sudo tee /etc/apt/sources.list.d/golioth.list
RUN apt update
RUN apt install goliothctl
RUN apt install coap
