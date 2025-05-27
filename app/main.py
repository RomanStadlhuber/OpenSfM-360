#!/usr/bin/env python3
from __future__ import annotations
from jsonargparse import auto_cli
from pathlib import Path
import shutil
import ffmpeg
import yaml
import json

OPENSFM_PATH = "/source/OpenSfM"
CAMERA_MODEL_JSON = {  # common camera model used for all 360 images
    "all": {
        "projection_type": "equirectangular",
        "width": 1920,
        "height": 960,
    }
}
# from: https://www.dropbox.com/scl/fo/3q1m88yp3mcsb6kg0hez8/AFSGDg_7Rr5X3INg2mKFUsk?dl=0&e=1&preview=config.yaml
CONFIG_YAML = {
    "feature_process_size": 1024,  # Resize the image if its size is larger than specified. Set to -1 for original size
    "feature_min_frames": 16000,  # If fewer frames are detected, sift_peak_threshold/surf_hessian_threshold is reduced.
    "processes": 8,  # Number of threads to use
}


def convert_video(
    input_path: str,
    dataset_dir: str,
    video_stride: int = 180,
    start: float = 0,
    end: float = 1.0,
    override_video_dimensions: bool = True,
) -> None:
    """Converts a 360° video into an OpenSfM dataset."""
    # clip start and end to [0;1]
    start = min(0, max(start, 1))
    end = min(1, max(end, 0))
    assert start < end, "Start time must be less than end time."
    dataset_dir = Path(dataset_dir)
    # remove existing dataset directory if it exists
    if dataset_dir.exists():
        shutil.rmtree(dataset_dir)
    print("create dataset folder")
    dataset_dir.mkdir(parents=True, exist_ok=True)
    print("create image folder")
    image_dir = dataset_dir / "images"
    image_dir.mkdir(parents=True, exist_ok=True)
    # load video info
    probe = ffmpeg.probe(input_path)
    if override_video_dimensions:
        width = probe["streams"][0]["width"]
        height = probe["streams"][0]["height"]
        print(f"Overriding camera model dimensions to {width}x{height}")
        camera_model_overrides = CAMERA_MODEL_JSON.copy()
        camera_model_overrides["all"]["width"] = width
        camera_model_overrides["all"]["height"] = height
        json.dump(camera_model_overrides, open(dataset_dir / "camera_models_overrides.json", "w"), indent=4)
    else:  # use default dimensions
        width = 1920
        height = 960
        json.dump(CAMERA_MODEL_JSON, open(dataset_dir / "camera_models_overrides.json", "w"), indent=4)
    # find absolute start and end time
    try:
        start = start * float(probe["streams"][0]["start_time"])
        end = end * float(probe["streams"][0]["duration"])
        t = end - start  # "-t" determines the trim duration after the start time
    except Exception as e:
        raise RuntimeError(f"Error parsing video info from ffprobe: {e}")

    print("creating config files")
    yaml.dump(CONFIG_YAML, open(dataset_dir / "config.yaml", "w"))
    filter_expr = f"select=not(mod(n\\,{video_stride}))"

    # Format start time as hh:mm:ss
    def seconds_to_hms(seconds):
        hours = int(seconds // 3600)
        minutes = int((seconds % 3600) // 60)
        secs = seconds % 60
        return f"{hours:02d}:{minutes:02d}:{secs:06.3f}"

    start_hms = seconds_to_hms(start)
    t_hms = seconds_to_hms(t)

    print(f"exporting frames from -ss {start_hms}, -t {t_hms}")

    (
        ffmpeg.input(input_path, ss=start_hms, t=t_hms)
        .output(
            str(image_dir / "frame_%04d.png"),
            vf=f"{filter_expr},scale={width}:{height}",
            vsync="vfr",
            qscale=2,
        )
        .run()
    )
    num_frames = len(list(image_dir.glob("*.png")))
    print(f"Exported {num_frames} frames to {image_dir}")


if __name__ == "__main__":
    auto_cli(convert_video)
