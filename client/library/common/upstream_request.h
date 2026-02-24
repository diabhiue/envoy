#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "envoy/event/dispatcher.h"
#include "envoy/http/async_client.h"
#include "envoy/http/header_map.h"
#include "envoy/upstream/cluster_manager.h"

#include "source/common/common/logger.h"

#include "absl/container/flat_hash_map.h"

namespace Envoy {
namespace Client {

/**
 * The result of an upstream HTTP request.
 */
struct UpstreamResponse {
  bool success{false};
  uint32_t status_code{0};
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
};

using UpstreamResponseCallback = std::function<void(UpstreamResponse)>;

/**
 * UpstreamRequestManager manages in-flight async HTTP requests to upstream clusters.
 *
 * It uses ClusterManager::getThreadLocalCluster()->httpAsyncClient() to send requests
 * via Envoy's connection pools, which handles endpoint selection and connection reuse.
 *
 * Thread model:
 *   - sendRequest() and cancelRequest() may be called from any thread.
 *   - The pending_ map is only ever accessed on the dispatcher thread.
 *   - Response callbacks fire on the dispatcher thread.
 */
class UpstreamRequestManager : public Logger::Loggable<Logger::Id::client> {
public:
  UpstreamRequestManager(Upstream::ClusterManager& cm, Event::Dispatcher& dispatcher);
  ~UpstreamRequestManager();

  /**
   * Send an HTTP request to the named cluster asynchronously.
   *
   * @param cluster_name the xDS cluster to route the request to.
   * @param headers request headers (including :method, :path, :scheme, :authority).
   * @param body optional request body.
   * @param callback invoked on the dispatcher thread when the response arrives or on failure.
   * @return a request_id that can be passed to cancelRequest(). Returns 0 on immediate failure
   *         (cluster not found or dispatcher unavailable).
   */
  uint64_t sendRequest(const std::string& cluster_name, Http::RequestHeaderMapPtr headers,
                       std::string body, UpstreamResponseCallback callback);

  /**
   * Cancel an in-flight request. Safe to call from any thread.
   * No-op if the request has already completed.
   *
   * @param request_id the id returned by sendRequest().
   */
  void cancelRequest(uint64_t request_id);

private:
  /**
   * Per-request state that implements Http::AsyncClient::Callbacks.
   * Owned by pending_; destroyed when the request completes or is cancelled.
   */
  class RequestState : public Http::AsyncClient::Callbacks {
  public:
    RequestState(uint64_t id, UpstreamRequestManager& manager, UpstreamResponseCallback cb);

    // Http::AsyncClient::Callbacks
    void onSuccess(const Http::AsyncClient::Request& request,
                   Http::ResponseMessagePtr&& response) override;
    void onFailure(const Http::AsyncClient::Request& request,
                   Http::AsyncClient::FailureReason reason) override;
    void onBeforeFinalizeUpstreamSpan(Tracing::Span& span,
                                      const Http::ResponseHeaderMap* response_headers) override;

    // Non-null once send() returns without inline failure.
    Http::AsyncClient::Request* request{nullptr};

  private:
    void finish(UpstreamResponse resp);

    uint64_t id_;
    UpstreamRequestManager& manager_;
    UpstreamResponseCallback callback_;
  };

  // Removes a completed or cancelled request from pending_.
  // Must be called on the dispatcher thread.
  void erase(uint64_t id);

  Upstream::ClusterManager& cm_;
  Event::Dispatcher& dispatcher_;
  std::atomic<uint64_t> next_id_{1};

  // Accessed only on the dispatcher thread — no mutex required.
  absl::flat_hash_map<uint64_t, std::unique_ptr<RequestState>> pending_;
};

} // namespace Client
} // namespace Envoy
