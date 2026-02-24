package io.envoyproxy.envoyclient;

/**
 * Callback invoked during every {@link EnvoyClient#pickEndpoint} call to let
 * the application enrich the LB context.
 *
 * <p>The callback receives the cluster name and a mutable {@link RequestContext}.
 * Any fields written to the context are used for the current LB decision.
 *
 * <p>Use cases:
 * <ul>
 *   <li>Inject session affinity keys from thread-local storage.
 *   <li>Add locality preferences based on runtime datacenter detection.
 *   <li>Override the target endpoint for canary or debugging.
 * </ul>
 *
 * <p>Implementations must be non-blocking and thread-safe; the callback is
 * invoked on whatever thread calls {@code pickEndpoint}.
 *
 * @see EnvoyClient#setLbContextProvider(LbContextProvider)
 */
@FunctionalInterface
public interface LbContextProvider {
  /**
   * Enrich {@code ctx} before the LB pick executes.
   *
   * @param clusterName the cluster being picked from.
   * @param ctx         mutable request context; set fields to influence the pick.
   */
  void provideLbContext(String clusterName, RequestContext ctx);
}
