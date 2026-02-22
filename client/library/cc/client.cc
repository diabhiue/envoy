#include "client/library/cc/client.h"

#include "source/server/options_impl_base.h"

#include "client/library/common/engine.h"

namespace EnvoyClient {

namespace {
Endpoint fromInternal(const Envoy::Client::EndpointInfo& info) {
  return Endpoint{info.address, info.port, info.weight, info.priority, info.health_status};
}
} // namespace

Client::Client(std::unique_ptr<Envoy::Client::ClientEngine> engine)
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

  return std::unique_ptr<Client>(new Client(std::move(engine)));
}

bool Client::waitReady(int timeout_seconds) {
  return engine_->waitReady(absl::Seconds(timeout_seconds));
}

std::vector<Endpoint> Client::resolve(const std::string& cluster_name) {
  std::vector<Envoy::Client::EndpointInfo> internal_endpoints;
  if (!engine_->configStore().resolve(cluster_name, internal_endpoints)) {
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
  // TODO(Phase 1): Implement ClientLoadBalancerContext that maps RequestContext fields
  // (override_host, hash_key, metadata) to Envoy's LoadBalancerContext interface.
  // For now, we use a null context which uses the default LB behavior.
  auto result = engine_->configStore().pickEndpoint(cluster_name, nullptr);
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

void Client::shutdown() {
  if (engine_ && !engine_->isTerminated()) {
    engine_->terminate();
  }
}

} // namespace EnvoyClient
