package io.envoyproxy.envoyclient;

import java.util.Map;

/**
 * Lightweight hook that runs around the server-enforced filter chain.
 *
 * <p>Interceptors are registered via {@link EnvoyClient#addInterceptor} and
 * execute in registration order. With the default {@code CLIENT_WRAPS_SERVER}
 * merge policy:
 * <ol>
 *   <li>All interceptors in {@link InterceptorPhase#PRE_REQUEST}</li>
 *   <li>Server filter chain (header mutation, credential injection, policy checks)</li>
 *   <li>All interceptors in {@link InterceptorPhase#POST_REQUEST}</li>
 * </ol>
 *
 * <p>The headers map passed to {@link #onHeaders} is a mutable copy. Changes
 * made to it are visible to subsequent interceptors and the filter chain.
 *
 * <p>Implementations must be thread-safe and non-blocking.
 */
@FunctionalInterface
public interface Interceptor {

  /**
   * Called once per phase during filter application.
   *
   * @param headers   mutable view of the current request/response headers.
   *                  Add, modify, or remove entries as needed.
   * @param cluster   name of the cluster this request targets.
   * @param phase     which phase is currently executing.
   * @return {@code true} to continue processing; {@code false} to deny the
   *         request (causes {@link EnvoyClient#applyRequestFilters} to throw
   *         an {@link EnvoyClientException} with status DENIED).
   */
  boolean onHeaders(Map<String, String> headers, String cluster, InterceptorPhase phase);
}
