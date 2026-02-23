#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "envoy/upstream/cluster_manager.h"
#include "envoy/upstream/load_balancer.h"

#include "source/common/common/logger.h"

#include "absl/container/flat_hash_map.h"
#include "absl/synchronization/mutex.h"

namespace Envoy {
namespace Client {

/**
 * Lightweight endpoint information exposed to the application.
 */
struct EndpointInfo {
  std::string address;
  uint32_t port;
  uint32_t weight;
  uint32_t priority;
  // 0=unknown, 1=healthy, 2=degraded, 3=unhealthy
  uint32_t health_status;
};

/**
 * Config change event type.
 */
enum class ConfigEvent { Added, Updated, Removed };

/**
 * Callback for config changes.
 */
using ConfigWatchCallback =
    std::function<void(const std::string& resource_type, const std::string& resource_name,
                       ConfigEvent event)>;

/**
 * ConfigStore provides a read-only view of xDS state for the client library.
 *
 * Rather than building a separate xDS subscription layer, the ConfigStore wraps
 * Envoy's existing ClusterManager. The ClusterManager handles CDS/EDS subscriptions
 * and maintains the full cluster/endpoint state. ConfigStore provides a simplified
 * query interface on top of it.
 *
 * For endpoint resolution: queries go through ClusterManager's thread-local cluster
 * to access PrioritySet and the configured LoadBalancer.
 *
 * For LB policy overrides: the ConfigStore maintains client-side override state
 * that takes precedence over server-configured policies.
 */
class ConfigStore : public Logger::Loggable<Logger::Id::client> {
public:
  explicit ConfigStore(Upstream::ClusterManager& cluster_manager);

  /**
   * Resolve all healthy endpoints for a cluster.
   * @param cluster_name the cluster to resolve.
   * @param out_endpoints populated with endpoint information.
   * @return true if the cluster exists and has endpoints.
   */
  bool resolve(const std::string& cluster_name, std::vector<EndpointInfo>& out_endpoints);

  /**
   * Pick a single endpoint using the configured LB policy.
   * @param cluster_name the cluster to pick from.
   * @param context optional LB context for hash-based policies.
   * @return the selected endpoint, or nullopt if unavailable.
   */
  absl::optional<EndpointInfo>
  pickEndpoint(const std::string& cluster_name,
               Upstream::LoadBalancerContext* context = nullptr);

  /**
   * Set a client-side LB policy override for a specific cluster.
   * @param cluster_name the cluster to override.
   * @param lb_policy_name the LB policy name (e.g., "envoy.load_balancing_policies.round_robin").
   *                       Pass empty string to clear the override.
   */
  void setClusterLbPolicy(const std::string& cluster_name, const std::string& lb_policy_name);

  /**
   * Set a default LB policy override for all clusters.
   * @param lb_policy_name the default LB policy name. Pass empty string to clear.
   */
  void setDefaultLbPolicy(const std::string& lb_policy_name);

  /**
   * Register a callback for config change notifications.
   * @param resource_type the resource type to watch (empty = all types).
   * @param callback the callback to invoke on changes.
   */
  void watchConfig(const std::string& resource_type, ConfigWatchCallback callback);

  /**
   * Notify watchers of a config change. Called by the engine when xDS updates arrive.
   */
  void notifyConfigChange(const std::string& resource_type, const std::string& resource_name,
                          ConfigEvent event);

  /**
   * Record the outcome of a request to an endpoint for feedback-driven LB.
   * @param address the endpoint IP address string.
   * @param port the endpoint port.
   * @param status_code HTTP status code (0 = connection-level error).
   * @param latency_ms observed request latency in milliseconds.
   */
  void reportResult(const std::string& address, uint32_t port, uint32_t status_code,
                    uint64_t latency_ms);

private:
  static EndpointInfo hostToEndpointInfo(const Upstream::Host& host);

  /**
   * Pick an endpoint using a client-side LB algorithm instead of Envoy's
   * server-configured policy. Called when a cluster or default LB override is set.
   */
  absl::optional<EndpointInfo> pickEndpointWithPolicy(const std::string& cluster_name,
                                                      const std::string& lb_policy,
                                                      Upstream::LoadBalancerContext* context);

  Upstream::ClusterManager& cluster_manager_;

  // Client-side LB policy overrides and round-robin counters
  absl::Mutex lb_override_mutex_;
  absl::flat_hash_map<std::string, std::string> cluster_lb_overrides_
      ABSL_GUARDED_BY(lb_override_mutex_);
  std::string default_lb_override_ ABSL_GUARDED_BY(lb_override_mutex_);
  // Per-cluster round-robin counters for the "round_robin" override policy.
  absl::flat_hash_map<std::string, uint64_t> rr_counters_ ABSL_GUARDED_BY(lb_override_mutex_);

  // Global counter for random-style selection (used by non-RR override policies).
  std::atomic<uint64_t> random_counter_{0};

  // Per-endpoint call stats for feedback-driven LB (keyed by "address:port").
  struct EndpointCallStats {
    uint64_t total_requests{0};
    uint64_t error_requests{0};
    uint64_t total_latency_ms{0};
  };
  absl::Mutex stats_mutex_;
  absl::flat_hash_map<std::string, EndpointCallStats> endpoint_call_stats_
      ABSL_GUARDED_BY(stats_mutex_);

  // Config watchers
  absl::Mutex watchers_mutex_;
  std::vector<std::pair<std::string, ConfigWatchCallback>> watchers_
      ABSL_GUARDED_BY(watchers_mutex_);
};

} // namespace Client
} // namespace Envoy
