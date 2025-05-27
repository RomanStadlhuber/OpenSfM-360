#!/bin/bash
if [ "$#" -lt 2 ]; then
    echo "Usage: $0 INPUT_PATH DATA_DIR [VIDEO_STRIDE] [0 <= START <= 1] [start < END <= 1]"
    exit 1
fi

INPUT_PATH="$1"
DATA_DIR="$2"
VIDEO_STRIDE="${3:-30}"
START="${4:-0}"
END="${5:-1}"

# convert video to dataset
cd /source/OpenSfM/app
echo "creating dataset from video"
uv run main.py "$INPUT_PATH" "$DATA_DIR" --video_stride "$VIDEO_STRIDE" --start "$START" --end "$END"
echo "dataset created"
cd /source/OpenSfM
echo "starting reconstruction"
# sparse reconstruction & pose estimation
./bin/opensfm_run_all "$DATA_DIR"
# dense reconstruction
./bin/opensfm undistort "$DATA_DIR"
./bin/opensfm compute_depthmaps "$DATA_DIR"
echo "done"
