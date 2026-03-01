#pragma once

#include "envoy/event/timer.h"
#include "envoy/server/instance.h"
#include "envoy/thread_local/thread_local.h"

#include "source/common/common/logger.h"
#include "source/common/common/thread.h"
#include "source/common/event/real_time_system.h"
#include "source/exe/platform_impl.h"
#include "source/exe/stripped_main_base.h"
#include "source/server/listener_hooks.h"
#include "source/server/options_impl_base.h"

#include "client/library/common/config_store.h"
#include "client/library/common/engine_interface.h"

#include "source/common/common/posix/thread_impl.h"

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
class ClientEngine : public ClientEngineInterface, public Logger::Loggable<Logger::Id::client> {
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
  bool waitReady(absl::Duration timeout = absl::Seconds(30)) override;

  /**
   * Terminate the engine. Blocks until the background thread exits.
   */
  void terminate() override;

  /**
   * @return true if the engine has been terminated.
   */
  bool isTerminated() const override { return terminated_; }

  /**
   * @return the ConfigStore for endpoint resolution and LB decisions.
   * Only valid after waitReady() returns true.
   */
  ConfigStore& configStore() override {
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

  /**
   * Run fn on the engine's main dispatcher thread, blocking until fn completes.
   * If already on the dispatcher thread, fn is called directly.
   */
  void runOnDispatcherAndWait(std::function<void()> fn) override;

private:
  void main();

  std::shared_ptr<Envoy::OptionsImplBase> options_;
  Event::RealTimeSystem real_time_system_; // NO_CHECK_FORMAT(real_time)
  DefaultListenerHooks default_listener_hooks_;
  ProdComponentFactory prod_component_factory_;

  std::unique_ptr<StrippedMainBase> base_;
  Server::Instance* server_{nullptr};
  std::unique_ptr<ConfigStore> config_store_;

  // Per-worker keepalive: prevents worker dispatch loops from exiting before
  // shutdownGlobalThreading() is called. Workers use RunType::Block (libevent
  // flag=0), which exits the event loop when there are no registered events.
  // Without listeners, workers have no persistent events. Keeping a long-lived
  // timer registered on each worker dispatcher prevents the premature exit that
  // would otherwise trigger ASSERT(shutdown_) in ThreadLocalImpl::shutdownThread.
  ThreadLocal::SlotPtr worker_keepalive_slot_;

  // Background thread
  Thread::PosixThreadPtr main_thread_{nullptr};
  // pthread ID of the engine's background thread. Set at the start of main()
  // and used by runOnDispatcherAndWait() to detect calls from the engine thread.
  // We compare against this rather than disp.isThreadSafe() because isThreadSafe()
  // returns true for ANY thread before the event loop starts (run_tid_.isEmpty()).
  pthread_t engine_pthread_{};
  // Notified once server_ has been assigned (or definitively left null due to
  // init failure). terminate() waits on this before dereferencing server_.
  absl::Notification server_started_;
  absl::Notification engine_running_;
  bool terminated_{false};
  Thread::MutexBasicLockable mutex_;
};

} // namespace Client
} // namespace Envoy
