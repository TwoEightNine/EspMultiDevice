# Assistant conversation contract

Build with tools/build_assistant_contract.sh. The isolated runtime sketch
checks strict secure URL rejection, bounded protocol JSON, explicit
features.mcp false, v1/v2/v3 binary envelopes, malformed input, session
matching, heartbeat and reset envelopes, malformed reset acknowledgements,
caption truncation, UTF-8-safe five-turn transcript eviction and draft
cancellation, state/timeout contracts, queue limits, overflow, and
generation/request filtering, plus the deterministic assistant avatar asset
mapping and filename contract. It performs no network or audio I/O.
