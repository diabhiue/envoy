package io.envoyproxy.envoyclient;

/**
 * Information about a single upstream endpoint as received from EDS.
 */
public final class Endpoint {
  private final String address;
  private final int port;
  private final int weight;
  private final int priority;
  private final HealthStatus healthStatus;

  public Endpoint(String address, int port, int weight, int priority, HealthStatus healthStatus) {
    this.address = address;
    this.port = port;
    this.weight = weight;
    this.priority = priority;
    this.healthStatus = healthStatus;
  }

  /** IP address or hostname of the endpoint. */
  public String getAddress() { return address; }

  /** Port number. */
  public int getPort() { return port; }

  /** Relative weight for weighted LB policies. */
  public int getWeight() { return weight; }

  /** Priority group (lower = higher priority). */
  public int getPriority() { return priority; }

  /** Health status as reported by EDS. */
  public HealthStatus getHealthStatus() { return healthStatus; }

  /** Returns "address:port". */
  public String hostAndPort() { return address + ":" + port; }

  @Override
  public String toString() {
    return "Endpoint{" + address + ":" + port
        + " weight=" + weight
        + " priority=" + priority
        + " health=" + healthStatus + "}";
  }

  /** Health status values mirroring the C ABI enum. */
  public enum HealthStatus {
    UNKNOWN(0), HEALTHY(1), DEGRADED(2), UNHEALTHY(3);

    private final int value;
    HealthStatus(int value) { this.value = value; }

    public static HealthStatus fromInt(int v) {
      for (HealthStatus s : values()) {
        if (s.value == v) return s;
      }
      return UNKNOWN;
    }
  }
}
