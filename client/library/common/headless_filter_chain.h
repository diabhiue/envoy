#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "envoy/event/dispatcher.h"
#include "envoy/http/filter.h"
#include "envoy/http/filter_factory.h"
#include "envoy/http/header_map.h"
#include "envoy/upstream/cluster_manager.h"

#include "source/common/common/logger.h"

#include "absl/synchronization/mutex.h"

namespace Envoy {
namespace Client {

/**
 * Result of running the filter chain for one request or response.
 */
struct FilterChainResult {
  enum class Status {
    Allow,  // All filters allowed the request.
    Deny,   // A filter denied the request.
  };
  Status status{Status::Allow};
  uint32_t deny_code{403}; // HTTP status code sent by the denying filter.
};

/**
 * Interceptor phases passed to the interceptor callback.
 */
enum class InterceptorPhase : uint32_t {
  PreRequest = 0,
  PostRequest = 1,
  PreResponse = 2,
  PostResponse = 3,
};

/**
 * Controls how client-injected native filters are ordered relative to
 * server-pushed native filters (from LDS/ECDS) in the filter chain.
 *
 * | Policy             | Execution order                          |
 * |--------------------|------------------------------------------|
 * | ClientBeforeServer | client filters → server filters          |
 * | ServerBeforeClient | server filters → client filters          |
 * | ClientWrapsServer  | client filters → server filters → client |
 *                        (same as ClientBeforeServer until server  |
 *                         filters are pushed via LDS/ECDS)        |
 * | ClientOnly         | only client filters (server ignored)     |
 * | ServerOnly         | only server filters (client ignored)     |
 *
 * The default is ClientWrapsServer, which matches the interceptor behaviour
 * (pre-request interceptors already run before the native chain, post-request
 * interceptors after).  For native filters ClientWrapsServer is identical to
 * ClientBeforeServer until a separate "post-server" client filter group is
 * introduced.
 */
enum class FilterMergePolicy {
  ClientWrapsServer,   // default
  ClientBeforeServer,
  ServerBeforeClient,
  ClientOnly,
  ServerOnly,
};

/**
 * A client interceptor.
 *
 * An interceptor is a lightweight C++ hook that runs before and after the
 * Envoy filter chain for each request/response. Unlike full Envoy filters,
 * interceptors are synchronous and work on plain header maps.
 *
 * Return true to continue; return false to deny the request.
 */
struct ClientInterceptor {
  std::string name;
  // Called for request phases (PreRequest, PostRequest). May be null.
  std::function<bool(Http::RequestHeaderMap&, const std::string& cluster, InterceptorPhase)>
      on_request;
  // Called for response phases (PreResponse, PostResponse). May be null.
  std::function<bool(Http::ResponseHeaderMap&, const std::string& cluster, InterceptorPhase)>
      on_response;
};

/**
 * Completion callback for applyRequestFilters / applyResponseFilters.
 * Called on the engine's dispatcher thread when the filter chain completes.
 *
 * The header map pointer is guaranteed to be non-null on entry. Callers should
 * move or copy the headers before returning if they need them beyond the callback.
 */
using RequestFilterCompletionCb =
    std::function<void(FilterChainResult, Http::RequestHeaderMapPtr)>;
using ResponseFilterCompletionCb =
    std::function<void(FilterChainResult, Http::ResponseHeaderMapPtr)>;

/**
 * HeadlessFilterChain executes HTTP filters and client interceptors outside of
 * HttpConnectionManager.
 *
 * It provides the "filter layer" for the Envoy Client Library:
 *   1. PRE_REQUEST interceptors run first.
 *   2. Envoy native filter chain runs (added via addFilterFactory()).
 *   3. POST_REQUEST interceptors run after the filter chain.
 *
 * The filter chain supports headers-only mode (no body streaming). Async
 * filters are supported: a filter can return StopIteration and later call
 * continueDecoding() on its StreamDecoderFilterCallbacks to resume.
 *
 * Thread safety:
 *   - addInterceptor / removeInterceptor may be called from any thread.
 *   - addFilterFactory / addClientFilterFactory / setFilterMergePolicy must be
 *     called from the engine's dispatcher thread.
 *   - applyRequestFilters / applyResponseFilters must be called from the
 *     engine's dispatcher thread (because filters use the dispatcher).
 */
class HeadlessFilterChain : public Logger::Loggable<Logger::Id::client> {
public:
  HeadlessFilterChain(Event::Dispatcher& dispatcher,
                      Upstream::ClusterManager& cluster_manager,
                      TimeSource& time_source);

  ~HeadlessFilterChain();

  /**
   * Add a server-side native filter factory (e.g. from LDS/ECDS config).
   * Thread: must be called on the dispatcher thread.
   * @param name a human-readable name for logging.
   * @param factory the filter factory callback.
   */
  void addFilterFactory(const std::string& name, Http::FilterFactoryCb factory);

  /**
   * Add a client-side native filter factory (injected by the application via
   * addNativeFilter). Placement relative to server filters is controlled by
   * the active FilterMergePolicy.
   * Thread: must be called on the dispatcher thread.
   * @param name a human-readable name for logging.
   * @param factory the filter factory callback.
   */
  void addClientFilterFactory(const std::string& name, Http::FilterFactoryCb factory);

  /**
   * Set the merge policy that controls ordering of client vs server native
   * filters. Defaults to FilterMergePolicy::ClientWrapsServer.
   * Thread: must be called on the dispatcher thread.
   */
  void setFilterMergePolicy(FilterMergePolicy policy);

  /**
   * Add or replace a client interceptor. Safe to call from any thread.
   * If an interceptor with the same name exists it is replaced.
   * @param interceptor the interceptor to add.
   */
  void addInterceptor(ClientInterceptor interceptor);

  /**
   * Remove a previously registered interceptor by name. Safe to call from any thread.
   * @param name the name of the interceptor to remove.
   */
  void removeInterceptor(const std::string& name);

  /**
   * Run the request filter pipeline (interceptors + native filters) for a request.
   *
   * Called on the engine's dispatcher thread.  The callback is invoked on the
   * dispatcher thread when the pipeline completes.
   *
   * @param cluster_name the target cluster (used to fetch cluster info for filters).
   * @param headers the request headers to process. Ownership is transferred to
   *        the filter chain; the callback receives the (possibly modified) headers.
   * @param cb completion callback.
   */
  void applyRequestFilters(const std::string& cluster_name, Http::RequestHeaderMapPtr headers,
                           RequestFilterCompletionCb cb);

  /**
   * Run the response filter pipeline (interceptors) for a response.
   *
   * Called on the engine's dispatcher thread.
   *
   * @param cluster_name the source cluster.
   * @param headers the response headers to process.
   * @param cb completion callback.
   */
  void applyResponseFilters(const std::string& cluster_name, Http::ResponseHeaderMapPtr headers,
                            ResponseFilterCompletionCb cb);

  /**
   * @return the number of registered server-side native filter factories.
   */
  size_t filterFactoryCount() const { return filter_factories_.size(); }

  /**
   * @return the number of registered client-side native filter factories.
   */
  size_t clientFilterFactoryCount() const { return client_filter_factories_.size(); }

  /**
   * @return the number of registered interceptors.
   */
  size_t interceptorCount() const {
    absl::ReaderMutexLock lock(&interceptors_mutex_);
    return interceptors_.size();
  }

private:
  /**
   * Snapshot of the interceptor list taken at the start of each request so
   * that add/removeInterceptor mid-flight doesn't affect in-flight requests.
   */
  using InterceptorSnapshot = std::vector<ClientInterceptor>;

  /**
   * Run all interceptors for a given phase over request headers.
   * @return FilterChainResult::Allow if all interceptors returned true; Deny otherwise.
   */
  FilterChainResult runRequestInterceptors(const InterceptorSnapshot& snapshot,
                                           Http::RequestHeaderMap& headers,
                                           const std::string& cluster_name,
                                           InterceptorPhase phase);

  /**
   * Run all interceptors for a given phase over response headers.
   * @return FilterChainResult::Allow if all interceptors returned true; Deny otherwise.
   */
  FilterChainResult runResponseInterceptors(const InterceptorSnapshot& snapshot,
                                            Http::ResponseHeaderMap& headers,
                                            const std::string& cluster_name,
                                            InterceptorPhase phase);

  /**
   * ActiveRequest holds the state for a single in-flight applyRequestFilters call.
   * It implements StreamDecoderFilterCallbacks (and ScopeTrackedObject) to give
   * native Envoy filters the callbacks they require.
   */
  class ActiveRequest;

  Event::Dispatcher& dispatcher_;
  Upstream::ClusterManager& cluster_manager_;
  TimeSource& time_source_;

  // Native filter factories (decoder side). Accessed only from the dispatcher thread.
  struct FilterFactoryEntry {
    std::string name;
    Http::FilterFactoryCb factory;
  };
  // Server-side filter factories (from LDS/ECDS config).
  std::vector<FilterFactoryEntry> filter_factories_;
  // Client-side filter factories (injected via addClientFilterFactory / addNativeFilter).
  std::vector<FilterFactoryEntry> client_filter_factories_;
  // Controls how client and server filter factories are ordered. Dispatcher thread only.
  FilterMergePolicy merge_policy_{FilterMergePolicy::ClientWrapsServer};

  // Interceptors. Protected by interceptors_mutex_ for cross-thread add/remove.
  mutable absl::Mutex interceptors_mutex_;
  std::vector<ClientInterceptor> interceptors_ ABSL_GUARDED_BY(interceptors_mutex_);
};

} // namespace Client
} // namespace Envoy
