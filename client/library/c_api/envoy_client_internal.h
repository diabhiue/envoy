#pragma once

#include <memory>

#include "client/library/cc/client.h"
#include "client/library/c_api/envoy_client.h"

// Internal definition of the opaque handle exposed as envoy_client_handle.
// Production code (envoy_client.cc) and tests both include this header so
// they agree on the struct layout without exposing it in the public C API.
struct envoy_client_engine {
  std::unique_ptr<EnvoyClient::Client> client;
};
