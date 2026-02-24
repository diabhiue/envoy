#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "absl/time/time.h"

#include "client/library/common/config_store.h"

namespace Envoy {
namespace Client {

/**
 * The result of an upstream HTTP request made via sendRequest().
 */
struct UpstreamResponse {
  bool success{false};
  uint32_t status_code{0};
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
};

using UpstreamResponseCallback = std::function<void(UpstreamResponse)>;

/**
 * Abstract interface for ClientEngine.
 *
 * Exists to allow test injection of a mock engine into EnvoyClient::Client
 * without starting a real Envoy server.
 */
class ClientEngineInterface {
public:
  virtual ~ClientEngineInterface() = default;

  virtual bool waitReady(absl::Duration timeout) = 0;
  virtual void terminate() = 0;
  virtual bool isTerminated() const = 0;
  virtual ConfigStore& configStore() = 0;

  /**
   * Send an HTTP request to an xDS-managed cluster asynchronously.
   *
   * @param cluster_name the cluster to route the request to.
   * @param headers request headers as (name, value) pairs.
   * @param body optional request body.
   * @param callback invoked when the response arrives or on failure.
   * @return a request_id for cancellation. Returns 0 if the request cannot be initiated.
   */
  virtual uint64_t
  sendRequest(const std::string& cluster_name,
              const std::vector<std::pair<std::string, std::string>>& headers,
              const std::string& body, UpstreamResponseCallback callback) = 0;

  /**
   * Cancel an in-flight request.
   * @param request_id the id returned by sendRequest().
   */
  virtual void cancelRequest(uint64_t request_id) = 0;
};

} // namespace Client
} // namespace Envoy
