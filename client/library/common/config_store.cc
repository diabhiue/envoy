#include "client/library/common/config_store.h"

#include "envoy/upstream/thread_local_cluster.h"

#include "absl/strings/str_cat.h"

namespace Envoy {
namespace Client {

ConfigStore::ConfigStore(Upstream::ClusterManager& cluster_manager)
    : cluster_manager_(cluster_manager) {}

bool ConfigStore::resolve(const std::string& cluster_name,
                          std::vector<EndpointInfo>& out_endpoints) {
  auto* tlc = cluster_manager_.getThreadLocalCluster(cluster_name);
  if (tlc == nullptr) {
    ENVOY_LOG(debug, "client: cluster '{}' not found", cluster_name);
    return false;
  }

  out_endpoints.clear();
  const auto& priority_set = tlc->prioritySet();
  for (size_t priority = 0; priority < priority_set.hostSetsPerPriority().size(); ++priority) {
    const auto& host_set = priority_set.hostSetsPerPriority()[priority];
    for (const auto& host : host_set->hosts()) {
      out_endpoints.push_back(hostToEndpointInfo(*host));
    }
  }

  return !out_endpoints.empty();
}

absl::optional<EndpointInfo>
ConfigStore::pickEndpoint(const std::string& cluster_name,
                          Upstream::LoadBalancerContext* context) {
  // Check for a client-side LB policy override (cluster-specific beats default).
  std::string lb_policy;
  {
    absl::ReaderMutexLock lock(&lb_override_mutex_);
    auto it = cluster_lb_overrides_.find(cluster_name);
    if (it != cluster_lb_overrides_.end()) {
      lb_policy = it->second;
    } else if (!default_lb_override_.empty()) {
      lb_policy = default_lb_override_;
    }
  }

  if (!lb_policy.empty()) {
    return pickEndpointWithPolicy(cluster_name, lb_policy, context);
  }

  // Default: delegate to Envoy's server-configured LB.
  auto* tlc = cluster_manager_.getThreadLocalCluster(cluster_name);
  if (tlc == nullptr) {
    ENVOY_LOG(debug, "client: cluster '{}' not found for pick", cluster_name);
    return absl::nullopt;
  }

  auto host = Upstream::LoadBalancer::onlyAllowSynchronousHostSelection(
      tlc->loadBalancer().chooseHost(context));
  if (host == nullptr) {
    ENVOY_LOG(debug, "client: no host available for cluster '{}'", cluster_name);
    return absl::nullopt;
  }

  return hostToEndpointInfo(*host);
}

absl::optional<EndpointInfo>
ConfigStore::pickEndpointWithPolicy(const std::string& cluster_name,
                                    const std::string& lb_policy,
                                    Upstream::LoadBalancerContext* context) {
  auto* tlc = cluster_manager_.getThreadLocalCluster(cluster_name);
  if (tlc == nullptr) {
    ENVOY_LOG(debug, "client: cluster '{}' not found for policy-based pick", cluster_name);
    return absl::nullopt;
  }

  // Collect healthy endpoints from the lowest-numbered priority level that has any.
  const auto& priority_set = tlc->prioritySet();
  std::vector<EndpointInfo> candidates;
  for (size_t p = 0; p < priority_set.hostSetsPerPriority().size(); ++p) {
    const auto& host_set = priority_set.hostSetsPerPriority()[p];
    for (const auto& host : host_set->hosts()) {
      auto ep = hostToEndpointInfo(*host);
      if (ep.health_status == 1) { // Healthy
        candidates.push_back(std::move(ep));
      }
    }
    if (!candidates.empty()) {
      break;
    }
  }

  // Fall back to all hosts (any health) if no healthy ones were found.
  if (candidates.empty()) {
    for (size_t p = 0; p < priority_set.hostSetsPerPriority().size(); ++p) {
      const auto& host_set = priority_set.hostSetsPerPriority()[p];
      for (const auto& host : host_set->hosts()) {
        candidates.push_back(hostToEndpointInfo(*host));
      }
      if (!candidates.empty()) {
        break;
      }
    }
  }

  if (candidates.empty()) {
    ENVOY_LOG(debug, "client: no endpoints for '{}' with override policy '{}'", cluster_name,
              lb_policy);
    return absl::nullopt;
  }

  // Hash-based selection (ring_hash, maglev): use the context's hash key.
  if (context != nullptr && (lb_policy.find("ring_hash") != std::string::npos ||
                              lb_policy.find("maglev") != std::string::npos)) {
    auto hash_key_opt = context->computeHashKey();
    if (hash_key_opt.has_value()) {
      const size_t idx = static_cast<size_t>(hash_key_opt.value().hash) % candidates.size();
      ENVOY_LOG(debug, "client: hash-based pick for '{}': index {}/{}", cluster_name, idx,
                candidates.size());
      return candidates[idx];
    }
  }

  // Round-robin: use a per-cluster monotonically increasing counter.
  if (lb_policy.find("round_robin") != std::string::npos) {
    absl::MutexLock lock(&lb_override_mutex_);
    const size_t idx =
        static_cast<size_t>(rr_counters_[cluster_name]++) % candidates.size();
    ENVOY_LOG(debug, "client: round-robin pick for '{}': index {}/{}", cluster_name, idx,
              candidates.size());
    return candidates[idx];
  }

  // Random / least_request / any unrecognized policy: use a global monotonic counter.
  const size_t idx =
      static_cast<size_t>(random_counter_.fetch_add(1, std::memory_order_relaxed)) %
      candidates.size();
  ENVOY_LOG(debug, "client: random pick for '{}': index {}/{}", cluster_name, idx,
            candidates.size());
  return candidates[idx];
}

void ConfigStore::setClusterLbPolicy(const std::string& cluster_name,
                                     const std::string& lb_policy_name) {
  absl::MutexLock lock(&lb_override_mutex_);
  if (lb_policy_name.empty()) {
    cluster_lb_overrides_.erase(cluster_name);
    ENVOY_LOG(info, "client: LB policy override cleared for cluster '{}'", cluster_name);
  } else {
    cluster_lb_overrides_[cluster_name] = lb_policy_name;
    ENVOY_LOG(info, "client: LB policy override for cluster '{}' set to '{}'", cluster_name,
              lb_policy_name);
  }
}

void ConfigStore::setDefaultLbPolicy(const std::string& lb_policy_name) {
  absl::MutexLock lock(&lb_override_mutex_);
  default_lb_override_ = lb_policy_name;
  ENVOY_LOG(info, "client: default LB policy override set to '{}'", lb_policy_name);
}

void ConfigStore::watchConfig(const std::string& resource_type, ConfigWatchCallback callback) {
  absl::MutexLock lock(&watchers_mutex_);
  watchers_.emplace_back(resource_type, std::move(callback));
}

void ConfigStore::notifyConfigChange(const std::string& resource_type,
                                     const std::string& resource_name, ConfigEvent event) {
  absl::MutexLock lock(&watchers_mutex_);
  for (const auto& [filter_type, callback] : watchers_) {
    if (filter_type.empty() || filter_type == resource_type) {
      callback(resource_type, resource_name, event);
    }
  }
}

void ConfigStore::reportResult(const std::string& address, uint32_t port,
                               uint32_t status_code, uint64_t latency_ms) {
  const std::string key = absl::StrCat(address, ":", port);
  absl::MutexLock lock(&stats_mutex_);
  auto& stats = endpoint_call_stats_[key];
  ++stats.total_requests;
  stats.total_latency_ms += latency_ms;
  // Count connection errors (status 0) and server errors (5xx) as failures.
  if (status_code == 0 || status_code >= 500) {
    ++stats.error_requests;
  }
  ENVOY_LOG(debug,
            "client: reportResult {}:{} status={} latency={}ms "
            "(total={} errors={})",
            address, port, status_code, latency_ms, stats.total_requests,
            stats.error_requests);
}

EndpointInfo ConfigStore::hostToEndpointInfo(const Upstream::Host& host) {
  EndpointInfo info;
  const auto& address = host.address();
  if (address->type() == Network::Address::Type::Ip) {
    info.address = address->ip()->addressAsString();
    info.port = address->ip()->port();
  } else {
    info.address = address->asString();
    info.port = 0;
  }
  info.weight = host.weight();
  info.priority = host.priority();

  switch (host.coarseHealth()) {
  case Upstream::Host::Health::Healthy:
    info.health_status = 1;
    break;
  case Upstream::Host::Health::Degraded:
    info.health_status = 2;
    break;
  case Upstream::Host::Health::Unhealthy:
    info.health_status = 3;
    break;
  default:
    info.health_status = 0;
    break;
  }
  return info;
}

} // namespace Client
} // namespace Envoy
