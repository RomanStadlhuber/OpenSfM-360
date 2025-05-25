#!/bin/bash
xhost +
docker run \
-it \
--privileged \
--network host \
-v /dev:/dev \
-e DISPLAY=$DISPLAY \
-e PUID=$(id -u) \
-e PGID=$(id -g) \
-v /tmp/.X11-unix:/tmp/.X11-unix \
-v /dev:/dev \
-v $(pwd)/app:/app \
-v /mnt/data:/mnt/data \
--name opensfm-dev \
opensfm:dev \
bash
xhost -
