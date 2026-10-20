"""Convert the supplied green-screen videos into a validated ESP32 frame pack."""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import subprocess
import zlib
from pathlib import Path

import imageio_ffmpeg
import numpy as np
from PIL import Image, ImageDraw

MAGIC = b"BDGZ565\0"
HEADER = struct.Struct("<8s6I")
CLIP = struct.Struct("<16sII")
FRAME = struct.Struct("<II")
SIZE = 360
DECODE_SIZE = 540
BACKGROUND = (255, 240, 245)
NAMES = ("walk", "jump", "sit")


def decode(path: Path, fps: int) -> list[np.ndarray]:
    command = [imageio_ffmpeg.get_ffmpeg_exe(), "-v", "error", "-i", str(path),
               "-an", "-vf", f"fps={fps},scale={DECODE_SIZE}:{DECODE_SIZE}",
               "-f", "rawvideo", "-pix_fmt", "rgb24", "pipe:1"]
    result = subprocess.run(command, capture_output=True, check=True)
    frame_size = DECODE_SIZE * DECODE_SIZE * 3
    if not result.stdout or len(result.stdout) % frame_size:
        raise ValueError(f"Incomplete video frames: {path}")
    data = np.frombuffer(result.stdout, dtype=np.uint8).reshape(-1, DECODE_SIZE, DECODE_SIZE, 3)
    return list(data)


def matte(rgb: np.ndarray) -> np.ndarray:
    signed = rgb.astype(np.int16)
    green_excess = signed[:, :, 1] - np.maximum(signed[:, :, 0], signed[:, :, 2])
    return (255 * (1 - np.clip((green_excess - 12) / 65, 0, 1))).astype(np.uint8)


def encode565(image: Image.Image) -> bytes:
    pixels = np.asarray(image.convert("RGB"), dtype=np.uint16)
    packed = ((pixels[:, :, 0] >> 3) << 11) | ((pixels[:, :, 1] >> 2) << 5) | (pixels[:, :, 2] >> 3)
    return packed.astype("<u2").tobytes()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--preview-dir", type=Path, required=True)
    parser.add_argument("--fps", type=int, default=15, choices=range(5, 31))
    args = parser.parse_args()
    clips = {name: decode(args.source / f"{name}.mp4", args.fps) for name in NAMES}
    bounds = [DECODE_SIZE, DECODE_SIZE, 0, 0]
    for frames in clips.values():
        for rgb in frames:
            ys, xs = np.nonzero(matte(rgb) > 127)
            if not xs.size:
                raise ValueError("Video contains an empty character frame")
            bounds = [min(bounds[0], int(xs.min())), min(bounds[1], int(ys.min())),
                      max(bounds[2], int(xs.max()) + 1), max(bounds[3], int(ys.max()) + 1)]
    scale = min(270 / (bounds[2] - bounds[0]), 270 / (bounds[3] - bounds[1]))
    shape = (round((bounds[2] - bounds[0]) * scale), round((bounds[3] - bounds[1]) * scale))
    position = ((SIZE - shape[0]) // 2, 40)
    args.preview_dir.mkdir(parents=True, exist_ok=True)
    compressed: list[bytes] = []
    descriptors = []
    preview_tiles = []
    for name, frames in clips.items():
        descriptors.append((name, len(compressed), len(frames)))
        previews = []
        for rgb in frames:
            alpha = matte(rgb)
            clean = rgb.copy()
            fringe = (alpha > 0) & (alpha < 255)
            clean[:, :, 1][fringe] = np.minimum(clean[:, :, 1][fringe],
                                               np.maximum(clean[:, :, 0][fringe], clean[:, :, 2][fringe]))
            foreground = Image.fromarray(np.dstack((clean, alpha))).crop(tuple(bounds))
            foreground = foreground.resize(shape, Image.Resampling.LANCZOS)
            canvas = Image.new("RGB", (SIZE, SIZE), BACKGROUND)
            canvas.paste(foreground, position, foreground)
            raw = encode565(canvas)
            block = zlib.compress(raw, level=9)
            if zlib.decompress(block) != raw:
                raise ValueError("Frame compression failed round-trip verification")
            compressed.append(block)
            previews.append(canvas)
        previews[0].save(args.preview_dir / f"{name}.png")
        previews[0].save(args.preview_dir / f"{name}.gif", save_all=True, append_images=previews[1:],
                         duration=round(1000 / args.fps), loop=0)
        tile = previews[len(previews) // 2].copy()
        ImageDraw.Draw(tile).text((130, 330), name, fill=(100, 54, 86))
        preview_tiles.append(tile)
    offset = HEADER.size + len(descriptors) * CLIP.size + len(compressed) * FRAME.size
    pack = bytearray(HEADER.pack(MAGIC, 1, SIZE, SIZE, args.fps, len(descriptors), len(compressed)))
    for name, first, count in descriptors:
        pack.extend(CLIP.pack(name.encode("ascii"), first, count))
    for block in compressed:
        pack.extend(FRAME.pack(offset, len(block)))
        offset += len(block)
    for block in compressed:
        pack.extend(block)
    if len(pack) != offset:
        raise ValueError("Frame pack offsets do not match file size")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(pack)
    sheet = Image.new("RGB", (SIZE * len(preview_tiles), SIZE), BACKGROUND)
    for index, tile in enumerate(preview_tiles):
        sheet.paste(tile, (index * SIZE, 0))
    sheet.save(args.preview_dir / "contact-sheet.png")
    manifest = {"format": "BDGZ565-v1", "size": [SIZE, SIZE], "fps": args.fps,
                "frames": len(compressed), "bytes": len(pack), "sha256": hashlib.sha256(pack).hexdigest(),
                "source_bounds": bounds, "character_size": list(shape),
                "clips": [{"name": name, "first_frame": first, "frame_count": count}
                          for name, first, count in descriptors]}
    args.output.with_suffix(".json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(manifest, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
