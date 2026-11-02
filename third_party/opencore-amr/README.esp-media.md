# OpenCORE AMR-NB decoder subset

This directory vendors the decoder-only AMR-NB source subset from OpenCORE AMR
0.1.6.  The source archive was `opencore-amr-0.1.6.tar.gz` with SHA-256
`483eb4061088e2b34b358e47540b5d495a96cd468e361050fae615b1809dc4a1`.

It is used only for 3GP/MP4 `samr` tracks (8 kHz mono AMR-NB).  AMR-NB
encoding and AMR-WB are excluded.  `OpenCoreAmr_*.c` and `OpenCoreAmr_api.c`
in `src/vendor_shims/opencore_amr/` are build wrappers; the upstream source and
Apache-2.0 notice are retained in this directory.  Quoted includes of headers in
other directories of the tree are rewritten relative to the including file by
`tools/localize_vendor_includes.py`, because Arduino offers no per-project
include paths.  This include-path-only patch changes no declarations,
definitions, or behavior.
