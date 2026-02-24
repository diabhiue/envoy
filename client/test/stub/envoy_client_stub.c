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

/* Maximum interceptors supported by the stub. */
#define MAX_INTERCEPTORS 8

typedef struct {
  char                        name[128];
  envoy_client_interceptor_cb callback;
  void*                       context;
} stub_interceptor;

/* Concrete definition of the opaque handle type. */
struct envoy_client_engine {
  envoy_client_lb_context_cb lb_context_cb;
  void*                      lb_context_user_ctx;
  envoy_client_config_cb     config_cb;
  void*                      config_user_ctx;
  char                       lb_override[256];
  char                       default_lb_override[256];
  stub_interceptor            interceptors[MAX_INTERCEPTORS];
  int                        num_interceptors;
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

/* ---- Header helpers ---------------------------------------------------- */

/* Deep-copy src headers; caller owns and must call envoy_client_free_headers. */
static envoy_client_headers* stub_copy_headers(const envoy_client_headers* src) {
  envoy_client_headers* h = malloc(sizeof(envoy_client_headers));
  if (!src || src->count == 0 || !src->headers) {
    h->headers = NULL;
    h->count = 0;
    return h;
  }
  h->count = src->count;
  h->headers = calloc(src->count, sizeof(envoy_client_header));
  for (size_t i = 0; i < src->count; i++) {
    if (src->headers[i].key)
      h->headers[i].key = strndup(src->headers[i].key, src->headers[i].key_len);
    h->headers[i].key_len = src->headers[i].key_len;
    if (src->headers[i].value)
      h->headers[i].value = strndup(src->headers[i].value, src->headers[i].value_len);
    h->headers[i].value_len = src->headers[i].value_len;
  }
  return h;
}

void envoy_client_free_headers(envoy_client_headers* h) {
  if (!h) return;
  if (h->headers) {
    for (size_t i = 0; i < h->count; i++) {
      free((char*)h->headers[i].key);
      free((char*)h->headers[i].value);
    }
    free(h->headers);
  }
  free(h);
}

/* Run all registered interceptors for a given phase on the working headers. */
static envoy_client_status stub_run_interceptors(envoy_client_handle h,
                                                   envoy_client_headers* headers,
                                                   const char* cluster,
                                                   envoy_client_interceptor_phase phase) {
  for (int i = 0; i < h->num_interceptors; i++) {
    envoy_client_status s =
        h->interceptors[i].callback(headers, cluster, phase, h->interceptors[i].context);
    if (s != ENVOY_CLIENT_OK) return s;
  }
  return ENVOY_CLIENT_OK;
}

uint64_t envoy_client_apply_request_filters(envoy_client_handle h, const char* cluster,
                                             envoy_client_headers* headers,
                                             envoy_client_filter_cb cb, void* ctx) {
  if (!h || !cluster || !cb) { if (cb) cb(ENVOY_CLIENT_ERROR, NULL, ctx); return 0; }
  envoy_client_headers* working = stub_copy_headers(headers);
  envoy_client_status status = stub_run_interceptors(h, working, cluster,
                                                      ENVOY_CLIENT_PHASE_PRE_REQUEST);
  if (status == ENVOY_CLIENT_OK) {
    /* Server filter chain: pass-through (Phase 2). */
    status = stub_run_interceptors(h, working, cluster, ENVOY_CLIENT_PHASE_POST_REQUEST);
  }
  cb(status, working, ctx);
  return 0;
}

uint64_t envoy_client_apply_response_filters(envoy_client_handle h, const char* cluster,
                                              envoy_client_headers* headers,
                                              envoy_client_filter_cb cb, void* ctx) {
  if (!h || !cluster || !cb) { if (cb) cb(ENVOY_CLIENT_ERROR, NULL, ctx); return 0; }
  envoy_client_headers* working = stub_copy_headers(headers);
  envoy_client_status status = stub_run_interceptors(h, working, cluster,
                                                      ENVOY_CLIENT_PHASE_PRE_RESPONSE);
  if (status == ENVOY_CLIENT_OK) {
    status = stub_run_interceptors(h, working, cluster, ENVOY_CLIENT_PHASE_POST_RESPONSE);
  }
  cb(status, working, ctx);
  return 0;
}

void envoy_client_cancel_filter(envoy_client_handle h, uint64_t request_id) {
  (void)h; (void)request_id; /* No-op: Phase 2 is synchronous. */
}

/* ---- Interceptors ------------------------------------------------------- */

envoy_client_status envoy_client_add_interceptor(envoy_client_handle h, const char* name,
                                                 envoy_client_interceptor_cb cb, void* ctx) {
  if (!h || !name || !cb) return ENVOY_CLIENT_ERROR;
  if (h->num_interceptors >= MAX_INTERCEPTORS) return ENVOY_CLIENT_ERROR;
  for (int i = 0; i < h->num_interceptors; i++) {
    if (strcmp(h->interceptors[i].name, name) == 0) return ENVOY_CLIENT_ERROR; /* duplicate */
  }
  int idx = h->num_interceptors++;
  snprintf(h->interceptors[idx].name, sizeof(h->interceptors[idx].name), "%s", name);
  h->interceptors[idx].callback = cb;
  h->interceptors[idx].context  = ctx;
  return ENVOY_CLIENT_OK;
}

envoy_client_status envoy_client_remove_interceptor(envoy_client_handle h, const char* name) {
  if (!h || !name) return ENVOY_CLIENT_ERROR;
  for (int i = 0; i < h->num_interceptors; i++) {
    if (strcmp(h->interceptors[i].name, name) == 0) {
      /* Shift remaining interceptors down. */
      for (int j = i; j < h->num_interceptors - 1; j++) {
        h->interceptors[j] = h->interceptors[j + 1];
      }
      h->num_interceptors--;
      return ENVOY_CLIENT_OK;
    }
  }
  return ENVOY_CLIENT_ERROR; /* not found */
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
