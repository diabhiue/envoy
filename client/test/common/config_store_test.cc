#include "client/library/common/config_store.h"

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

namespace Envoy {
namespace Client {
namespace {

class ConfigStoreTest : public testing::Test {
public:
  ConfigStoreTest() : config_store_(cm_) {
    cm_.initializeThreadLocalClusters({"test_cluster"});
  }

  // Helper to create a mock host with the given address and port.
  std::shared_ptr<NiceMock<Upstream::MockHost>> makeHost(const std::string& ip, uint32_t port,
                                                         uint32_t weight = 1,
                                                         uint32_t priority = 0) {
    auto host = std::make_shared<NiceMock<Upstream::MockHost>>();
    auto address = *Network::Utility::resolveUrl(fmt::format("tcp://{}:{}", ip, port));
    ON_CALL(*host, address()).WillByDefault(Return(address));
    ON_CALL(*host, weight()).WillByDefault(Return(weight));
    ON_CALL(*host, priority()).WillByDefault(Return(priority));
    ON_CALL(*host, coarseHealth()).WillByDefault(Return(Upstream::Host::Health::Healthy));
    return host;
  }

  // Set up the mock priority set with hosts.
  void setupHosts(const std::vector<std::shared_ptr<NiceMock<Upstream::MockHost>>>& hosts) {
    auto& priority_set = cm_.thread_local_cluster_.cluster_.priority_set_;
    auto* host_set = priority_set.getMockHostSet(0);
    host_set->hosts_.clear();
    for (const auto& h : hosts) {
      host_set->hosts_.push_back(h);
    }
  }

  NiceMock<Upstream::MockClusterManager> cm_;
  ConfigStore config_store_;
};

// Test: Resolve returns empty when cluster does not exist.
TEST_F(ConfigStoreTest, ResolveUnknownCluster) {
  std::vector<EndpointInfo> endpoints;
  EXPECT_FALSE(config_store_.resolve("nonexistent_cluster", endpoints));
  EXPECT_TRUE(endpoints.empty());
}

// Test: Resolve returns endpoints when cluster exists.
TEST_F(ConfigStoreTest, ResolveExistingCluster) {
  auto host1 = makeHost("10.0.0.1", 8080, 100, 0);
  auto host2 = makeHost("10.0.0.2", 8081, 200, 0);
  setupHosts({host1, host2});

  std::vector<EndpointInfo> endpoints;
  EXPECT_TRUE(config_store_.resolve("test_cluster", endpoints));
  ASSERT_EQ(2, endpoints.size());

  EXPECT_EQ("10.0.0.1", endpoints[0].address);
  EXPECT_EQ(8080, endpoints[0].port);
  EXPECT_EQ(100, endpoints[0].weight);
  EXPECT_EQ(0, endpoints[0].priority);
  EXPECT_EQ(1, endpoints[0].health_status); // Healthy

  EXPECT_EQ("10.0.0.2", endpoints[1].address);
  EXPECT_EQ(8081, endpoints[1].port);
  EXPECT_EQ(200, endpoints[1].weight);
}

// Test: Resolve returns empty when cluster has no hosts.
TEST_F(ConfigStoreTest, ResolveEmptyCluster) {
  setupHosts({});

  std::vector<EndpointInfo> endpoints;
  EXPECT_FALSE(config_store_.resolve("test_cluster", endpoints));
  EXPECT_TRUE(endpoints.empty());
}

// Test: PickEndpoint returns nullopt when cluster does not exist.
TEST_F(ConfigStoreTest, PickEndpointUnknownCluster) {
  auto result = config_store_.pickEndpoint("nonexistent_cluster");
  EXPECT_FALSE(result.has_value());
}

// Test: PickEndpoint returns the host chosen by the LB.
TEST_F(ConfigStoreTest, PickEndpointSuccess) {
  auto host = makeHost("10.0.0.5", 9090, 50, 0);
  // Set the LB's default host.
  cm_.thread_local_cluster_.lb_.host_ = host;

  auto result = config_store_.pickEndpoint("test_cluster");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ("10.0.0.5", result->address);
  EXPECT_EQ(9090, result->port);
  EXPECT_EQ(50, result->weight);
  EXPECT_EQ(1, result->health_status); // Healthy
}

// Test: PickEndpoint returns nullopt when LB returns no host.
TEST_F(ConfigStoreTest, PickEndpointNoHost) {
  cm_.thread_local_cluster_.lb_.host_ = nullptr;

  auto result = config_store_.pickEndpoint("test_cluster");
  EXPECT_FALSE(result.has_value());
}

// Test: Health status mapping.
TEST_F(ConfigStoreTest, HealthStatusMapping) {
  auto healthy = makeHost("10.0.0.1", 8080);
  ON_CALL(*healthy, coarseHealth()).WillByDefault(Return(Upstream::Host::Health::Healthy));

  auto degraded = makeHost("10.0.0.2", 8080);
  ON_CALL(*degraded, coarseHealth()).WillByDefault(Return(Upstream::Host::Health::Degraded));

  auto unhealthy = makeHost("10.0.0.3", 8080);
  ON_CALL(*unhealthy, coarseHealth()).WillByDefault(Return(Upstream::Host::Health::Unhealthy));

  setupHosts({healthy, degraded, unhealthy});

  std::vector<EndpointInfo> endpoints;
  EXPECT_TRUE(config_store_.resolve("test_cluster", endpoints));
  ASSERT_EQ(3, endpoints.size());

  EXPECT_EQ(1, endpoints[0].health_status); // Healthy
  EXPECT_EQ(2, endpoints[1].health_status); // Degraded
  EXPECT_EQ(3, endpoints[2].health_status); // Unhealthy
}

// Test: SetClusterLbPolicy stores override (no crash).
TEST_F(ConfigStoreTest, SetClusterLbPolicy) {
  config_store_.setClusterLbPolicy("test_cluster",
                                   "envoy.load_balancing_policies.round_robin");
  // Setting empty string clears the override.
  config_store_.setClusterLbPolicy("test_cluster", "");
}

// Test: SetDefaultLbPolicy stores override (no crash).
TEST_F(ConfigStoreTest, SetDefaultLbPolicy) {
  config_store_.setDefaultLbPolicy("envoy.load_balancing_policies.least_request");
  config_store_.setDefaultLbPolicy("");
}

// Test: Config watch callback is invoked on notify.
TEST_F(ConfigStoreTest, ConfigWatchNotification) {
  bool callback_invoked = false;
  std::string received_type;
  std::string received_name;
  ConfigEvent received_event;

  config_store_.watchConfig("cluster",
                            [&](const std::string& type, const std::string& name,
                                ConfigEvent event) {
                              callback_invoked = true;
                              received_type = type;
                              received_name = name;
                              received_event = event;
                            });

  config_store_.notifyConfigChange("cluster", "my-service", ConfigEvent::Added);

  EXPECT_TRUE(callback_invoked);
  EXPECT_EQ("cluster", received_type);
  EXPECT_EQ("my-service", received_name);
  EXPECT_EQ(ConfigEvent::Added, received_event);
}

// Test: Config watch with empty filter receives all events.
TEST_F(ConfigStoreTest, ConfigWatchAllTypes) {
  int callback_count = 0;

  config_store_.watchConfig("", [&](const std::string&, const std::string&, ConfigEvent) {
    callback_count++;
  });

  config_store_.notifyConfigChange("cluster", "svc1", ConfigEvent::Added);
  config_store_.notifyConfigChange("endpoint", "svc1", ConfigEvent::Updated);
  config_store_.notifyConfigChange("route", "route1", ConfigEvent::Removed);

  EXPECT_EQ(3, callback_count);
}

// Test: Config watch with type filter only receives matching events.
TEST_F(ConfigStoreTest, ConfigWatchFilteredType) {
  int callback_count = 0;

  config_store_.watchConfig("endpoint",
                            [&](const std::string&, const std::string&, ConfigEvent) {
                              callback_count++;
                            });

  config_store_.notifyConfigChange("cluster", "svc1", ConfigEvent::Added);
  config_store_.notifyConfigChange("endpoint", "svc1", ConfigEvent::Updated);

  EXPECT_EQ(1, callback_count);
}

// Test: Multiple watchers all receive notifications.
TEST_F(ConfigStoreTest, MultipleWatchers) {
  int count1 = 0, count2 = 0;

  config_store_.watchConfig("", [&](const std::string&, const std::string&, ConfigEvent) {
    count1++;
  });
  config_store_.watchConfig("", [&](const std::string&, const std::string&, ConfigEvent) {
    count2++;
  });

  config_store_.notifyConfigChange("cluster", "svc1", ConfigEvent::Added);

  EXPECT_EQ(1, count1);
  EXPECT_EQ(1, count2);
}

} // namespace
} // namespace Client
} // namespace Envoy
