#pragma once

// Copy this file to LocalAssistantBackendConfig.h (ignored by git), replace
// the hostname, and paste the Caddy root CA PEM. Never put tokens or API keys
// in this firmware configuration.
namespace LocalAssistantBackendConfig {
inline constexpr char BootstrapUrl[] =
    "https://192-168-1-10.sslip.io:8443/xiaozhi/ota/";
inline constexpr char ActivationUrl[] =
    "https://192-168-1-10.sslip.io:8443/xiaozhi/ota/activate";
inline constexpr char CaCertificate[] = R"PEM(
-----BEGIN CERTIFICATE-----
REPLACE WITH THE CADDY LOCAL ROOT CERTIFICATE
-----END CERTIFICATE-----
)PEM";
}  // namespace LocalAssistantBackendConfig

