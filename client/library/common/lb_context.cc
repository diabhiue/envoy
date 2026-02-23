#include "client/library/common/lb_context.h"

#include "source/common/common/hash.h"
#include "source/common/http/header_map_impl.h"

namespace Envoy {
namespace Client {

ClientLoadBalancerContext::ClientLoadBalancerContext(const std::string& hash_key,
                                                     const std::string& override_host,
                                                     bool override_strict, const std::string& path,
                                                     const std::string& authority)
    : override_host_(override_host), override_strict_(override_strict) {
  if (!hash_key.empty()) {
    hash_key_ = HashUtil::xxHash64(hash_key);
  }

  if (!path.empty() || !authority.empty()) {
    headers_ = Http::RequestHeaderMapImpl::create();
    if (!path.empty()) {
      headers_->setPath(path);
    }
    if (!authority.empty()) {
      headers_->setHost(authority);
    }
  }
}

absl::optional<uint64_t> ClientLoadBalancerContext::computeHashKey() { return hash_key_; }

const Http::RequestHeaderMap* ClientLoadBalancerContext::downstreamHeaders() const {
  return headers_.get();
}

absl::optional<Upstream::LoadBalancerContext::OverrideHost>
ClientLoadBalancerContext::overrideHostToSelect() const {
  if (override_host_.empty()) {
    return absl::nullopt;
  }
  return Upstream::LoadBalancerContext::OverrideHost{override_host_, override_strict_};
}

} // namespace Client
} // namespace Envoy
