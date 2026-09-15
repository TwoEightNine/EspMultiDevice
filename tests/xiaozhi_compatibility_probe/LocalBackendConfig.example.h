#pragma once

// Copy to LocalBackendConfig.h (ignored by Git), replace the address, and paste
// the PEM root produced by tools/xiaozhi_local's Caddy instance.
namespace XiaozhiProbeConfig {
inline constexpr char TargetName[] = "local-self-hosted";
inline constexpr char BootstrapUrl[] = "https://192-168-1-10.sslip.io:8443/xiaozhi/ota/";
inline constexpr char ActivationUrl[] = "https://192-168-1-10.sslip.io:8443/xiaozhi/ota/activate";
inline constexpr char CaCertificate[] = R"PEM(-----BEGIN CERTIFICATE-----
PASTE THE LOCAL CADDY ROOT CERTIFICATE HERE
-----END CERTIFICATE-----
)PEM";
} // namespace XiaozhiProbeConfig
