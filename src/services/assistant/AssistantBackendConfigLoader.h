#pragma once

#include "XiaozhiBootstrap.h"

#if __has_include("LocalAssistantBackendConfig.h")
#include "LocalAssistantBackendConfig.h"
#define ESP_MEDIA_HAS_LOCAL_ASSISTANT_BACKEND 1
#else
#define ESP_MEDIA_HAS_LOCAL_ASSISTANT_BACKEND 0
#endif

inline AssistantBackendConfig loadAssistantBackendConfig() {
#if ESP_MEDIA_HAS_LOCAL_ASSISTANT_BACKEND
  return {
    LocalAssistantBackendConfig::BootstrapUrl,
    LocalAssistantBackendConfig::ActivationUrl,
    LocalAssistantBackendConfig::CaCertificate,
    sizeof(LocalAssistantBackendConfig::CaCertificate)
  };
#else
  return {nullptr, nullptr, nullptr, 0};
#endif
}

