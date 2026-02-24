#include "client/library/c_api/envoy_client_internal.h"

#include <cstring>
#include <string>

extern "C" {

envoy_client_handle envoy_client_create(const char* bootstrap_config, size_t config_len) {
  if (bootstrap_config == nullptr || config_len == 0) {
    return nullptr;
  }

  std::string config(bootstrap_config, config_len);
  auto client = EnvoyClient::Client::create(config);
  if (client == nullptr) {
    return nullptr;
  }

  auto* handle = new envoy_client_engine();
  handle->client = std::move(client);
  return handle;
}

envoy_client_status envoy_client_wait_ready(envoy_client_handle handle, uint32_t timeout_seconds) {
  if (handle == nullptr || handle->client == nullptr) {
    return ENVOY_CLIENT_ERROR;
  }
  return handle->client->waitReady(static_cast<int>(timeout_seconds)) ? ENVOY_CLIENT_OK
                                                                      : ENVOY_CLIENT_TIMEOUT;
}

void envoy_client_destroy(envoy_client_handle handle) {
  if (handle != nullptr) {
    handle->client->shutdown();
    delete handle;
  }
}

envoy_client_status envoy_client_resolve(envoy_client_handle handle, const char* cluster_name,
                                         envoy_client_endpoint_list* out_endpoints) {
  if (handle == nullptr || cluster_name == nullptr || out_endpoints == nullptr) {
    return ENVOY_CLIENT_ERROR;
  }

  auto endpoints = handle->client->resolve(cluster_name);
  if (endpoints.empty()) {
    out_endpoints->endpoints = nullptr;
    out_endpoints->count = 0;
    return ENVOY_CLIENT_UNAVAILABLE;
  }

  out_endpoints->count = endpoints.size();
  // Use malloc/calloc so cross-language callers (Go, Java/JNI) can free with free().
  out_endpoints->endpoints =
      static_cast<envoy_client_endpoint*>(calloc(endpoints.size(), sizeof(envoy_client_endpoint)));
  for (size_t i = 0; i < endpoints.size(); ++i) {
    // strdup allocates with malloc so callers can free() the address string.
    out_endpoints->endpoints[i].address = strdup(endpoints[i].address.c_str());
    out_endpoints->endpoints[i].port = endpoints[i].port;
    out_endpoints->endpoints[i].weight = endpoints[i].weight;
    out_endpoints->endpoints[i].priority = endpoints[i].priority;
    out_endpoints->endpoints[i].health_status = endpoints[i].health_status;
  }

  return ENVOY_CLIENT_OK;
}

envoy_client_status envoy_client_pick_endpoint(envoy_client_handle handle,
                                               const char* cluster_name,
                                               const envoy_client_request_context* request_ctx,
                                               envoy_client_endpoint* out_endpoint) {
  if (handle == nullptr || cluster_name == nullptr || out_endpoint == nullptr) {
    return ENVOY_CLIENT_ERROR;
  }

  EnvoyClient::RequestContext ctx;
  if (request_ctx != nullptr) {
    if (request_ctx->path != nullptr) {
      ctx.path = request_ctx->path;
    }
    if (request_ctx->authority != nullptr) {
      ctx.authority = request_ctx->authority;
    }
    if (request_ctx->override_host != nullptr) {
      ctx.override_host = request_ctx->override_host;
      ctx.override_host_strict = request_ctx->override_host_strict != 0;
    }
    if (request_ctx->hash_key != nullptr && request_ctx->hash_key_len > 0) {
      ctx.hash_key = std::string(request_ctx->hash_key, request_ctx->hash_key_len);
    }
  }

  // Invoke the LB context provider (if registered) so the app can enrich the
  // context before the pick — e.g. inject a consistent-hash key or an
  // override host derived from request-level state.
  if (handle->lb_context_cb != nullptr) {
    envoy_client_request_context mutable_ctx{};
    mutable_ctx.path = ctx.path.empty() ? nullptr : ctx.path.c_str();
    mutable_ctx.authority = ctx.authority.empty() ? nullptr : ctx.authority.c_str();
    mutable_ctx.override_host = ctx.override_host.empty() ? nullptr : ctx.override_host.c_str();
    mutable_ctx.override_host_strict = ctx.override_host_strict ? 1u : 0u;
    mutable_ctx.hash_key = ctx.hash_key.empty() ? nullptr : ctx.hash_key.c_str();
    mutable_ctx.hash_key_len = ctx.hash_key.size();
    mutable_ctx.metadata = nullptr;

    handle->lb_context_cb(cluster_name, &mutable_ctx, handle->lb_context_user_ctx);

    // Read back any fields the callback may have added or changed.
    if (mutable_ctx.path != nullptr) {
      ctx.path = mutable_ctx.path;
    }
    if (mutable_ctx.authority != nullptr) {
      ctx.authority = mutable_ctx.authority;
    }
    if (mutable_ctx.override_host != nullptr) {
      ctx.override_host = mutable_ctx.override_host;
    }
    ctx.override_host_strict = mutable_ctx.override_host_strict != 0;
    if (mutable_ctx.hash_key != nullptr && mutable_ctx.hash_key_len > 0) {
      ctx.hash_key = std::string(mutable_ctx.hash_key, mutable_ctx.hash_key_len);
    }
  }

  auto result = handle->client->pickEndpoint(cluster_name, ctx);
  if (!result.has_value()) {
    return ENVOY_CLIENT_UNAVAILABLE;
  }

  // strdup allocates with malloc so cross-language callers can free() it.
  out_endpoint->address = strdup(result->address.c_str());
  out_endpoint->port = result->port;
  out_endpoint->weight = result->weight;
  out_endpoint->priority = result->priority;
  out_endpoint->health_status = result->health_status;

  return ENVOY_CLIENT_OK;
}

void envoy_client_free_endpoints(envoy_client_endpoint_list* endpoints) {
  if (endpoints == nullptr) {
    return;
  }
  if (endpoints->endpoints != nullptr) {
    for (size_t i = 0; i < endpoints->count; ++i) {
      free(const_cast<char*>(endpoints->endpoints[i].address));
    }
    free(endpoints->endpoints);
    endpoints->endpoints = nullptr;
  }
  endpoints->count = 0;
}

void envoy_client_report_result(envoy_client_handle handle,
                                const envoy_client_endpoint* endpoint,
                                uint32_t status_code, uint64_t latency_ms) {
  if (handle == nullptr || endpoint == nullptr || endpoint->address == nullptr) {
    return;
  }
  handle->client->reportResult(endpoint->address, endpoint->port, status_code, latency_ms);
}

envoy_client_status envoy_client_set_cluster_lb_policy(envoy_client_handle handle,
                                                       const char* cluster_name,
                                                       const char* lb_policy_name) {
  if (handle == nullptr || cluster_name == nullptr) {
    return ENVOY_CLIENT_ERROR;
  }
  handle->client->setClusterLbPolicy(cluster_name, lb_policy_name ? lb_policy_name : "");
  return ENVOY_CLIENT_OK;
}

envoy_client_status envoy_client_set_default_lb_policy(envoy_client_handle handle,
                                                       const char* lb_policy_name) {
  if (handle == nullptr) {
    return ENVOY_CLIENT_ERROR;
  }
  handle->client->setDefaultLbPolicy(lb_policy_name ? lb_policy_name : "");
  return ENVOY_CLIENT_OK;
}

envoy_client_status envoy_client_watch_config(envoy_client_handle handle,
                                              const char* resource_type,
                                              envoy_client_config_cb callback, void* context) {
  if (handle == nullptr || callback == nullptr) {
    return ENVOY_CLIENT_ERROR;
  }

  std::string type = resource_type ? resource_type : "";
  handle->client->watchConfig(
      type, [callback, context](const EnvoyClient::ConfigEvent& event) {
        envoy_client_config_event c_event;
        if (event.event == "added") {
          c_event = ENVOY_CLIENT_CONFIG_ADDED;
        } else if (event.event == "updated") {
          c_event = ENVOY_CLIENT_CONFIG_UPDATED;
        } else {
          c_event = ENVOY_CLIENT_CONFIG_REMOVED;
        }
        callback(event.resource_type.c_str(), event.resource_name.c_str(), c_event, context);
      });
  return ENVOY_CLIENT_OK;
}

uint64_t envoy_client_apply_request_filters(envoy_client_handle handle,
                                             const char* cluster_name,
                                             envoy_client_headers* headers,
                                             envoy_client_filter_cb callback, void* context) {
  if (handle == nullptr || cluster_name == nullptr || callback == nullptr) {
    // Fire the callback with an error so the caller is never left waiting.
    if (callback != nullptr) {
      callback(ENVOY_CLIENT_ERROR, nullptr, context);
    }
    return 0;
  }
  return handle->filter_chain_manager->applyRequestFilters(cluster_name, headers, callback,
                                                           context);
}

uint64_t envoy_client_apply_response_filters(envoy_client_handle handle,
                                              const char* cluster_name,
                                              envoy_client_headers* headers,
                                              envoy_client_filter_cb callback, void* context) {
  if (handle == nullptr || cluster_name == nullptr || callback == nullptr) {
    if (callback != nullptr) {
      callback(ENVOY_CLIENT_ERROR, nullptr, context);
    }
    return 0;
  }
  return handle->filter_chain_manager->applyResponseFilters(cluster_name, headers, callback,
                                                            context);
}

void envoy_client_cancel_filter(envoy_client_handle handle, uint64_t request_id) {
  if (handle != nullptr) {
    handle->filter_chain_manager->cancelFilter(request_id);
  }
}

void envoy_client_free_headers(envoy_client_headers* headers) {
  EnvoyClient::FilterChainManager::freeHeaders(headers);
}

envoy_client_status envoy_client_add_interceptor(envoy_client_handle handle, const char* name,
                                                 envoy_client_interceptor_cb callback,
                                                 void* context) {
  if (handle == nullptr || name == nullptr || callback == nullptr) {
    return ENVOY_CLIENT_ERROR;
  }
  return handle->filter_chain_manager->addInterceptor(name, callback, context);
}

envoy_client_status envoy_client_remove_interceptor(envoy_client_handle handle,
                                                    const char* name) {
  if (handle == nullptr || name == nullptr) {
    return ENVOY_CLIENT_ERROR;
  }
  return handle->filter_chain_manager->removeInterceptor(name);
}

uint64_t envoy_client_send_request(envoy_client_handle handle, const char* cluster_name,
                                   const envoy_client_headers* request_headers,
                                   const void* request_body, size_t request_body_len,
                                   envoy_client_response_cb response_cb, void* context) {
  if (handle == nullptr || cluster_name == nullptr || response_cb == nullptr) {
    if (response_cb != nullptr) {
      response_cb(ENVOY_CLIENT_ERROR, nullptr, context);
    }
    return 0;
  }

  // Convert C headers to std pairs.
  std::vector<std::pair<std::string, std::string>> headers;
  if (request_headers != nullptr) {
    headers.reserve(request_headers->count);
    for (size_t i = 0; i < request_headers->count; ++i) {
      const auto& h = request_headers->headers[i];
      headers.emplace_back(std::string(h.key, h.key_len), std::string(h.value, h.value_len));
    }
  }

  // Copy body into a std::string.
  std::string body;
  if (request_body != nullptr && request_body_len > 0) {
    body.assign(static_cast<const char*>(request_body), request_body_len);
  }

  return handle->client->sendRequest(
      cluster_name, headers, body,
      [response_cb, context](Envoy::Client::UpstreamResponse resp) {
        if (!resp.success) {
          response_cb(ENVOY_CLIENT_ERROR, nullptr, context);
          return;
        }

        // Build envoy_client_response.
        auto* c_resp = new envoy_client_response();
        c_resp->status_code = resp.status_code;

        // Convert headers.
        c_resp->headers = new envoy_client_headers();
        c_resp->headers->count = resp.headers.size();
        c_resp->headers->headers = static_cast<envoy_client_header*>(
            calloc(resp.headers.size(), sizeof(envoy_client_header)));
        for (size_t i = 0; i < resp.headers.size(); ++i) {
          const auto& [k, v] = resp.headers[i];
          c_resp->headers->headers[i].key = strdup(k.c_str());
          c_resp->headers->headers[i].key_len = k.size();
          c_resp->headers->headers[i].value = strdup(v.c_str());
          c_resp->headers->headers[i].value_len = v.size();
        }

        // Copy body.
        char* body_buf = nullptr;
        if (!resp.body.empty()) {
          body_buf = static_cast<char*>(malloc(resp.body.size()));
          memcpy(body_buf, resp.body.data(), resp.body.size());
        }
        c_resp->body = body_buf;
        c_resp->body_len = resp.body.size();

        response_cb(ENVOY_CLIENT_OK, c_resp, context);

        // The caller is responsible for freeing c_resp via envoy_client_free_response.
      });
}

void envoy_client_cancel_request(envoy_client_handle handle, uint64_t request_id) {
  if (handle != nullptr && request_id != 0) {
    handle->client->cancelRequest(request_id);
  }
}

void envoy_client_free_response(envoy_client_response* response) {
  if (response == nullptr) {
    return;
  }
  if (response->headers != nullptr) {
    for (size_t i = 0; i < response->headers->count; ++i) {
      free(const_cast<char*>(response->headers->headers[i].key));
      free(const_cast<char*>(response->headers->headers[i].value));
    }
    free(response->headers->headers);
    delete response->headers;
  }
  free(const_cast<char*>(response->body));
  delete response;
}

envoy_client_status envoy_client_set_lb_context_provider(envoy_client_handle handle,
                                                         envoy_client_lb_context_cb callback,
                                                         void* context) {
  if (handle == nullptr) {
    return ENVOY_CLIENT_ERROR;
  }
  handle->lb_context_cb = callback;
  handle->lb_context_user_ctx = context;
  return ENVOY_CLIENT_OK;
}

} // extern "C"
