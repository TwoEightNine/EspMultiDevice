#pragma once

// Copy this file to ProbeConfig.h (ignored by Git), replace both PEM blocks,
// and make Host exactly match a DNS name or IP SAN in the server certificate.
namespace AssistantProbeConfig {

inline constexpr char Host[] = "esp-media-probe.local";
inline constexpr uint16_t Port = 8765;
inline constexpr char Path[] = "/echo";
inline constexpr char Subprotocol[] = "esp-media-stage0";
inline constexpr char HeaderName[] = "X-ESP-Media-Probe";
inline constexpr char HeaderValue[] = "stage0";
inline constexpr char HeaderAcceptedMessage[] = "header-ok";

inline constexpr char CorrectCaPem[] = R"PEM(-----BEGIN CERTIFICATE-----
REPLACE WITH THE CA CERTIFICATE THAT SIGNED THE ECHO SERVER CERTIFICATE
-----END CERTIFICATE-----
)PEM";

inline constexpr char WrongCaPem[] = R"PEM(-----BEGIN CERTIFICATE-----
REPLACE WITH A DIFFERENT, VALID CA CERTIFICATE
-----END CERTIFICATE-----
)PEM";

} // namespace AssistantProbeConfig
