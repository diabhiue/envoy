#include "client/library/cc/client.h"

#include "source/server/options_impl_base.h"

#include "source/common/http/header_map_impl.h"

#include "client/library/common/engine.h"
#include "client/library/common/lb_context.h"

#include "absl/synchronization/notification.h"

namespace EnvoyClient {

namespace {
Endpoint fromInternal(const Envoy::Client::EndpointInfo& info) {
  return Endpoint{info.address, info.port, info.weight, info.priority, info.health_status};
}
} // namespace

Client::Client(std::unique_ptr<Envoy::Client::ClientEngineInterface> engine)
    : engine_(std::move(engine)) {}

Client::~Client() { shutdown(); }

std::unique_ptr<Client> Client::create(const std::string& bootstrap_yaml) {
  auto options = std::make_shared<Envoy::OptionsImplBase>();
  // Set the config YAML. The options will parse it into a Bootstrap proto.
  options->setConfigYaml(bootstrap_yaml);
  // We don't want the server to bind to any listeners — this is a client library.
  options->setDrainStrategy(Envoy::Server::DrainStrategy::Immediate);

  auto engine = std::make_unique<Envoy::Client::ClientEngine>(options);
  if (!engine->run()) {
    return nullptr;
  }

  return std::unique_ptr<Client>(
      new Client(std::unique_ptr<Envoy::Client::ClientEngineInterface>(std::move(engine))));
}

bool Client::waitReady(int timeout_seconds) {
  return engine_->waitReady(absl::Seconds(timeout_seconds));
}

std::vector<Endpoint> Client::resolve(const std::string& cluster_name) {
  // ConfigStore::resolve() calls ClusterManager::getThreadLocalCluster(), which
  // requires the calling thread to be registered with Envoy TLS. External threads
  // (e.g. Go goroutines) are not registered, so we dispatch to the engine's main
  // dispatcher thread, which is always TLS-registered.
  std::vector<Envoy::Client::EndpointInfo> internal_endpoints;
  bool found = false;
  engine_->runOnDispatcherAndWait([&]() {
    found = engine_->configStore().resolve(cluster_name, internal_endpoints);
  });
  if (!found) {
    return {};
  }

  std::vector<Endpoint> result;
  result.reserve(internal_endpoints.size());
  for (const auto& ep : internal_endpoints) {
    result.push_back(fromInternal(ep));
  }
  return result;
}

absl::optional<Endpoint> Client::pickEndpoint(const std::string& cluster_name,
                                              const RequestContext& ctx) {
  // Same TLS constraint as resolve() — dispatch to the engine's main dispatcher.
  Envoy::Client::ClientLoadBalancerContext lb_ctx(ctx.hash_key, ctx.override_host,
                                                  ctx.override_host_strict, ctx.path,
                                                  ctx.authority);
  absl::optional<Envoy::Client::EndpointInfo> result;
  engine_->runOnDispatcherAndWait([&]() {
    result = engine_->configStore().pickEndpoint(cluster_name, &lb_ctx);
  });
  if (!result.has_value()) {
    return absl::nullopt;
  }
  return fromInternal(result.value());
}

void Client::setClusterLbPolicy(const std::string& cluster_name, const std::string& lb_policy) {
  engine_->configStore().setClusterLbPolicy(cluster_name, lb_policy);
}

void Client::setDefaultLbPolicy(const std::string& lb_policy) {
  engine_->configStore().setDefaultLbPolicy(lb_policy);
}

void Client::watchConfig(const std::string& resource_type,
                         std::function<void(const ConfigEvent&)> callback) {
  engine_->configStore().watchConfig(
      resource_type,
      [callback = std::move(callback)](const std::string& type, const std::string& name,
                                       Envoy::Client::ConfigEvent event) {
        ConfigEvent ce;
        ce.resource_type = type;
        ce.resource_name = name;
        switch (event) {
        case Envoy::Client::ConfigEvent::Added:
          ce.event = "added";
          break;
        case Envoy::Client::ConfigEvent::Updated:
          ce.event = "updated";
          break;
        case Envoy::Client::ConfigEvent::Removed:
          ce.event = "removed";
          break;
        }
        callback(ce);
      });
}

void Client::reportResult(const std::string& address, uint32_t port, uint32_t status_code,
                          uint64_t latency_ms) {
  engine_->configStore().reportResult(address, port, status_code, latency_ms);
}

void Client::shutdown() {
  if (engine_ && !engine_->isTerminated()) {
    engine_->terminate();
  }
}

void Client::addInterceptor(Envoy::Client::ClientInterceptor interceptor) {
  auto* fc = engine_->filterChain();
  if (fc != nullptr) {
    fc->addInterceptor(std::move(interceptor));
  }
}

void Client::removeInterceptor(const std::string& name) {
  auto* fc = engine_->filterChain();
  if (fc != nullptr) {
    fc->removeInterceptor(name);
  }
}

Status Client::applyRequestFilters(const std::string& cluster_name,
                                   Http::RequestHeaderMap& headers) {
  auto* fc = engine_->filterChain();
  if (fc == nullptr) {
    return Status::Ok;
  }

  // Clone the headers so applyRequestFilters can take ownership via shared_ptr.
  auto owned = Http::RequestHeaderMapImpl::create();
  headers.iterate([&owned](const Http::HeaderEntry& entry) {
    owned->addCopy(Http::LowerCaseString(entry.key().getStringView()),
                   entry.value().getStringView());
    return Http::HeaderMap::Iterate::Continue;
  });

  // Run the filter chain on the dispatcher thread and wait for completion.
  Envoy::Client::FilterChainResult result;
  Http::RequestHeaderMapPtr result_headers;
  absl::Notification done;

  engine_->runOnDispatcherAndWait([&]() {
    fc->applyRequestFilters(
        cluster_name, std::move(owned),
        [&](Envoy::Client::FilterChainResult r, Http::RequestHeaderMapPtr h) {
          result = r;
          result_headers = std::move(h);
          done.Notify();
        });
  });

  done.WaitForNotification();

  if (result.status == Envoy::Client::FilterChainResult::Status::Allow && result_headers) {
    // Write back the (potentially modified) headers.
    headers.clear();
    result_headers->iterate([&headers](const Http::HeaderEntry& entry) {
      headers.addCopy(Http::LowerCaseString(entry.key().getStringView()),
                      entry.value().getStringView());
      return Http::HeaderMap::Iterate::Continue;
    });
    return Status::Ok;
  }
  return Status::Denied;
}

Status Client::applyResponseFilters(const std::string& cluster_name,
                                    Http::ResponseHeaderMap& headers) {
  auto* fc = engine_->filterChain();
  if (fc == nullptr) {
    return Status::Ok;
  }

  auto owned = Http::ResponseHeaderMapImpl::create();
  headers.iterate([&owned](const Http::HeaderEntry& entry) {
    owned->addCopy(Http::LowerCaseString(entry.key().getStringView()),
                   entry.value().getStringView());
    return Http::HeaderMap::Iterate::Continue;
  });

  Envoy::Client::FilterChainResult result;
  Http::ResponseHeaderMapPtr result_headers;
  absl::Notification done;

  engine_->runOnDispatcherAndWait([&]() {
    fc->applyResponseFilters(
        cluster_name, std::move(owned),
        [&](Envoy::Client::FilterChainResult r, Http::ResponseHeaderMapPtr h) {
          result = r;
          result_headers = std::move(h);
          done.Notify();
        });
  });

  done.WaitForNotification();

  if (result.status == Envoy::Client::FilterChainResult::Status::Allow && result_headers) {
    headers.clear();
    result_headers->iterate([&headers](const Http::HeaderEntry& entry) {
      headers.addCopy(Http::LowerCaseString(entry.key().getStringView()),
                      entry.value().getStringView());
      return Http::HeaderMap::Iterate::Continue;
    });
    return Status::Ok;
  }
  return Status::Denied;
}

} // namespace EnvoyClient
