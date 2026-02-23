#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "client/library/common/config_store.h"
#include "client/library/common/engine_interface.h"

namespace EnvoyClient {

/**
 * Status codes returned by the client library.
 */
enum class Status {
  Ok = 0,
  Error = 1,
  Denied = 2,
  Unavailable = 3,
  Timeout = 4,
};

/**
 * Endpoint information returned by resolve/pick operations.
 */
struct Endpoint {
  std::string address;
  uint32_t port;
  uint32_t weight;
  uint32_t priority;
  uint32_t health_status; // 0=unknown, 1=healthy, 2=degraded, 3=unhealthy
};

/**
 * Request context for LB decisions. All fields are optional.
 */
struct RequestContext {
  std::string path;
  std::string authority;

  // Client-side LB overrides
  std::string override_host;       // "ip:port" to pin to specific endpoint
  bool override_host_strict{false}; // fail if override_host is unhealthy
  std::string hash_key;            // explicit hash for consistent-hashing LB
};

/**
 * Config change event.
 */
struct ConfigEvent {
  std::string resource_type; // "cluster", "endpoint", "route", "listener"
  std::string resource_name;
  std::string event; // "added", "updated", "removed"
};

/**
 * Client is the main entry point for the Envoy Client Library.
 *
 * It provides:
 * - Endpoint resolution (resolve all endpoints for a cluster)
 * - Endpoint picking (select one endpoint via server-configured LB policy)
 * - LB policy overrides (per-cluster and default)
 * - Config change notifications
 *
 * Example usage:
 *   auto client = EnvoyClient::Client::create(bootstrap_yaml);
 *   client->waitReady();
 *   auto endpoint = client->pickEndpoint("my-service");
 *   // Use endpoint->address and endpoint->port to connect
 */
class Client {
public:
  /**
   * Create a client from bootstrap YAML configuration.
   * @param bootstrap_yaml the Envoy bootstrap config in YAML format.
   * @return the client instance, or nullptr on error.
   */
  static std::unique_ptr<Client> create(const std::string& bootstrap_yaml);

  ~Client();

  /**
   * Block until the engine is ready (xDS config received).
   * @param timeout_seconds maximum time to wait.
   * @return true if ready, false if timed out.
   */
  bool waitReady(int timeout_seconds = 30);

  /**
   * Resolve all endpoints for a cluster.
   * @param cluster_name the cluster to resolve.
   * @return list of endpoints, empty if cluster not found.
   */
  std::vector<Endpoint> resolve(const std::string& cluster_name);

  /**
   * Pick a single endpoint using the configured LB policy.
   * @param cluster_name the cluster to pick from.
   * @param ctx optional request context for LB decisions.
   * @return the selected endpoint, or nullopt if unavailable.
   */
  absl::optional<Endpoint> pickEndpoint(const std::string& cluster_name,
                                        const RequestContext& ctx = {});

  /**
   * Override the LB policy for a specific cluster.
   * @param cluster_name the cluster to override.
   * @param lb_policy the LB policy name. Empty string clears the override.
   */
  void setClusterLbPolicy(const std::string& cluster_name, const std::string& lb_policy);

  /**
   * Set the default LB policy override for all clusters.
   * @param lb_policy the LB policy name. Empty string clears the override.
   */
  void setDefaultLbPolicy(const std::string& lb_policy);

  /**
   * Watch for config changes.
   * @param resource_type the resource type to watch (empty = all).
   * @param callback invoked on each config change.
   */
  void watchConfig(const std::string& resource_type,
                   std::function<void(const ConfigEvent&)> callback);

  /**
   * Record the outcome of a request for feedback-driven LB.
   * @param address endpoint IP address.
   * @param port endpoint port.
   * @param status_code HTTP status code (0 = connection error).
   * @param latency_ms request latency in milliseconds.
   */
  void reportResult(const std::string& address, uint32_t port, uint32_t status_code,
                    uint64_t latency_ms);

  /**
   * Shut down the client engine.
   */
  void shutdown();

  /**
   * Test-only factory: create a Client around an already-constructed engine.
   * Not for production use.
   */
  static std::unique_ptr<Client>
  createForTesting(std::unique_ptr<Envoy::Client::ClientEngineInterface> engine) {
    return std::unique_ptr<Client>(new Client(std::move(engine)));
  }

private:
  explicit Client(std::unique_ptr<Envoy::Client::ClientEngineInterface> engine);

  std::unique_ptr<Envoy::Client::ClientEngineInterface> engine_;
};

} // namespace EnvoyClient
