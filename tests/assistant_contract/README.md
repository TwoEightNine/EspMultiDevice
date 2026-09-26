# Assistant conversation contract

Build with tools/build_assistant_contract.sh. The isolated runtime sketch
checks strict secure URL rejection, bounded protocol JSON, explicit
MCP disablement plus custom device-tool capability, strict device command/result
schemas, v1/v2/v3 binary envelopes, malformed input, session
matching, heartbeat and reset envelopes, malformed reset acknowledgements,
fixed-panel geometry, voice-control hold activation, two-line human ellipsizing, three-line assistant paging,
background path/dimension contracts, UTF-8-safe five-turn transcript eviction and draft
cancellation, state/timeout contracts, queue limits, overflow, and
generation/request filtering, plus the deterministic assistant avatar asset
mapping and filename contract. It performs no network or audio I/O.
