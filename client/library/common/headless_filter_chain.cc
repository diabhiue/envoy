#include "client/library/common/headless_filter_chain.h"

#include <atomic>
#include <ostream>

#include "envoy/http/codec.h"
#include "envoy/http/header_map.h"
#include "envoy/http/metadata_interface.h"
#include "envoy/router/router.h"
#include "envoy/upstream/thread_local_cluster.h"

#include "source/common/common/scope_tracked_object_stack.h"
#include "source/common/http/header_map_impl.h"
#include "source/common/stream_info/filter_state_impl.h"
#include "source/common/stream_info/stream_info_impl.h"
#include "source/common/tracing/null_span_impl.h"

namespace Envoy {
namespace Client {

// ---------------------------------------------------------------------------
// ActiveRequest — one in-flight applyRequestFilters() call.
//
// Implements StreamDecoderFilterCallbacks + ScopeTrackedObject so native Envoy
// filters can interact with the filter chain the way they do inside HCM.
//
// Lifetime: ActiveRequest is always heap-allocated as a shared_ptr. It holds a
// self-reference (self_) to keep itself alive while an async filter is waiting.
// When the request completes (allow or deny), complete() releases self_ so the
// object is destroyed once all external references drop.
// ---------------------------------------------------------------------------
class HeadlessFilterChain::ActiveRequest
    : public std::enable_shared_from_this<ActiveRequest>,
      public Http::StreamDecoderFilterCallbacks,
      public ScopeTrackedObject {
public:
  ActiveRequest(HeadlessFilterChain& chain, const std::string& cluster_name,
                Http::RequestHeaderMapPtr headers, RequestFilterCompletionCb cb)
      : chain_(chain), cluster_name_(cluster_name), headers_(std::move(headers)),
        cb_(std::move(cb)),
        stream_info_(Http::Protocol::Http2, chain_.time_source_,
                     /*downstream_connection_info_provider=*/nullptr,
                     StreamInfo::FilterState::LifeSpan::FilterChain),
        stream_id_(nextStreamId()) {}

  /**
   * Must be called once right after construction via make_shared. Stores a
   * self-reference so async filters keep the ActiveRequest alive.
   */
  void activate() { self_ = shared_from_this(); }

  /**
   * Populate filters_ from the chain's filter factories, setting ourselves
   * as the StreamDecoderFilterCallbacks for each filter.
   */
  void buildFilterChain() {
    class ChainFactoryCallbacks : public Http::FilterChainFactoryCallbacks {
    public:
      explicit ChainFactoryCallbacks(ActiveRequest& req) : req_(req) {}
      void addStreamDecoderFilter(Http::StreamDecoderFilterSharedPtr filter) override {
        filter->setDecoderFilterCallbacks(req_);
        req_.filters_.push_back(std::move(filter));
      }
      void addStreamEncoderFilter(Http::StreamEncoderFilterSharedPtr) override {}
      void addStreamFilter(Http::StreamFilterSharedPtr filter) override {
        filter->setDecoderFilterCallbacks(req_);
        req_.filters_.push_back(filter);
      }
      void addAccessLogHandler(AccessLog::InstanceSharedPtr) override {}

    private:
      ActiveRequest& req_;
    };
    ChainFactoryCallbacks cbs(*this);
    for (const auto& entry : chain_.filter_factories_) {
      entry.factory(cbs);
    }
  }

  /**
   * Drive the filter chain starting at filter_index_.
   * Stops when a filter returns StopIteration (async) or all filters pass.
   */
  void runFilters() {
    while (filter_index_ < filters_.size()) {
      Http::StreamDecoderFilterSharedPtr& filter = filters_[filter_index_];
      const Http::FilterHeadersStatus status =
          filter->decodeHeaders(*headers_, /*end_stream=*/true);
      if (status == Http::FilterHeadersStatus::StopIteration ||
          status == Http::FilterHeadersStatus::StopAllIterationAndBuffer ||
          status == Http::FilterHeadersStatus::StopAllIterationAndWatermark) {
        // Filter is async — it will call continueDecoding() later.
        return;
      }
      ++filter_index_;
    }
    // All filters passed.
    complete(FilterChainResult{FilterChainResult::Status::Allow});
  }

  // ---------------------------------------------------------------------------
  // StreamDecoderFilterCallbacks
  // ---------------------------------------------------------------------------

  void continueDecoding() override {
    if (completed_) {
      return;
    }
    ++filter_index_;
    runFilters();
  }

  void sendLocalReply(Http::Code response_code, absl::string_view /*body_text*/,
                      std::function<void(Http::ResponseHeaderMap&)> /*modify_headers*/,
                      const absl::optional<Grpc::Status::GrpcStatus> /*grpc_status*/,
                      absl::string_view /*details*/) override {
    FilterChainResult result;
    result.status = FilterChainResult::Status::Deny;
    result.deny_code = static_cast<uint32_t>(response_code);
    complete(result);
  }

  void sendGoAwayAndClose(bool /*graceful*/ = false) override {}

  // Body / trailer / metadata — no-ops in headers-only mode.
  const Buffer::Instance* decodingBuffer() override { return nullptr; }
  void modifyDecodingBuffer(std::function<void(Buffer::Instance&)>) override {}
  void addDecodedData(Buffer::Instance&, bool) override {}
  void injectDecodedDataToFilterChain(Buffer::Instance&, bool) override {}
  Http::RequestTrailerMap& addDecodedTrailers() override {
    if (!dummy_trailers_) {
      dummy_trailers_ = Http::RequestTrailerMapImpl::create();
    }
    return *dummy_trailers_;
  }
  Http::MetadataMapVector& addDecodedMetadata() override { return metadata_map_vector_; }
  void encode1xxHeaders(Http::ResponseHeaderMapPtr&&) override {}
  void encodeHeaders(Http::ResponseHeaderMapPtr&&, bool, absl::string_view) override {}
  void encodeData(Buffer::Instance&, bool) override {}
  void encodeTrailers(Http::ResponseTrailerMapPtr&&) override {}
  void encodeMetadata(Http::MetadataMapPtr&&) override {}
  void onDecoderFilterAboveWriteBufferHighWatermark() override {}
  void onDecoderFilterBelowWriteBufferLowWatermark() override {}
  void addDownstreamWatermarkCallbacks(Http::DownstreamWatermarkCallbacks&) override {}
  void removeDownstreamWatermarkCallbacks(Http::DownstreamWatermarkCallbacks&) override {}
  Buffer::BufferMemoryAccountSharedPtr account() const override { return nullptr; }
  bool recreateStream(const Http::ResponseHeaderMap*) override { return false; }
  void addUpstreamSocketOptions(const Network::Socket::OptionsSharedPtr&) override {}
  Network::Socket::OptionsSharedPtr getUpstreamSocketOptions() const override { return nullptr; }
  void setUpstreamOverrideHost(Upstream::LoadBalancerContext::OverrideHost) override {}
  absl::optional<Upstream::LoadBalancerContext::OverrideHost>
  upstreamOverrideHost() const override {
    return absl::nullopt;
  }
  bool shouldLoadShed() const override { return false; }

  // ---------------------------------------------------------------------------
  // StreamFilterCallbacks (base)
  // ---------------------------------------------------------------------------

  OptRef<const Network::Connection> connection() override { return {}; }
  Event::Dispatcher& dispatcher() override { return chain_.dispatcher_; }

  void resetStream(Http::StreamResetReason /*reason*/ = Http::StreamResetReason::LocalReset,
                   absl::string_view /*transport_failure_reason*/ = "") override {
    FilterChainResult result;
    result.status = FilterChainResult::Status::Deny;
    result.deny_code = 503;
    complete(result);
  }

  Router::RouteConstSharedPtr route() override { return nullptr; }

  Upstream::ClusterInfoConstSharedPtr clusterInfo() override {
    auto* tlc = chain_.cluster_manager_.getThreadLocalCluster(cluster_name_);
    return tlc ? tlc->info() : nullptr;
  }

  uint64_t streamId() const override { return stream_id_; }
  StreamInfo::StreamInfo& streamInfo() override { return stream_info_; }
  Tracing::Span& activeSpan() override { return Tracing::NullSpan::instance(); }
  OptRef<const Tracing::Config> tracingConfig() const override { return {}; }
  const ScopeTrackedObject& scope() override { return *this; }
  void restoreContextOnContinue(ScopeTrackedObjectStack&) override {}
  void resetIdleTimer() override {}
  const Router::RouteSpecificFilterConfig* mostSpecificPerFilterConfig() const override {
    return nullptr;
  }
  Router::RouteSpecificFilterConfigs perFilterConfigs() const override { return {}; }
  Http::Http1StreamEncoderOptionsOptRef http1StreamEncoderOptions() override { return {}; }
  OptRef<Http::UpstreamStreamFilterCallbacks> upstreamCallbacks() override { return {}; }
  OptRef<Http::DownstreamStreamFilterCallbacks> downstreamCallbacks() override { return {}; }
  absl::string_view filterConfigName() const override { return "headless"; }

  Http::RequestHeaderMapOptRef requestHeaders() override {
    return headers_ ? makeOptRef(*headers_) : Http::RequestHeaderMapOptRef{};
  }
  Http::RequestTrailerMapOptRef requestTrailers() override { return {}; }
  Http::ResponseHeaderMapOptRef informationalHeaders() override { return {}; }
  Http::ResponseHeaderMapOptRef responseHeaders() override { return {}; }
  Http::ResponseTrailerMapOptRef responseTrailers() override { return {}; }
  void setBufferLimit(uint64_t) override {}
  uint64_t bufferLimit() override { return 0; }

  // ---------------------------------------------------------------------------
  // ScopeTrackedObject
  // ---------------------------------------------------------------------------

  void dumpState(std::ostream& os, int indent_level) const override {
    const std::string spaces(indent_level * 2, ' ');
    os << spaces << "HeadlessFilterChain::ActiveRequest " << this
       << " cluster=" << cluster_name_ << " stream_id=" << stream_id_ << "\n";
  }

  // Expose headers for the caller-facing callback after filter chain completes.
  Http::RequestHeaderMapPtr& headers() { return headers_; }

private:
  static uint64_t nextStreamId() {
    static std::atomic<uint64_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed);
  }

  void complete(FilterChainResult result) {
    if (completed_) {
      return;
    }
    completed_ = true;
    cb_(result, std::move(headers_));
    // Release the self-reference last so the object isn't destroyed mid-flight.
    self_.reset();
  }

  HeadlessFilterChain& chain_;
  const std::string cluster_name_;
  Http::RequestHeaderMapPtr headers_;
  RequestFilterCompletionCb cb_;
  StreamInfo::StreamInfoImpl stream_info_;
  const uint64_t stream_id_;

  std::vector<Http::StreamDecoderFilterSharedPtr> filters_;
  size_t filter_index_{0};
  bool completed_{false};

  // Self-reference keeping this object alive while async filters are in flight.
  std::shared_ptr<ActiveRequest> self_;

  // No-op storage for methods we don't need in headers-only mode.
  Http::RequestTrailerMapPtr dummy_trailers_;
  Http::MetadataMapVector metadata_map_vector_;
};

// ---------------------------------------------------------------------------
// HeadlessFilterChain
// ---------------------------------------------------------------------------

HeadlessFilterChain::HeadlessFilterChain(Event::Dispatcher& dispatcher,
                                         Upstream::ClusterManager& cluster_manager,
                                         TimeSource& time_source)
    : dispatcher_(dispatcher), cluster_manager_(cluster_manager), time_source_(time_source) {}

HeadlessFilterChain::~HeadlessFilterChain() = default;

void HeadlessFilterChain::addFilterFactory(const std::string& name, Http::FilterFactoryCb factory) {
  filter_factories_.push_back({name, std::move(factory)});
  ENVOY_LOG(debug, "client: registered filter factory '{}'", name);
}

void HeadlessFilterChain::addInterceptor(ClientInterceptor interceptor) {
  absl::MutexLock lock(&interceptors_mutex_);
  for (auto& existing : interceptors_) {
    if (existing.name == interceptor.name) {
      existing = std::move(interceptor);
      ENVOY_LOG(debug, "client: replaced interceptor '{}'", existing.name);
      return;
    }
  }
  ENVOY_LOG(debug, "client: registered interceptor '{}'", interceptor.name);
  interceptors_.push_back(std::move(interceptor));
}

void HeadlessFilterChain::removeInterceptor(const std::string& name) {
  absl::MutexLock lock(&interceptors_mutex_);
  interceptors_.erase(
      std::remove_if(interceptors_.begin(), interceptors_.end(),
                     [&name](const ClientInterceptor& i) { return i.name == name; }),
      interceptors_.end());
}

FilterChainResult
HeadlessFilterChain::runRequestInterceptors(const InterceptorSnapshot& snapshot,
                                            Http::RequestHeaderMap& headers,
                                            const std::string& cluster_name,
                                            InterceptorPhase phase) {
  for (const auto& interceptor : snapshot) {
    if (interceptor.on_request) {
      if (!interceptor.on_request(headers, cluster_name, phase)) {
        ENVOY_LOG(debug, "client: interceptor '{}' denied request (phase {})", interceptor.name,
                  static_cast<uint32_t>(phase));
        return FilterChainResult{FilterChainResult::Status::Deny, 403};
      }
    }
  }
  return FilterChainResult{FilterChainResult::Status::Allow};
}

FilterChainResult
HeadlessFilterChain::runResponseInterceptors(const InterceptorSnapshot& snapshot,
                                             Http::ResponseHeaderMap& headers,
                                             const std::string& cluster_name,
                                             InterceptorPhase phase) {
  for (const auto& interceptor : snapshot) {
    if (interceptor.on_response) {
      if (!interceptor.on_response(headers, cluster_name, phase)) {
        ENVOY_LOG(debug, "client: interceptor '{}' denied response (phase {})", interceptor.name,
                  static_cast<uint32_t>(phase));
        return FilterChainResult{FilterChainResult::Status::Deny, 403};
      }
    }
  }
  return FilterChainResult{FilterChainResult::Status::Allow};
}

void HeadlessFilterChain::applyRequestFilters(const std::string& cluster_name,
                                              Http::RequestHeaderMapPtr headers,
                                              RequestFilterCompletionCb cb) {
  // Snapshot interceptors so that add/removeInterceptor mid-request doesn't
  // affect this call's pipeline.
  InterceptorSnapshot snapshot;
  {
    absl::ReaderMutexLock lock(&interceptors_mutex_);
    snapshot = interceptors_;
  }

  // PRE_REQUEST interceptors.
  FilterChainResult pre =
      runRequestInterceptors(snapshot, *headers, cluster_name, InterceptorPhase::PreRequest);
  if (pre.status == FilterChainResult::Status::Deny) {
    cb(pre, std::move(headers));
    return;
  }

  if (!filter_factories_.empty()) {
    // Wrap the user callback to run POST_REQUEST interceptors after the native chain.
    auto wrap_cb = [snapshot, cluster_name, original_cb = std::move(cb),
                    this](FilterChainResult result,
                          Http::RequestHeaderMapPtr finished_headers) mutable {
      if (result.status == FilterChainResult::Status::Allow) {
        result = runRequestInterceptors(snapshot, *finished_headers, cluster_name,
                                        InterceptorPhase::PostRequest);
      }
      original_cb(result, std::move(finished_headers));
    };

    // ActiveRequest keeps itself alive via self_ until complete() is called.
    auto req = std::make_shared<ActiveRequest>(*this, cluster_name, std::move(headers),
                                               std::move(wrap_cb));
    req->activate(); // Must be done before runFilters() so self_ is set.
    req->buildFilterChain();
    req->runFilters();
    // req goes out of scope here but ActiveRequest::self_ keeps the object alive
    // until the filter chain completes (synchronously or via continueDecoding()).
  } else {
    // No native filters: run POST_REQUEST interceptors immediately.
    FilterChainResult post =
        runRequestInterceptors(snapshot, *headers, cluster_name, InterceptorPhase::PostRequest);
    cb(post, std::move(headers));
  }
}

void HeadlessFilterChain::applyResponseFilters(const std::string& cluster_name,
                                               Http::ResponseHeaderMapPtr headers,
                                               ResponseFilterCompletionCb cb) {
  InterceptorSnapshot snapshot;
  {
    absl::ReaderMutexLock lock(&interceptors_mutex_);
    snapshot = interceptors_;
  }

  FilterChainResult pre =
      runResponseInterceptors(snapshot, *headers, cluster_name, InterceptorPhase::PreResponse);
  if (pre.status == FilterChainResult::Status::Deny) {
    cb(pre, std::move(headers));
    return;
  }

  // (Native encoder filter chain is Phase 3.)

  FilterChainResult post =
      runResponseInterceptors(snapshot, *headers, cluster_name, InterceptorPhase::PostResponse);
  cb(post, std::move(headers));
}

} // namespace Client
} // namespace Envoy
