#include "client/library/common/engine.h"

#include "source/common/common/posix/thread_impl.h"
#include "source/common/common/random_generator.h"
#include "source/common/http/header_map_impl.h"
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

void ClientEngine::terminate() {
  if (terminated_) {
    return;
  }
  terminated_ = true;

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
  ASSERT(server_ != nullptr);

  // Create the ConfigStore backed by ClusterManager.
  config_store_ = std::make_unique<ConfigStore>(server_->clusterManager());

  // Create the UpstreamRequestManager for Phase 3 upstream connection ownership.
  upstream_request_manager_ = std::make_unique<UpstreamRequestManager>(
      server_->clusterManager(), server_->dispatcher());

  // Register a post-init callback to notify waiters that the engine is ready.
  // PostInit fires after initial xDS config has been received and clusters are warming.
  auto postinit_handle = server_->lifecycleNotifier().registerCallback(
      Server::ServerLifecycleNotifier::Stage::PostInit, [this]() { engine_running_.Notify(); });

  // Run the event loop (blocks until shutdown).
  base_->runServer();

  // Event loop exited — clean up in reverse construction order.
  upstream_request_manager_.reset();
  config_store_.reset();
  base_.reset();
  server_ = nullptr;
}

uint64_t ClientEngine::sendRequest(
    const std::string& cluster_name,
    const std::vector<std::pair<std::string, std::string>>& headers_vec,
    const std::string& body, UpstreamResponseCallback callback) {
  ASSERT(upstream_request_manager_ != nullptr);

  // Convert the plain-string header pairs to an Envoy RequestHeaderMap.
  auto headers = Http::RequestHeaderMapImpl::create();
  for (const auto& [key, value] : headers_vec) {
    headers->addCopy(Http::LowerCaseString(key), value);
  }

  return upstream_request_manager_->sendRequest(cluster_name, std::move(headers), body,
                                                std::move(callback));
}

void ClientEngine::cancelRequest(uint64_t request_id) {
  ASSERT(upstream_request_manager_ != nullptr);
  upstream_request_manager_->cancelRequest(request_id);
}

} // namespace Client
} // namespace Envoy
