package io.envoyproxy.envoyclient;

/**
 * Per-request metadata used to influence load balancing decisions.
 *
 * <p>All fields are optional. The zero/null value for each field means "use the
 * server-configured default".
 */
public final class RequestContext {
  private String path;
  private String authority;
  private String overrideHost;
  private boolean overrideHostStrict;
  private String hashKey;

  /** Request path for route matching. */
  public String getPath() { return path; }
  public RequestContext setPath(String path) { this.path = path; return this; }

  /** Value of the {@code :authority} header. */
  public String getAuthority() { return authority; }
  public RequestContext setAuthority(String authority) { this.authority = authority; return this; }

  /**
   * Override host — bypass LB and route to this endpoint directly
   * ("ip:port"). When {@link #isOverrideHostStrict()} is {@code false} the
   * library falls back to normal LB if the endpoint is unhealthy.
   */
  public String getOverrideHost() { return overrideHost; }
  public RequestContext setOverrideHost(String overrideHost) {
    this.overrideHost = overrideHost;
    return this;
  }

  /** When {@code true}, return UNAVAILABLE rather than fall back if the override host is unhealthy. */
  public boolean isOverrideHostStrict() { return overrideHostStrict; }
  public RequestContext setOverrideHostStrict(boolean strict) {
    this.overrideHostStrict = strict;
    return this;
  }

  /**
   * Explicit hash key for consistent-hashing LB policies (ring-hash, maglev).
   * Overrides the server-configured hash policy for this request.
   */
  public String getHashKey() { return hashKey; }
  public RequestContext setHashKey(String hashKey) { this.hashKey = hashKey; return this; }
}
