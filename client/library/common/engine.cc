#include "client/library/common/engine.h"

#include "source/common/common/posix/thread_impl.h"
#include "source/common/common/random_generator.h"
#include "source/common/runtime/runtime_impl.h"
#include "source/server/null_overload_manager.h"

namespace Envoy {
namespace Client {

/**
 * ServerClientLite is the lightweight Server::Instance for the client library.
 *
 * Compared to a full Envoy server:
 * - No heap shrinker (not needed for a library)
 * - Null overload manager (the app manages its own resources)
 * - No guard dog (no watchdog thread)
 * - No HDS delegate (no health discovery service delegation)
 *
 * The ClusterManager IS present — it manages:
 * - xDS subscriptions (CDS/EDS) for app upstream endpoint discovery
 * - Connections to external filter services (ext_authz, JWKS, OAuth — Phase 3)
 */
class ServerClientLite : public Server::InstanceBase {
public:
  using Server::InstanceBase::InstanceBase;

  void maybeCreateHeapShrinker() override {
    // No heap shrinker needed for a client library.
  }

  absl::StatusOr<std::unique_ptr<Server::OverloadManager>> createOverloadManager() override {
    return std::make_unique<Server::NullOverloadManager>(threadLocal(), true);
  }

  std::unique_ptr<Server::OverloadManager> createNullOverloadManager() override {
    return std::make_unique<Server::NullOverloadManager>(threadLocal(), true);
  }

  std::unique_ptr<Server::GuardDog> maybeCreateGuardDog(absl::string_view) override {
    return nullptr;
  }

  std::unique_ptr<Server::HdsDelegateApi>
  maybeCreateHdsDelegate(Server::Configuration::ServerFactoryContext&, Stats::Scope&,
                         Grpc::RawAsyncClientPtr&&, Stats::Store&,
                         Ssl::ContextManager&) override {
    return nullptr;
  }
};

ClientEngine::ClientEngine(std::shared_ptr<Envoy::OptionsImplBase> options)
    : options_(std::move(options)) {
  // Disable signal handling — the application owns signal handling, not the library.
  options_->setSignalHandling(false);
}

ClientEngine::~ClientEngine() {
  if (!terminated_) {
    terminate();
  }
}

bool ClientEngine::run() {
  auto thread_factory = Thread::PosixThreadFactory::create();
  main_thread_ =
      thread_factory->createThread([this]() -> void { main(); }, /* options= */ {},
                                   /* crash_on_failure= */ false);
  return main_thread_ != nullptr;
}

bool ClientEngine::waitReady(absl::Duration timeout) {
  return engine_running_.WaitForNotificationWithTimeout(timeout);
}

void ClientEngine::runOnDispatcherAndWait(std::function<void()> fn) {
  if (server_ == nullptr) {
    fn();
    return;
  }
  // Check whether we are already on the engine's background thread.
  // We cannot use disp.isThreadSafe() here because isThreadSafe() returns
  // true for ALL threads before the event loop starts (run_tid_.isEmpty()),
  // which would incorrectly call fn() directly on an unregistered external
  // thread (e.g. a Go goroutine) while the event loop is still starting up.
  if (pthread_equal(pthread_self(), engine_pthread_)) {
    // Already on the engine thread (TLS-registered) — call directly.
    fn();
  } else {
    // External thread (e.g. a Go goroutine): post to the dispatcher and block.
    absl::Notification done;
    server_->dispatcher().post([&fn, &done]() {
      fn();
      done.Notify();
    });
    done.WaitForNotification();
  }
}

void ClientEngine::terminate() {
  if (terminated_) {
    return;
  }
  terminated_ = true;

  if (main_thread_ != nullptr) {
    // Wait until the background thread has assigned server_ (or failed to do
    // so). Without this barrier, terminate() may read a null server_ while
    // main() is still inside base_->init(), causing join() to block forever
    // because no shutdown is ever posted.
    server_started_.WaitForNotification();
  }

  if (server_ != nullptr) {
    // Post shutdown to the engine's dispatcher thread.
    server_->dispatcher().post([this]() { server_->shutdown(); });
  }

  if (main_thread_ != nullptr) {
    main_thread_->join();
    main_thread_.reset();
  }
}

void ClientEngine::main() {
  // Record the engine thread's pthread ID so that runOnDispatcherAndWait()
  // can distinguish between calls from the engine thread and external threads.
  engine_pthread_ = pthread_self();

  StrippedMainBase::CreateInstanceFunction create_instance =
      [](Init::Manager& init_manager, const Server::Options& options,
         Event::TimeSystem& time_system, ListenerHooks& hooks, Server::HotRestart& restarter,
         Stats::StoreRoot& store, Thread::BasicLockable& access_log_lock,
         Server::ComponentFactory& component_factory, Random::RandomGeneratorPtr&& random_generator,
         ThreadLocal::Instance& tls, Thread::ThreadFactory& thread_factory,
         Filesystem::Instance& file_system, std::unique_ptr<ProcessContext> process_context,
         Buffer::WatermarkFactorySharedPtr watermark_factory) {
        auto local_address = Network::Utility::getLocalAddress(options.localAddressIpVersion());
        auto server = std::make_unique<ServerClientLite>(
            init_manager, options, time_system, hooks, restarter, store, access_log_lock,
            std::move(random_generator), tls, thread_factory, file_system,
            std::move(process_context), watermark_factory);
        server->initialize(local_address, component_factory);
        return server;
      };

  auto random_generator = std::make_unique<Random::RandomGeneratorImpl>();
  base_ = std::make_unique<StrippedMainBase>(*options_, prod_component_factory_,
                                             std::make_unique<PlatformImpl>(), *random_generator);
  base_->init(real_time_system_, default_listener_hooks_, std::move(random_generator), nullptr,
              create_instance);

  server_ = base_->server();
  // Notify terminate() that server_ is now safe to dereference (or is
  // definitively null if init failed). This prevents a race where terminate()
  // is called before the background thread has assigned server_.
  server_started_.Notify();

  if (server_ == nullptr) {
    return;
  }

  // Create the ConfigStore backed by ClusterManager.
  config_store_ = std::make_unique<ConfigStore>(server_->clusterManager());

  // Register a keepalive TLS slot to prevent worker dispatch loops from exiting
  // before shutdownGlobalThreading() is called.
  //
  // Workers are created (and their dispatchers registered with TLS) during
  // server initialize(), but their threads don't start until startWorkers()
  // fires inside runServer(). Setting this TLS slot now queues a keepalive
  // initializer in each worker's dispatcher. When a worker thread starts, it
  // processes this initializer and creates a long-lived timer. The timer keeps
  // the RunType::Block event loop alive until the explicit dispatcher->exit()
  // call that arrives from WorkerImpl::stop() during shutdown.
  worker_keepalive_slot_ = server_->threadLocal().allocateSlot();
  worker_keepalive_slot_->set([](Event::Dispatcher& dispatcher)
                                  -> ThreadLocal::ThreadLocalObjectSharedPtr {
    struct KeepaliveState : public ThreadLocal::ThreadLocalObject {
      explicit KeepaliveState(Event::TimerPtr t) : timer_(std::move(t)) {}
      Event::TimerPtr timer_;
    };
    // One-hour timer: long enough for any test or production use, short enough
    // that it doesn't hold resources indefinitely if something goes wrong.
    // WorkerImpl::stop() calls dispatcher->exit() which exits the event loop
    // regardless of pending timers, so the timer duration is not a bottleneck.
    auto timer = dispatcher.createTimer([]() {});
    timer->enableTimer(std::chrono::hours(1));
    return std::make_shared<KeepaliveState>(std::move(timer));
  });

  {
    // Register a post-init callback to notify waiters that the engine is ready.
    // PostInit fires after initial xDS config has been received and clusters are warming.
    //
    // Scope postinit_handle so it is destroyed before base_.reset():
    // The handle's destructor deregisters the callback from the server's lifecycle
    // notifier. If postinit_handle outlived base_, it would try to deregister from
    // an already-destroyed notifier, causing a use-after-free / double-free.
    auto postinit_handle = server_->lifecycleNotifier().registerCallback(
        Server::ServerLifecycleNotifier::Stage::PostInit, [this]() { engine_running_.Notify(); });

    // Run the event loop (blocks until shutdown).
    base_->runServer();
  } // postinit_handle deregistered here, while the server lifecycle notifier is still alive.

  // Event loop exited — clean up.
  config_store_.reset();
  worker_keepalive_slot_.reset();
  base_.reset();
  server_ = nullptr;
}

} // namespace Client
} // namespace Envoy
