OpenSfM ![Docker workflow](https://github.com/mapillary/opensfm/workflows/Docker%20CI/badge.svg)
=======

## 360 Video Primer

Use the `--recursive` flag to check out this repository including its submodules.

```bash
git clone https://github.com/RomanStadlhuber/OpenSfM-360.git
```

For a quick start set a `DATA_DIR` (absolute path, e.g. `/data`), then do

```bash
# build the image that runs the 360 video to dataset exporter "app"
./app/build_app.sh
# run a contaner that converts a video to a dataset and then processes it
docker run -v $DATA_DIR:$DATA_DIR --rm -it opensfm-360:latest /mnt/data/path/to/video.mp4 /mnt/data/path/to/dataset
```

### Development

The development container is named `opensfm-dev` and mounts the `./app` folder at `/app` as a read-write volume.

```bash
# build container for development purposes, contains byobo und neovim
./app/build_dev.sh
# start development container (does not use --rm, container will persist)
./app/run_dev.sh
```


## Overview
OpenSfM is a Structure from Motion library written in Python. The library serves as a processing pipeline for reconstructing camera poses and 3D scenes from multiple images. It consists of basic modules for Structure from Motion (feature detection/matching, minimal solvers) with a focus on building a robust and scalable reconstruction pipeline. It also integrates external sensor (e.g. GPS, accelerometer) measurements for geographical alignment and robustness. A JavaScript viewer is provided to preview the models and debug the pipeline.

<p align="center">
  <img src="https://opensfm.org/docs/_images/berlin_viewer.jpg" />
</p>

Checkout this [blog post with more demos](http://blog.mapillary.com/update/2014/12/15/sfm-preview.html)


## Getting Started

* [Building the library][]
* [Running a reconstruction][]
* [Documentation][]


[Building the library]: https://opensfm.org/docs/building.html (OpenSfM building instructions)
[Running a reconstruction]: https://opensfm.org/docs/using.html (OpenSfM usage)
[Documentation]: https://opensfm.org/docs/ (OpenSfM documentation)

## License
OpenSfM is BSD-style licensed, as found in the LICENSE file.  See also the Facebook Open Source [Terms of Use][] and [Privacy Policy][]

[Terms of Use]: https://opensource.facebook.com/legal/terms (Facebook Open Source - Terms of Use)
[Privacy Policy]: https://opensource.facebook.com/legal/privacy (Facebook Open Source - Privacy Policy)
