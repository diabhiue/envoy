#include "client/library/cc/client.h"

#include "source/common/network/utility.h"

#include "test/mocks/upstream/cluster_manager.h"
#include "test/mocks/upstream/host.h"
#include "test/mocks/upstream/host_set.h"
#include "test/mocks/upstream/priority_set.h"
#include "test/mocks/upstream/thread_local_cluster.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using ::testing::_;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::ReturnRef;

namespace EnvoyClient {
namespace {

// ---------------------------------------------------------------------------
// MockClientEngine
// ---------------------------------------------------------------------------

class MockClientEngine : public Envoy::Client::ClientEngineInterface {
public:
  explicit MockClientEngine(Envoy::Upstream::ClusterManager& cm) : config_store_(cm) {}

  MOCK_METHOD(bool, waitReady, (absl::Duration timeout));
  MOCK_METHOD(void, terminate, ());
  MOCK_METHOD(bool, isTerminated, (), (const));

  Envoy::Client::ConfigStore& configStore() override { return config_store_; }

  // Expose the underlying ConfigStore so tests can set up state directly.
  Envoy::Client::ConfigStore& store() { return config_store_; }

private:
  Envoy::Client::ConfigStore config_store_;
};

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class ClientTest : public testing::Test {
public:
  ClientTest() {
    cm_.initializeThreadLocalClusters({"svc"});

    // NiceMock suppresses "uninteresting call" failures for mock methods
    // invoked by Client::~Client() → shutdown() → isTerminated()/terminate().
    auto engine = std::make_unique<NiceMock<MockClientEngine>>(cm_);
    engine_ = engine.get();
    client_ = Client::createForTesting(std::move(engine));
  }

  // Helper to add hosts to the mock priority set for "svc".
  std::shared_ptr<NiceMock<Envoy::Upstream::MockHost>> addHost(const std::string& ip,
                                                                uint32_t port,
                                                                uint32_t weight = 1,
                                                                uint32_t priority = 0) {
    auto host = std::make_shared<NiceMock<Envoy::Upstream::MockHost>>();
    auto address =
        *Envoy::Network::Utility::resolveUrl(fmt::format("tcp://{}:{}", ip, port));
    ON_CALL(*host, address()).WillByDefault(Return(address));
    ON_CALL(*host, weight()).WillByDefault(Return(weight));
    ON_CALL(*host, priority()).WillByDefault(Return(priority));
    ON_CALL(*host, coarseHealth())
        .WillByDefault(Return(Envoy::Upstream::Host::Health::Healthy));

    auto& priority_set = cm_.thread_local_cluster_.cluster_.priority_set_;
    auto* host_set = priority_set.getMockHostSet(0);
    host_set->hosts_.push_back(host);
    return host;
  }

  NiceMock<Envoy::Upstream::MockClusterManager> cm_;
  NiceMock<MockClientEngine>* engine_{nullptr}; // owned by client_
  std::unique_ptr<Client> client_;
};

// ---------------------------------------------------------------------------
// waitReady
// ---------------------------------------------------------------------------

TEST_F(ClientTest, WaitReadyDelegatesToEngine) {
  EXPECT_CALL(*engine_, waitReady(absl::Seconds(5))).WillOnce(Return(true));
  EXPECT_TRUE(client_->waitReady(5));
}

TEST_F(ClientTest, WaitReadyTimeout) {
  EXPECT_CALL(*engine_, waitReady(_)).WillOnce(Return(false));
  EXPECT_FALSE(client_->waitReady(1));
}

// ---------------------------------------------------------------------------
// resolve
// ---------------------------------------------------------------------------

TEST_F(ClientTest, ResolveUnknownClusterReturnsEmpty) {
  auto endpoints = client_->resolve("nonexistent");
  EXPECT_TRUE(endpoints.empty());
}

TEST_F(ClientTest, ResolveTranslatesEndpointFields) {
  addHost("10.0.0.1", 8080, /*weight=*/100, /*priority=*/0);
  addHost("10.0.0.2", 9090, /*weight=*/200, /*priority=*/1);

  auto endpoints = client_->resolve("svc");
  ASSERT_EQ(2u, endpoints.size());

  EXPECT_EQ("10.0.0.1", endpoints[0].address);
  EXPECT_EQ(8080u, endpoints[0].port);
  EXPECT_EQ(100u, endpoints[0].weight);
  EXPECT_EQ(0u, endpoints[0].priority);
  EXPECT_EQ(1u, endpoints[0].health_status); // Healthy

  EXPECT_EQ("10.0.0.2", endpoints[1].address);
  EXPECT_EQ(9090u, endpoints[1].port);
  EXPECT_EQ(200u, endpoints[1].weight);
  EXPECT_EQ(1u, endpoints[1].priority);
}

TEST_F(ClientTest, ResolveEmptyClusterReturnsEmpty) {
  // "svc" exists but has no hosts.
  auto endpoints = client_->resolve("svc");
  EXPECT_TRUE(endpoints.empty());
}

// ---------------------------------------------------------------------------
// pickEndpoint
// ---------------------------------------------------------------------------

TEST_F(ClientTest, PickEndpointUnknownCluster) {
  auto result = client_->pickEndpoint("nonexistent");
  EXPECT_FALSE(result.has_value());
}

TEST_F(ClientTest, PickEndpointReturnsLbSelection) {
  auto host = std::make_shared<NiceMock<Envoy::Upstream::MockHost>>();
  auto address = *Envoy::Network::Utility::resolveUrl("tcp://10.0.0.5:7070");
  ON_CALL(*host, address()).WillByDefault(Return(address));
  ON_CALL(*host, weight()).WillByDefault(Return(50u));
  ON_CALL(*host, priority()).WillByDefault(Return(0u));
  ON_CALL(*host, coarseHealth())
      .WillByDefault(Return(Envoy::Upstream::Host::Health::Healthy));
  cm_.thread_local_cluster_.lb_.host_ = host;

  auto result = client_->pickEndpoint("svc");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ("10.0.0.5", result->address);
  EXPECT_EQ(7070u, result->port);
  EXPECT_EQ(50u, result->weight);
  EXPECT_EQ(1u, result->health_status); // Healthy
}

TEST_F(ClientTest, PickEndpointNoLbHost) {
  cm_.thread_local_cluster_.lb_.host_ = nullptr;
  auto result = client_->pickEndpoint("svc");
  EXPECT_FALSE(result.has_value());
}

// ---------------------------------------------------------------------------
// Health status mapping through pickEndpoint
// ---------------------------------------------------------------------------

TEST_F(ClientTest, HealthStatusDegradedMapping) {
  auto host = std::make_shared<NiceMock<Envoy::Upstream::MockHost>>();
  auto address = *Envoy::Network::Utility::resolveUrl("tcp://1.2.3.4:80");
  ON_CALL(*host, address()).WillByDefault(Return(address));
  ON_CALL(*host, weight()).WillByDefault(Return(1u));
  ON_CALL(*host, priority()).WillByDefault(Return(0u));
  ON_CALL(*host, coarseHealth())
      .WillByDefault(Return(Envoy::Upstream::Host::Health::Degraded));
  cm_.thread_local_cluster_.lb_.host_ = host;

  auto result = client_->pickEndpoint("svc");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(2u, result->health_status); // Degraded
}

TEST_F(ClientTest, HealthStatusUnhealthyMapping) {
  auto host = std::make_shared<NiceMock<Envoy::Upstream::MockHost>>();
  auto address = *Envoy::Network::Utility::resolveUrl("tcp://1.2.3.4:80");
  ON_CALL(*host, address()).WillByDefault(Return(address));
  ON_CALL(*host, weight()).WillByDefault(Return(1u));
  ON_CALL(*host, priority()).WillByDefault(Return(0u));
  ON_CALL(*host, coarseHealth())
      .WillByDefault(Return(Envoy::Upstream::Host::Health::Unhealthy));
  cm_.thread_local_cluster_.lb_.host_ = host;

  auto result = client_->pickEndpoint("svc");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(3u, result->health_status); // Unhealthy
}

// ---------------------------------------------------------------------------
// setClusterLbPolicy / setDefaultLbPolicy (smoke — no crash)
// ---------------------------------------------------------------------------

TEST_F(ClientTest, SetClusterLbPolicyNocrash) {
  client_->setClusterLbPolicy("svc", "envoy.load_balancing_policies.round_robin");
  client_->setClusterLbPolicy("svc", ""); // clear
}

TEST_F(ClientTest, SetDefaultLbPolicyNocrash) {
  client_->setDefaultLbPolicy("envoy.load_balancing_policies.least_request");
  client_->setDefaultLbPolicy(""); // clear
}

// ---------------------------------------------------------------------------
// watchConfig — event string translation
// ---------------------------------------------------------------------------

TEST_F(ClientTest, WatchConfigTranslatesAddedEvent) {
  std::vector<ConfigEvent> received;
  client_->watchConfig("cluster",
                       [&](const ConfigEvent& ev) { received.push_back(ev); });

  engine_->store().notifyConfigChange("cluster", "svc-a",
                                      Envoy::Client::ConfigEvent::Added);

  ASSERT_EQ(1u, received.size());
  EXPECT_EQ("cluster", received[0].resource_type);
  EXPECT_EQ("svc-a", received[0].resource_name);
  EXPECT_EQ("added", received[0].event);
}

TEST_F(ClientTest, WatchConfigTranslatesUpdatedEvent) {
  std::vector<ConfigEvent> received;
  client_->watchConfig("", [&](const ConfigEvent& ev) { received.push_back(ev); });

  engine_->store().notifyConfigChange("endpoint", "svc-b",
                                      Envoy::Client::ConfigEvent::Updated);

  ASSERT_EQ(1u, received.size());
  EXPECT_EQ("updated", received[0].event);
}

TEST_F(ClientTest, WatchConfigTranslatesRemovedEvent) {
  std::vector<ConfigEvent> received;
  client_->watchConfig("", [&](const ConfigEvent& ev) { received.push_back(ev); });

  engine_->store().notifyConfigChange("route", "route-1",
                                      Envoy::Client::ConfigEvent::Removed);

  ASSERT_EQ(1u, received.size());
  EXPECT_EQ("removed", received[0].event);
}

TEST_F(ClientTest, WatchConfigTypeFilter) {
  int count = 0;
  client_->watchConfig("cluster", [&](const ConfigEvent&) { count++; });

  engine_->store().notifyConfigChange("cluster", "x", Envoy::Client::ConfigEvent::Added);
  engine_->store().notifyConfigChange("endpoint", "y", Envoy::Client::ConfigEvent::Added);

  EXPECT_EQ(1, count);
}

TEST_F(ClientTest, MultipleWatchersAllReceiveEvents) {
  int count1 = 0, count2 = 0;
  client_->watchConfig("", [&](const ConfigEvent&) { count1++; });
  client_->watchConfig("", [&](const ConfigEvent&) { count2++; });

  engine_->store().notifyConfigChange("cluster", "svc", Envoy::Client::ConfigEvent::Added);

  EXPECT_EQ(1, count1);
  EXPECT_EQ(1, count2);
}

// ---------------------------------------------------------------------------
// shutdown
// ---------------------------------------------------------------------------

TEST_F(ClientTest, ShutdownCallsTerminate) {
  // First isTerminated() call (from shutdown()) returns false → terminate() fires.
  // Subsequent calls (from ~Client() → shutdown()) return true → no second terminate().
  EXPECT_CALL(*engine_, isTerminated())
      .WillOnce(Return(false))
      .WillRepeatedly(Return(true));
  EXPECT_CALL(*engine_, terminate()).Times(1);
  client_->shutdown();
}

TEST_F(ClientTest, ShutdownIsIdempotentWhenAlreadyTerminated) {
  EXPECT_CALL(*engine_, isTerminated()).WillRepeatedly(Return(true));
  EXPECT_CALL(*engine_, terminate()).Times(0);
  client_->shutdown();
  client_->shutdown();
}

} // namespace
} // namespace EnvoyClient
