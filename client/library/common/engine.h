#pragma once

#include "envoy/server/instance.h"

#include "source/common/common/logger.h"
#include "source/common/common/thread.h"
#include "source/common/event/real_time_system.h"
#include "source/exe/platform_impl.h"
#include "source/exe/stripped_main_base.h"
#include "source/server/listener_hooks.h"
#include "source/server/options_impl_base.h"

#include "client/library/common/config_store.h"

#include "absl/synchronization/notification.h"

namespace Envoy {
namespace Client {

/**
 * ClientEngine is the core of the Envoy Client Library.
 *
 * It bootstraps a lightweight Envoy instance (ServerClientLite) on a dedicated thread,
 * subscribes to xDS for cluster/endpoint discovery, and exposes a ConfigStore for
 * endpoint resolution and LB decisions.
 *
 * Lifecycle:
 *   1. Construct with bootstrap config
 *   2. Call run() — starts the engine on a background thread
 *   3. Call waitReady() — blocks until initial xDS config is received
 *   4. Use configStore() for endpoint resolution / LB picks
 *   5. Call terminate() to shut down
 *
 * Thread model:
 *   - The engine runs Envoy's event loop on a dedicated background thread
 *   - configStore().resolve() and configStore().pickEndpoint() are safe to call
 *     from any thread (they read from thread-local cluster state)
 *   - Interceptors and filter callbacks run on the engine's Dispatcher thread
 */
class ClientEngine : public Logger::Loggable<Logger::Id::client> {
public:
  /**
   * Construct a client engine.
   * @param options the bootstrap options (contains parsed bootstrap YAML/JSON).
   */
  explicit ClientEngine(std::shared_ptr<Envoy::OptionsImplBase> options);

  ~ClientEngine();

  /**
   * Start the engine on a background thread.
   * @return true if the thread was created successfully.
   */
  bool run();

  /**
   * Block until the engine is ready (server initialized, initial xDS config received).
   * @param timeout maximum time to wait.
   * @return true if ready, false if timed out.
   */
  bool waitReady(absl::Duration timeout = absl::Seconds(30));

  /**
   * Terminate the engine. Blocks until the background thread exits.
   */
  void terminate();

  /**
   * @return true if the engine has been terminated.
   */
  bool isTerminated() const { return terminated_; }

  /**
   * @return the ConfigStore for endpoint resolution and LB decisions.
   * Only valid after waitReady() returns true.
   */
  ConfigStore& configStore() {
    ASSERT(config_store_ != nullptr);
    return *config_store_;
  }

  /**
   * @return the underlying Server instance (for advanced use only).
   * Only valid after waitReady() returns true.
   */
  Server::Instance& server() {
    ASSERT(server_ != nullptr);
    return *server_;
  }

  /**
   * @return the engine's Dispatcher for posting callbacks.
   */
  Event::Dispatcher& dispatcher() {
    ASSERT(server_ != nullptr);
    return server_->dispatcher();
  }

private:
  void main();

  std::shared_ptr<Envoy::OptionsImplBase> options_;
  Event::RealTimeSystem real_time_system_; // NO_CHECK_FORMAT(real_time)
  DefaultListenerHooks default_listener_hooks_;
  ProdComponentFactory prod_component_factory_;

  std::unique_ptr<StrippedMainBase> base_;
  Server::Instance* server_{nullptr};
  std::unique_ptr<ConfigStore> config_store_;

  // Background thread
  Thread::PosixThreadPtr main_thread_{nullptr};
  absl::Notification engine_running_;
  bool terminated_{false};
  Thread::MutexBasicLockable mutex_;
};

} // namespace Client
} // namespace Envoy
