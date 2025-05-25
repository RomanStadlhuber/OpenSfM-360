FROM ubuntu:20.04 AS development

ARG DEBIAN_FRONTEND=noninteractive

# Install apt-getable dependencies
RUN apt-get update \
    && apt-get install -y \
        build-essential \
        cmake \
        git \
        libeigen3-dev \
        libopencv-dev \
        libceres-dev \
        python3-dev \
        python3-numpy \
        python3-opencv \
        python3-pip \
        python3-pyproj \
        python3-scipy \
        python3-yaml \
        python3-virtualenv \
        ffmpeg \
        curl \
        ca-certificates \
        byobu \
        neovim \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/* /tmp/* /var/tmp/*

ADD https://astral.sh/uv/install.sh /uv-installer.sh
RUN bash /uv-installer.sh && rm /uv-installer.sh
ENV PATH="/root/.local/bin/:$PATH"

COPY . /source/OpenSfM

COPY /app/init.vim /root/.config/nvim/init.vim

WORKDIR /source/OpenSfM

RUN pip3 install -r requirements.txt && \
    python3 setup.py build


FROM development AS deploy
WORKDIR /source/OpenSfM/app
RUN uv sync --frozen
COPY app/entrypoint.sh /entrypoint.sh
ENTRYPOINT ["/entrypoint.sh"]