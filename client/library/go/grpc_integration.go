package envoyclient

// gRPC integration for the Envoy Client Library.
//
// RegisterGRPC wires the library into gRPC-Go as a custom resolver and
// balancer so existing gRPC code can benefit from xDS endpoint resolution
// and Envoy LB policies with minimal changes:
//
//	envoyclient.RegisterGRPC(client)
//
//	conn, err := grpc.Dial(
//	    "envoy:///my-cluster",
//	    grpc.WithDefaultServiceConfig(`{"loadBalancingConfig":[{"envoy_lb":{}}]}`),
//	    grpc.WithInsecure(),
//	)
//
// Per-call LB context (hash key, override host) is injected via gRPC call
// options using the helpers in this file:
//
//	ctx = envoyclient.WithHashKey(ctx, sessionID)
//	conn.Invoke(ctx, "/svc.Svc/Method", req, resp)

import (
	"context"
	"fmt"
	"sync"

	"google.golang.org/grpc/attributes"
	"google.golang.org/grpc/balancer"
	"google.golang.org/grpc/balancer/base"
	"google.golang.org/grpc/resolver"
)

// scheme is the URI scheme used to identify Envoy-backed targets.
// Use "envoy:///cluster-name" as the gRPC target address.
const scheme = "envoy"

// envoyBalancerName is the name registered with the gRPC balancer registry.
const envoyBalancerName = "envoy_lb"

// RegisterGRPC registers the Envoy resolver and balancer with the gRPC
// runtime. Call once during program initialisation, before any Dial calls.
func RegisterGRPC(client *Client) {
	resolver.Register(&envoyResolverBuilder{client: client})
	balancer.Register(newEnvoyBalancerBuilder(client))
}

// ---------------------------------------------------------------------------
// Per-call context keys
// ---------------------------------------------------------------------------

type hashKeyCtxKey struct{}
type overrideHostCtxKey struct{}

// WithHashKey injects a consistent-hashing key into a gRPC call context. The
// Envoy balancer uses it to pin the request to a stable endpoint (ring-hash /
// maglev policies).
func WithHashKey(ctx context.Context, key string) context.Context {
	return context.WithValue(ctx, hashKeyCtxKey{}, key)
}

// HashKeyFromContext returns the hash key set by WithHashKey, and whether it
// was present.
func HashKeyFromContext(ctx context.Context) (string, bool) {
	k, ok := ctx.Value(hashKeyCtxKey{}).(string)
	return k, ok
}

// WithOverrideHost pins a gRPC call to a specific upstream endpoint
// ("ip:port"). If the endpoint is unhealthy the balancer falls back to normal
// LB (non-strict mode).
func WithOverrideHost(ctx context.Context, host string) context.Context {
	return context.WithValue(ctx, overrideHostCtxKey{}, host)
}

// OverrideHostFromContext returns the override host set by WithOverrideHost.
func OverrideHostFromContext(ctx context.Context) (string, bool) {
	h, ok := ctx.Value(overrideHostCtxKey{}).(string)
	return h, ok
}

// ---------------------------------------------------------------------------
// Resolver
// ---------------------------------------------------------------------------

// envoyResolverBuilder implements resolver.Builder and creates an
// envoyResolver for each "envoy:///<cluster>" target.
type envoyResolverBuilder struct {
	client *Client
}

func (b *envoyResolverBuilder) Scheme() string { return scheme }

func (b *envoyResolverBuilder) Build(target resolver.Target,
	cc resolver.ClientConn, _ resolver.BuildOptions) (resolver.Resolver, error) {

	cluster := target.Endpoint()
	if cluster == "" {
		cluster = target.URL.Host
	}

	r := &envoyResolver{client: b.client, cluster: cluster, cc: cc}

	// Push initial addresses synchronously.
	r.updateAddresses()

	// Watch for future EDS changes and push updated addresses.
	if err := b.client.WatchConfig("endpoint", func(ev ConfigEvent) {
		if ev.ResourceName == cluster || ev.ResourceName == "" {
			r.updateAddresses()
		}
	}); err != nil {
		return nil, fmt.Errorf("envoyclient: WatchConfig: %w", err)
	}

	return r, nil
}

type envoyResolver struct {
	client  *Client
	cluster string
	cc      resolver.ClientConn
}

func (r *envoyResolver) updateAddresses() {
	endpoints, err := r.client.Resolve(r.cluster)
	if err != nil || len(endpoints) == 0 {
		return
	}
	addrs := make([]resolver.Address, 0, len(endpoints))
	for _, ep := range endpoints {
		addr := resolver.Address{
			Addr: fmt.Sprintf("%s:%d", ep.Address, ep.Port),
			// Carry both the Endpoint and the cluster name through gRPC's
			// attribute system. base.PickerBuildInfo only exposes per-address
			// SubConnInfo, so the cluster name must live on the address itself.
			BalancerAttributes: attributes.New(endpointAttrKey{}, ep).
				WithValue(clusterAttrKey{}, r.cluster),
		}
		addrs = append(addrs, addr)
	}
	_ = r.cc.UpdateState(resolver.State{Addresses: addrs})
}

func (r *envoyResolver) ResolveNow(_ resolver.ResolveNowOptions) { r.updateAddresses() }
func (r *envoyResolver) Close()                                   {}

// ---------------------------------------------------------------------------
// Balancer
// ---------------------------------------------------------------------------

// Attribute key types (unexported to avoid collisions).
type endpointAttrKey struct{}
type clusterAttrKey struct{}

func newEnvoyBalancerBuilder(client *Client) balancer.Builder {
	return base.NewBalancerBuilder(
		envoyBalancerName,
		&envoyPickerBuilder{client: client},
		base.Config{},
	)
}

// envoyPickerBuilder implements base.PickerBuilder.
type envoyPickerBuilder struct {
	client *Client
}

func (b *envoyPickerBuilder) Build(info base.PickerBuildInfo) balancer.Picker {
	// Map "address" → SubConn for fast lookup after PickEndpoint returns.
	subconns := make(map[string]balancer.SubConn, len(info.ReadySCs))
	for sc, sci := range info.ReadySCs {
		subconns[sci.Address.Addr] = sc
	}

	// Extract the cluster name from the first ready SubConn's BalancerAttributes.
	// The resolver embeds it there (along with the Endpoint) so the picker can
	// call PickEndpoint with the correct cluster name.
	cluster := ""
	for _, sci := range info.ReadySCs {
		if sci.Address.BalancerAttributes != nil {
			if c, ok := sci.Address.BalancerAttributes.Value(clusterAttrKey{}).(string); ok {
				cluster = c
				break
			}
		}
	}

	return &envoyPicker{client: b.client, cluster: cluster, subconns: subconns}
}

// envoyPicker implements balancer.Picker. It delegates endpoint selection to
// the Envoy Client Library and maps the result to a gRPC SubConn.
type envoyPicker struct {
	client   *Client
	cluster  string
	mu       sync.Mutex
	subconns map[string]balancer.SubConn
}

func (p *envoyPicker) Pick(info balancer.PickInfo) (balancer.PickResult, error) {
	ctx := &RequestContext{
		Path: info.FullMethodName,
	}
	// Honour per-call context values set by the application.
	if key, ok := HashKeyFromContext(info.Ctx); ok {
		ctx.HashKey = key
	}
	if host, ok := OverrideHostFromContext(info.Ctx); ok {
		ctx.OverrideHost = host
	}

	ep, err := p.client.PickEndpoint(p.cluster, ctx)
	if err != nil {
		return balancer.PickResult{}, err
	}
	if ep == nil {
		return balancer.PickResult{}, balancer.ErrNoSubConnAvailable
	}

	addr := fmt.Sprintf("%s:%d", ep.Address, ep.Port)
	p.mu.Lock()
	sc, ok := p.subconns[addr]
	p.mu.Unlock()
	if !ok {
		return balancer.PickResult{}, balancer.ErrNoSubConnAvailable
	}

	// Report the RPC outcome back to the library for feedback-driven LB.
	capturedEp := *ep
	return balancer.PickResult{
		SubConn: sc,
		Done: func(di balancer.DoneInfo) {
			var code uint32
			if di.Err == nil {
				code = 200
			}
			p.client.ReportResult(&capturedEp, code, 0)
		},
	}, nil
}
