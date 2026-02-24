#pragma once

#include <memory>

#include "client/library/cc/client.h"
#include "client/library/c_api/envoy_client.h"
#include "client/library/common/filter_chain_manager.h"

// Internal definition of the opaque handle exposed as envoy_client_handle.
// Production code (envoy_client.cc) and tests both include this header so
// they agree on the struct layout without exposing it in the public C API.
struct envoy_client_engine {
  std::unique_ptr<EnvoyClient::Client> client;

  // LB context provider: invoked during every pick_endpoint call so the app
  // can enrich the LB context (e.g. inject a hash key or override host).
  envoy_client_lb_context_cb lb_context_cb{nullptr};
  void* lb_context_user_ctx{nullptr};

  // Filter chain manager: handles client interceptors and (Phase 3+) the
  // server-pushed filter chain for apply_request/response_filters.
  std::unique_ptr<EnvoyClient::FilterChainManager> filter_chain_manager{
      std::make_unique<EnvoyClient::FilterChainManager>()};
};
