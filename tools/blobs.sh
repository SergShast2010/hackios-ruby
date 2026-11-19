#!/bin/sh
# Large binaries (images, logs, build outputs) live outside git in ../blobs,
# addressed by sha256 and listed in BLOBS.sha256.
# Usage: tools/blobs.sh verify | restore     (BLOB_DIR defaults to ../blobs)
set -eu
cd "$(dirname "$0")/.."
B=${BLOB_DIR:-../blobs}
case "${1:-verify}" in
  verify) sha256sum --quiet -c BLOBS.sha256 && echo "blobs ok" ;;
  restore)
    while read -r sum path; do
      if [ -f "$path" ] && [ "$(sha256sum < "$path" | cut -d' ' -f1)" = "$sum" ]; then continue; fi
      mkdir -p "$(dirname "$path")"; cp "$B/$sum" "$path"; echo "restored $path"
    done < BLOBS.sha256
    sha256sum --quiet -c BLOBS.sha256 && echo "blobs ok" ;;
  *) echo "usage: $0 verify|restore" >&2; exit 2 ;;
esac
