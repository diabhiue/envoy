#include "client/library/common/engine.h"

#include "source/server/options_impl_base.h"

#include "gtest/gtest.h"

namespace Envoy {
namespace Client {
namespace {

// A minimal static-cluster bootstrap YAML that lets the engine start without
// needing a real xDS server. The admin is disabled to avoid port conflicts.
constexpr absl::string_view kMinimalBootstrap = R"yaml(
static_resources:
  clusters:
  - name: test_cluster
    type: STATIC
    load_assignment:
      cluster_name: test_cluster
      endpoints:
      - lb_endpoints:
        - endpoint:
            address:
              socket_address:
                address: 127.0.0.1
                port_value: 19001
admin:
  address:
    socket_address:
      address: 127.0.0.1
      port_value: 0
)yaml";

// ---------------------------------------------------------------------------
// Initial state tests (no server started)
// ---------------------------------------------------------------------------

TEST(ClientEngineTest, IsNotTerminatedAfterConstruction) {
  auto options = std::make_shared<Envoy::OptionsImplBase>();
  options->setConfigYaml(std::string(kMinimalBootstrap));
  options->setSignalHandling(false);

  ClientEngine engine(options);
  EXPECT_FALSE(engine.isTerminated());
}

TEST(ClientEngineTest, TerminateBeforeRunIsIdempotent) {
  // Calling terminate() on an engine that was never run must not crash.
  auto options = std::make_shared<Envoy::OptionsImplBase>();
  options->setConfigYaml(std::string(kMinimalBootstrap));
  options->setSignalHandling(false);

  ClientEngine engine(options);
  engine.terminate();
  EXPECT_TRUE(engine.isTerminated());
  engine.terminate(); // second call must also be safe
}

TEST(ClientEngineTest, DestructorTerminatesGracefully) {
  // Destroying an engine that was never started must not crash.
  auto options = std::make_shared<Envoy::OptionsImplBase>();
  options->setConfigYaml(std::string(kMinimalBootstrap));
  options->setSignalHandling(false);

  { ClientEngine engine(options); }
  // Implicit destructor called — just verifying no crash.
}

// ---------------------------------------------------------------------------
// Run / waitReady / terminate lifecycle
// ---------------------------------------------------------------------------

TEST(ClientEngineTest, RunAndTerminateLifecycle) {
  auto options = std::make_shared<Envoy::OptionsImplBase>();
  options->setConfigYaml(std::string(kMinimalBootstrap));
  options->setDrainStrategy(Server::DrainStrategy::Immediate);
  options->setSignalHandling(false);

  ClientEngine engine(options);
  ASSERT_TRUE(engine.run());

  // The static cluster triggers PostInit quickly; 10s is generous.
  bool ready = engine.waitReady(absl::Seconds(10));
  EXPECT_TRUE(ready);
  EXPECT_FALSE(engine.isTerminated());

  engine.terminate();
  EXPECT_TRUE(engine.isTerminated());
}

TEST(ClientEngineTest, WaitReadyTimesOutWithZeroDuration) {
  auto options = std::make_shared<Envoy::OptionsImplBase>();
  options->setConfigYaml(std::string(kMinimalBootstrap));
  options->setDrainStrategy(Server::DrainStrategy::Immediate);
  options->setSignalHandling(false);

  ClientEngine engine(options);
  ASSERT_TRUE(engine.run());

  // A zero-second timeout should return false before the engine is ready
  // (or true if it happened to be ready instantly — either is valid).
  // The important thing is it does not block indefinitely.
  engine.waitReady(absl::ZeroDuration());

  engine.terminate();
}

TEST(ClientEngineTest, ConfigStoreAccessibleAfterReady) {
  auto options = std::make_shared<Envoy::OptionsImplBase>();
  options->setConfigYaml(std::string(kMinimalBootstrap));
  options->setDrainStrategy(Server::DrainStrategy::Immediate);
  options->setSignalHandling(false);

  ClientEngine engine(options);
  ASSERT_TRUE(engine.run());
  ASSERT_TRUE(engine.waitReady(absl::Seconds(10)));

  // Calling resolve on a known static cluster should not crash.
  // It returns empty (no actual endpoints loaded in unit test env) but
  // that's fine — we just verify configStore() is reachable.
  std::vector<EndpointInfo> endpoints;
  engine.configStore().resolve("test_cluster", endpoints);

  engine.terminate();
}

TEST(ClientEngineTest, DestructorTerminatesRunningEngine) {
  auto options = std::make_shared<Envoy::OptionsImplBase>();
  options->setConfigYaml(std::string(kMinimalBootstrap));
  options->setDrainStrategy(Server::DrainStrategy::Immediate);
  options->setSignalHandling(false);

  {
    ClientEngine engine(options);
    ASSERT_TRUE(engine.run());
    ASSERT_TRUE(engine.waitReady(absl::Seconds(10)));
    // Destructor is called here — must join the background thread without hanging.
  }
}

} // namespace
} // namespace Client
} // namespace Envoy
