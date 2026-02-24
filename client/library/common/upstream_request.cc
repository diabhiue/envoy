#include "client/library/common/upstream_request.h"

#include "envoy/upstream/thread_local_cluster.h"

#include "source/common/buffer/buffer_impl.h"
#include "source/common/http/message_impl.h"

#include "absl/strings/numbers.h"

namespace Envoy {
namespace Client {

// ---------------------------------------------------------------------------
// UpstreamRequestManager
// ---------------------------------------------------------------------------

UpstreamRequestManager::UpstreamRequestManager(Upstream::ClusterManager& cm,
                                               Event::Dispatcher& dispatcher)
    : cm_(cm), dispatcher_(dispatcher) {}

UpstreamRequestManager::~UpstreamRequestManager() {
  // The dispatcher has stopped by the time this destructor runs (called from
  // ClientEngine::main() after runServer() returns). Any entries still in
  // pending_ did not get a response because the cluster manager cleaned up
  // connections during shutdown — drop them without calling cancel().
  pending_.clear();
}

uint64_t UpstreamRequestManager::sendRequest(const std::string& cluster_name,
                                             Http::RequestHeaderMapPtr headers,
                                             std::string body,
                                             UpstreamResponseCallback callback) {
  uint64_t id = next_id_.fetch_add(1, std::memory_order_relaxed);

  dispatcher_.post([this, id, cluster_name, headers = std::move(headers),
                    body = std::move(body), callback = std::move(callback)]() mutable {
    auto* tlc = cm_.getThreadLocalCluster(cluster_name);
    if (tlc == nullptr) {
      ENVOY_LOG(warn, "envoy client: cluster '{}' not found for send_request", cluster_name);
      callback(UpstreamResponse{false, 503, {}, "cluster not found"});
      return;
    }

    // Build the request message.
    auto message = std::make_unique<Http::RequestMessageImpl>(std::move(headers));
    if (!body.empty()) {
      message->body().add(body);
    }

    // Register state before send() so the callback object is alive during send().
    auto state = std::make_unique<RequestState>(id, *this, std::move(callback));
    RequestState* state_ptr = state.get();
    pending_[id] = std::move(state);

    // send() may call onFailure() inline (which calls erase(id)), so state_ptr
    // may be dangling after this call. Only access it via pending_ afterwards.
    Http::AsyncClient::Request* req =
        tlc->httpAsyncClient().send(std::move(message), *state_ptr, {});

    // If the request is still pending (send() did not fail inline), record
    // the Request* so cancelRequest() can call req->cancel() later.
    auto it = pending_.find(id);
    if (it != pending_.end()) {
      it->second->request = req;
    }
  });

  return id;
}

void UpstreamRequestManager::cancelRequest(uint64_t request_id) {
  dispatcher_.post([this, request_id]() {
    auto it = pending_.find(request_id);
    if (it == pending_.end()) {
      return;
    }
    if (it->second->request != nullptr) {
      it->second->request->cancel();
    }
    // Erase here; the cancelled callback will not fire after cancel().
    pending_.erase(it);
  });
}

void UpstreamRequestManager::erase(uint64_t id) { pending_.erase(id); }

// ---------------------------------------------------------------------------
// RequestState
// ---------------------------------------------------------------------------

UpstreamRequestManager::RequestState::RequestState(uint64_t id, UpstreamRequestManager& manager,
                                                   UpstreamResponseCallback cb)
    : id_(id), manager_(manager), callback_(std::move(cb)) {}

void UpstreamRequestManager::RequestState::onSuccess(const Http::AsyncClient::Request&,
                                                     Http::ResponseMessagePtr&& response) {
  UpstreamResponse resp;
  resp.success = true;

  // Extract HTTP status code from the :status pseudo-header.
  const auto* status_header = response->headers().Status();
  if (status_header != nullptr) {
    uint64_t code = 0;
    if (absl::SimpleAtoi(status_header->value().getStringView(), &code)) {
      resp.status_code = static_cast<uint32_t>(code);
    }
  }

  // Collect all response headers.
  response->headers().iterate([&resp](const Http::HeaderEntry& entry) {
    resp.headers.emplace_back(std::string(entry.key().getStringView()),
                              std::string(entry.value().getStringView()));
    return Http::HeaderMap::Iterate::Continue;
  });

  resp.body = response->bodyAsString();
  finish(std::move(resp));
}

void UpstreamRequestManager::RequestState::onFailure(const Http::AsyncClient::Request&,
                                                     Http::AsyncClient::FailureReason reason) {
  UpstreamResponse resp;
  resp.success = false;
  resp.status_code =
      (reason == Http::AsyncClient::FailureReason::Reset) ? 503 : 507;
  finish(std::move(resp));
}

void UpstreamRequestManager::RequestState::onBeforeFinalizeUpstreamSpan(
    Tracing::Span&, const Http::ResponseHeaderMap*) {
  // No tracing integration yet.
}

void UpstreamRequestManager::RequestState::finish(UpstreamResponse resp) {
  callback_(std::move(resp));
  manager_.erase(id_);
}

} // namespace Client
} // namespace Envoy
