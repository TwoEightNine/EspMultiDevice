#!/usr/bin/env sh
# Generate deterministic 320x480 / 30 fps fixtures for on-device direct-video
# streaming regression testing. Use FFmpeg 9.0.1 so fixtures match pinned code.
set -eu

output_dir=${1:-tests/video/fixtures}
version=$(ffmpeg -version | sed -n '1p')
case "$version" in
  *"ffmpeg version 9.0.1"*) ;;
  *) echo "FFmpeg 9.0.1 is required; found: $version" >&2; exit 1 ;;
esac

mkdir -p "$output_dir"
ffmpeg -hide_banner -loglevel error -y -f lavfi -i testsrc2=size=320x480:rate=30 \
  -frames:v 60 -an -c:v h263 -f 3gp "$output_dir/h263-320x480-30fps.3gp"
ffmpeg -hide_banner -loglevel error -y -f lavfi -i testsrc2=size=320x480:rate=30 \
  -frames:v 60 -an -c:v mpeg4 -profile:v 0 -f mp4 "$output_dir/mpeg4p2-320x480-30fps.mp4"
ffmpeg -hide_banner -loglevel error -y -f lavfi -i testsrc2=size=320x480:rate=30 \
  -f lavfi -i sine=frequency=440:sample_rate=44100 -frames:v 60 -shortest \
  -c:v libx264 -profile:v baseline -pix_fmt yuv420p -c:a aac \
  "$output_dir/h264-aac-320x480-30fps.mp4"
