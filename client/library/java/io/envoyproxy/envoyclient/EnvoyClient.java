package io.envoyproxy.envoyclient;

import java.io.Closeable;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Main entry point for the Envoy Client Library.
 *
 * <p>The library provides xDS-driven endpoint resolution and load balancing
 * without owning the data path. The application makes its own connections;
 * the library answers "where to send" using server-pushed CDS/EDS configuration
 * and optional client-side overrides.
 *
 * <h2>Quick start</h2>
 * <pre>{@code
 * try (EnvoyClient client = EnvoyClient.create(bootstrapYaml)) {
 *     client.waitReady(30);
 *     Endpoint ep = client.pickEndpoint("my-cluster", null);
 *     // Dial ep.getAddress():ep.getPort() with your gRPC/HTTP client
 * }
 * }</pre>
 *
 * <p>Instances are safe for concurrent use from multiple threads.
 */
public final class EnvoyClient implements Closeable {

  static {
    System.loadLibrary("envoy_client_jni");
  }

  // Native handle — a C pointer cast to a Java long.
  private final long nativeHandle;
  private final AtomicBoolean closed = new AtomicBoolean(false);

  private EnvoyClient(long nativeHandle) {
    this.nativeHandle = nativeHandle;
  }

  // ---------------------------------------------------------------------------
  // Factory
  // ---------------------------------------------------------------------------

  /**
   * Creates a client from a bootstrap YAML string and starts the engine.
   *
   * @param bootstrapYaml Envoy bootstrap configuration in YAML format.
   * @return a new client instance.
   * @throws EnvoyClientException if the engine could not be created (e.g.
   *         malformed bootstrap config).
   */
  public static EnvoyClient create(String bootstrapYaml) {
    if (bootstrapYaml == null || bootstrapYaml.isEmpty()) {
      throw new IllegalArgumentException("bootstrapYaml must not be null or empty");
    }
    byte[] bytes = bootstrapYaml.getBytes(StandardCharsets.UTF_8);
    long handle = nativeCreate(bytes);
    if (handle == 0) {
      throw new EnvoyClientException("Failed to create Envoy client engine (check bootstrap config)");
    }
    return new EnvoyClient(handle);
  }

  // ---------------------------------------------------------------------------
  // Lifecycle
  // ---------------------------------------------------------------------------

  /**
   * Blocks until the engine has received its initial xDS configuration or until
   * {@code timeoutSeconds} elapses.
   *
   * @param timeoutSeconds maximum wait in seconds.
   * @throws EnvoyClientException if the wait times out or the engine reports an error.
   */
  public void waitReady(int timeoutSeconds) {
    int status = nativeWaitReady(nativeHandle, timeoutSeconds);
    if (status == 4 /* TIMEOUT */) {
      throw new EnvoyClientException("Timed out waiting for initial xDS config");
    }
    if (status != 0 /* OK */) {
      throw new EnvoyClientException("waitReady failed with status " + status);
    }
  }

  /**
   * Shuts down the engine and releases all native resources. After this call
   * the client must not be used.
   */
  @Override
  public void close() {
    if (closed.compareAndSet(false, true)) {
      nativeDestroy(nativeHandle);
    }
  }

  // ---------------------------------------------------------------------------
  // Endpoint resolution
  // ---------------------------------------------------------------------------

  /**
   * Returns all known endpoints for {@code clusterName}.
   *
   * @return list of endpoints; empty if the cluster has no healthy endpoints or
   *         is not yet known.
   */
  public List<Endpoint> resolve(String clusterName) {
    if (clusterName == null) throw new IllegalArgumentException("clusterName must not be null");
    Endpoint[] endpoints = nativeResolve(nativeHandle, clusterName);
    if (endpoints == null || endpoints.length == 0) {
      return Collections.emptyList();
    }
    List<Endpoint> result = new ArrayList<>(endpoints.length);
    Collections.addAll(result, endpoints);
    return Collections.unmodifiableList(result);
  }

  /**
   * Selects a single endpoint for {@code clusterName} using the configured LB
   * policy.
   *
   * @param clusterName the cluster to pick from.
   * @param ctx         optional per-request LB context; may be {@code null}.
   * @return the selected endpoint, or {@code null} if no endpoints are available.
   */
  public Endpoint pickEndpoint(String clusterName, RequestContext ctx) {
    if (clusterName == null) throw new IllegalArgumentException("clusterName must not be null");
    return nativePickEndpoint(nativeHandle, clusterName, ctx);
  }

  // ---------------------------------------------------------------------------
  // LB feedback
  // ---------------------------------------------------------------------------

  /**
   * Reports the outcome of a completed request for feedback-driven LB (e.g.
   * least-request, outlier detection).
   *
   * @param endpoint   the endpoint that handled the request.
   * @param statusCode HTTP response status code; pass 0 for connection failures.
   * @param latencyMs  end-to-end request latency in milliseconds.
   */
  public void reportResult(Endpoint endpoint, int statusCode, long latencyMs) {
    if (endpoint == null) return;
    nativeReportResult(nativeHandle, endpoint.getAddress(), endpoint.getPort(),
        statusCode, latencyMs);
  }

  // ---------------------------------------------------------------------------
  // LB policy overrides
  // ---------------------------------------------------------------------------

  /**
   * Overrides the LB policy for a specific cluster.
   *
   * @param clusterName  the cluster to configure.
   * @param lbPolicyName Envoy LB policy name (e.g.
   *                     {@code "envoy.load_balancing_policies.round_robin"}).
   *                     Pass {@code null} or empty string to clear the override.
   * @throws EnvoyClientException on failure.
   */
  public void setClusterLbPolicy(String clusterName, String lbPolicyName) {
    if (clusterName == null) throw new IllegalArgumentException("clusterName must not be null");
    int status = nativeSetClusterLbPolicy(nativeHandle, clusterName,
        lbPolicyName != null ? lbPolicyName : "");
    if (status != 0) throw new EnvoyClientException("setClusterLbPolicy failed: status " + status);
  }

  /**
   * Sets the default LB policy override applied to all clusters that do not
   * have a per-cluster override.
   *
   * @param lbPolicyName Envoy LB policy name, or {@code null}/empty to clear.
   * @throws EnvoyClientException on failure.
   */
  public void setDefaultLbPolicy(String lbPolicyName) {
    int status = nativeSetDefaultLbPolicy(nativeHandle,
        lbPolicyName != null ? lbPolicyName : "");
    if (status != 0) throw new EnvoyClientException("setDefaultLbPolicy failed: status " + status);
  }

  // ---------------------------------------------------------------------------
  // Config watch
  // ---------------------------------------------------------------------------

  /**
   * Registers a callback that is invoked on every xDS config change.
   *
   * @param resourceType resource kind to watch: {@code "cluster"},
   *                     {@code "endpoint"}, {@code "route"}, or
   *                     {@code "listener"}. Pass {@code null} to watch all
   *                     types.
   * @param watcher      callback; must be non-blocking.
   * @throws EnvoyClientException on failure.
   */
  public void watchConfig(String resourceType, ConfigWatcher watcher) {
    if (watcher == null) throw new IllegalArgumentException("watcher must not be null");
    int status = nativeWatchConfig(nativeHandle,
        resourceType != null ? resourceType : "", watcher);
    if (status != 0) throw new EnvoyClientException("watchConfig failed: status " + status);
  }

  // ---------------------------------------------------------------------------
  // LB context provider
  // ---------------------------------------------------------------------------

  /**
   * Registers a callback invoked during every {@link #pickEndpoint} call.
   *
   * <p>The callback may modify the {@link RequestContext} to inject a hash key,
   * override host, or other LB hints derived from application-level state (e.g.
   * session ID from a thread-local).
   *
   * <p>Only one provider is active at a time; calling this method again replaces
   * the previous one.
   *
   * @param provider the LB context provider; must be non-blocking and thread-safe.
   * @throws EnvoyClientException on failure.
   */
  public void setLbContextProvider(LbContextProvider provider) {
    if (provider == null) throw new IllegalArgumentException("provider must not be null");
    int status = nativeSetLbContextProvider(nativeHandle, provider);
    if (status != 0) {
      throw new EnvoyClientException("setLbContextProvider failed: status " + status);
    }
  }

  // ---------------------------------------------------------------------------
  // Filter chain (Phase 2)
  // ---------------------------------------------------------------------------

  /**
   * Applies client interceptors and the server-pushed filter chain to request
   * headers. The execution order (CLIENT_WRAPS_SERVER) is:
   * <ol>
   *   <li>Interceptors in PRE_REQUEST phase</li>
   *   <li>Server filter chain (pass-through in Phase 2)</li>
   *   <li>Interceptors in POST_REQUEST phase</li>
   * </ol>
   *
   * @param clusterName the cluster this request targets.
   * @param headers     the request headers to process.
   * @return the (possibly modified) headers after all filters have run.
   * @throws EnvoyClientException if a filter or interceptor denies the request.
   */
  public Map<String, String> applyRequestFilters(String clusterName, Map<String, String> headers) {
    if (clusterName == null) throw new IllegalArgumentException("clusterName must not be null");
    String[] result = nativeApplyRequestFilters(nativeHandle, clusterName, mapToArray(headers));
    if (result == null) throw new EnvoyClientException("Request denied by filter chain");
    return arrayToMap(result);
  }

  /**
   * Applies client interceptors and the server-pushed filter chain to response
   * headers. Same semantics as {@link #applyRequestFilters}.
   */
  public Map<String, String> applyResponseFilters(String clusterName, Map<String, String> headers) {
    if (clusterName == null) throw new IllegalArgumentException("clusterName must not be null");
    String[] result = nativeApplyResponseFilters(nativeHandle, clusterName, mapToArray(headers));
    if (result == null) throw new EnvoyClientException("Response denied by filter chain");
    return arrayToMap(result);
  }

  /**
   * Registers a named interceptor. Interceptors execute in registration order.
   *
   * @param name        unique name; throws if already registered.
   * @param interceptor the callback; must be non-blocking and thread-safe.
   * @throws EnvoyClientException if {@code name} is already registered.
   */
  public void addInterceptor(String name, Interceptor interceptor) {
    if (name == null) throw new IllegalArgumentException("name must not be null");
    if (interceptor == null) throw new IllegalArgumentException("interceptor must not be null");
    int status = nativeAddInterceptor(nativeHandle, name, interceptor);
    if (status != 0) throw new EnvoyClientException("addInterceptor failed: status " + status);
  }

  /**
   * Removes a previously registered interceptor by name.
   *
   * @throws EnvoyClientException if no interceptor with that name exists.
   */
  public void removeInterceptor(String name) {
    if (name == null) throw new IllegalArgumentException("name must not be null");
    int status = nativeRemoveInterceptor(nativeHandle, name);
    if (status != 0) throw new EnvoyClientException("removeInterceptor failed: status " + status);
  }

  // ---------------------------------------------------------------------------
  // Internal: header map ↔ flat String[] conversion for JNI
  // ---------------------------------------------------------------------------

  /** Converts a Map<String,String> to a flat [k0, v0, k1, v1, ...] String array. */
  private static String[] mapToArray(Map<String, String> m) {
    if (m == null || m.isEmpty()) return new String[0];
    String[] arr = new String[m.size() * 2];
    int i = 0;
    for (Map.Entry<String, String> e : m.entrySet()) {
      arr[i++] = e.getKey();
      arr[i++] = e.getValue() != null ? e.getValue() : "";
    }
    return arr;
  }

  /** Converts a flat [k0, v0, k1, v1, ...] String array back to a LinkedHashMap. */
  private static Map<String, String> arrayToMap(String[] arr) {
    if (arr == null || arr.length == 0) return Collections.emptyMap();
    Map<String, String> result = new LinkedHashMap<>(arr.length / 2);
    for (int i = 0; i + 1 < arr.length; i += 2) {
      result.put(arr[i], arr[i + 1]);
    }
    return Collections.unmodifiableMap(result);
  }

  // ---------------------------------------------------------------------------
  // JNI declarations
  // ---------------------------------------------------------------------------

  /** Creates the native engine. Returns a non-zero handle or 0 on failure. */
  private static native long nativeCreate(byte[] bootstrapConfig);

  /** Waits for the engine to be ready. Returns a C status code. */
  private static native int nativeWaitReady(long handle, int timeoutSeconds);

  /** Shuts down the native engine and frees memory. */
  private static native void nativeDestroy(long handle);

  /** Resolves all endpoints for the cluster. */
  private static native Endpoint[] nativeResolve(long handle, String clusterName);

  /**
   * Picks a single endpoint. The {@link RequestContext} is passed by value; the
   * JNI layer reads its fields and builds the C struct.
   */
  private static native Endpoint nativePickEndpoint(long handle, String clusterName,
      RequestContext ctx);

  /** Reports a request outcome. */
  private static native void nativeReportResult(long handle, String address, int port,
      int statusCode, long latencyMs);

  /** Sets a per-cluster LB policy override. Returns a C status code. */
  private static native int nativeSetClusterLbPolicy(long handle, String clusterName,
      String lbPolicyName);

  /** Sets the default LB policy override. Returns a C status code. */
  private static native int nativeSetDefaultLbPolicy(long handle, String lbPolicyName);

  /**
   * Registers a config watcher. The JNI layer stores a global ref to
   * {@code watcher} and invokes {@link ConfigWatcher#onConfigEvent} from the C
   * callback. Returns a C status code.
   */
  private static native int nativeWatchConfig(long handle, String resourceType,
      ConfigWatcher watcher);

  /**
   * Registers an LB context provider. The JNI layer stores a global ref to
   * {@code provider} and invokes
   * {@link LbContextProvider#provideLbContext(String, RequestContext)} from the
   * C callback. Returns a C status code.
   */
  private static native int nativeSetLbContextProvider(long handle, LbContextProvider provider);

  /**
   * Applies request filters. Passes headers as flat [k,v,...] array; returns
   * modified headers in the same format. Returns null on DENIED.
   */
  private static native String[] nativeApplyRequestFilters(long handle, String clusterName,
      String[] headers);

  /**
   * Applies response filters. Same format as nativeApplyRequestFilters.
   */
  private static native String[] nativeApplyResponseFilters(long handle, String clusterName,
      String[] headers);

  /**
   * Registers a named interceptor. The JNI layer stores a global ref and
   * invokes {@link Interceptor#onHeaders} from the C callback. Returns a C
   * status code.
   */
  private static native int nativeAddInterceptor(long handle, String name, Interceptor interceptor);

  /**
   * Removes a named interceptor. Returns a C status code.
   */
  private static native int nativeRemoveInterceptor(long handle, String name);
}
