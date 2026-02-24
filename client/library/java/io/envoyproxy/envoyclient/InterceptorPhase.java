package io.envoyproxy.envoyclient;

/**
 * Indicates when an interceptor runs relative to the server-pushed filter chain
 * (CLIENT_WRAPS_SERVER ordering — the default merge policy).
 */
public enum InterceptorPhase {
  /** Runs before the server filter chain on the request path. */
  PRE_REQUEST(0),
  /** Runs after the server filter chain on the request path. */
  POST_REQUEST(1),
  /** Runs before the server filter chain on the response path. */
  PRE_RESPONSE(2),
  /** Runs after the server filter chain on the response path. */
  POST_RESPONSE(3);

  private final int value;

  InterceptorPhase(int value) {
    this.value = value;
  }

  public int getValue() {
    return value;
  }

  public static InterceptorPhase fromInt(int value) {
    for (InterceptorPhase p : values()) {
      if (p.value == value) return p;
    }
    return PRE_REQUEST;
  }
}
