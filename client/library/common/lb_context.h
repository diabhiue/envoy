#pragma once

#include <string>

#include "envoy/upstream/load_balancer.h"

#include "source/common/http/header_map_impl.h"
#include "source/common/upstream/load_balancer_context_base.h"

namespace Envoy {
namespace Client {

/**
 * ClientLoadBalancerContext translates per-request LB hints (hash key,
 * override host, path/authority) into the Upstream::LoadBalancerContext
 * interface understood by Envoy's LB implementations (ring hash, maglev,
 * random, round-robin, etc.).
 *
 * Lifecycle: constructed on the stack around each pickEndpoint() call.
 * The string references passed to the constructor must remain valid for
 * the duration of the pick.
 */
class ClientLoadBalancerContext : public Upstream::LoadBalancerContextBase {
public:
  /**
   * @param hash_key       Explicit hash for consistent-hashing LB. Empty = no hash.
   * @param override_host  "ip:port" to pin to a specific endpoint. Empty = no override.
   * @param override_strict Fail if override_host is unhealthy.
   * @param path           Request path for header-based LB policies. Empty = omit.
   * @param authority      Request authority (:authority / Host header). Empty = omit.
   */
  ClientLoadBalancerContext(const std::string& hash_key, const std::string& override_host,
                            bool override_strict, const std::string& path,
                            const std::string& authority);

  // Upstream::LoadBalancerContext overrides
  absl::optional<uint64_t> computeHashKey() override;
  const Http::RequestHeaderMap* downstreamHeaders() const override;
  absl::optional<Upstream::LoadBalancerContext::OverrideHost> overrideHostToSelect() const override;

private:
  const std::string& override_host_;
  bool override_strict_;

  // Pre-computed hash; empty hash_key argument → nullopt.
  absl::optional<uint64_t> hash_key_;

  // Header map built when path/authority are non-empty; otherwise nullptr.
  Http::RequestHeaderMapPtr headers_;
};

} // namespace Client
} // namespace Envoy
