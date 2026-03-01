// Package envoyclient provides a Go binding for the Envoy Client Library.
//
// The library gives any Go application xDS-driven endpoint resolution and
// load balancing without owning the data path. The application makes its
// own connections; the library answers "where to send" using server-pushed
// xDS configuration (CDS/EDS) and optional client-side overrides.
//
// Quick start:
//
//	client, err := envoyclient.New(bootstrapYAML)
//	if err != nil { log.Fatal(err) }
//	defer client.Close()
//
//	if err := client.WaitReady(30); err != nil { log.Fatal(err) }
//
//	ep, err := client.PickEndpoint("my-cluster", nil)
//	// dial ep.Address:ep.Port with your own gRPC/HTTP client
package envoyclient

/*
#cgo CFLAGS: -I${SRCDIR}/../../../
#include "client/library/c_api/envoy_client.h"
#include <stdlib.h>

// Trampolines are defined in callbacks.c (CGo requires that preambles contain
// only declarations, not definitions, when //export is used).
extern void configCBTrampoline(const char* rt, const char* rn,
                               envoy_client_config_event event, void* ctx);
extern void lbContextCBTrampoline(const char* cluster,
                                  envoy_client_request_context* ctx,
                                  void* userCtx);
*/
import "C"

import (
	"errors"
	"fmt"
	"sync"
	"unsafe"
)

// ---------------------------------------------------------------------------
// Public types
// ---------------------------------------------------------------------------

// Status is a result code returned by client operations.
type Status int

const (
	StatusOK          Status = 0
	StatusError       Status = 1
	StatusDenied      Status = 2
	StatusUnavailable Status = 3
	StatusTimeout     Status = 4
)

func (s Status) Error() string {
	switch s {
	case StatusError:
		return "envoyclient: error"
	case StatusDenied:
		return "envoyclient: request denied by filter"
	case StatusUnavailable:
		return "envoyclient: no endpoints available"
	case StatusTimeout:
		return "envoyclient: timeout waiting for xDS config"
	default:
		return fmt.Sprintf("envoyclient: unknown status %d", int(s))
	}
}

// Endpoint holds information about a single upstream endpoint as received
// from EDS.
type Endpoint struct {
	Address      string
	Port         uint32
	Weight       uint32
	Priority     uint32
	HealthStatus uint32 // 0=unknown 1=healthy 2=degraded 3=unhealthy
}

// RequestContext carries optional per-request metadata used to influence the
// LB decision. All fields are optional; zero value means "use defaults".
type RequestContext struct {
	// Path is the request path used for route matching.
	Path string
	// Authority is the :authority header value.
	Authority string

	// OverrideHost pins the request to a specific endpoint ("ip:port").
	// When OverrideHostStrict is false the library falls back to normal LB if
	// the pinned endpoint is unhealthy.
	OverrideHost       string
	OverrideHostStrict bool

	// HashKey provides an explicit hash for consistent-hashing policies
	// (ring-hash, maglev). Overrides the server-configured hash policy.
	HashKey string
}

// ConfigEventType classifies an xDS resource change.
type ConfigEventType int

const (
	ConfigAdded   ConfigEventType = 0
	ConfigUpdated ConfigEventType = 1
	ConfigRemoved ConfigEventType = 2
)

// ConfigEvent is delivered to WatchConfig callbacks when an xDS resource
// changes.
type ConfigEvent struct {
	// ResourceType is one of "cluster", "endpoint", "route", "listener".
	ResourceType string
	ResourceName string
	EventType    ConfigEventType
}

// ---------------------------------------------------------------------------
// Internal callback registry
//
// CGo cannot store Go function values in C memory because the GC may move
// them. Instead we assign each callback an integer key and pass that key as
// the void* context pointer. The C trampolines below look up the key in the
// appropriate map.
// ---------------------------------------------------------------------------

var (
	callbackMu sync.RWMutex

	configCallbacks   = make(map[uintptr]func(ConfigEvent))
	configCallbackSeq uintptr

	lbCallbacks   = make(map[uintptr]func(string, *RequestContext))
	lbCallbackSeq uintptr
)

func registerConfigCallback(cb func(ConfigEvent)) uintptr {
	callbackMu.Lock()
	defer callbackMu.Unlock()
	configCallbackSeq++
	id := configCallbackSeq
	configCallbacks[id] = cb
	return id
}

func unregisterConfigCallback(id uintptr) {
	callbackMu.Lock()
	defer callbackMu.Unlock()
	delete(configCallbacks, id)
}

func registerLbCallback(cb func(string, *RequestContext)) uintptr {
	callbackMu.Lock()
	defer callbackMu.Unlock()
	lbCallbackSeq++
	id := lbCallbackSeq
	lbCallbacks[id] = cb
	return id
}

func unregisterLbCallback(id uintptr) {
	callbackMu.Lock()
	defer callbackMu.Unlock()
	delete(lbCallbacks, id)
}

// ---------------------------------------------------------------------------
// C trampolines (exported so CGo can produce C function pointers)
// ---------------------------------------------------------------------------

// goConfigCB is called by the native library on every xDS config change.
//
//export goConfigCB
func goConfigCB(resourceType *C.char, resourceName *C.char,
	event C.envoy_client_config_event, ctx unsafe.Pointer) {

	id := uintptr(ctx)
	callbackMu.RLock()
	cb := configCallbacks[id]
	callbackMu.RUnlock()
	if cb == nil {
		return
	}

	var et ConfigEventType
	switch event {
	case C.ENVOY_CLIENT_CONFIG_ADDED:
		et = ConfigAdded
	case C.ENVOY_CLIENT_CONFIG_UPDATED:
		et = ConfigUpdated
	default:
		et = ConfigRemoved
	}
	cb(ConfigEvent{
		ResourceType: C.GoString(resourceType),
		ResourceName: C.GoString(resourceName),
		EventType:    et,
	})
}

// goLbContextCB is called by the native library during every pick_endpoint so
// the application can enrich the LB context (e.g. inject a session affinity
// key from thread-local storage).
//
//export goLbContextCB
func goLbContextCB(clusterName *C.char, ctx *C.envoy_client_request_context,
	userCtx unsafe.Pointer) {

	id := uintptr(userCtx)
	callbackMu.RLock()
	cb := lbCallbacks[id]
	callbackMu.RUnlock()
	if cb == nil || ctx == nil {
		return
	}

	// Populate a Go RequestContext from the C struct so the callback can read
	// and modify it.
	rctx := &RequestContext{}
	if ctx.path != nil {
		rctx.Path = C.GoString(ctx.path)
	}
	if ctx.authority != nil {
		rctx.Authority = C.GoString(ctx.authority)
	}
	if ctx.override_host != nil {
		rctx.OverrideHost = C.GoString(ctx.override_host)
	}
	rctx.OverrideHostStrict = ctx.override_host_strict != 0
	if ctx.hash_key != nil && ctx.hash_key_len > 0 {
		rctx.HashKey = C.GoStringN(ctx.hash_key, C.int(ctx.hash_key_len))
	}

	cb(C.GoString(clusterName), rctx)

	// Write mutations back to the C struct. Newly allocated C strings are
	// transferred to the C caller, which will free them after copying.
	// Do NOT use defer C.free here: the C code reads these pointers after
	// this function returns, so freeing them with defer would be a
	// use-after-free (deferred cleanup runs before the C caller reads back).
	if rctx.OverrideHost != "" {
		ctx.override_host = C.CString(rctx.OverrideHost)
	}
	if rctx.OverrideHostStrict {
		ctx.override_host_strict = 1
	} else {
		ctx.override_host_strict = 0
	}
	if rctx.HashKey != "" {
		cs := C.CString(rctx.HashKey)
		ctx.hash_key = cs
		ctx.hash_key_len = C.size_t(len(rctx.HashKey))
	}
}

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------

// Client is the main entry point for the Envoy Client Library.
//
// Instances are safe for concurrent use from multiple goroutines.
type Client struct {
	handle C.envoy_client_handle
}

// New creates a Client from a bootstrap YAML string.
// The engine starts immediately; call WaitReady before using the client.
func New(bootstrapYAML string) (*Client, error) {
	if bootstrapYAML == "" {
		return nil, errors.New("envoyclient: bootstrap YAML must not be empty")
	}
	cs := C.CString(bootstrapYAML)
	defer C.free(unsafe.Pointer(cs))

	handle := C.envoy_client_create(cs, C.size_t(len(bootstrapYAML)))
	if handle == nil {
		return nil, errors.New("envoyclient: failed to create engine (check bootstrap config)")
	}
	return &Client{handle: handle}, nil
}

// WaitReady blocks until the engine has received its initial xDS configuration
// or timeoutSeconds elapses.
func (c *Client) WaitReady(timeoutSeconds int) error {
	status := C.envoy_client_wait_ready(c.handle, C.uint32_t(timeoutSeconds))
	switch status {
	case C.ENVOY_CLIENT_OK:
		return nil
	case C.ENVOY_CLIENT_TIMEOUT:
		return StatusTimeout
	default:
		return StatusError
	}
}

// Close shuts down the engine and releases all native resources. The Client
// must not be used after Close returns.
func (c *Client) Close() {
	if c.handle != nil {
		C.envoy_client_destroy(c.handle)
		c.handle = nil
	}
}

// Resolve returns all known endpoints for clusterName. Returns nil, nil if
// the cluster exists but has no healthy endpoints, or if the cluster is
// unknown.
func (c *Client) Resolve(clusterName string) ([]Endpoint, error) {
	cs := C.CString(clusterName)
	defer C.free(unsafe.Pointer(cs))

	var list C.envoy_client_endpoint_list
	status := C.envoy_client_resolve(c.handle, cs, &list)
	switch status {
	case C.ENVOY_CLIENT_UNAVAILABLE:
		return nil, nil
	case C.ENVOY_CLIENT_OK:
		// handled below
	default:
		return nil, StatusError
	}
	defer C.envoy_client_free_endpoints(&list)

	n := int(list.count)
	if n == 0 {
		return nil, nil
	}
	// Cast the C array into a Go slice (read-only view, no copy needed until
	// we extract string fields).
	cSlice := (*[1 << 20]C.envoy_client_endpoint)(unsafe.Pointer(list.endpoints))[:n:n]
	endpoints := make([]Endpoint, n)
	for i, ce := range cSlice {
		endpoints[i] = Endpoint{
			Address:      C.GoString(ce.address),
			Port:         uint32(ce.port),
			Weight:       uint32(ce.weight),
			Priority:     uint32(ce.priority),
			HealthStatus: uint32(ce.health_status),
		}
	}
	return endpoints, nil
}

// PickEndpoint selects a single endpoint for clusterName using the
// server-configured (or overridden) LB policy. ctx may be nil.
//
// Returns nil, nil when no endpoints are available.
func (c *Client) PickEndpoint(clusterName string, ctx *RequestContext) (*Endpoint, error) {
	cs := C.CString(clusterName)
	defer C.free(unsafe.Pointer(cs))

	// Build the C request-context struct and keep C strings alive for the call.
	var (
		cctxPtr  *C.envoy_client_request_context
		cctxVal  C.envoy_client_request_context
		csPath, csAuth, csOverride, csHash *C.char
	)
	if ctx != nil {
		if ctx.Path != "" {
			csPath = C.CString(ctx.Path)
			defer C.free(unsafe.Pointer(csPath))
			cctxVal.path = csPath
		}
		if ctx.Authority != "" {
			csAuth = C.CString(ctx.Authority)
			defer C.free(unsafe.Pointer(csAuth))
			cctxVal.authority = csAuth
		}
		if ctx.OverrideHost != "" {
			csOverride = C.CString(ctx.OverrideHost)
			defer C.free(unsafe.Pointer(csOverride))
			cctxVal.override_host = csOverride
		}
		if ctx.OverrideHostStrict {
			cctxVal.override_host_strict = 1
		}
		if ctx.HashKey != "" {
			csHash = C.CString(ctx.HashKey)
			defer C.free(unsafe.Pointer(csHash))
			cctxVal.hash_key = csHash
			cctxVal.hash_key_len = C.size_t(len(ctx.HashKey))
		}
		cctxPtr = &cctxVal
	}

	var out C.envoy_client_endpoint
	status := C.envoy_client_pick_endpoint(c.handle, cs, cctxPtr, &out)
	switch status {
	case C.ENVOY_CLIENT_UNAVAILABLE:
		return nil, nil
	case C.ENVOY_CLIENT_OK:
		// handled below
	default:
		return nil, StatusError
	}

	ep := &Endpoint{
		Address:      C.GoString(out.address),
		Port:         uint32(out.port),
		Weight:       uint32(out.weight),
		Priority:     uint32(out.priority),
		HealthStatus: uint32(out.health_status),
	}
	// The C layer allocates the address with new[]; free it.
	C.free(unsafe.Pointer(out.address))
	return ep, nil
}

// ReportResult records the outcome of a completed request for feedback-driven
// LB (e.g. least-request). statusCode is the HTTP response status code; pass
// 0 for connection failures. latencyMs is the end-to-end request latency.
func (c *Client) ReportResult(ep *Endpoint, statusCode uint32, latencyMs uint64) {
	if ep == nil {
		return
	}
	csAddr := C.CString(ep.Address)
	defer C.free(unsafe.Pointer(csAddr))

	cep := C.envoy_client_endpoint{
		address:       csAddr,
		port:          C.uint32_t(ep.Port),
		weight:        C.uint32_t(ep.Weight),
		priority:      C.uint32_t(ep.Priority),
		health_status: C.uint32_t(ep.HealthStatus),
	}
	C.envoy_client_report_result(c.handle, &cep, C.uint32_t(statusCode), C.uint64_t(latencyMs))
}

// SetClusterLbPolicy overrides the LB policy for a specific cluster. Pass an
// empty string to clear the override and revert to the server-configured policy.
func (c *Client) SetClusterLbPolicy(clusterName, lbPolicy string) error {
	csCluster := C.CString(clusterName)
	defer C.free(unsafe.Pointer(csCluster))
	csPolicy := C.CString(lbPolicy)
	defer C.free(unsafe.Pointer(csPolicy))

	if status := C.envoy_client_set_cluster_lb_policy(c.handle, csCluster, csPolicy); status != C.ENVOY_CLIENT_OK {
		return StatusError
	}
	return nil
}

// SetDefaultLbPolicy sets the fallback LB policy used when no per-cluster
// override is set. Pass an empty string to clear.
func (c *Client) SetDefaultLbPolicy(lbPolicy string) error {
	csPolicy := C.CString(lbPolicy)
	defer C.free(unsafe.Pointer(csPolicy))

	if status := C.envoy_client_set_default_lb_policy(c.handle, csPolicy); status != C.ENVOY_CLIENT_OK {
		return StatusError
	}
	return nil
}

// WatchConfig registers cb to be invoked on every xDS resource change.
// resourceType filters events to a specific resource kind ("cluster",
// "endpoint", "route", "listener"). An empty string watches all types.
//
// Callbacks are invoked on the Envoy dispatcher thread; do not perform
// blocking operations inside cb.
func (c *Client) WatchConfig(resourceType string, cb func(ConfigEvent)) error {
	id := registerConfigCallback(cb)

	var csType *C.char
	if resourceType != "" {
		csType = C.CString(resourceType)
		defer C.free(unsafe.Pointer(csType))
	}

	status := C.envoy_client_watch_config(
		c.handle,
		csType,
		C.envoy_client_config_cb(C.configCBTrampoline),
		unsafe.Pointer(id),
	)
	if status != C.ENVOY_CLIENT_OK {
		unregisterConfigCallback(id)
		return StatusError
	}
	return nil
}

// SetLbContextProvider registers cb to be invoked during every PickEndpoint
// call. The callback receives the cluster name and a mutable RequestContext;
// any fields set by the callback are used for the LB decision.
//
// Use cases: inject session affinity keys, add locality preferences, exclude
// endpoints based on application-level circuit-breaker state.
//
// Only one provider may be active at a time; calling SetLbContextProvider
// again replaces the previous one.
func (c *Client) SetLbContextProvider(cb func(cluster string, ctx *RequestContext)) error {
	if cb == nil {
		// Clear the provider.
		status := C.envoy_client_set_lb_context_provider(c.handle, nil, nil)
		if status != C.ENVOY_CLIENT_OK {
			return StatusError
		}
		return nil
	}

	id := registerLbCallback(cb)

	status := C.envoy_client_set_lb_context_provider(
		c.handle,
		C.envoy_client_lb_context_cb(C.lbContextCBTrampoline),
		unsafe.Pointer(id),
	)
	if status != C.ENVOY_CLIENT_OK {
		unregisterLbCallback(id)
		return StatusError
	}
	return nil
}
