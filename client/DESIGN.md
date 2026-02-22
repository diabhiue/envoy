# Envoy Client Library — Design Document

## Status: DRAFT / Brainstorm

## Problem Statement

Today, applications that want xDS-driven service mesh behavior in-process have three options,
none of which are satisfactory:

| Approach | What you get | What you lose |
|----------|-------------|---------------|
| **Sidecar Envoy** | Full feature set | Extra process, latency hop, operational complexity |
| **gRPC xDS** (`grpc-go/xds`, `grpc-java/xds`, etc.) | Endpoint resolution, some LB policies | No filter chain, no server-enforced policy, partial xDS, reimplemented per language |
| **Envoy Mobile** | Full Envoy in-process | Tied to Android/iOS, owns the entire data path (replaces your HTTP client), large binary |

### The Gap

gRPC clients today each have **their own bespoke xDS client** implementation that:

- Only understand a subset of xDS (CDS, EDS, LDS, RDS — but not the full filter chain)
- Cannot run Envoy HTTP filters (so no server-enforced policy like ext_authz)
- Duplicate LB policy logic across languages with subtle behavioral differences
- Don't support auth header injection or server-pushed filters
- Cannot call out to external authorization services

Server operators who deploy Envoy sidecars get centralized policy enforcement. But organizations
wanting to move away from sidecars (for latency, operational, or resource reasons) lose all of that.

## Goal

Build an **Envoy Client Library** — a language-agnostic, embeddable library that any client
(gRPC, HTTP, custom protocols) can link against to get:

1. **xDS subscription** — endpoints, routes, clusters, listeners, secrets
2. **Load balancing** — server-configured LB policies (round-robin, ring-hash, least-request, etc.)
3. **Filter chain execution** — server-enforced Envoy HTTP filters running in-process, including
   filters that make outbound calls (ext_authz, jwt_authn, oauth2)
4. **Auth header injection** — centrally managed credentials appended to requests
5. **Health checking** — client-side endpoint health driven by xDS config

### Non-Goals (initially)

- Replacing the application's HTTP/gRPC transport (the library does NOT own the data path)
- Body-level filter processing (compression, transcoding) — headers-only initially
- Full connection pooling to upstream targets
- Downstream listener/accept functionality

## Architecture

### High-Level View

```
┌───────────────────────────────────────────────────────────┐
│                    APPLICATION                            │
│         (gRPC / HTTP / custom protocol client)            │
│                                                           │
│  App makes its own connections. The library only answers:  │
│  "where to send" and "what headers to add/modify"         │
└──────────────────────┬────────────────────────────────────┘
                       │  sync or async calls
┌──────────────────────▼────────────────────────────────────┐
│              LANGUAGE BINDING LAYER                        │
│   Go (CGo) │ Java (JNI) │ Python (cffi) │ Rust (FFI)    │
│   C++ (direct)                                            │
└──────────────────────┬────────────────────────────────────┘
                       │
┌──────────────────────▼────────────────────────────────────┐
│               C ABI SURFACE (stable)                      │
└──────────────────────┬────────────────────────────────────┘
                       │
┌──────────────────────▼────────────────────────────────────┐
│            ENVOY CLIENT CORE (C++)                        │
│                                                           │
│  ┌──────────────────────────────────────────────────────┐ │
│  │ ClientEngine                                         │ │
│  │ • Owns a Dispatcher (event loop on dedicated thread) │ │
│  │ • Manages xDS subscriptions via GrpcMux              │ │
│  │ • Runs filter chains with full async support          │ │
│  │ • Owns async gRPC/HTTP clients for filter callouts   │ │
│  └───────┬──────────────┬────────────────┬──────────────┘ │
│          │              │                │                │
│  ┌───────▼────────┐ ┌──▼─────────────┐ ┌▼─────────────┐  │
│  │  XDS ENGINE    │ │ FILTER ENGINE  │ │ DECISION     │  │
│  │                │ │                │ │ ENGINE       │  │
│  │ Reuses:        │ │ Runs Envoy     │ │              │  │
│  │ • GrpcMux      │ │ HTTP filters:  │ │ • LB policy  │  │
│  │ • WatchMap     │ │                │ │   selection  │  │
│  │ • SubsFactory  │ │ • ext_authz    │ │ • Endpoint   │  │
│  │ • XdsManager   │ │   (calls out   │ │   picking    │  │
│  │ • EdsResCache  │ │   to authz     │ │ • Route      │  │
│  │ • Delta/SOTW   │ │   service)     │ │   matching   │  │
│  │ • Failover     │ │ • ext_proc     │ │ • Health     │  │
│  │                │ │ • jwt_authn    │ │   tracking   │  │
│  │                │ │ • oauth2       │ │              │  │
│  │                │ │ • credential   │ │ Reuses:      │  │
│  │                │ │   _injector    │ │ • LB factory │  │
│  │                │ │ • header       │ │ • Ring hash  │  │
│  │                │ │   _mutation    │ │ • Round rob. │  │
│  │                │ │ • rbac         │ │ • Least req  │  │
│  │                │ │ • wasm         │ │ • Maglev     │  │
│  └────────────────┘ └────────────────┘ └──────────────┘  │
│                                                           │
│  ┌──────────────────────────────────────────────────────┐ │
│  │ ServerClientLite (extends Server::InstanceBase)      │ │
│  │ • No heap shrinker, no guard dog, no HDS delegate    │ │
│  │ • Null overload manager                              │ │
│  │ • No downstream listener accept                      │ │
│  │ • ClusterManager in reduced mode:                    │ │
│  │   - manages connections to ext services only         │ │
│  │     (authz, OAuth, JWKS endpoints)                   │ │
│  │   - does NOT manage connections to app's upstreams   │ │
│  │ • Async gRPC/HTTP client factory for filter callouts │ │
│  └──────────────────────────────────────────────────────┘ │
└───────────────────────────────────────────────────────────┘
```

### Key Design Decision: Library Does NOT Own the Data Path

This is the critical difference from Envoy Mobile. The client library:

- Does **NOT** make HTTP connections to the application's upstream targets
- Does **NOT** manage connection pools to those targets
- Does **NOT** parse HTTP frames on the application's request/response path

It answers two questions:
1. **"Where should I send this?"** → endpoint resolution + LB pick
2. **"What should the request/response look like?"** → filter chain execution (header mutation,
   auth injection, authz checks)

The application's existing gRPC/HTTP client makes the actual connections and sends the data.

However, the library **does** own connections to **external filter services** — when a filter
like `ext_authz` needs to call an authorization service, or `jwt_authn` needs to fetch JWKS,
or `oauth2` needs to exchange tokens, the library's internal `ClusterManager` manages those
connections.

### Why ClusterManager Cannot Be Fully Replaced

From exploring the codebase, filters that make outbound calls depend on `ClusterManager`:

- `ext_authz` → `GrpcClientImpl` uses `Grpc::AsyncClient` from `ClusterManager::grpcAsyncClientManager()`
- `ext_authz` (HTTP mode) → `RawHttpClientImpl` uses `Http::AsyncClient` from `ClusterManager`
- `jwt_authn` → `JwksAsyncFetcher` uses `Http::AsyncClient` for JWKS endpoint fetching
- `oauth2` → `OAuth2Client` uses `Http::AsyncClient` for token exchange
- `ext_proc` → Uses streaming `Grpc::AsyncClient` for bi-directional processing

These async clients require:
- Connection pool management to the external services
- The Envoy `Dispatcher` (event loop) for non-blocking I/O
- Cluster configuration for the external services (timeouts, TLS, retry)

Therefore, the client library uses `ClusterManager` in a **reduced capacity**:
- It manages clusters/connections for external filter services (authz, OAuth, JWKS)
- It does NOT create clusters for the application's actual upstream targets
- The application's upstream endpoints are tracked in a lightweight `ConfigStore` (read-only
  view of xDS state for endpoint resolution and LB)

## Reuse of Existing Envoy Components

### From Envoy Core (linked directly)

| Component | Location | Used For |
|-----------|----------|----------|
| `GrpcMux` (SOTW + Delta) | `source/extensions/config_subscription/grpc/xds_mux/` | xDS subscription multiplexing |
| `GrpcStream` | `source/extensions/config_subscription/grpc/grpc_stream.h` | gRPC stream with backoff/retry |
| `WatchMap` | `source/extensions/config_subscription/grpc/watch_map.h` | Multi-watcher resource dispatch |
| `SubscriptionFactory` | `source/common/config/subscription_factory_impl.h` | Creating subscriptions from ConfigSource |
| `XdsManager` | `source/common/config/xds_manager_impl.h` | Central xDS coordination |
| `EdsResourcesCache` | `source/extensions/config_subscription/grpc/eds_resources_cache_impl.h` | Endpoint caching with TTL |
| `StrippedMainBase` | `source/exe/stripped_main_base.h` | Lightweight bootstrap (shared with Envoy Mobile) |
| LB Policy Factories | `source/extensions/load_balancing_policies/*/config.h` | Pluggable LB algorithms |
| HTTP Filter Factories | `source/extensions/filters/http/*/config.h` | Filter instantiation |
| `Grpc::AsyncClient` | `source/common/grpc/typed_async_client.h` | Async gRPC for filter callouts |
| `Http::AsyncClient` | Per `ClusterManager` | Async HTTP for filter callouts |
| Transport Socket Match | `source/common/upstream/transport_socket_match_impl.h` | Per-endpoint TLS config |
| `Dispatcher` | `source/common/event/` | Event loop for async I/O |

### From Envoy Mobile (patterns reused, not code)

| Pattern | Mobile Location | Client Library Adaptation |
|---------|----------------|--------------------------|
| `ServerLite` | `mobile/library/common/engine_common.cc` | `ServerClientLite` — same idea, different feature set |
| `EngineCommon` | `mobile/library/common/engine_common.h` | `ClientEngineCommon` — wraps `StrippedMainBase` |
| `EngineBuilder` | `mobile/library/cc/engine_builder.h` | `ClientEngineBuilder` — programmatic config |
| JNI bridge | `mobile/library/jni/` | Reuse JNI patterns for Java bindings |
| Extension registry | `@envoy_build_config//:extension_registry` | Separate registry with client-relevant extensions |

### NOT Reused

| Component | Reason |
|-----------|--------|
| `ListenerManager` | No downstream listeners in a client library |
| `HttpConnectionManager` | No downstream HTTP connections to manage |
| HTTP codecs (H1/H2/H3) | Application's HTTP client handles encoding |
| Connection pools | Application manages its own connections |
| `AdminServer` | Not needed for embedded client |
| `GuardDog` | Lightweight; no watchdog needed |
| `HdsDelegate` | No health discovery service delegation |
| Heap shrinker | Not needed for library |

## Filter Execution Model

### The Challenge

Envoy's filter chain is designed to run inside `HttpConnectionManager`, processing a live HTTP
stream. In the client library, there is no HTTP stream — the application calls the library with
headers, and the library runs filters and returns modified headers.

We need a **headless filter chain execution mode** that:
1. Accepts headers from the application
2. Runs filters in order (some sync, some async with outbound calls)
3. Returns modified headers (or a deny decision) via callback

### How Filters Make Outbound Calls

The async call pattern used by Envoy filters (explored in detail):

```
Filter::decodeHeaders()
  │
  ├── Sync filter (header_mutation, credential_injector, rbac):
  │   Mutates headers immediately, returns FilterHeadersStatus::Continue
  │
  └── Async filter (ext_authz, ext_proc, oauth2):
      1. Prepares request (CheckRequest, ProcessingRequest, etc.)
      2. Calls client_->check(*this, request, ...) or client_->send(...)
         - This posts the gRPC/HTTP request to the Dispatcher event loop
         - Returns immediately
      3. Returns FilterHeadersStatus::StopIteration (pauses filter chain)
      4. When response arrives (on Dispatcher thread):
         - onComplete() / onSuccess() callback fires
         - Processes response (allow/deny, header additions)
         - Calls decoder_callbacks_->continueDecoding() to resume chain
      5. Cancel path: onDestroy() calls client_->cancel()
```

### Headless Filter Chain Design

```cpp
// client/library/common/filter_chain_manager.h

class HeadlessFilterChain {
public:
  // Called by the application (via C ABI) to apply request filters.
  // This is ASYNC because filters like ext_authz may call out to external services.
  //
  // The callback fires when all filters have completed (or one denies the request).
  void applyRequestFilters(
      const std::string& cluster_name,
      Http::RequestHeaderMap& headers,
      const StreamInfo::StreamInfo& stream_info,
      std::function<void(FilterResult result, Http::RequestHeaderMap& modified_headers)> callback);

  // Same for response filters.
  void applyResponseFilters(
      const std::string& cluster_name,
      Http::ResponseHeaderMap& headers,
      const StreamInfo::StreamInfo& stream_info,
      std::function<void(FilterResult result, Http::ResponseHeaderMap& modified_headers)> callback);

private:
  // The filter chain config comes from LDS/ECDS via xDS
  std::vector<Http::FilterFactoryCb> filter_factories_;

  // Dispatcher for async I/O (filter callouts)
  Event::Dispatcher& dispatcher_;

  // ClusterManager for filter callouts (ext_authz, JWKS, OAuth)
  Upstream::ClusterManager& cluster_manager_;
};
```

The `HeadlessFilterChain` creates a synthetic `StreamDecoderFilterCallbacks` implementation that:
- Provides `dispatcher()` access for timers and async operations
- Provides `clusterManager()` access for filter callouts
- Implements `continueDecoding()` to advance through the filter chain
- Implements `sendLocalReply()` to handle filter denials
- Does NOT implement body/trailer streaming (headers-only mode)

### Filter Categories for Client Library

| Category | Filters | Execution Mode | Outbound Calls? |
|----------|---------|---------------|----------------|
| **Header mutation** | `header_mutation`, `credential_injector` | Sync | No |
| **Policy evaluation** | `rbac`, `rate_limit` (metadata only) | Sync | No |
| **External authorization** | `ext_authz` | Async | Yes — gRPC/HTTP to authz service |
| **Token management** | `jwt_authn`, `oauth2` | Async (first call) / Sync (cached) | Yes — JWKS/token endpoints |
| **External processing** | `ext_proc` | Async (streaming) | Yes — gRPC to processor |
| **Programmable** | `lua`, `wasm` | Sync or Async | Depends on script |

## xDS Resource Types

### Priority Tiers

| Resource | xDS Service | Purpose in Client Library | Priority |
|----------|------------|--------------------------|----------|
| **CDS** (Cluster) | Cluster Discovery Service | Service definitions, LB policy, circuit breakers, outlier detection | P0 |
| **EDS** (Endpoints) | Endpoint Discovery Service | Endpoint addresses, weights, health, locality, priority | P0 |
| **LDS** (Listeners) | Listener Discovery Service | Filter chain configuration (which filters to apply per service) | P1 |
| **RDS** (Routes) | Route Discovery Service | Route matching → cluster, header transforms, retry policy | P1 |
| **SDS** (Secrets) | Secret Discovery Service | TLS certificates for mTLS to upstream and filter callout targets | P1 |
| **ECDS** (Extension Config) | Extension Config DS | Dynamic filter config updates without full LDS push | P2 |
| **VHDS** (Virtual Hosts) | Virtual Host DS | On-demand virtual host loading for large route tables | P2 |
| **LEDS** (Locality Endpoints) | Locality Endpoint DS | Per-locality endpoint subscriptions for scale | P3 |

### Config Flow

```
Control Plane (Istio / custom xDS server)
    │
    │ CDS: defines clusters (services) with LB policy, circuit breakers
    │ EDS: provides endpoints per cluster
    │ LDS: defines filter chain per listener (which filters to run)
    │ RDS: defines routes (path/header matching → cluster selection)
    │ SDS: provides TLS certs/keys
    │
    ▼
XdsManager
    ├── GrpcMux (ADS or per-resource-type streams)
    │   ├── CDS → ConfigStore::onClustersUpdated()
    │   ├── EDS → ConfigStore::onEndpointsUpdated()
    │   ├── LDS → FilterChainManager::onFiltersUpdated()
    │   ├── RDS → ConfigStore::onRoutesUpdated()
    │   └── SDS → SecretManager::onSecretsUpdated()
    │
    ▼
ConfigStore (read-only query interface for application)
    ├── resolve(cluster) → endpoints[]
    ├── pick(cluster, metadata) → endpoint
    └── matchRoute(domain, path, headers) → cluster + route_config
```

## Server-Enforced Filters

This is the most novel capability. Today, server operators can enforce policy on traffic only
through sidecars. With the Envoy Client Library, the control plane pushes filter configuration
to clients, and the library enforces it in-process.

### Example: Control Plane Pushes Policy

```yaml
# LDS pushed to client via xDS
listeners:
- name: client_egress
  filter_chains:
  - filters:
    - name: envoy.filters.http.rbac
      typed_config:
        rules:
          policies:
            allow-frontend:
              permissions:
              - header: { name: ":path", prefix_match: "/api/v1/" }
              principals:
              - authenticated: { principal_name: { exact: "frontend-service" } }

    - name: envoy.filters.http.ext_authz
      typed_config:
        grpc_service:
          envoy_grpc:
            cluster_name: authz-service
        transport_api_version: V3

    - name: envoy.filters.http.header_mutation
      typed_config:
        mutations:
          request_mutations:
          - append:
              header:
                key: x-request-id
                value: "%REQ_WITHOUT_QUERY(x-request-id)%"
          - append:
              header:
                key: x-envoy-client-library
                value: "true"

    - name: envoy.filters.http.credential_injector
      typed_config:
        credential:
          name: envoy.http.injected_credentials.oauth2
          typed_config:
            token_endpoint:
              cluster: oauth-token-service
              uri: "https://auth.example.com/token"
```

### Execution Flow

```
Application calls: client.apply_request_filters("my-service", headers)
    │
    ▼
ClientEngine (Dispatcher thread)
    │
    ├── 1. RBAC filter (sync)
    │   Evaluates policy → ALLOW (request matches /api/v1/ from frontend-service)
    │
    ├── 2. ext_authz filter (async)
    │   ├── Sends CheckRequest to authz-service via gRPC
    │   ├── Waits for CheckResponse
    │   └── Response: ALLOW + additional headers {x-auth-decision: "approved"}
    │
    ├── 3. header_mutation filter (sync)
    │   Adds x-request-id, x-envoy-client-library headers
    │
    └── 4. credential_injector filter (sync/async)
        ├── Checks token cache → miss on first call
        ├── Fetches OAuth token from oauth-token-service (async)
        ├── Caches token
        └── Injects Authorization: Bearer <token>
    │
    ▼
Callback fires with modified headers:
  Original headers + {
    x-auth-decision: "approved",
    x-request-id: "<generated>",
    x-envoy-client-library: "true",
    Authorization: "Bearer <oauth-token>"
  }
    │
    ▼
Application sends request with modified headers via its own gRPC/HTTP client
```

## C ABI Surface

The stable C ABI is the foundation for all language bindings. It is intentionally minimal
and opaque-handle based.

```c
// client/library/c_api/envoy_client.h

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handles
typedef struct envoy_client_engine* envoy_client_handle;

// Status codes
typedef enum {
  ENVOY_CLIENT_OK = 0,
  ENVOY_CLIENT_ERROR = 1,
  ENVOY_CLIENT_DENIED = 2,       // Filter denied the request
  ENVOY_CLIENT_UNAVAILABLE = 3,  // No endpoints available
  ENVOY_CLIENT_TIMEOUT = 4,      // Filter callout timed out
} envoy_client_status;

// Key-value header pair
typedef struct {
  const char* key;
  size_t key_len;
  const char* value;
  size_t value_len;
} envoy_client_header;

// Header map (array of key-value pairs)
typedef struct {
  envoy_client_header* headers;
  size_t count;
} envoy_client_headers;

// Endpoint information
typedef struct {
  const char* address;    // IP address or hostname
  uint32_t port;
  uint32_t weight;
  uint32_t priority;
  // health: 0=unknown, 1=healthy, 2=degraded, 3=unhealthy
  uint32_t health_status;
} envoy_client_endpoint;

// List of endpoints
typedef struct {
  envoy_client_endpoint* endpoints;
  size_t count;
} envoy_client_endpoint_list;

// Request metadata for LB decisions
typedef struct {
  envoy_client_headers* metadata;  // Key-value metadata for hash-based LB
  const char* path;                // Request path for route matching
  const char* authority;           // :authority header value
} envoy_client_request_context;

// Callback for async filter operations
typedef void (*envoy_client_filter_cb)(
    envoy_client_status status,
    envoy_client_headers* modified_headers,
    void* context
);

// Callback for xDS resource updates
typedef void (*envoy_client_xds_cb)(
    const char* type_url,
    const char* resource_name,
    const void* resource_data,
    size_t resource_len,
    void* context
);

// --- Lifecycle ---

// Create a client engine from bootstrap YAML or JSON.
envoy_client_handle envoy_client_create(
    const char* bootstrap_config,
    size_t config_len
);

// Destroy the client engine and free all resources.
void envoy_client_destroy(envoy_client_handle handle);

// --- Endpoint Resolution (synchronous, reads from xDS config store) ---

// Resolve all endpoints for a cluster.
envoy_client_status envoy_client_resolve(
    envoy_client_handle handle,
    const char* cluster_name,
    envoy_client_endpoint_list* out_endpoints
);

// Pick a single endpoint using the server-configured LB policy.
envoy_client_status envoy_client_pick_endpoint(
    envoy_client_handle handle,
    const char* cluster_name,
    const envoy_client_request_context* request_ctx,
    envoy_client_endpoint* out_endpoint
);

// Free an endpoint list returned by envoy_client_resolve.
void envoy_client_free_endpoints(envoy_client_endpoint_list* endpoints);

// --- Filter Execution (async — filters may call out to external services) ---

// Apply request filters for a cluster. Callback fires when all filters complete.
// Returns a request_id that can be used to cancel.
uint64_t envoy_client_apply_request_filters(
    envoy_client_handle handle,
    const char* cluster_name,
    envoy_client_headers* headers,
    envoy_client_filter_cb callback,
    void* context
);

// Apply response filters for a cluster.
uint64_t envoy_client_apply_response_filters(
    envoy_client_handle handle,
    const char* cluster_name,
    envoy_client_headers* headers,
    envoy_client_filter_cb callback,
    void* context
);

// Cancel an in-flight filter operation.
void envoy_client_cancel_filter(
    envoy_client_handle handle,
    uint64_t request_id
);

// --- LB Feedback ---

// Report the result of a request for feedback-driven LB (e.g., least-request).
void envoy_client_report_result(
    envoy_client_handle handle,
    const envoy_client_endpoint* endpoint,
    uint32_t status_code,
    uint64_t latency_ms
);

// --- Route Matching ---

// Match a request to a route and return the target cluster name.
envoy_client_status envoy_client_match_route(
    envoy_client_handle handle,
    const char* authority,
    const char* path,
    const envoy_client_headers* headers,
    char* out_cluster_name,
    size_t cluster_name_buf_len
);

// --- xDS Subscription (for advanced use / custom resource types) ---

// Subscribe to raw xDS resource updates.
envoy_client_status envoy_client_subscribe(
    envoy_client_handle handle,
    const char* type_url,
    const char* resource_name,
    envoy_client_xds_cb callback,
    void* context
);

// Free headers returned by filter callbacks.
void envoy_client_free_headers(envoy_client_headers* headers);

#ifdef __cplusplus
}
#endif
```

## Language Binding Examples

### Go (via CGo)

```go
package envoyclient

/*
#cgo LDFLAGS: -lenvoyclient
#include "envoy_client.h"
*/
import "C"
import "unsafe"

type Client struct {
    handle C.envoy_client_handle
}

func New(bootstrapYAML string) (*Client, error) {
    cs := C.CString(bootstrapYAML)
    defer C.free(unsafe.Pointer(cs))
    handle := C.envoy_client_create(cs, C.size_t(len(bootstrapYAML)))
    if handle == nil {
        return nil, errors.New("failed to create envoy client")
    }
    return &Client{handle: handle}, nil
}

// PickEndpoint selects an endpoint using the xDS-configured LB policy.
func (c *Client) PickEndpoint(cluster string, ctx *RequestContext) (*Endpoint, error) {
    // ... CGo bridge to envoy_client_pick_endpoint
}

// ApplyRequestFilters runs the server-pushed filter chain.
// This is async because filters like ext_authz may call external services.
func (c *Client) ApplyRequestFilters(cluster string, headers map[string]string) (map[string]string, error) {
    // ... CGo bridge to envoy_client_apply_request_filters
    // Uses a channel to bridge async callback to sync Go call
}

// Integrate with gRPC as a custom resolver + balancer
func init() {
    resolver.Register(&envoyResolver{})
    balancer.Register(&envoyBalancerBuilder{})
}
```

### gRPC Integration (Go)

```go
// As a gRPC resolver — replaces grpc-go/xds
type envoyResolver struct {
    client *Client
}

func (r *envoyResolver) Build(target resolver.Target, cc resolver.ClientConn, opts resolver.BuildOptions) (resolver.Resolver, error) {
    // Subscribe to endpoint changes for target.URL.Host
    // On update: cc.UpdateState() with new addresses from client.Resolve()
}

// As a gRPC balancer — uses xDS-configured LB policy
type envoyBalancer struct {
    client *Client
}

func (b *envoyBalancer) Pick(info balancer.PickInfo) (balancer.PickResult, error) {
    endpoint, err := b.client.PickEndpoint(b.cluster, &RequestContext{
        Path: info.FullMethodName,
    })
    // Return the subconn corresponding to this endpoint
}

// As a gRPC interceptor — runs server-enforced filters
func EnvoyUnaryInterceptor(client *Client) grpc.UnaryClientInterceptor {
    return func(ctx context.Context, method string, req, reply interface{},
                cc *grpc.ClientConn, invoker grpc.UnaryInvoker, opts ...grpc.CallOption) error {
        // Apply request filters (may call ext_authz, inject credentials, etc.)
        modifiedHeaders, err := client.ApplyRequestFilters(cluster, extractHeaders(ctx))
        if err != nil {
            return err // Request denied by filter
        }
        ctx = injectHeaders(ctx, modifiedHeaders)
        return invoker(ctx, method, req, reply, cc, opts...)
    }
}
```

## Directory Structure

```
envoy/
├── api/                     # Protobuf definitions (shared)
├── source/                  # Envoy core (shared, linked against)
├── envoy/                   # Public interfaces (shared)
├── mobile/                  # Envoy Mobile (Android/iOS)
│
├── client/                  # Envoy Client Library (NEW)
│   ├── BUILD
│   ├── DESIGN.md            # This document
│   │
│   ├── bazel/               # Client-specific build rules
│   │   └── client.bzl
│   │
│   ├── library/
│   │   ├── cc/              # C++ public API
│   │   │   ├── BUILD
│   │   │   ├── client_engine_builder.h
│   │   │   ├── client_engine_builder.cc
│   │   │   ├── client_engine.h
│   │   │   ├── client_engine.cc
│   │   │   ├── endpoint_picker.h
│   │   │   └── filter_chain_client.h
│   │   │
│   │   ├── common/          # Core implementation
│   │   │   ├── BUILD
│   │   │   ├── client_engine_common.h     # Wraps StrippedMainBase
│   │   │   ├── client_engine_common.cc
│   │   │   ├── config_store.h             # Lightweight xDS state store
│   │   │   ├── config_store.cc
│   │   │   ├── headless_filter_chain.h    # Filter execution without HCM
│   │   │   ├── headless_filter_chain.cc
│   │   │   └── extensions/               # Client-specific extensions
│   │   │       └── filters/http/
│   │   │           └── client_bridge/     # Bridge filters to app callbacks
│   │   │
│   │   ├── c_api/           # Stable C ABI
│   │   │   ├── BUILD
│   │   │   ├── envoy_client.h             # Public C header
│   │   │   └── envoy_client.cc
│   │   │
│   │   ├── go/              # Go bindings (CGo)
│   │   │   ├── envoyclient.go
│   │   │   └── grpc_integration.go
│   │   │
│   │   ├── java/            # Java bindings (JNI)
│   │   │   └── io/envoyproxy/envoyclient/
│   │   │       ├── EnvoyClient.java
│   │   │       ├── EndpointPicker.java
│   │   │       └── FilterChainClient.java
│   │   │
│   │   └── jni/             # JNI bridge
│   │       ├── BUILD
│   │       └── jni_impl.cc
│   │
│   ├── test/
│   │   ├── cc/
│   │   ├── common/
│   │   └── integration/     # End-to-end tests with xDS server
│   │
│   └── examples/
│       ├── go_grpc/          # Go gRPC client using the library
│       ├── java_grpc/        # Java gRPC client using the library
│       └── cpp_http/         # C++ HTTP client using the library
```

## Bootstrap Configuration

### Programmatic (recommended)

```cpp
auto client = ClientEngineBuilder()
    .setXdsServer("xds.example.com", 443)
    .setNodeId("frontend-client-1")
    .setClusterId("web-frontend")
    .enableFilter("envoy.filters.http.ext_authz")
    .enableFilter("envoy.filters.http.credential_injector")
    .enableFilter("envoy.filters.http.rbac")
    .enableLbPolicy("envoy.load_balancing_policies.round_robin")
    .enableLbPolicy("envoy.load_balancing_policies.ring_hash")
    .setStatsSink("envoy.stat_sinks.statsd", statsd_config)
    .build();
```

### YAML (for advanced configuration)

```yaml
# Minimal bootstrap for client library
node:
  id: "frontend-client-1"
  cluster: "web-frontend"
  metadata:
    role: "client"

# Connection to xDS control plane
dynamic_resources:
  ads_config:
    api_type: GRPC
    transport_api_version: V3
    grpc_services:
    - envoy_grpc:
        cluster_name: xds_cluster

  cds_config: { ads: {} }
  lds_config: { ads: {} }

# Static clusters for infrastructure services
static_resources:
  clusters:
  - name: xds_cluster
    type: STRICT_DNS
    load_assignment:
      cluster_name: xds_cluster
      endpoints:
      - lb_endpoints:
        - endpoint:
            address:
              socket_address:
                address: xds.example.com
                port_value: 443
    transport_socket:
      name: envoy.transport_sockets.tls
      typed_config:
        "@type": type.googleapis.com/envoy.extensions.transport_sockets.tls.v3.UpstreamTlsContext

  # Cluster for ext_authz service (filter callout target)
  # This can also be dynamically configured via CDS
  - name: authz-service
    type: STRICT_DNS
    load_assignment:
      cluster_name: authz-service
      endpoints:
      - lb_endpoints:
        - endpoint:
            address:
              socket_address:
                address: authz.example.com
                port_value: 443
```

## Comparison: `mobile/` vs `client/`

| Aspect | `mobile/` | `client/` |
|--------|-----------|-----------|
| **Owns data path** | Yes (full HTTP codec, conn pools) | No (app's client sends data) |
| **Event loop** | Yes (for full proxy) | Yes (for xDS + filter callouts only) |
| **Async outbound calls** | Yes (full upstream) | Yes (filter callouts: ext_authz, JWKS, etc.) |
| **Platform bindings** | Android (JNI), iOS (Swift/ObjC) | Go (CGo), Java (JNI), Python, Rust, C++ |
| **Bootstrap** | `StrippedMainBase` + `ServerLite` | `StrippedMainBase` + `ServerClientLite` |
| **Filters** | Full chain (request + response body) | Header-focused + async callouts |
| **Target binary size** | ~15-20MB | ~8-12MB (no codec, no conn pool for app upstreams) |
| **ListenerManager** | API listener (no TCP accept) | Not needed |
| **ClusterManager** | Full (owns all connections) | Reduced (filter callout connections only) |
| **Use case** | Mobile app HTTP networking | Any client needing xDS + server-enforced policy |

## Open Design Questions

### 1. Trust Model for Server-Enforced Filters

If the client library enforces server-pushed filters, what prevents a malicious control plane
from injecting harmful filters?

**Options:**
- A. Allowlist of permitted filter types compiled into the binary (default safe set)
- B. Client-side filter policy: accept only filters from signed configuration
- C. Trust the control plane (same trust model as sidecar Envoy)
- D. Hybrid: allowlist by default, with opt-in for dynamic filter types

### 2. Binary Distribution

**Options:**
- A. Static library (`.a`) + header — simplest, users link at build time
- B. Shared library (`.so`/`.dylib`) — allows updating without recompilation
- C. Language-specific packages with embedded native code (Go module, Maven artifact, PyPI wheel)
- D. All of the above

### 3. Graceful Degradation When xDS Is Unreachable

**Options:**
- A. Cache last-known-good configuration to disk (like Envoy's config dump)
- B. Fall back to static bootstrap endpoints
- C. Return error to application (let app decide)
- D. A + B combined with configurable policy

### 4. Health Checking

**Options:**
- A. Only consume EDS health status from control plane (passive)
- B. Active health checking from the client (reuse Envoy's health checker)
- C. Application reports health via `report_result()` API (client-side outlier detection)
- D. All three, configurable

### 5. Observability

**Options:**
- A. Reuse Envoy's stats system (counters, gauges, histograms via stats sinks)
- B. Integrate with OpenTelemetry natively
- C. Expose raw stats via C ABI, let language bindings integrate with their native observability
- D. A + C

### 6. Body-Level Filter Support

**Options:**
- A. Never (headers only — keeps library lean)
- B. Opt-in with explicit API (application passes body chunks to library)
- C. WASM-only body filters (sandboxed, portable)
- D. Full body support matching Envoy's filter model

## Implementation Phases

### Phase 1: xDS Core + Endpoint Resolution
- `ServerClientLite` bootstrap via `StrippedMainBase`
- xDS subscriptions: CDS + EDS
- `ConfigStore` with endpoint resolution
- LB policy execution (round-robin, ring-hash, least-request)
- C ABI: `create`, `destroy`, `resolve`, `pick_endpoint`, `report_result`
- C++ API + one language binding (Go or Java)

### Phase 2: Filter Chain + Auth
- LDS + RDS subscriptions
- `HeadlessFilterChain` execution
- Synchronous filters: `header_mutation`, `credential_injector`, `rbac`
- C ABI: `apply_request_filters`, `apply_response_filters`

### Phase 3: Async Filter Callouts
- `ext_authz` support (gRPC + HTTP modes)
- `jwt_authn` with JWKS fetching
- `oauth2` token management
- Full async callback model in C ABI

### Phase 4: gRPC Integration + Multi-Language
- gRPC resolver/balancer/interceptor integration (Go, Java, C++)
- Drop-in replacement for `grpc-xds` in each language
- Python and Rust bindings

### Phase 5: Advanced Features
- SDS (mTLS certificate management)
- ECDS (dynamic filter config)
- WASM filter support
- Config caching for graceful degradation
- OpenTelemetry integration
