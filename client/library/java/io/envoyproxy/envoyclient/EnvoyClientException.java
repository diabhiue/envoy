package io.envoyproxy.envoyclient;

/**
 * Thrown when an Envoy Client Library operation fails.
 */
public class EnvoyClientException extends RuntimeException {
  public EnvoyClientException(String message) {
    super(message);
  }

  public EnvoyClientException(String message, Throwable cause) {
    super(message, cause);
  }
}
