// Tests for the Go Envoy Client Library binding.
//
// These tests use a stub bootstrap config so the engine does not actually
// connect to an xDS server. They exercise the Go API surface (argument
// marshalling, error handling, callback wiring) independently of live xDS.
package envoyclient_test

import (
	"testing"

	envoyclient "github.com/envoyproxy/envoy/client/library/go"
)

// minimalBootstrap is a minimal static Envoy bootstrap that satisfies the
// engine without connecting to any xDS server. Endpoints are provided
// statically via EDS so tests can exercise resolve/pick without live xDS.
const minimalBootstrap = `
node:
  id: "test-node"
  cluster: "test-cluster"

static_resources:
  clusters:
  - name: test-cluster
    type: STATIC
    load_assignment:
      cluster_name: test-cluster
      endpoints:
      - lb_endpoints:
        - endpoint:
            address:
              socket_address:
                address: 127.0.0.1
                port_value: 8080
          load_balancing_weight: 1
        - endpoint:
            address:
              socket_address:
                address: 127.0.0.1
                port_value: 8081
          load_balancing_weight: 2
`

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

func TestNew_EmptyConfig_ReturnsError(t *testing.T) {
	_, err := envoyclient.New("")
	if err == nil {
		t.Fatal("expected error for empty bootstrap config, got nil")
	}
}

func TestNew_InvalidYAML_ReturnsError(t *testing.T) {
	_, err := envoyclient.New("!!not valid yaml{{{{")
	if err == nil {
		t.Fatal("expected error for invalid YAML, got nil")
	}
}

func TestNew_ValidConfig_Succeeds(t *testing.T) {
	c, err := envoyclient.New(minimalBootstrap)
	if err != nil {
		t.Fatalf("New() returned unexpected error: %v", err)
	}
	c.Close()
}

func TestClose_Idempotent(t *testing.T) {
	c, err := envoyclient.New(minimalBootstrap)
	if err != nil {
		t.Fatalf("New() error: %v", err)
	}
	// Closing twice must not panic or crash.
	c.Close()
	c.Close()
}

// ---------------------------------------------------------------------------
// WaitReady
// ---------------------------------------------------------------------------

func TestWaitReady_StaticConfig_NoTimeout(t *testing.T) {
	c, err := envoyclient.New(minimalBootstrap)
	if err != nil {
		t.Fatalf("New() error: %v", err)
	}
	defer c.Close()

	if err := c.WaitReady(10); err != nil {
		t.Fatalf("WaitReady() unexpected error: %v", err)
	}
}

// ---------------------------------------------------------------------------
// Resolve
// ---------------------------------------------------------------------------

func TestResolve_KnownCluster_ReturnsEndpoints(t *testing.T) {
	c := mustNew(t)
	defer c.Close()
	mustWaitReady(t, c)

	endpoints, err := c.Resolve("test-cluster")
	if err != nil {
		t.Fatalf("Resolve() error: %v", err)
	}
	if len(endpoints) == 0 {
		t.Fatal("Resolve() returned empty endpoint list for known cluster")
	}
	for _, ep := range endpoints {
		if ep.Address == "" {
			t.Error("Resolve() returned endpoint with empty address")
		}
		if ep.Port == 0 {
			t.Error("Resolve() returned endpoint with zero port")
		}
	}
}

func TestResolve_UnknownCluster_ReturnsNil(t *testing.T) {
	c := mustNew(t)
	defer c.Close()
	mustWaitReady(t, c)

	endpoints, err := c.Resolve("no-such-cluster")
	if err != nil {
		t.Fatalf("Resolve() unexpected error for unknown cluster: %v", err)
	}
	if len(endpoints) != 0 {
		t.Fatalf("Resolve() expected empty list for unknown cluster, got %d endpoints", len(endpoints))
	}
}

// ---------------------------------------------------------------------------
// PickEndpoint
// ---------------------------------------------------------------------------

func TestPickEndpoint_NilContext_Succeeds(t *testing.T) {
	c := mustNew(t)
	defer c.Close()
	mustWaitReady(t, c)

	ep, err := c.PickEndpoint("test-cluster", nil)
	if err != nil {
		t.Fatalf("PickEndpoint() error: %v", err)
	}
	if ep == nil {
		t.Fatal("PickEndpoint() returned nil for known cluster")
	}
	if ep.Address == "" || ep.Port == 0 {
		t.Errorf("PickEndpoint() returned incomplete endpoint: %+v", ep)
	}
}

func TestPickEndpoint_WithHashKey_Succeeds(t *testing.T) {
	c := mustNew(t)
	defer c.Close()
	mustWaitReady(t, c)

	ctx := &envoyclient.RequestContext{HashKey: "session-abc-123"}
	ep, err := c.PickEndpoint("test-cluster", ctx)
	if err != nil {
		t.Fatalf("PickEndpoint() with hash key error: %v", err)
	}
	if ep == nil {
		t.Fatal("PickEndpoint() returned nil for known cluster with hash key")
	}
}

func TestPickEndpoint_ConsistentHash_SameKeyReturnsSameEndpoint(t *testing.T) {
	c := mustNew(t)
	defer c.Close()
	mustWaitReady(t, c)

	// Set ring-hash so consistent hashing is active.
	if err := c.SetDefaultLbPolicy("envoy.load_balancing_policies.ring_hash"); err != nil {
		// ring-hash may not be compiled in for all test builds; skip gracefully.
		t.Skipf("SetDefaultLbPolicy ring_hash skipped: %v", err)
	}

	ctx := &envoyclient.RequestContext{HashKey: "sticky-user-99"}
	first, err := c.PickEndpoint("test-cluster", ctx)
	if err != nil || first == nil {
		t.Skipf("PickEndpoint skipped: err=%v ep=%v", err, first)
	}
	for i := 0; i < 5; i++ {
		ep, _ := c.PickEndpoint("test-cluster", ctx)
		if ep != nil && ep.Address != first.Address {
			t.Errorf("consistent hash pick %d: got %s, want %s", i, ep.Address, first.Address)
		}
	}
}

func TestPickEndpoint_UnknownCluster_ReturnsNil(t *testing.T) {
	c := mustNew(t)
	defer c.Close()
	mustWaitReady(t, c)

	ep, err := c.PickEndpoint("no-such-cluster", nil)
	if err != nil {
		t.Fatalf("PickEndpoint() unexpected error for unknown cluster: %v", err)
	}
	if ep != nil {
		t.Fatalf("PickEndpoint() expected nil for unknown cluster, got %+v", ep)
	}
}

// ---------------------------------------------------------------------------
// ReportResult
// ---------------------------------------------------------------------------

func TestReportResult_NilEndpoint_NoPanic(t *testing.T) {
	c := mustNew(t)
	defer c.Close()
	// Should be a no-op, not a panic.
	c.ReportResult(nil, 200, 10)
}

func TestReportResult_ValidEndpoint_NoPanic(t *testing.T) {
	c := mustNew(t)
	defer c.Close()
	mustWaitReady(t, c)

	ep, _ := c.PickEndpoint("test-cluster", nil)
	if ep == nil {
		t.Skip("no endpoint available")
	}
	c.ReportResult(ep, 200, 42)
	c.ReportResult(ep, 503, 1000)
}

// ---------------------------------------------------------------------------
// LB policy overrides
// ---------------------------------------------------------------------------

func TestSetClusterLbPolicy_RoundRobin_Succeeds(t *testing.T) {
	c := mustNew(t)
	defer c.Close()

	err := c.SetClusterLbPolicy("test-cluster", "envoy.load_balancing_policies.round_robin")
	if err != nil {
		t.Fatalf("SetClusterLbPolicy() error: %v", err)
	}
}

func TestSetClusterLbPolicy_EmptyString_ClearsOverride(t *testing.T) {
	c := mustNew(t)
	defer c.Close()

	if err := c.SetClusterLbPolicy("test-cluster", "envoy.load_balancing_policies.round_robin"); err != nil {
		t.Fatalf("setup: %v", err)
	}
	if err := c.SetClusterLbPolicy("test-cluster", ""); err != nil {
		t.Fatalf("clear: %v", err)
	}
}

func TestSetDefaultLbPolicy_Succeeds(t *testing.T) {
	c := mustNew(t)
	defer c.Close()

	if err := c.SetDefaultLbPolicy("envoy.load_balancing_policies.round_robin"); err != nil {
		t.Fatalf("SetDefaultLbPolicy() error: %v", err)
	}
}

// ---------------------------------------------------------------------------
// WatchConfig
// ---------------------------------------------------------------------------

func TestWatchConfig_EmptyType_Succeeds(t *testing.T) {
	c := mustNew(t)
	defer c.Close()

	// Empty resource type = watch all. Should register without error.
	err := c.WatchConfig("", func(ev envoyclient.ConfigEvent) {})
	if err != nil {
		t.Fatalf("WatchConfig('') error: %v", err)
	}
}

func TestWatchConfig_ClusterType_Succeeds(t *testing.T) {
	c := mustNew(t)
	defer c.Close()

	err := c.WatchConfig("cluster", func(ev envoyclient.ConfigEvent) {})
	if err != nil {
		t.Fatalf("WatchConfig('cluster') error: %v", err)
	}
}

// ---------------------------------------------------------------------------
// SetLbContextProvider
// ---------------------------------------------------------------------------

func TestSetLbContextProvider_InjectsHashKey(t *testing.T) {
	c := mustNew(t)
	defer c.Close()
	mustWaitReady(t, c)

	providerCalled := false
	err := c.SetLbContextProvider(func(cluster string, ctx *envoyclient.RequestContext) {
		providerCalled = true
		ctx.HashKey = "injected-by-provider"
	})
	if err != nil {
		t.Fatalf("SetLbContextProvider() error: %v", err)
	}

	_, _ = c.PickEndpoint("test-cluster", nil)
	if !providerCalled {
		t.Error("LB context provider was not called during PickEndpoint")
	}
}

func TestSetLbContextProvider_InjectsOverrideHost(t *testing.T) {
	c := mustNew(t)
	defer c.Close()
	mustWaitReady(t, c)

	err := c.SetLbContextProvider(func(cluster string, ctx *envoyclient.RequestContext) {
		ctx.OverrideHost = "127.0.0.1:8080"
	})
	if err != nil {
		t.Fatalf("SetLbContextProvider() error: %v", err)
	}

	ep, err := c.PickEndpoint("test-cluster", nil)
	if err != nil {
		t.Fatalf("PickEndpoint() error: %v", err)
	}
	if ep != nil && ep.Port != 8080 {
		t.Errorf("expected port 8080 from override host, got %d", ep.Port)
	}
}

// ---------------------------------------------------------------------------
// Status error formatting
// ---------------------------------------------------------------------------

func TestStatusError_Formatting(t *testing.T) {
	cases := []struct {
		status envoyclient.Status
		substr string
	}{
		{envoyclient.StatusError, "error"},
		{envoyclient.StatusDenied, "denied"},
		{envoyclient.StatusUnavailable, "unavailable"},
		{envoyclient.StatusTimeout, "timeout"},
	}
	for _, tc := range cases {
		msg := tc.status.Error()
		if msg == "" {
			t.Errorf("Status(%d).Error() returned empty string", tc.status)
		}
	}
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

func mustNew(t *testing.T) *envoyclient.Client {
	t.Helper()
	c, err := envoyclient.New(minimalBootstrap)
	if err != nil {
		t.Fatalf("New() error: %v", err)
	}
	return c
}

func mustWaitReady(t *testing.T, c *envoyclient.Client) {
	t.Helper()
	if err := c.WaitReady(10); err != nil {
		t.Fatalf("WaitReady() error: %v", err)
	}
}
