#include "client/library/c_api/envoy_client_internal.h"

#include <cstring>
#include <string>

#include "envoy/http/header_map.h"
#include "source/common/http/header_map_impl.h"

// ---------------------------------------------------------------------------
// Internal C++ helpers — must live outside extern "C" to avoid the
// -Wreturn-type-c-linkage warning for functions returning C++ types.
// ---------------------------------------------------------------------------

namespace {

// Convert envoy_client_headers to Http::RequestHeaderMap.
Envoy::Http::RequestHeaderMapPtr fromCHeaders(const envoy_client_headers* in_headers) {
  auto hdr_map = Envoy::Http::RequestHeaderMapImpl::create();
  if (in_headers != nullptr) {
    for (size_t i = 0; i < in_headers->count; ++i) {
      const auto& h = in_headers->headers[i];
      if (h.key && h.value) {
        hdr_map->addCopy(Envoy::Http::LowerCaseString(std::string(h.key, h.key_len)),
                         std::string(h.value, h.value_len));
      }
    }
  }
  return hdr_map;
}

// Convert envoy_client_headers to Http::ResponseHeaderMap.
Envoy::Http::ResponseHeaderMapPtr fromCResponseHeaders(const envoy_client_headers* in_headers) {
  auto hdr_map = Envoy::Http::ResponseHeaderMapImpl::create();
  if (in_headers != nullptr) {
    for (size_t i = 0; i < in_headers->count; ++i) {
      const auto& h = in_headers->headers[i];
      if (h.key && h.value) {
        hdr_map->addCopy(Envoy::Http::LowerCaseString(std::string(h.key, h.key_len)),
                         std::string(h.value, h.value_len));
      }
    }
  }
  return hdr_map;
}

// Populate an envoy_client_headers struct from an Http::HeaderMap.
// Allocates memory that the caller must free with envoy_client_free_headers().
void toCHeaders(const Envoy::Http::HeaderMap& hdr_map, envoy_client_headers* out) {
  size_t count = 0;
  hdr_map.iterate([&count](const Envoy::Http::HeaderEntry&) {
    ++count;
    return Envoy::Http::HeaderMap::Iterate::Continue;
  });
  out->count = count;
  if (count == 0) {
    out->headers = nullptr;
    return;
  }
  out->headers =
      static_cast<envoy_client_header*>(calloc(count, sizeof(envoy_client_header)));
  size_t idx = 0;
  hdr_map.iterate([&](const Envoy::Http::HeaderEntry& entry) {
    const auto ks = entry.key().getStringView();
    const auto vs = entry.value().getStringView();
    char* key_str = static_cast<char*>(malloc(ks.size() + 1));
    memcpy(key_str, ks.data(), ks.size());
    key_str[ks.size()] = '\0';
    char* val_str = static_cast<char*>(malloc(vs.size() + 1));
    memcpy(val_str, vs.data(), vs.size());
    val_str[vs.size()] = '\0';
    out->headers[idx].key = key_str;
    out->headers[idx].key_len = ks.size();
    out->headers[idx].value = val_str;
    out->headers[idx].value_len = vs.size();
    ++idx;
    return Envoy::Http::HeaderMap::Iterate::Continue;
  });
}

} // namespace

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

    // Save original pointers so we can detect strings newly allocated by the
    // callback. The Go binding allocates C strings (via C.CString) and
    // transfers ownership to us; we must free them after copying.
    const char* orig_override_host = mutable_ctx.override_host;
    const char* orig_hash_key = mutable_ctx.hash_key;

    handle->lb_context_cb(cluster_name, &mutable_ctx, handle->lb_context_user_ctx);

    // Read back any fields the callback may have added or changed.
    // Free any strings that were newly allocated by the callback (pointer
    // differs from the original) after copying them into ctx.
    if (mutable_ctx.path != nullptr) {
      ctx.path = mutable_ctx.path;
    }
    if (mutable_ctx.authority != nullptr) {
      ctx.authority = mutable_ctx.authority;
    }
    if (mutable_ctx.override_host != nullptr) {
      ctx.override_host = mutable_ctx.override_host;
      if (mutable_ctx.override_host != orig_override_host) {
        free(const_cast<char*>(mutable_ctx.override_host));
      }
    }
    ctx.override_host_strict = mutable_ctx.override_host_strict != 0;
    if (mutable_ctx.hash_key != nullptr && mutable_ctx.hash_key_len > 0) {
      ctx.hash_key = std::string(mutable_ctx.hash_key, mutable_ctx.hash_key_len);
      if (mutable_ctx.hash_key != orig_hash_key) {
        free(const_cast<char*>(mutable_ctx.hash_key));
      }
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

envoy_client_status envoy_client_add_interceptor(envoy_client_handle handle, const char* name,
                                                 envoy_client_interceptor_cb callback,
                                                 void* context) {
  if (handle == nullptr || name == nullptr || callback == nullptr) {
    return ENVOY_CLIENT_ERROR;
  }

  // Wrap the C callback in a C++ interceptor that converts Http::RequestHeaderMap
  // ↔ envoy_client_headers so the C callback can inspect (but not structurally modify)
  // the request headers.
  Envoy::Client::ClientInterceptor interceptor;
  interceptor.name = name;

  interceptor.on_request = [callback, context](Envoy::Http::RequestHeaderMap& headers,
                                               const std::string& cluster,
                                               Envoy::Client::InterceptorPhase phase) -> bool {
    // Build a temporary envoy_client_headers view of the Http::RequestHeaderMap.
    // Point directly into Envoy's internal string storage (valid during the callback).
    std::vector<envoy_client_header> c_headers;
    headers.iterate([&c_headers](const Envoy::Http::HeaderEntry& entry) {
      c_headers.push_back(
          {entry.key().getStringView().data(), entry.key().getStringView().size(),
           entry.value().getStringView().data(), entry.value().getStringView().size()});
      return Envoy::Http::HeaderMap::Iterate::Continue;
    });
    envoy_client_headers c_hdr_map;
    c_hdr_map.headers = c_headers.empty() ? nullptr : c_headers.data();
    c_hdr_map.count = c_headers.size();

    const auto c_phase = static_cast<envoy_client_interceptor_phase>(phase);
    const envoy_client_status status =
        callback(&c_hdr_map, cluster.c_str(), c_phase, context);
    return status == ENVOY_CLIENT_OK;
  };

  interceptor.on_response = [callback, context](Envoy::Http::ResponseHeaderMap& headers,
                                                const std::string& cluster,
                                                Envoy::Client::InterceptorPhase phase) -> bool {
    std::vector<envoy_client_header> c_headers;
    headers.iterate([&c_headers](const Envoy::Http::HeaderEntry& entry) {
      c_headers.push_back(
          {entry.key().getStringView().data(), entry.key().getStringView().size(),
           entry.value().getStringView().data(), entry.value().getStringView().size()});
      return Envoy::Http::HeaderMap::Iterate::Continue;
    });
    envoy_client_headers c_hdr_map;
    c_hdr_map.headers = c_headers.empty() ? nullptr : c_headers.data();
    c_hdr_map.count = c_headers.size();

    const auto c_phase = static_cast<envoy_client_interceptor_phase>(phase);
    const envoy_client_status status =
        callback(&c_hdr_map, cluster.c_str(), c_phase, context);
    return status == ENVOY_CLIENT_OK;
  };

  handle->client->addInterceptor(std::move(interceptor));
  return ENVOY_CLIENT_OK;
}

envoy_client_status envoy_client_remove_interceptor(envoy_client_handle handle,
                                                    const char* name) {
  if (handle == nullptr || name == nullptr) {
    return ENVOY_CLIENT_ERROR;
  }
  handle->client->removeInterceptor(name);
  return ENVOY_CLIENT_OK;
}

// --- Filter Application ---

envoy_client_status
envoy_client_apply_request_filters(envoy_client_handle handle, const char* cluster_name,
                                   const envoy_client_headers* in_headers,
                                   envoy_client_headers* out_headers) {
  if (handle == nullptr || cluster_name == nullptr) {
    return ENVOY_CLIENT_ERROR;
  }

  auto hdr_map = fromCHeaders(in_headers);
  const EnvoyClient::Status status =
      handle->client->applyRequestFilters(cluster_name, *hdr_map);

  if (status == EnvoyClient::Status::Ok) {
    if (out_headers != nullptr) {
      toCHeaders(*hdr_map, out_headers);
    }
    return ENVOY_CLIENT_OK;
  }
  return ENVOY_CLIENT_DENIED;
}

envoy_client_status
envoy_client_apply_response_filters(envoy_client_handle handle, const char* cluster_name,
                                    const envoy_client_headers* in_headers,
                                    envoy_client_headers* out_headers) {
  if (handle == nullptr || cluster_name == nullptr) {
    return ENVOY_CLIENT_ERROR;
  }

  auto hdr_map = fromCResponseHeaders(in_headers);
  const EnvoyClient::Status status =
      handle->client->applyResponseFilters(cluster_name, *hdr_map);

  if (status == EnvoyClient::Status::Ok) {
    if (out_headers != nullptr) {
      toCHeaders(*hdr_map, out_headers);
    }
    return ENVOY_CLIENT_OK;
  }
  return ENVOY_CLIENT_DENIED;
}

void envoy_client_free_headers(envoy_client_headers* headers) {
  if (headers == nullptr || headers->headers == nullptr) {
    return;
  }
  for (size_t i = 0; i < headers->count; ++i) {
    free(const_cast<char*>(headers->headers[i].key));
    free(const_cast<char*>(headers->headers[i].value));
  }
  free(headers->headers);
  headers->headers = nullptr;
  headers->count = 0;
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
