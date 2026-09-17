# Stage 2 assistant contract

Build with tools/build_assistant_contract.sh. The isolated runtime sketch
checks strict secure URL rejection, bounded protocol JSON, explicit
features.mcp false, v1/v2/v3 binary envelopes, malformed input, session
matching, caption truncation, state/timeout contracts, queue limits, overflow,
and generation filtering, plus the deterministic assistant avatar asset
mapping and filename contract. It performs no network or audio I/O.
