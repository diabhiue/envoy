#include "client/library/common/filter_chain_manager.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace EnvoyClient {

FilterChainManager::FilterChainManager() = default;

envoy_client_status FilterChainManager::addInterceptor(const std::string& name,
                                                        envoy_client_interceptor_cb callback,
                                                        void* context) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& ic : interceptors_) {
    if (ic.name == name) {
      return ENVOY_CLIENT_ERROR; // name already registered
    }
  }
  interceptors_.push_back({name, callback, context});
  return ENVOY_CLIENT_OK;
}

envoy_client_status FilterChainManager::removeInterceptor(const std::string& name) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = std::find_if(interceptors_.begin(), interceptors_.end(),
                          [&name](const Interceptor& ic) { return ic.name == name; });
  if (it == interceptors_.end()) {
    return ENVOY_CLIENT_ERROR;
  }
  interceptors_.erase(it);
  return ENVOY_CLIENT_OK;
}

// static
envoy_client_headers* FilterChainManager::copyHeaders(const envoy_client_headers* src) {
  auto* copy = static_cast<envoy_client_headers*>(malloc(sizeof(envoy_client_headers)));
  if (src == nullptr || src->count == 0 || src->headers == nullptr) {
    copy->headers = nullptr;
    copy->count = 0;
    return copy;
  }

  copy->count = src->count;
  copy->headers =
      static_cast<envoy_client_header*>(calloc(src->count, sizeof(envoy_client_header)));
  for (size_t i = 0; i < src->count; ++i) {
    const envoy_client_header& s = src->headers[i];
    envoy_client_header& d = copy->headers[i];
    if (s.key != nullptr && s.key_len > 0) {
      // strndup copies exactly key_len bytes and null-terminates.
      d.key = strndup(s.key, s.key_len);
      d.key_len = s.key_len;
    }
    if (s.value != nullptr) {
      d.value = strndup(s.value, s.value_len);
      d.value_len = s.value_len;
    }
  }
  return copy;
}

// static
void FilterChainManager::freeHeaders(envoy_client_headers* headers) {
  if (headers == nullptr) {
    return;
  }
  if (headers->headers != nullptr) {
    for (size_t i = 0; i < headers->count; ++i) {
      free(const_cast<char*>(headers->headers[i].key));
      free(const_cast<char*>(headers->headers[i].value));
    }
    free(headers->headers);
  }
  free(headers);
}

envoy_client_status FilterChainManager::runInterceptors(envoy_client_headers* headers,
                                                         const char* cluster_name,
                                                         envoy_client_interceptor_phase phase) {
  // Snapshot interceptors under lock, then invoke without holding the lock so
  // interceptors can call back into the library (e.g. to read endpoints).
  std::vector<Interceptor> snapshot;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot = interceptors_;
  }

  for (const auto& ic : snapshot) {
    envoy_client_status status = ic.callback(headers, cluster_name, phase, ic.context);
    if (status != ENVOY_CLIENT_OK) {
      return status;
    }
  }
  return ENVOY_CLIENT_OK;
}

uint64_t FilterChainManager::applyRequestFilters(const char* cluster_name,
                                                   envoy_client_headers* headers,
                                                   envoy_client_filter_cb callback,
                                                   void* context) {
  // Deep-copy the caller's headers; this copy travels through the filter chain
  // and ownership is ultimately transferred to the callback caller.
  envoy_client_headers* working = copyHeaders(headers);

  // Phase 1: pre-request client interceptors.
  envoy_client_status status =
      runInterceptors(working, cluster_name, ENVOY_CLIENT_PHASE_PRE_REQUEST);

  if (status == ENVOY_CLIENT_OK) {
    // Phase 2: server filter chain.
    // TODO(Phase 3): wire HeadlessFilterChain here for header_mutation /
    //                credential_injector / rbac and async filters.

    // Phase 3: post-request client interceptors.
    status = runInterceptors(working, cluster_name, ENVOY_CLIENT_PHASE_POST_REQUEST);
  }

  // Fire the callback. Ownership of `working` transfers to the caller.
  callback(status, working, context);

  // Return 0: synchronous completion, nothing to cancel.
  return 0;
}

uint64_t FilterChainManager::applyResponseFilters(const char* cluster_name,
                                                    envoy_client_headers* headers,
                                                    envoy_client_filter_cb callback,
                                                    void* context) {
  envoy_client_headers* working = copyHeaders(headers);

  envoy_client_status status =
      runInterceptors(working, cluster_name, ENVOY_CLIENT_PHASE_PRE_RESPONSE);

  if (status == ENVOY_CLIENT_OK) {
    // TODO(Phase 3): server filter chain.
    status = runInterceptors(working, cluster_name, ENVOY_CLIENT_PHASE_POST_RESPONSE);
  }

  callback(status, working, context);
  return 0;
}

void FilterChainManager::cancelFilter(uint64_t /*request_id*/) {
  // No-op for Phase 2 (synchronous).
}

} // namespace EnvoyClient
