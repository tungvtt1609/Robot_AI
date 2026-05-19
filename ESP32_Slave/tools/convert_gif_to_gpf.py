#!/usr/bin/env python3
import argparse
import struct
import subprocess
import sys


def probe_frame_count(path):
    cmd = [
        "ffprobe", "-v", "error", "-select_streams", "v:0",
        "-show_entries", "stream=nb_frames", "-of", "default=nw=1:nk=1", path,
    ]
    result = subprocess.run(cmd, check=True, capture_output=True, text=True)
    text = result.stdout.strip()
    return int(text) if text and text != "N/A" else 0


def main():
    parser = argparse.ArgumentParser(description="Convert GIF to ESP32 LCD predecoded GPF stream")
    parser.add_argument("input")
    parser.add_argument("output")
    parser.add_argument("--width", type=int, default=160)
    parser.add_argument("--height", type=int, default=240)
    parser.add_argument("--fps", type=int, default=24)
    args = parser.parse_args()

    frame_count = probe_frame_count(args.input)
    if frame_count <= 0:
        raise SystemExit("Unable to determine frame count")

    ffmpeg_cmd = [
        "ffmpeg", "-v", "error", "-i", args.input,
        "-vf", f"fps={args.fps},scale={args.width}:{args.height}:flags=lanczos",
        "-f", "rawvideo", "-pix_fmt", "rgb24", "-",
    ]
    proc = subprocess.Popen(ffmpeg_cmd, stdout=subprocess.PIPE)
    frame_size = args.width * args.height * 3
    converted = 0

    with open(args.output, "wb") as out:
        out.write(struct.pack("<4sHHHHHH", b"GPF1", args.width, args.height, 320, 480, args.fps, 0))

        while True:
            raw = proc.stdout.read(frame_size)
            if not raw:
                break
            if len(raw) != frame_size:
                raise SystemExit("Truncated raw frame from ffmpeg")

            frame = bytearray(args.width * args.height * 2)
            for i in range(args.width * args.height):
                r = raw[i * 3]
                g = raw[i * 3 + 1]
                b = raw[i * 3 + 2]
                rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
                panel_word = ((rgb565 >> 8) | ((rgb565 & 0xFF) << 8)) & 0xFFFF
                struct.pack_into("<H", frame, i * 2, panel_word)
            out.write(frame)
            converted += 1

        proc.wait()
        if proc.returncode != 0:
            raise SystemExit(f"ffmpeg failed with exit code {proc.returncode}")

        out.seek(14)
        out.write(struct.pack("<H", converted))

    print(f"Wrote {converted} frames at {args.width}x{args.height}, target {args.fps} fps: {args.output}")


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as exc:
        print(exc.stderr, file=sys.stderr)
        raise
