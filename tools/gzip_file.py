"""Gzip one file (used at build time to shrink the web UI before embedding it)."""
import gzip
import sys

src, dst = sys.argv[1], sys.argv[2]
with open(src, "rb") as f:
    data = f.read()
with open(dst, "wb") as f:
    # mtime=0 keeps the output identical between builds
    f.write(gzip.compress(data, compresslevel=9, mtime=0))
