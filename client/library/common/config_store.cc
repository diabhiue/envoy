#include "client/library/common/config_store.h"

#include "envoy/upstream/thread_local_cluster.h"

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

void ConfigStore::setClusterLbPolicy(const std::string& cluster_name,
                                     const std::string& lb_policy_name) {
  absl::MutexLock lock(&lb_override_mutex_);
  if (lb_policy_name.empty()) {
    cluster_lb_overrides_.erase(cluster_name);
  } else {
    cluster_lb_overrides_[cluster_name] = lb_policy_name;
  }
  // TODO(Phase 1): Apply the override to the cluster's LB factory.
  // This requires intercepting cluster creation or replacing the LB on the ThreadLocalCluster.
  // For now, we store the intent; actual LB replacement will be implemented
  // when we have the full ClientEngine lifecycle wired up.
  ENVOY_LOG(info, "client: LB policy override for cluster '{}' set to '{}'", cluster_name,
            lb_policy_name);
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
