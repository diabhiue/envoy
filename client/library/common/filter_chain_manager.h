#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "client/library/c_api/envoy_client.h"

namespace EnvoyClient {

/**
 * FilterChainManager orchestrates client interceptors and (future) server-pushed
 * filter chains for request and response header processing.
 *
 * Phase 2 supports synchronous execution only. The filter_cb is always fired
 * before apply_request/response_filters returns, and request_id is always 0.
 *
 * Execution order (CLIENT_WRAPS_SERVER merge policy — the default):
 *   1. Interceptors in PHASE_PRE_REQUEST / PHASE_PRE_RESPONSE
 *   2. Server filter chain (pass-through placeholder — Phase 3 will wire HeadlessFilterChain)
 *   3. Interceptors in PHASE_POST_REQUEST / PHASE_POST_RESPONSE
 *   4. Callback with final (possibly modified) headers
 */
class FilterChainManager {
public:
  FilterChainManager();

  /**
   * Register a named interceptor. Returns ENVOY_CLIENT_ERROR if the name is
   * already registered.
   */
  envoy_client_status addInterceptor(const std::string& name, envoy_client_interceptor_cb callback,
                                     void* context);

  /**
   * Remove a previously registered interceptor. Returns ENVOY_CLIENT_ERROR if
   * no interceptor with that name exists.
   */
  envoy_client_status removeInterceptor(const std::string& name);

  /**
   * Apply request filters (interceptors + server chain) to the provided headers.
   * The callback is invoked synchronously. Ownership of modified_headers in the
   * callback is transferred to the caller; free with envoy_client_free_headers().
   * Returns 0 (Phase 2: synchronous, cannot be cancelled).
   */
  uint64_t applyRequestFilters(const char* cluster_name, envoy_client_headers* headers,
                                envoy_client_filter_cb callback, void* context);

  /**
   * Apply response filters. Same semantics as applyRequestFilters.
   */
  uint64_t applyResponseFilters(const char* cluster_name, envoy_client_headers* headers,
                                 envoy_client_filter_cb callback, void* context);

  /**
   * Cancel an in-flight filter operation. No-op for Phase 2.
   */
  void cancelFilter(uint64_t request_id);

  /**
   * Deep-copy a headers struct. Caller takes ownership and must free with
   * freeHeaders(). The struct pointer itself is heap-allocated.
   */
  static envoy_client_headers* copyHeaders(const envoy_client_headers* src);

  /**
   * Free a headers struct allocated by copyHeaders(). Also frees the struct
   * pointer so this must not be called on stack-allocated structs.
   */
  static void freeHeaders(envoy_client_headers* headers);

private:
  /**
   * Run all registered interceptors for a given phase. Returns ENVOY_CLIENT_OK
   * if all pass or ENVOY_CLIENT_DENIED if any interceptor aborts.
   */
  envoy_client_status runInterceptors(envoy_client_headers* headers, const char* cluster_name,
                                       envoy_client_interceptor_phase phase);

  struct Interceptor {
    std::string name;
    envoy_client_interceptor_cb callback;
    void* context;
  };

  mutable std::mutex mutex_;
  std::vector<Interceptor> interceptors_;
};

} // namespace EnvoyClient
