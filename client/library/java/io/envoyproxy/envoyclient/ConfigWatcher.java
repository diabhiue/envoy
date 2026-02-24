package io.envoyproxy.envoyclient;

/**
 * Callback interface for xDS config-change notifications.
 *
 * <p>Implementations must be non-blocking. Callbacks are invoked on the Envoy
 * dispatcher thread; use an executor to hand off to application threads if
 * further processing is needed.
 *
 * @see EnvoyClient#watchConfig(String, ConfigWatcher)
 */
@FunctionalInterface
public interface ConfigWatcher {
  /**
   * Called whenever an xDS resource matching the registered type changes.
   *
   * @param event describes what changed.
   */
  void onConfigEvent(ConfigEvent event);
}
