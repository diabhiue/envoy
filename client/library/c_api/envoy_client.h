#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle to the client engine.
typedef struct envoy_client_engine* envoy_client_handle;

// Status codes.
typedef enum {
  ENVOY_CLIENT_OK = 0,
  ENVOY_CLIENT_ERROR = 1,
  ENVOY_CLIENT_DENIED = 2,
  ENVOY_CLIENT_UNAVAILABLE = 3,
  ENVOY_CLIENT_TIMEOUT = 4,
} envoy_client_status;

// Key-value header pair.
typedef struct {
  const char* key;
  size_t key_len;
  const char* value;
  size_t value_len;
} envoy_client_header;

// Header map (array of key-value pairs).
typedef struct {
  envoy_client_header* headers;
  size_t count;
} envoy_client_headers;

// Endpoint information.
typedef struct {
  const char* address;
  uint32_t port;
  uint32_t weight;
  uint32_t priority;
  uint32_t health_status; // 0=unknown, 1=healthy, 2=degraded, 3=unhealthy
} envoy_client_endpoint;

// List of endpoints.
typedef struct {
  envoy_client_endpoint* endpoints;
  size_t count;
} envoy_client_endpoint_list;

// Request metadata for LB decisions.
typedef struct {
  envoy_client_headers* metadata; // Key-value metadata for subset LB matching
  const char* path;               // Request path for route matching
  const char* authority;           // :authority header value

  // --- Client-side LB overrides (all optional, NULL/0 = not set) ---

  // Override host: bypass LB and pick this endpoint directly.
  const char* override_host;
  uint32_t override_host_strict; // 1 = fail if unhealthy, 0 = fall back to LB

  // Hash key for consistent-hashing LB (ring-hash, maglev).
  const char* hash_key;
  size_t hash_key_len;
} envoy_client_request_context;

// --- Lifecycle ---

// Create a client engine from bootstrap YAML.
// Returns NULL on failure.
envoy_client_handle envoy_client_create(const char* bootstrap_config, size_t config_len);

// Wait for the engine to be ready (initial xDS config received).
// Returns ENVOY_CLIENT_OK if ready, ENVOY_CLIENT_TIMEOUT if timed out.
envoy_client_status envoy_client_wait_ready(envoy_client_handle handle, uint32_t timeout_seconds);

// Destroy the client engine and free all resources.
void envoy_client_destroy(envoy_client_handle handle);

// --- Endpoint Resolution ---

// Resolve all endpoints for a cluster.
envoy_client_status envoy_client_resolve(envoy_client_handle handle, const char* cluster_name,
                                         envoy_client_endpoint_list* out_endpoints);

// Pick a single endpoint using the configured LB policy.
envoy_client_status envoy_client_pick_endpoint(envoy_client_handle handle,
                                               const char* cluster_name,
                                               const envoy_client_request_context* request_ctx,
                                               envoy_client_endpoint* out_endpoint);

// Free an endpoint list returned by envoy_client_resolve.
void envoy_client_free_endpoints(envoy_client_endpoint_list* endpoints);

// --- LB Feedback ---

// Report the result of a request for feedback-driven LB.
void envoy_client_report_result(envoy_client_handle handle, const envoy_client_endpoint* endpoint,
                                uint32_t status_code, uint64_t latency_ms);

// --- LB Policy Override ---

// Override the LB policy for a specific cluster. Pass NULL lb_policy_name to clear.
envoy_client_status envoy_client_set_cluster_lb_policy(envoy_client_handle handle,
                                                       const char* cluster_name,
                                                       const char* lb_policy_name);

// Set the default LB policy override for all clusters. Pass NULL to clear.
envoy_client_status envoy_client_set_default_lb_policy(envoy_client_handle handle,
                                                       const char* lb_policy_name);

// --- Config Watch ---

typedef enum {
  ENVOY_CLIENT_CONFIG_ADDED = 0,
  ENVOY_CLIENT_CONFIG_UPDATED = 1,
  ENVOY_CLIENT_CONFIG_REMOVED = 2,
} envoy_client_config_event;

// Callback for xDS config changes.
typedef void (*envoy_client_config_cb)(const char* resource_type, const char* resource_name,
                                       envoy_client_config_event event, void* context);

// Watch for config changes. resource_type NULL = watch all types.
envoy_client_status envoy_client_watch_config(envoy_client_handle handle,
                                              const char* resource_type,
                                              envoy_client_config_cb callback, void* context);

// --- Filter Chain (Phase 2) ---

// Callback fired when filter application completes (sync or async).
// Caller takes ownership of modified_headers and must call envoy_client_free_headers.
typedef void (*envoy_client_filter_cb)(envoy_client_status status,
                                       envoy_client_headers* modified_headers, void* context);

// Apply request filters (interceptors + server filter chain) for a cluster.
// Callback fires when all filters have run. Returns a request_id for cancellation.
// Phase 2: fires synchronously before this function returns; request_id is always 0.
uint64_t envoy_client_apply_request_filters(envoy_client_handle handle,
                                             const char* cluster_name,
                                             envoy_client_headers* headers,
                                             envoy_client_filter_cb callback, void* context);

// Apply response filters for a cluster. Same semantics as apply_request_filters.
uint64_t envoy_client_apply_response_filters(envoy_client_handle handle,
                                              const char* cluster_name,
                                              envoy_client_headers* headers,
                                              envoy_client_filter_cb callback, void* context);

// Cancel an in-flight filter operation (no-op for Phase 2 synchronous operations).
void envoy_client_cancel_filter(envoy_client_handle handle, uint64_t request_id);

// Free headers returned by filter callbacks. Also frees the struct pointer itself.
void envoy_client_free_headers(envoy_client_headers* headers);

// --- Client Interceptors ---

typedef enum {
  ENVOY_CLIENT_PHASE_PRE_REQUEST = 0,
  ENVOY_CLIENT_PHASE_POST_REQUEST = 1,
  ENVOY_CLIENT_PHASE_PRE_RESPONSE = 2,
  ENVOY_CLIENT_PHASE_POST_RESPONSE = 3,
} envoy_client_interceptor_phase;

// Interceptor callback. Return ENVOY_CLIENT_OK to continue, ENVOY_CLIENT_DENIED to abort.
typedef envoy_client_status (*envoy_client_interceptor_cb)(envoy_client_headers* headers,
                                                           const char* cluster_name,
                                                           envoy_client_interceptor_phase phase,
                                                           void* context);

// Register a named interceptor. Interceptors execute in registration order.
envoy_client_status envoy_client_add_interceptor(envoy_client_handle handle, const char* name,
                                                 envoy_client_interceptor_cb callback,
                                                 void* context);

// Remove a previously registered interceptor by name.
envoy_client_status envoy_client_remove_interceptor(envoy_client_handle handle, const char* name);

// --- LB Context Provider ---

// Callback invoked during pick_endpoint to let the app enrich the LB context.
typedef void (*envoy_client_lb_context_cb)(const char* cluster_name,
                                           envoy_client_request_context* ctx, void* context);

// Set a global LB context provider.
envoy_client_status envoy_client_set_lb_context_provider(envoy_client_handle handle,
                                                         envoy_client_lb_context_cb callback,
                                                         void* context);

#ifdef __cplusplus
}
#endif
