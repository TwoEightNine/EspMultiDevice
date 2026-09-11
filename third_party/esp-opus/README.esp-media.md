# ESP Media import of 78/esp-opus

This is the mono encoder/decoder subset of 78/esp-opus 1.0.5. Version, source,
commit, and archive hash are recorded in `VERSION.txt`; the registry metadata is
retained as `idf_component.yml`.

The component metadata labels the package MIT. The contained Opus codec source
has its own BSD-style terms and royalty-free patent declarations, retained in
`COPYING` and `LICENSE_PLEASE_READ.txt`. Preserve all three records when
redistributing or updating the import.

The retained implementation contains core Opus, the required repacketizer,
CELT, SILK common/fixed-point code, and the ESP32-S3 Xtensa LX7 headers. Tests,
tools, demos, floating-point SILK, DNN/DRED, multistream/projection source, and
ARM/MIPS/x86 code are excluded. A dependency-linked diagnostic build proves the
remaining source set resolves for the mono APIs.

Every implementation unit is compiled separately through
`src/vendor_shims/opus/`. `OpusBuildConfig.h` mirrors the ESP component's
fixed-point, disabled-float-API, LX7, stack-allocation, and `-O2` configuration.
Project code uses the direct Opus C API only through `OpusCodec`; the separate
`esp-opus-encoder` wrapper is not imported.

Arduino's `build_opt.h` response file does not expand sketch-path placeholders,
and Arduino IDE compiler processes do not have a stable working directory. This
import therefore carries an include-path-only portability patch: 134 quoted
includes across 63 files that cross the upstream `celt/`, `silk/`, `src/`, and
`include/` directories use paths relative to the including file. No
declarations, definitions, build configuration, or codec behavior are changed.
The SHA-256 in `VERSION.txt` identifies the unmodified source archive from which
this patched subset came.
