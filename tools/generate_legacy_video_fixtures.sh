#!/usr/bin/env sh
# Generate deterministic 176x144 / 10 fps fixtures for on-device legacy-video
# regression testing. Use FFmpeg 9.0.1 so the fixtures match the pinned source.
set -eu

output_dir=${1:-tests/video/fixtures}
version=$(ffmpeg -version | sed -n '1p')
case "$version" in
  *"ffmpeg version 9.0.1"*) ;;
  *) echo "FFmpeg 9.0.1 is required; found: $version" >&2; exit 1 ;;
esac

mkdir -p "$output_dir"
ffmpeg -hide_banner -loglevel error -y -f lavfi -i testsrc2=size=176x144:rate=10 \
  -frames:v 20 -an -c:v h263 -f 3gp "$output_dir/h263-176x144-10fps.3gp"
ffmpeg -hide_banner -loglevel error -y -f lavfi -i testsrc2=size=176x144:rate=10 \
  -frames:v 20 -an -c:v mpeg4 -profile:v 0 -f mp4 "$output_dir/mpeg4p2-176x144-10fps.mp4"
