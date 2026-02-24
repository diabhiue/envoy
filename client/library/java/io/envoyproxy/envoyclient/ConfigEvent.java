package io.envoyproxy.envoyclient;

/**
 * An xDS resource change event delivered to {@link ConfigWatcher} callbacks.
 */
public final class ConfigEvent {
  /** Resource kinds reported in {@link #getResourceType()}. */
  public enum Type {
    ADDED, UPDATED, REMOVED
  }

  private final String resourceType;
  private final String resourceName;
  private final Type type;

  public ConfigEvent(String resourceType, String resourceName, Type type) {
    this.resourceType = resourceType;
    this.resourceName = resourceName;
    this.type = type;
  }

  /**
   * The xDS resource kind: {@code "cluster"}, {@code "endpoint"},
   * {@code "route"}, or {@code "listener"}.
   */
  public String getResourceType() { return resourceType; }

  /** Name of the changed resource. */
  public String getResourceName() { return resourceName; }

  /** Whether the resource was added, updated, or removed. */
  public Type getType() { return type; }

  @Override
  public String toString() {
    return "ConfigEvent{" + type + " " + resourceType + "/" + resourceName + "}";
  }
}
