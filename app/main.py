#!/usr/bin/env python3
from __future__ import annotations
from jsonargparse import auto_cli
from pathlib import Path
import subprocess
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
# from: https://www.dropbox.com/scl/fo/3q1m88yp3mcsb6kg0hez8/AFSGDg_7Rr5X3INg2mKFUsk?dl=0&e=1&preview=config.yaml&rlkey=h6p1g8b0pcnn3uwu1s5uvnald
CONFIG_YAML = {
    "feature_process_size": 1024,    # Resize the image if its size is larger than specified. Set to -1 for original size
    "feature_min_frames": 16000,     # If fewer frames are detected, sift_peak_threshold/surf_hessian_threshold is reduced.
    "processes": 8,                  # Number of threads to use
}


class Main:
    def __init__(self, input_path: str, dataset_dir: str, video_stride: int = 30):
        self.input_path = Path(input_path)
        self.dataset_dir = Path(dataset_dir)
        self.stride = video_stride
        pass

    def convert_video(self) -> None:
        """Converts a 360° video into an OpenSfM dataset."""
        print("create dataset folder")
        self.dataset_dir.mkdir(parents=True, exist_ok=True)
        print("create image folder")
        image_dir = self.dataset_dir  / "images"
        image_dir.mkdir(parents=True, exist_ok=True)
        print("creating config files")
        json.dump(CAMERA_MODEL_JSON, open(self.dataset_dir  / "camera_models_overrides.json", "w"), indent=4)
        yaml.dump(CONFIG_YAML, open(self.dataset_dir  / "config.yaml", "w"))
        print("exporting frames")
        filter_expr = f"select=not(mod(n\\,{self.stride}))"
        (ffmpeg
            .input(self.input_path)
            .output(
            str(image_dir / "frame_%04d.png"),
            vf=f"{filter_expr},scale=1920:960",
            vsync="vfr",
            qscale=2
            )
            .run()
        )
        num_frames = len(list(image_dir.glob("*.png")))
        print(f"Exported {num_frames} frames to {image_dir}")

    ## # TODO: in the future, generate a dataset from directory of (unordered) images
    ## def convert_images(self) -> None:
    ##     raise NotImplementedError("TODO: convert (unordered) images to OpenSfM dataset")


if __name__ == "__main__":
    auto_cli(Main, description="Generate OpenSfM datasets from 360° videos and process them.")
