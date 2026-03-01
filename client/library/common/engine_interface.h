#pragma once

#include <functional>

#include "absl/time/time.h"

#include "client/library/common/config_store.h"

namespace Envoy {
namespace Client {

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
   * Run fn on the engine's main dispatcher thread, blocking until fn completes.
   *
   * ConfigStore::resolve() and ConfigStore::pickEndpoint() call
   * ClusterManager::getThreadLocalCluster(), which requires the calling thread
   * to be registered with Envoy TLS. When called from an external thread (e.g.
   * a Go goroutine), this method dispatches fn to the registered dispatcher
   * thread and waits for the result.
   *
   * The default implementation calls fn() directly, which is correct for test
   * engines that use MockClusterManager (no TLS registration required).
   */
  virtual void runOnDispatcherAndWait(std::function<void()> fn) { fn(); }
};

} // namespace Client
} // namespace Envoy
