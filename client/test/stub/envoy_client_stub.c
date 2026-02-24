/*
 * Stub implementation of the Envoy Client C ABI for unit testing.
 *
 * Provides a minimal in-process implementation that:
 * - Returns two static endpoints for "test-cluster"
 * - Fires an initial CONFIG_ADDED event on watchConfig
 * - Invokes the LB context provider callback during pick_endpoint
 * - Returns UNAVAILABLE for unknown clusters
 *
 * Build as a shared library:
 *   gcc -shared -fPIC -I../../.. -o libenvoy_client.so envoy_client_stub.c
 */

#include "client/library/c_api/envoy_client.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Concrete definition of the opaque handle type. */
struct envoy_client_engine {
  envoy_client_lb_context_cb lb_context_cb;
  void*                      lb_context_user_ctx;
  envoy_client_config_cb     config_cb;
  void*                      config_user_ctx;
  char                       lb_override[256];
  char                       default_lb_override[256];
};

/* ---- Lifecycle ---------------------------------------------------------- */

envoy_client_handle envoy_client_create(const char* bootstrap_config, size_t config_len) {
  if (!bootstrap_config || config_len == 0) return NULL;
  struct envoy_client_engine* h = calloc(1, sizeof(*h));
  return h;
}

envoy_client_status envoy_client_wait_ready(envoy_client_handle h, uint32_t timeout_seconds) {
  (void)timeout_seconds;
  return h ? ENVOY_CLIENT_OK : ENVOY_CLIENT_ERROR;
}

void envoy_client_destroy(envoy_client_handle h) { free(h); }

/* ---- Endpoint resolution ------------------------------------------------ */

static const char* KNOWN_CLUSTER = "test-cluster";

envoy_client_status envoy_client_resolve(envoy_client_handle h, const char* cluster_name,
                                         envoy_client_endpoint_list* out) {
  if (!h || !cluster_name || !out) return ENVOY_CLIENT_ERROR;
  if (strcmp(cluster_name, KNOWN_CLUSTER) != 0) {
    out->endpoints = NULL;
    out->count     = 0;
    return ENVOY_CLIENT_UNAVAILABLE;
  }
  out->count     = 2;
  out->endpoints = malloc(2 * sizeof(envoy_client_endpoint));

  out->endpoints[0].address       = strdup("127.0.0.1");
  out->endpoints[0].port          = 8080;
  out->endpoints[0].weight        = 1;
  out->endpoints[0].priority      = 0;
  out->endpoints[0].health_status = 1; /* HEALTHY */

  out->endpoints[1].address       = strdup("127.0.0.1");
  out->endpoints[1].port          = 8081;
  out->endpoints[1].weight        = 2;
  out->endpoints[1].priority      = 0;
  out->endpoints[1].health_status = 1;

  return ENVOY_CLIENT_OK;
}

envoy_client_status envoy_client_pick_endpoint(envoy_client_handle h,
                                               const char* cluster_name,
                                               const envoy_client_request_context* req_ctx,
                                               envoy_client_endpoint* out) {
  if (!h || !cluster_name || !out) return ENVOY_CLIENT_ERROR;
  if (strcmp(cluster_name, KNOWN_CLUSTER) != 0) return ENVOY_CLIENT_UNAVAILABLE;

  /* Copy context so the LB provider can mutate it. */
  envoy_client_request_context mutable_ctx;
  memset(&mutable_ctx, 0, sizeof(mutable_ctx));
  if (req_ctx) mutable_ctx = *req_ctx;

  /* Invoke the LB context provider if registered. */
  if (h->lb_context_cb) {
    h->lb_context_cb(cluster_name, &mutable_ctx, h->lb_context_user_ctx);
  }

  /* If an override host was set, honour it (trivially: pick port 8080/8081). */
  uint32_t chosen_port = 8080;
  if (mutable_ctx.override_host) {
    /* Parse "127.0.0.1:<port>" */
    const char* colon = strrchr(mutable_ctx.override_host, ':');
    if (colon) chosen_port = (uint32_t)atoi(colon + 1);
  }

  out->address       = strdup("127.0.0.1");
  out->port          = chosen_port;
  out->weight        = 1;
  out->priority      = 0;
  out->health_status = 1;
  return ENVOY_CLIENT_OK;
}

void envoy_client_free_endpoints(envoy_client_endpoint_list* list) {
  if (!list || !list->endpoints) return;
  for (size_t i = 0; i < list->count; i++) free((char*)list->endpoints[i].address);
  free(list->endpoints);
  list->endpoints = NULL;
  list->count     = 0;
}

/* ---- LB feedback -------------------------------------------------------- */

void envoy_client_report_result(envoy_client_handle h, const envoy_client_endpoint* ep,
                                uint32_t status_code, uint64_t latency_ms) {
  (void)h; (void)ep; (void)status_code; (void)latency_ms;
}

/* ---- LB policy overrides ----------------------------------------------- */

envoy_client_status envoy_client_set_cluster_lb_policy(envoy_client_handle h,
                                                       const char* cluster, const char* policy) {
  if (!h || !cluster) return ENVOY_CLIENT_ERROR;
  if (policy) snprintf(h->lb_override, sizeof(h->lb_override), "%s", policy);
  else        h->lb_override[0] = '\0';
  return ENVOY_CLIENT_OK;
}

envoy_client_status envoy_client_set_default_lb_policy(envoy_client_handle h, const char* policy) {
  if (!h) return ENVOY_CLIENT_ERROR;
  if (policy) snprintf(h->default_lb_override, sizeof(h->default_lb_override), "%s", policy);
  else        h->default_lb_override[0] = '\0';
  return ENVOY_CLIENT_OK;
}

/* ---- Config watch ------------------------------------------------------- */

envoy_client_status envoy_client_watch_config(envoy_client_handle h, const char* resource_type,
                                              envoy_client_config_cb cb, void* ctx) {
  if (!h || !cb) return ENVOY_CLIENT_ERROR;
  h->config_cb        = cb;
  h->config_user_ctx  = ctx;
  /* Fire an initial ADDED event for the static cluster so watch tests pass. */
  const char* type = (resource_type && resource_type[0]) ? resource_type : "cluster";
  cb(type, KNOWN_CLUSTER, ENVOY_CLIENT_CONFIG_ADDED, ctx);
  return ENVOY_CLIENT_OK;
}

/* ---- Interceptors (Phase 2 stubs) -------------------------------------- */

envoy_client_status envoy_client_add_interceptor(envoy_client_handle h, const char* name,
                                                 envoy_client_interceptor_cb cb, void* ctx) {
  (void)h; (void)name; (void)cb; (void)ctx;
  return ENVOY_CLIENT_OK;
}

envoy_client_status envoy_client_remove_interceptor(envoy_client_handle h, const char* name) {
  (void)h; (void)name;
  return ENVOY_CLIENT_OK;
}

/* ---- LB context provider ----------------------------------------------- */

envoy_client_status envoy_client_set_lb_context_provider(envoy_client_handle h,
                                                         envoy_client_lb_context_cb cb,
                                                         void* ctx) {
  if (!h) return ENVOY_CLIENT_ERROR;
  h->lb_context_cb       = cb;
  h->lb_context_user_ctx = ctx;
  return ENVOY_CLIENT_OK;
}
