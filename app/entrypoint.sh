#!/bin/bash
if [ "$#" -ne 2 ]; then
    echo "Usage: $0 INPUT_PATH DATA_DIR"
    exit 1
fi

INPUT_PATH="$1"
DATA_DIR="$2"

# convert video to dataset
cd /source/OpenSfM/app
echo "creating dataset from video"
uv run main.py $INPUT_PATH $DATA_DIR convert_video
echo "dataset created"
cd /source/OpenSfM
echo "starting reconstruction"
./bin/opensfm_run_all $DATA_DIR
echo "done"
