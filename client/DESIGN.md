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

The Envoy Client Library fills this gap by providing full xDS + filter chain + upstream connection
management for any client platform, without the Mobile constraints.

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

- Body-level filter processing (compression, transcoding) — headers-only initially
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

### Key Design Decision: Library Owns the Data Path

The client library manages upstream connections directly, including to the application's upstream
targets. This includes:

- **Making HTTP/gRPC connections** to upstream services discovered via xDS (CDS/EDS)
- **Managing connection pools** to those upstream targets
- **Owning connections** to external filter services (ext_authz, JWKS, OAuth token endpoints)

It answers three questions:
1. **"Where should I send this?"** → endpoint resolution + LB pick
2. **"What should the request/response look like?"** → filter chain execution (header mutation,
   auth injection, authz checks)
3. **"Send it"** → the library makes the upstream connection and forwards the request

The library's `ClusterManager` manages all upstream clusters — both the application's service
targets and external filter service dependencies (ext_authz, JWKS, OAuth).

### Why ClusterManager Is Central

`ClusterManager` is the backbone of all upstream connectivity in the client library:

- `ext_authz` → `GrpcClientImpl` uses `Grpc::AsyncClient` from `ClusterManager::grpcAsyncClientManager()`
- `ext_authz` (HTTP mode) → `RawHttpClientImpl` uses `Http::AsyncClient` from `ClusterManager`
- `jwt_authn` → `JwksAsyncFetcher` uses `Http::AsyncClient` for JWKS endpoint fetching
- `oauth2` → `OAuth2Client` uses `Http::AsyncClient` for token exchange
- `ext_proc` → Uses streaming `Grpc::AsyncClient` for bi-directional processing
- **Application upstreams** → `ClusterManager` pools connections to services discovered via CDS/EDS

All upstream connections require:
- Connection pool management
- The Envoy `Dispatcher` (event loop) for non-blocking I/O
- Cluster configuration (timeouts, TLS, retry, circuit breakers)

Therefore, the client library uses `ClusterManager` in **full capacity**:
- It manages clusters/connections for the application's upstream targets (CDS/EDS-discovered)
- It manages clusters/connections for external filter services (authz, OAuth, JWKS)
- The `ConfigStore` provides a lightweight read-only view of xDS state for endpoint resolution
  and LB decisions, complementing the full `ClusterManager`

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

### Reused (additional — upstream data path)

| Component | Location | Used For |
|-----------|----------|----------|
| `ClusterManager` | `source/common/upstream/` | Full connection pool management for all upstreams |
| HTTP connection pools | `source/common/http/conn_pool_base.h` | Pooling upstream HTTP/gRPC connections |
| HTTP codecs (H1/H2/H3) | `source/common/http/` | Encoding requests to upstream targets |
| `Router::Filter` | `source/common/router/` | Request forwarding to upstream clusters |

### NOT Reused

| Component | Reason |
|-----------|--------|
| `ListenerManager` | No downstream listeners in a client library |
| `HttpConnectionManager` | No downstream HTTP connections to manage |
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

## Client-Side Extensibility

The previous sections describe **server-enforced** behavior — the control plane pushes config and
the library executes it. But real-world adoption requires that applications can also **override,
extend, and hook into** the library's behavior. Without escape hatches, developers won't adopt it.

The extensibility model follows a principle: **the server sets defaults, the client can override**.
This mirrors Envoy's own model where route-level config can override cluster-level config.

### 1. LB Policy Override

The server configures LB policy per cluster via CDS (e.g., round-robin, ring-hash). The client
library should allow applications to override this at multiple levels:

#### a. Engine-Level Default Override

Set at build time, applies to all clusters unless the server or a per-call override says otherwise.

```cpp
auto client = ClientEngineBuilder()
    .setXdsServer("xds.example.com", 443)
    // Override: always use round-robin regardless of server config
    .setDefaultLbPolicy("envoy.load_balancing_policies.round_robin")
    .build();
```

#### b. Per-Cluster Override

Override LB policy for a specific cluster, leaving others server-configured.

```cpp
auto client = ClientEngineBuilder()
    .setXdsServer("xds.example.com", 443)
    // Only override LB for this specific cluster
    .setClusterLbPolicy("my-latency-sensitive-service",
                         "envoy.load_balancing_policies.least_request")
    .build();
```

#### c. Per-Request Override (via Request Context)

At pick time, the application can influence the LB decision:

```cpp
// Override host: pin to a specific endpoint (e.g., for debugging, canary, or session stickiness)
envoy_client_request_context ctx;
ctx.override_host = "10.0.1.5:8080";       // bypass LB, pick this endpoint directly
ctx.override_host_strict = false;           // fall back to LB if host is unhealthy

// Hash key: provide an application-level hash for consistent hashing
ctx.hash_key = session_id;                  // stick user to same endpoint
ctx.hash_key_len = strlen(session_id);

// Metadata match: subset LB — only pick endpoints matching these labels
ctx.metadata = &canary_metadata;            // e.g., {"canary": "true"}
```

#### d. LB Policy Override Precedence

```
Per-request override_host  (highest priority — bypasses LB entirely)
    ↓
Per-request hash_key / metadata_match  (influences LB decision)
    ↓
Per-cluster client override  (setClusterLbPolicy)
    ↓
Engine-level client default  (setDefaultLbPolicy)
    ↓
Server-configured LB policy via CDS  (lowest priority — the default)
```

This maps to how Envoy itself resolves LB decisions: `LoadBalancerContext::overrideHostToSelect()`
has highest priority, then `computeHashKey()` and `metadataMatchCriteria()` influence the algorithm,
and the cluster config determines the algorithm itself.

### 2. Client Interceptors

Interceptors are lightweight application-level hooks that run **around** the server-enforced filter
chain. They are simpler than full Envoy filters — just callbacks that see headers and can modify them.

This is inspired by Envoy Mobile's `PlatformBridgeFilter` pattern, but simplified for the
client library's headers-only model.

#### Interceptor Interface (C++)

```cpp
// client/library/cc/interceptor.h

class ClientInterceptor {
public:
  virtual ~ClientInterceptor() = default;

  // Called BEFORE server-enforced filters run on the request.
  // Return CONTINUE to proceed, DENY to short-circuit with an error.
  virtual InterceptorStatus onRequestHeaders(Http::RequestHeaderMap& headers,
                                              const StreamInfo::StreamInfo& info) {
    return InterceptorStatus::Continue;
  }

  // Called AFTER server-enforced filters have completed on the request.
  // The headers now include all server-filter mutations (auth tokens, etc.)
  virtual InterceptorStatus onRequestHeadersComplete(Http::RequestHeaderMap& headers,
                                                      const StreamInfo::StreamInfo& info) {
    return InterceptorStatus::Continue;
  }

  // Called BEFORE server-enforced filters run on the response.
  virtual InterceptorStatus onResponseHeaders(Http::ResponseHeaderMap& headers,
                                               const StreamInfo::StreamInfo& info) {
    return InterceptorStatus::Continue;
  }

  // Called AFTER server-enforced filters have completed on the response.
  virtual InterceptorStatus onResponseHeadersComplete(Http::ResponseHeaderMap& headers,
                                                       const StreamInfo::StreamInfo& info) {
    return InterceptorStatus::Continue;
  }
};
```

#### Interceptor Registration

```cpp
auto client = ClientEngineBuilder()
    .setXdsServer("xds.example.com", 443)
    // Add interceptors (executed in registration order)
    .addInterceptor("tracing", std::make_shared<TracingInterceptor>(tracer))
    .addInterceptor("metrics", std::make_shared<MetricsInterceptor>(stats))
    .addInterceptor("app-headers", std::make_shared<AppHeaderInterceptor>())
    .build();
```

#### Execution Order

```
Application calls: apply_request_filters("my-service", headers)
    │
    ▼
┌── Client Interceptors: onRequestHeaders() ──┐
│   1. TracingInterceptor  → adds trace-id     │
│   2. MetricsInterceptor → records start time │
│   3. AppHeaderInterceptor → adds x-app-ver   │
└──────────────┬───────────────────────────────┘
               │
               ▼
┌── Server-Enforced Filter Chain ──────────────┐
│   1. RBAC           → policy check           │
│   2. ext_authz      → external authorization │
│   3. header_mutation → server-mandated headers│
│   4. credential_injector → OAuth token       │
└──────────────┬───────────────────────────────┘
               │
               ▼
┌── Client Interceptors: onRequestHeadersComplete() ─┐
│   1. TracingInterceptor  → adds final span info     │
│   2. MetricsInterceptor → no-op                     │
│   3. AppHeaderInterceptor → no-op                   │
└──────────────┬──────────────────────────────────────┘
               │
               ▼
Callback fires with final headers
```

#### Interceptor via C ABI

For language bindings that can't use the C++ interceptor interface:

```c
// C ABI interceptor callback
typedef envoy_client_status (*envoy_client_interceptor_cb)(
    envoy_client_headers* headers,         // mutable — interceptor can modify
    const char* cluster_name,
    uint32_t phase,                        // 0=pre_request, 1=post_request,
                                           // 2=pre_response, 3=post_response
    void* context
);

// Register an interceptor
envoy_client_status envoy_client_add_interceptor(
    envoy_client_handle handle,
    const char* name,
    envoy_client_interceptor_cb callback,
    void* context
);

// Remove an interceptor
envoy_client_status envoy_client_remove_interceptor(
    envoy_client_handle handle,
    const char* name
);
```

### 3. LB Context Callback

For advanced use cases, the application can register a callback that the library invokes during
endpoint selection. This lets the app provide dynamic, per-request context to the LB algorithm
without having to set it on every `request_context` struct.

```c
// LB context provider callback — called during pick_endpoint
typedef void (*envoy_client_lb_context_cb)(
    const char* cluster_name,
    envoy_client_request_context* ctx,     // mutable — callback can enrich
    void* context
);

// Register a global LB context provider
envoy_client_status envoy_client_set_lb_context_provider(
    envoy_client_handle handle,
    envoy_client_lb_context_cb callback,
    void* context
);
```

**Use cases:**
- Inject session affinity keys from thread-local storage
- Add locality preferences based on current datacenter detection
- Dynamically exclude endpoints based on circuit-breaker state the app tracks
- Enrich metadata for subset routing based on runtime feature flags

### 4. Client-Side Filter Injection

Beyond simple interceptors, advanced users may want to inject **full Envoy filters** that run
alongside server-pushed filters. This follows Envoy Mobile's `addNativeFilter` pattern.

```cpp
auto client = ClientEngineBuilder()
    .setXdsServer("xds.example.com", 443)
    // Inject a native Envoy filter (runs as part of the filter chain)
    .addNativeFilter("envoy.filters.http.lua",
                      R"pb(inline_code: "function envoy_on_request(h) h:headers():add('x-lua', 'yes') end")pb")
    // Control where client filters appear relative to server filters
    .setFilterMergePolicy(FilterMergePolicy::CLIENT_BEFORE_SERVER)
    .build();
```

#### Filter Merge Policies

When both client-injected filters and server-pushed filters exist, the merge policy controls
ordering:

| Policy | Order | Use Case |
|--------|-------|----------|
| `CLIENT_BEFORE_SERVER` | Client filters → Server filters | App sets context before policy enforcement |
| `SERVER_BEFORE_CLIENT` | Server filters → Client filters | App reacts to server-enforced mutations |
| `CLIENT_WRAPS_SERVER` | Client pre → Server filters → Client post | **Default.** Client observes both input and output |
| `CLIENT_ONLY` | Only client filters (ignore server) | Testing, local development |
| `SERVER_ONLY` | Only server filters (ignore client) | Strict server control, compliance |

`CLIENT_WRAPS_SERVER` is the default because it's the most flexible: interceptors see headers both
before and after server filters run, and injected native filters can be placed at either end.

### 5. xDS Watch Callbacks

Applications may want to react to xDS config changes — not just consume the final resolved
endpoints, but observe when clusters are added/removed, when filter chains change, etc.

```c
// Watch callback for config changes
typedef void (*envoy_client_config_cb)(
    const char* resource_type,            // "cluster", "endpoint", "route", "listener"
    const char* resource_name,
    uint32_t event,                       // 0=added, 1=updated, 2=removed
    void* context
);

// Register a config change watcher
envoy_client_status envoy_client_watch_config(
    envoy_client_handle handle,
    const char* resource_type,             // NULL = watch all types
    envoy_client_config_cb callback,
    void* context
);
```

**Use cases:**
- Logging/alerting when endpoints change
- Updating application-level routing tables
- Triggering connection pool warm-up in the application when new clusters appear
- Debugging xDS push behavior

### 6. Summary: Server vs Client Control

| Capability | Server (xDS) | Client (API) | Override? |
|------------|-------------|-------------|-----------|
| **LB policy** | CDS cluster config | `setDefaultLbPolicy`, `setClusterLbPolicy`, per-request context | Client wins if set |
| **Filter chain** | LDS/ECDS pushed filters | `addNativeFilter`, interceptors | Merge policy controls ordering |
| **Endpoint selection** | EDS endpoints, health | `override_host`, `metadata_match`, LB context callback | Client can pin or filter |
| **Route matching** | RDS route config | `match_route()` API (read-only) | Server only (client just queries) |
| **TLS/mTLS** | SDS certificates | Bootstrap static certs | Either source |
| **Config observability** | N/A | `watch_config()` callback | Client only (observation) |

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
  envoy_client_headers* metadata;  // Key-value metadata for subset LB matching
  const char* path;                // Request path for route matching
  const char* authority;           // :authority header value

  // --- Client-side LB overrides (all optional, NULL/0 = not set) ---

  // Override host: bypass LB and pick this endpoint directly.
  // Format: "ip:port" (e.g., "10.0.1.5:8080")
  const char* override_host;
  // If true, return UNAVAILABLE when override_host is unhealthy.
  // If false, fall back to normal LB when override_host is unhealthy.
  uint32_t override_host_strict;

  // Hash key: provide an explicit hash for consistent-hashing LB (ring-hash, maglev).
  // Overrides the server-configured hash policy for this request.
  const char* hash_key;
  size_t hash_key_len;
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

// --- Client Interceptors ---

// Interceptor phases
typedef enum {
  ENVOY_CLIENT_PHASE_PRE_REQUEST = 0,   // Before server filters on request
  ENVOY_CLIENT_PHASE_POST_REQUEST = 1,  // After server filters on request
  ENVOY_CLIENT_PHASE_PRE_RESPONSE = 2,  // Before server filters on response
  ENVOY_CLIENT_PHASE_POST_RESPONSE = 3, // After server filters on response
} envoy_client_interceptor_phase;

// Interceptor callback. Return ENVOY_CLIENT_OK to continue, ENVOY_CLIENT_DENIED to abort.
typedef envoy_client_status (*envoy_client_interceptor_cb)(
    envoy_client_headers* headers,
    const char* cluster_name,
    envoy_client_interceptor_phase phase,
    void* context
);

// Register a named interceptor. Interceptors execute in registration order.
envoy_client_status envoy_client_add_interceptor(
    envoy_client_handle handle,
    const char* name,
    envoy_client_interceptor_cb callback,
    void* context
);

// Remove a previously registered interceptor by name.
envoy_client_status envoy_client_remove_interceptor(
    envoy_client_handle handle,
    const char* name
);

// --- LB Context Provider ---

// Callback invoked during pick_endpoint to let the app enrich the LB context.
// The callback can modify ctx (set hash_key, override_host, metadata, etc.)
typedef void (*envoy_client_lb_context_cb)(
    const char* cluster_name,
    envoy_client_request_context* ctx,
    void* context
);

// Set a global LB context provider. Only one can be active at a time.
envoy_client_status envoy_client_set_lb_context_provider(
    envoy_client_handle handle,
    envoy_client_lb_context_cb callback,
    void* context
);

// --- Config Watch ---

typedef enum {
  ENVOY_CLIENT_CONFIG_ADDED = 0,
  ENVOY_CLIENT_CONFIG_UPDATED = 1,
  ENVOY_CLIENT_CONFIG_REMOVED = 2,
} envoy_client_config_event;

// Callback for xDS config changes.
typedef void (*envoy_client_config_cb)(
    const char* resource_type,
    const char* resource_name,
    envoy_client_config_event event,
    void* context
);

// Watch for config changes. resource_type NULL = watch all types.
envoy_client_status envoy_client_watch_config(
    envoy_client_handle handle,
    const char* resource_type,
    envoy_client_config_cb callback,
    void* context
);

// --- LB Policy Override ---

// Override the LB policy for a specific cluster. Pass NULL to clear the override.
envoy_client_status envoy_client_set_cluster_lb_policy(
    envoy_client_handle handle,
    const char* cluster_name,
    const char* lb_policy_name   // e.g., "envoy.load_balancing_policies.round_robin"
);

// Set the default LB policy override for all clusters. Pass NULL to clear.
envoy_client_status envoy_client_set_default_lb_policy(
    envoy_client_handle handle,
    const char* lb_policy_name
);

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
// The RequestContext allows per-request LB overrides (hash key, override host, metadata).
func (c *Client) PickEndpoint(cluster string, ctx *RequestContext) (*Endpoint, error) {
    // ... CGo bridge to envoy_client_pick_endpoint
}

// PickEndpointWithOverride bypasses LB and picks a specific endpoint.
// Falls back to normal LB if the endpoint is unhealthy and strict=false.
func (c *Client) PickEndpointWithOverride(cluster, host string, strict bool) (*Endpoint, error) {
    ctx := &RequestContext{
        OverrideHost:       host,
        OverrideHostStrict: strict,
    }
    return c.PickEndpoint(cluster, ctx)
}

// ApplyRequestFilters runs interceptors + server-pushed filter chain.
// This is async because filters like ext_authz may call external services.
func (c *Client) ApplyRequestFilters(cluster string, headers map[string]string) (map[string]string, error) {
    // ... CGo bridge to envoy_client_apply_request_filters
    // Uses a channel to bridge async callback to sync Go call
}

// --- Client-Side Extensibility ---

// Interceptor is a lightweight hook that runs around the server-enforced filter chain.
type Interceptor interface {
    // OnRequest is called before (phase=Pre) and after (phase=Post) server filters.
    OnRequest(headers map[string]string, cluster string, phase Phase) error
    // OnResponse is called before (phase=Pre) and after (phase=Post) server filters.
    OnResponse(headers map[string]string, cluster string, phase Phase) error
}

type Phase int
const (
    PhasePre  Phase = iota  // Before server filters
    PhasePost               // After server filters
)

// AddInterceptor registers a named interceptor. Interceptors execute in registration order.
func (c *Client) AddInterceptor(name string, interceptor Interceptor) error {
    // ... CGo bridge to envoy_client_add_interceptor
}

// RemoveInterceptor removes a previously registered interceptor.
func (c *Client) RemoveInterceptor(name string) error {
    // ... CGo bridge to envoy_client_remove_interceptor
}

// SetClusterLbPolicy overrides the LB policy for a specific cluster.
func (c *Client) SetClusterLbPolicy(cluster, lbPolicy string) error {
    // ... CGo bridge to envoy_client_set_cluster_lb_policy
}

// SetDefaultLbPolicy overrides the default LB policy for all clusters.
func (c *Client) SetDefaultLbPolicy(lbPolicy string) error {
    // ... CGo bridge to envoy_client_set_default_lb_policy
}

// LbContextProvider is called during endpoint selection to enrich the LB context.
type LbContextProvider func(cluster string, ctx *RequestContext)

// SetLbContextProvider registers a callback invoked during every pick_endpoint call.
func (c *Client) SetLbContextProvider(provider LbContextProvider) error {
    // ... CGo bridge to envoy_client_set_lb_context_provider
}

// ConfigEvent represents an xDS config change.
type ConfigEvent struct {
    ResourceType string // "cluster", "endpoint", "route", "listener"
    ResourceName string
    Event        string // "added", "updated", "removed"
}

// WatchConfig registers a callback for xDS config changes.
// Pass empty resourceType to watch all types.
func (c *Client) WatchConfig(resourceType string, cb func(ConfigEvent)) error {
    // ... CGo bridge to envoy_client_watch_config
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

    // Watch for config changes to trigger connection pool warm-up
    r.client.WatchConfig("endpoint", func(e ConfigEvent) {
        if e.Event == "added" || e.Event == "updated" {
            cc.UpdateState(/* ... */)
        }
    })
}

// As a gRPC balancer — uses xDS-configured LB policy (with client overrides)
type envoyBalancer struct {
    client *Client
}

func (b *envoyBalancer) Pick(info balancer.PickInfo) (balancer.PickResult, error) {
    ctx := &RequestContext{
        Path: info.FullMethodName,
    }

    // Per-request LB override: if the caller set a target host in context,
    // use override_host to pin to that endpoint (e.g., for debugging or stickiness)
    if host, ok := info.Ctx.Value(targetHostKey{}).(string); ok {
        ctx.OverrideHost = host
        ctx.OverrideHostStrict = false // fall back to LB if host is unhealthy
    }

    // Per-request hash key: if the caller set a session ID, use it for
    // consistent hashing so the same user hits the same endpoint
    if sessionID, ok := info.Ctx.Value(sessionIDKey{}).(string); ok {
        ctx.HashKey = sessionID
    }

    endpoint, err := b.client.PickEndpoint(b.cluster, ctx)
    // Return the subconn corresponding to this endpoint
}

// As a gRPC interceptor — runs client interceptors + server-enforced filters
func EnvoyUnaryInterceptor(client *Client) grpc.UnaryClientInterceptor {
    return func(ctx context.Context, method string, req, reply interface{},
                cc *grpc.ClientConn, invoker grpc.UnaryInvoker, opts ...grpc.CallOption) error {
        // Apply request filters:
        // 1. Client interceptors (pre) — e.g., tracing, app headers
        // 2. Server-enforced filters — e.g., ext_authz, credential_injector
        // 3. Client interceptors (post) — e.g., final logging
        modifiedHeaders, err := client.ApplyRequestFilters(cluster, extractHeaders(ctx))
        if err != nil {
            return err // Request denied by server filter or client interceptor
        }
        ctx = injectHeaders(ctx, modifiedHeaders)
        return invoker(ctx, method, req, reply, cc, opts...)
    }
}

// Example: setting up a client with interceptors for gRPC
func setupGRPCClient() *grpc.ClientConn {
    client, _ := New(bootstrapYAML)

    // Override LB for a specific service
    client.SetClusterLbPolicy("payments", "envoy.load_balancing_policies.least_request")

    // Add a tracing interceptor
    client.AddInterceptor("tracing", &tracingInterceptor{tracer: otel.Tracer("grpc")})

    // Provide session affinity via LB context
    client.SetLbContextProvider(func(cluster string, ctx *RequestContext) {
        if session := getCurrentSession(); session != nil {
            ctx.HashKey = session.ID
        }
    })

    conn, _ := grpc.Dial("envoy:///my-service",
        grpc.WithResolvers(&envoyResolver{client: client}),
        grpc.WithUnaryInterceptor(EnvoyUnaryInterceptor(client)),
    )
    return conn
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
    // --- Extension allowlist (which server-pushed filters/LB the binary supports) ---
    .enableFilter("envoy.filters.http.ext_authz")
    .enableFilter("envoy.filters.http.credential_injector")
    .enableFilter("envoy.filters.http.rbac")
    .enableLbPolicy("envoy.load_balancing_policies.round_robin")
    .enableLbPolicy("envoy.load_balancing_policies.ring_hash")
    .setStatsSink("envoy.stat_sinks.statsd", statsd_config)
    // --- Client-side overrides ---
    // Override LB policy for a latency-sensitive service
    .setClusterLbPolicy("payments-service",
                         "envoy.load_balancing_policies.least_request")
    // Inject a client-side Lua filter (runs alongside server-pushed filters)
    .addNativeFilter("envoy.filters.http.lua",
                      R"(inline_code: "function envoy_on_request(h) h:headers():add('x-client-version', '2.1') end")")
    // Control client vs server filter ordering
    .setFilterMergePolicy(FilterMergePolicy::CLIENT_WRAPS_SERVER)
    // Register interceptors (lightweight hooks, simpler than full filters)
    .addInterceptor("tracing", std::make_shared<TracingInterceptor>(tracer))
    .addInterceptor("metrics", std::make_shared<MetricsInterceptor>(stats))
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
| **Owns data path** | Yes (full HTTP codec, conn pools) | Yes (owns upstream connections + conn pools) |
| **Event loop** | Yes (for full proxy) | Yes (for xDS + upstream I/O + filter callouts) |
| **Async outbound calls** | Yes (full upstream) | Yes (upstream targets + filter callouts: ext_authz, JWKS, etc.) |
| **Platform bindings** | Android (JNI), iOS (Swift/ObjC) | Go (CGo), Java (JNI), Python, Rust, C++ |
| **Bootstrap** | `StrippedMainBase` + `ServerLite` | `StrippedMainBase` + `ServerClientLite` |
| **Filters** | Full chain (request + response body) | Header-focused + async callouts |
| **Client filters** | `addPlatformFilter` / `addNativeFilter` | `addNativeFilter` + interceptors + filter merge policy |
| **LB override** | N/A (library picks endpoint & connects) | Per-request, per-cluster, and default LB policy overrides |
| **Target binary size** | ~15-20MB | ~12-16MB (upstream conn pools + codecs included) |
| **ListenerManager** | API listener (no TCP accept) | Not needed |
| **ClusterManager** | Full (owns all connections) | Full (owns all upstream + filter callout connections) |
| **Use case** | Mobile app HTTP networking | Any client needing xDS + server-enforced policy + managed upstream connections |

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

### 7. Client Override vs Server Authority

When client-side LB overrides or filter injection conflict with server-enforced policy, who wins?
This is a security and governance question.

**Options:**
- A. Client always wins (maximum flexibility, minimum server control)
- B. Server can mark config as "non-overridable" via xDS metadata (server authority)
- C. Default to client wins, but support an `authority_mode` flag in bootstrap:
     - `CLIENT_AUTHORITY`: client overrides take effect (default, for trusted environments)
     - `SERVER_AUTHORITY`: server overrides are immutable (for compliance/regulated environments)
- D. Per-capability authority (e.g., server controls filters, client controls LB)

### 8. Interceptor Threading Model

Client interceptors run on the library's Dispatcher thread. Should the library allow interceptors
to block (e.g., making their own network calls)?

**Options:**
- A. Interceptors must be synchronous and non-blocking (simple, safe, fast)
- B. Support async interceptors with a callback/future model (complex but powerful)
- C. Synchronous by default, with an opt-in async path for specific interceptors
- D. Always dispatch interceptor callbacks to a user-provided thread pool

## Implementation Phases

### Phase 1: xDS Core + Endpoint Resolution + LB Overrides
- `ServerClientLite` bootstrap via `StrippedMainBase`
- xDS subscriptions: CDS + EDS
- `ConfigStore` with endpoint resolution
- LB policy execution (round-robin, ring-hash, least-request)
- **Client-side LB overrides**: `override_host`, `hash_key`, per-cluster LB policy, default LB policy
- **LB context provider** callback
- **Config watch** callbacks for CDS/EDS changes
- C ABI: `create`, `destroy`, `resolve`, `pick_endpoint`, `report_result`,
  `set_cluster_lb_policy`, `set_default_lb_policy`, `set_lb_context_provider`, `watch_config`
- C++ API + one language binding (Go or Java)

### Phase 2: Filter Chain + Auth + Interceptors
- LDS + RDS subscriptions
- `HeadlessFilterChain` execution
- Synchronous filters: `header_mutation`, `credential_injector`, `rbac`
- **Client interceptors**: pre/post request and response hooks
- **Client-side native filter injection** via `addNativeFilter`
- **Filter merge policy**: `CLIENT_WRAPS_SERVER` (default), `CLIENT_BEFORE_SERVER`, etc.
- C ABI: `apply_request_filters`, `apply_response_filters`, `add_interceptor`, `remove_interceptor`

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
- Server authority mode (non-overridable server config)
