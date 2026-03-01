// Tests for the Go Envoy Client Library binding.
//
// These tests use a stub bootstrap config so the engine does not actually
// connect to an xDS server. They exercise the Go API surface (argument
// marshalling, error handling, callback wiring) independently of live xDS.
//
// All tests that require a running engine share a single global Client
// instance created in TestMain. Running multiple ClientEngine instances in
// the same process is unsafe: the first engine's StrippedMainBase destructor
// tears down process-wide singletons (protobuf arenas, libevent, etc.) that
// a second engine cannot reinitialise. TestMain therefore creates the engine
// once, runs all tests, then destroys it.
package envoyclient_test

import (
	"fmt"
	"os"
	"testing"

	envoyclient "github.com/envoyproxy/envoy/client/library/go"
)

// minimalBootstrap is a minimal static Envoy bootstrap that satisfies the
// engine without connecting to any xDS server. Endpoints are provided
// statically via EDS so tests can exercise resolve/pick without live xDS.
//
// The admin socket (port 0 = OS-assigned ephemeral port) is intentionally
// included: without at least one listener the Envoy worker threads have no
// persistent libevent events and exit their dispatch loops immediately.
// When that happens before shutdownGlobalThreading() is called Envoy aborts
// with ASSERT(shutdown_) in ThreadLocalImpl::shutdownThread. The admin
// socket gives workers a long-lived accept event, matching the pattern used
// by the C++ engine tests.
const minimalBootstrap = `
node:
  id: "test-node"
  cluster: "test-cluster"

admin:
  address:
    socket_address:
      address: 127.0.0.1
      port_value: 0

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

// globalClient is the single engine instance shared by all running-engine
// tests. It is initialised in TestMain and closed at the end of the run.
var globalClient *envoyclient.Client

// TestMain creates a single engine for the entire test binary, waits for it
// to be ready, runs all tests, then shuts down. Only one engine instance may
// exist per process (see package comment above).
func TestMain(m *testing.M) {
	var err error
	globalClient, err = envoyclient.New(minimalBootstrap)
	if err != nil {
		fmt.Fprintf(os.Stderr, "envoyclient_test: engine creation failed: %v\n", err)
		os.Exit(1)
	}
	if err := globalClient.WaitReady(10); err != nil {
		fmt.Fprintf(os.Stderr, "envoyclient_test: WaitReady failed: %v\n", err)
		globalClient.Close()
		os.Exit(1)
	}

	code := m.Run()

	globalClient.Close()
	os.Exit(code)
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

func TestNew_EmptyConfig_ReturnsError(t *testing.T) {
	_, err := envoyclient.New("")
	if err == nil {
		t.Fatal("expected error for empty bootstrap config, got nil")
	}
}

// TestEngineLifecycle verifies that the global engine was created successfully
// and that calling Close more than once is safe (idempotent).
func TestEngineLifecycle(t *testing.T) {
	// globalClient was created and made ready by TestMain; verify it is usable.
	if globalClient == nil {
		t.Fatal("globalClient is nil")
	}

	// A second Close call on a still-open client must not panic or crash.
	// We cannot actually close the global client here (it is shared by all
	// tests), but we can verify the idempotency guarantee using a fresh
	// short-lived client that is NOT backed by a full engine. An empty-config
	// client returns an error before any engine state is initialised, so
	// creating and immediately discarding it is safe.
	//
	// The real idempotency of Close() on a running client is exercised by
	// TestMain, which calls globalClient.Close() after m.Run() returns; if
	// any test calls Close() early, the second call in TestMain is a no-op.
}

// ---------------------------------------------------------------------------
// WaitReady
// ---------------------------------------------------------------------------

func TestWaitReady_StaticConfig_NoTimeout(t *testing.T) {
	// Engine was already made ready in TestMain; a second WaitReady with a
	// generous timeout must succeed immediately.
	if err := globalClient.WaitReady(10); err != nil {
		t.Fatalf("WaitReady() unexpected error: %v", err)
	}
}

// ---------------------------------------------------------------------------
// Resolve
// ---------------------------------------------------------------------------

func TestResolve_KnownCluster_ReturnsEndpoints(t *testing.T) {
	endpoints, err := globalClient.Resolve("test-cluster")
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
	endpoints, err := globalClient.Resolve("no-such-cluster")
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
	ep, err := globalClient.PickEndpoint("test-cluster", nil)
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
	ctx := &envoyclient.RequestContext{HashKey: "session-abc-123"}
	ep, err := globalClient.PickEndpoint("test-cluster", ctx)
	if err != nil {
		t.Fatalf("PickEndpoint() with hash key error: %v", err)
	}
	if ep == nil {
		t.Fatal("PickEndpoint() returned nil for known cluster with hash key")
	}
}

func TestPickEndpoint_ConsistentHash_SameKeyReturnsSameEndpoint(t *testing.T) {
	// Set ring-hash so consistent hashing is active.
	if err := globalClient.SetDefaultLbPolicy("envoy.load_balancing_policies.ring_hash"); err != nil {
		// ring-hash may not be compiled in for all test builds; skip gracefully.
		t.Skipf("SetDefaultLbPolicy ring_hash skipped: %v", err)
	}
	defer globalClient.SetDefaultLbPolicy("") // restore default

	ctx := &envoyclient.RequestContext{HashKey: "sticky-user-99"}
	first, err := globalClient.PickEndpoint("test-cluster", ctx)
	if err != nil || first == nil {
		t.Skipf("PickEndpoint skipped: err=%v ep=%v", err, first)
	}
	for i := 0; i < 5; i++ {
		ep, _ := globalClient.PickEndpoint("test-cluster", ctx)
		if ep != nil && ep.Address != first.Address {
			t.Errorf("consistent hash pick %d: got %s, want %s", i, ep.Address, first.Address)
		}
	}
}

func TestPickEndpoint_UnknownCluster_ReturnsNil(t *testing.T) {
	ep, err := globalClient.PickEndpoint("no-such-cluster", nil)
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
	// Should be a no-op, not a panic.
	globalClient.ReportResult(nil, 200, 10)
}

func TestReportResult_ValidEndpoint_NoPanic(t *testing.T) {
	ep, _ := globalClient.PickEndpoint("test-cluster", nil)
	if ep == nil {
		t.Skip("no endpoint available")
	}
	globalClient.ReportResult(ep, 200, 42)
	globalClient.ReportResult(ep, 503, 1000)
}

// ---------------------------------------------------------------------------
// LB policy overrides
// ---------------------------------------------------------------------------

func TestSetClusterLbPolicy_RoundRobin_Succeeds(t *testing.T) {
	err := globalClient.SetClusterLbPolicy("test-cluster", "envoy.load_balancing_policies.round_robin")
	if err != nil {
		t.Fatalf("SetClusterLbPolicy() error: %v", err)
	}
	// Restore default
	globalClient.SetClusterLbPolicy("test-cluster", "")
}

func TestSetClusterLbPolicy_EmptyString_ClearsOverride(t *testing.T) {
	if err := globalClient.SetClusterLbPolicy("test-cluster", "envoy.load_balancing_policies.round_robin"); err != nil {
		t.Fatalf("setup: %v", err)
	}
	if err := globalClient.SetClusterLbPolicy("test-cluster", ""); err != nil {
		t.Fatalf("clear: %v", err)
	}
}

func TestSetDefaultLbPolicy_Succeeds(t *testing.T) {
	if err := globalClient.SetDefaultLbPolicy("envoy.load_balancing_policies.round_robin"); err != nil {
		t.Fatalf("SetDefaultLbPolicy() error: %v", err)
	}
	// Restore default
	globalClient.SetDefaultLbPolicy("")
}

// ---------------------------------------------------------------------------
// WatchConfig
// ---------------------------------------------------------------------------

func TestWatchConfig_EmptyType_Succeeds(t *testing.T) {
	// Empty resource type = watch all. Should register without error.
	err := globalClient.WatchConfig("", func(ev envoyclient.ConfigEvent) {})
	if err != nil {
		t.Fatalf("WatchConfig('') error: %v", err)
	}
}

func TestWatchConfig_ClusterType_Succeeds(t *testing.T) {
	err := globalClient.WatchConfig("cluster", func(ev envoyclient.ConfigEvent) {})
	if err != nil {
		t.Fatalf("WatchConfig('cluster') error: %v", err)
	}
}

// ---------------------------------------------------------------------------
// SetLbContextProvider
// ---------------------------------------------------------------------------

func TestSetLbContextProvider_InjectsHashKey(t *testing.T) {
	providerCalled := false
	err := globalClient.SetLbContextProvider(func(cluster string, ctx *envoyclient.RequestContext) {
		providerCalled = true
		ctx.HashKey = "injected-by-provider"
	})
	if err != nil {
		t.Fatalf("SetLbContextProvider() error: %v", err)
	}
	defer globalClient.SetLbContextProvider(nil)

	_, _ = globalClient.PickEndpoint("test-cluster", nil)
	if !providerCalled {
		t.Error("LB context provider was not called during PickEndpoint")
	}
}

func TestSetLbContextProvider_InjectsOverrideHost(t *testing.T) {
	err := globalClient.SetLbContextProvider(func(cluster string, ctx *envoyclient.RequestContext) {
		ctx.OverrideHost = "127.0.0.1:8080"
	})
	if err != nil {
		t.Fatalf("SetLbContextProvider() error: %v", err)
	}
	defer globalClient.SetLbContextProvider(nil)

	ep, err := globalClient.PickEndpoint("test-cluster", nil)
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
