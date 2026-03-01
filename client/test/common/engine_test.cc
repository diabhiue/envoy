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
//
// These tests do not call run(), so they never create the Envoy server runtime
// or initialise process-wide singletons. They are safe to run independently.
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
// Running-engine lifecycle tests
//
// All tests that require a fully-started engine are combined into a single
// TEST() to avoid the global-state corruption that arises when multiple
// ClientEngine instances each initialise and tear down the Envoy server
// runtime (protobuf arenas, libevent, etc.) within the same process.
// ---------------------------------------------------------------------------

TEST(ClientEngineTest, RunningEngineLifecycle) {
  auto options = std::make_shared<Envoy::OptionsImplBase>();
  options->setConfigYaml(std::string(kMinimalBootstrap));
  options->setDrainStrategy(Server::DrainStrategy::Immediate);
  options->setSignalHandling(false);

  ClientEngine engine(options);
  ASSERT_TRUE(engine.run());

  // --- WaitReady with zero duration ---
  // Should return without blocking regardless of whether the engine is ready.
  engine.waitReady(absl::ZeroDuration());

  // --- Full WaitReady ---
  // The static cluster triggers PostInit quickly; 10 s is generous.
  ASSERT_TRUE(engine.waitReady(absl::Seconds(10)));
  EXPECT_FALSE(engine.isTerminated());

  // --- ConfigStore accessible after ready ---
  // Verify configStore() returns a valid reference. resolve() requires the
  // caller to be an Envoy-registered thread; that is exercised by the Go and
  // CC integration tests which run on the dispatcher or use mocked clusters.
  EXPECT_NO_THROW(engine.configStore());

  // --- Terminate ---
  engine.terminate();
  EXPECT_TRUE(engine.isTerminated());

  // --- Idempotent second terminate ---
  engine.terminate();
}

} // namespace
} // namespace Client
} // namespace Envoy
