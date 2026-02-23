#include "client/library/c_api/envoy_client_internal.h"

#include <cstring>

#include "client/library/common/engine_interface.h"

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

namespace {

// ---------------------------------------------------------------------------
// Helpers: build a C API handle backed by a mock engine.
// ---------------------------------------------------------------------------

class MockClientEngine : public Envoy::Client::ClientEngineInterface {
public:
  explicit MockClientEngine(Envoy::Upstream::ClusterManager& cm) : config_store_(cm) {}

  MOCK_METHOD(bool, waitReady, (absl::Duration));
  MOCK_METHOD(void, terminate, ());
  MOCK_METHOD(bool, isTerminated, (), (const));
  Envoy::Client::ConfigStore& configStore() override { return config_store_; }
  Envoy::Client::ConfigStore& store() { return config_store_; }

private:
  Envoy::Client::ConfigStore config_store_;
};

// Creates a handle that uses a MockClientEngine. The caller takes ownership.
// cm must outlive the returned handle.
struct TestHandleOwner {
  NiceMock<Envoy::Upstream::MockClusterManager> cm;
  // NiceMock suppresses "uninteresting call" failures from envoy_client_destroy()
  // → Client::shutdown() → isTerminated()/terminate().
  NiceMock<MockClientEngine>* engine{nullptr};
  envoy_client_handle handle{nullptr};
};

std::unique_ptr<TestHandleOwner> makeTestHandle() {
  auto owner = std::make_unique<TestHandleOwner>();
  owner->cm.initializeThreadLocalClusters({"svc"});

  auto engine = std::make_unique<NiceMock<MockClientEngine>>(owner->cm);
  owner->engine = engine.get();

  auto client = EnvoyClient::Client::createForTesting(std::move(engine));

  auto* h = new envoy_client_engine();
  h->client = std::move(client);
  owner->handle = h;
  return owner;
}

// Add a host to a TestHandleOwner's mock cluster.
std::shared_ptr<NiceMock<Envoy::Upstream::MockHost>>
addHostToOwner(TestHandleOwner& owner, const std::string& ip, uint32_t port,
               uint32_t weight = 1, uint32_t priority = 0) {
  auto host = std::make_shared<NiceMock<Envoy::Upstream::MockHost>>();
  auto address =
      *Envoy::Network::Utility::resolveUrl(fmt::format("tcp://{}:{}", ip, port));
  ON_CALL(*host, address()).WillByDefault(Return(address));
  ON_CALL(*host, weight()).WillByDefault(Return(weight));
  ON_CALL(*host, priority()).WillByDefault(Return(priority));
  ON_CALL(*host, coarseHealth())
      .WillByDefault(Return(Envoy::Upstream::Host::Health::Healthy));
  auto& priority_set = owner.cm.thread_local_cluster_.cluster_.priority_set_;
  priority_set.getMockHostSet(0)->hosts_.push_back(host);
  return host;
}

// ---------------------------------------------------------------------------
// Null-handle / parameter guard tests
// ---------------------------------------------------------------------------

TEST(EnvoyClientApiTest, WaitReadyNullHandle) {
  EXPECT_EQ(ENVOY_CLIENT_ERROR, envoy_client_wait_ready(nullptr, 5));
}

TEST(EnvoyClientApiTest, DestroyNullHandleIsNoop) {
  // Must not crash.
  envoy_client_destroy(nullptr);
}

TEST(EnvoyClientApiTest, ResolveNullHandle) {
  envoy_client_endpoint_list out{};
  EXPECT_EQ(ENVOY_CLIENT_ERROR, envoy_client_resolve(nullptr, "svc", &out));
}

TEST(EnvoyClientApiTest, ResolveNullClusterName) {
  auto owner = makeTestHandle();
  envoy_client_endpoint_list out{};
  EXPECT_EQ(ENVOY_CLIENT_ERROR, envoy_client_resolve(owner->handle, nullptr, &out));
  envoy_client_destroy(owner->handle);
}

TEST(EnvoyClientApiTest, ResolveNullOutParam) {
  auto owner = makeTestHandle();
  EXPECT_EQ(ENVOY_CLIENT_ERROR, envoy_client_resolve(owner->handle, "svc", nullptr));
  envoy_client_destroy(owner->handle);
}

TEST(EnvoyClientApiTest, PickEndpointNullHandle) {
  envoy_client_endpoint out{};
  EXPECT_EQ(ENVOY_CLIENT_ERROR, envoy_client_pick_endpoint(nullptr, "svc", nullptr, &out));
}

TEST(EnvoyClientApiTest, PickEndpointNullClusterName) {
  auto owner = makeTestHandle();
  envoy_client_endpoint out{};
  EXPECT_EQ(ENVOY_CLIENT_ERROR,
            envoy_client_pick_endpoint(owner->handle, nullptr, nullptr, &out));
  envoy_client_destroy(owner->handle);
}

TEST(EnvoyClientApiTest, PickEndpointNullOutParam) {
  auto owner = makeTestHandle();
  EXPECT_EQ(ENVOY_CLIENT_ERROR,
            envoy_client_pick_endpoint(owner->handle, "svc", nullptr, nullptr));
  envoy_client_destroy(owner->handle);
}

TEST(EnvoyClientApiTest, SetClusterLbPolicyNullHandle) {
  EXPECT_EQ(ENVOY_CLIENT_ERROR,
            envoy_client_set_cluster_lb_policy(nullptr, "svc", "round_robin"));
}

TEST(EnvoyClientApiTest, SetClusterLbPolicyNullCluster) {
  auto owner = makeTestHandle();
  EXPECT_EQ(ENVOY_CLIENT_ERROR,
            envoy_client_set_cluster_lb_policy(owner->handle, nullptr, "round_robin"));
  envoy_client_destroy(owner->handle);
}

TEST(EnvoyClientApiTest, SetDefaultLbPolicyNullHandle) {
  EXPECT_EQ(ENVOY_CLIENT_ERROR, envoy_client_set_default_lb_policy(nullptr, "round_robin"));
}

TEST(EnvoyClientApiTest, WatchConfigNullHandle) {
  auto cb = [](const char*, const char*, envoy_client_config_event, void*) {};
  EXPECT_EQ(ENVOY_CLIENT_ERROR, envoy_client_watch_config(nullptr, "cluster", cb, nullptr));
}

TEST(EnvoyClientApiTest, WatchConfigNullCallback) {
  auto owner = makeTestHandle();
  EXPECT_EQ(ENVOY_CLIENT_ERROR,
            envoy_client_watch_config(owner->handle, "cluster", nullptr, nullptr));
  envoy_client_destroy(owner->handle);
}

// ---------------------------------------------------------------------------
// envoy_client_free_endpoints
// ---------------------------------------------------------------------------

TEST(EnvoyClientApiTest, FreeEndpointsNullIsNoop) {
  envoy_client_free_endpoints(nullptr); // must not crash
}

TEST(EnvoyClientApiTest, FreeEndpointsZeroCountIsNoop) {
  envoy_client_endpoint_list list{};
  list.endpoints = nullptr;
  list.count = 0;
  envoy_client_free_endpoints(&list); // must not crash
}

// ---------------------------------------------------------------------------
// envoy_client_resolve — functional path
// ---------------------------------------------------------------------------

TEST(EnvoyClientApiTest, ResolveUnavailableWhenNoHosts) {
  auto owner = makeTestHandle();
  envoy_client_endpoint_list out{};
  EXPECT_EQ(ENVOY_CLIENT_UNAVAILABLE, envoy_client_resolve(owner->handle, "svc", &out));
  EXPECT_EQ(0u, out.count);
  EXPECT_EQ(nullptr, out.endpoints);
  envoy_client_destroy(owner->handle);
}

TEST(EnvoyClientApiTest, ResolveReturnsEndpointsAndFreeWorks) {
  auto owner = makeTestHandle();
  addHostToOwner(*owner, "10.1.2.3", 8080, 100, 0);
  addHostToOwner(*owner, "10.1.2.4", 9090, 200, 1);

  envoy_client_endpoint_list out{};
  EXPECT_EQ(ENVOY_CLIENT_OK, envoy_client_resolve(owner->handle, "svc", &out));
  ASSERT_EQ(2u, out.count);
  ASSERT_NE(nullptr, out.endpoints);

  EXPECT_STREQ("10.1.2.3", out.endpoints[0].address);
  EXPECT_EQ(8080u, out.endpoints[0].port);
  EXPECT_EQ(100u, out.endpoints[0].weight);
  EXPECT_EQ(0u, out.endpoints[0].priority);
  EXPECT_EQ(1u, out.endpoints[0].health_status);

  EXPECT_STREQ("10.1.2.4", out.endpoints[1].address);
  EXPECT_EQ(9090u, out.endpoints[1].port);

  envoy_client_free_endpoints(&out);
  EXPECT_EQ(nullptr, out.endpoints);
  EXPECT_EQ(0u, out.count);

  envoy_client_destroy(owner->handle);
}

// ---------------------------------------------------------------------------
// envoy_client_pick_endpoint — functional path
// ---------------------------------------------------------------------------

TEST(EnvoyClientApiTest, PickEndpointUnavailableWhenNoHost) {
  auto owner = makeTestHandle();
  owner->cm.thread_local_cluster_.lb_.host_ = nullptr;

  envoy_client_endpoint out{};
  EXPECT_EQ(ENVOY_CLIENT_UNAVAILABLE,
            envoy_client_pick_endpoint(owner->handle, "svc", nullptr, &out));
  envoy_client_destroy(owner->handle);
}

TEST(EnvoyClientApiTest, PickEndpointReturnsAllocatedAddress) {
  auto owner = makeTestHandle();

  auto host = std::make_shared<NiceMock<Envoy::Upstream::MockHost>>();
  auto address = *Envoy::Network::Utility::resolveUrl("tcp://192.168.1.1:5000");
  ON_CALL(*host, address()).WillByDefault(Return(address));
  ON_CALL(*host, weight()).WillByDefault(Return(77u));
  ON_CALL(*host, priority()).WillByDefault(Return(0u));
  ON_CALL(*host, coarseHealth())
      .WillByDefault(Return(Envoy::Upstream::Host::Health::Healthy));
  owner->cm.thread_local_cluster_.lb_.host_ = host;

  envoy_client_endpoint out{};
  EXPECT_EQ(ENVOY_CLIENT_OK,
            envoy_client_pick_endpoint(owner->handle, "svc", nullptr, &out));

  EXPECT_STREQ("192.168.1.1", out.address);
  EXPECT_EQ(5000u, out.port);
  EXPECT_EQ(77u, out.weight);
  EXPECT_EQ(1u, out.health_status);

  // Caller owns the address string — free it.
  delete[] out.address;
  envoy_client_destroy(owner->handle);
}

TEST(EnvoyClientApiTest, PickEndpointWithRequestContextNullFields) {
  // Passing a context with all-null optional fields must not crash.
  auto owner = makeTestHandle();
  owner->cm.thread_local_cluster_.lb_.host_ = nullptr;

  envoy_client_request_context ctx{};
  ctx.path = nullptr;
  ctx.authority = nullptr;
  ctx.override_host = nullptr;
  ctx.hash_key = nullptr;
  ctx.hash_key_len = 0;

  envoy_client_endpoint out{};
  // Cluster exists but LB returns null — expect UNAVAILABLE, not a crash.
  EXPECT_EQ(ENVOY_CLIENT_UNAVAILABLE,
            envoy_client_pick_endpoint(owner->handle, "svc", &ctx, &out));
  envoy_client_destroy(owner->handle);
}

// ---------------------------------------------------------------------------
// LB policy override — smoke tests
// ---------------------------------------------------------------------------

TEST(EnvoyClientApiTest, SetClusterLbPolicyOk) {
  auto owner = makeTestHandle();
  EXPECT_EQ(ENVOY_CLIENT_OK,
            envoy_client_set_cluster_lb_policy(owner->handle, "svc", "round_robin"));
  EXPECT_EQ(ENVOY_CLIENT_OK,
            envoy_client_set_cluster_lb_policy(owner->handle, "svc", nullptr)); // clear
  envoy_client_destroy(owner->handle);
}

TEST(EnvoyClientApiTest, SetDefaultLbPolicyOk) {
  auto owner = makeTestHandle();
  EXPECT_EQ(ENVOY_CLIENT_OK,
            envoy_client_set_default_lb_policy(owner->handle, "least_request"));
  EXPECT_EQ(ENVOY_CLIENT_OK,
            envoy_client_set_default_lb_policy(owner->handle, nullptr)); // clear
  envoy_client_destroy(owner->handle);
}

// ---------------------------------------------------------------------------
// Config watch — event translation through C API
// ---------------------------------------------------------------------------

TEST(EnvoyClientApiTest, WatchConfigReceivesAddedEvent) {
  auto owner = makeTestHandle();

  struct Captured {
    std::string type;
    std::string name;
    envoy_client_config_event event{};
  } cap;

  auto cb = [](const char* type, const char* name, envoy_client_config_event ev,
               void* ctx) {
    auto* c = static_cast<Captured*>(ctx);
    c->type = type;
    c->name = name;
    c->event = ev;
  };

  EXPECT_EQ(ENVOY_CLIENT_OK,
            envoy_client_watch_config(owner->handle, "cluster", cb, &cap));

  owner->engine->store().notifyConfigChange("cluster", "my-svc",
                                            Envoy::Client::ConfigEvent::Added);

  EXPECT_EQ("cluster", cap.type);
  EXPECT_EQ("my-svc", cap.name);
  EXPECT_EQ(ENVOY_CLIENT_CONFIG_ADDED, cap.event);

  envoy_client_destroy(owner->handle);
}

TEST(EnvoyClientApiTest, WatchConfigReceivesUpdatedEvent) {
  auto owner = makeTestHandle();
  envoy_client_config_event received = ENVOY_CLIENT_CONFIG_ADDED;

  auto cb = [](const char*, const char*, envoy_client_config_event ev, void* ctx) {
    *static_cast<envoy_client_config_event*>(ctx) = ev;
  };
  EXPECT_EQ(ENVOY_CLIENT_OK,
            envoy_client_watch_config(owner->handle, nullptr, cb, &received));

  owner->engine->store().notifyConfigChange("endpoint", "ep",
                                            Envoy::Client::ConfigEvent::Updated);
  EXPECT_EQ(ENVOY_CLIENT_CONFIG_UPDATED, received);

  envoy_client_destroy(owner->handle);
}

TEST(EnvoyClientApiTest, WatchConfigReceivesRemovedEvent) {
  auto owner = makeTestHandle();
  envoy_client_config_event received = ENVOY_CLIENT_CONFIG_ADDED;

  auto cb = [](const char*, const char*, envoy_client_config_event ev, void* ctx) {
    *static_cast<envoy_client_config_event*>(ctx) = ev;
  };
  EXPECT_EQ(ENVOY_CLIENT_OK,
            envoy_client_watch_config(owner->handle, nullptr, cb, &received));

  owner->engine->store().notifyConfigChange("route", "r1",
                                            Envoy::Client::ConfigEvent::Removed);
  EXPECT_EQ(ENVOY_CLIENT_CONFIG_REMOVED, received);

  envoy_client_destroy(owner->handle);
}

// ---------------------------------------------------------------------------
// Stub functions — must return OK and not crash
// ---------------------------------------------------------------------------

TEST(EnvoyClientApiTest, ReportResultIsNoop) {
  auto owner = makeTestHandle();
  envoy_client_endpoint ep{};
  ep.address = "10.0.0.1";
  // Should not crash even with a stack-allocated address (not heap-allocated).
  envoy_client_report_result(owner->handle, &ep, 200, 42);
  envoy_client_destroy(owner->handle);
}

TEST(EnvoyClientApiTest, AddInterceptorReturnsOk) {
  auto owner = makeTestHandle();
  auto cb = [](envoy_client_headers*, const char*, envoy_client_interceptor_phase,
               void*) -> envoy_client_status { return ENVOY_CLIENT_OK; };
  EXPECT_EQ(ENVOY_CLIENT_OK,
            envoy_client_add_interceptor(owner->handle, "test", cb, nullptr));
  envoy_client_destroy(owner->handle);
}

TEST(EnvoyClientApiTest, RemoveInterceptorReturnsOk) {
  auto owner = makeTestHandle();
  EXPECT_EQ(ENVOY_CLIENT_OK, envoy_client_remove_interceptor(owner->handle, "test"));
  envoy_client_destroy(owner->handle);
}

TEST(EnvoyClientApiTest, SetLbContextProviderNullHandleReturnsError) {
  auto cb = [](const char*, envoy_client_request_context*, void*) {};
  EXPECT_EQ(ENVOY_CLIENT_ERROR, envoy_client_set_lb_context_provider(nullptr, cb, nullptr));
}

TEST(EnvoyClientApiTest, SetLbContextProviderCallbackInvokedOnPick) {
  auto owner = makeTestHandle();

  struct Capture {
    bool called{false};
    std::string seen_cluster;
  } cap;

  // Captureless lambda converts to a plain function pointer.
  auto cb = [](const char* cluster_name, envoy_client_request_context* ctx, void* user_ctx) {
    auto* c = static_cast<Capture*>(user_ctx);
    c->called = true;
    c->seen_cluster = cluster_name;
    // Enrich the context: inject a consistent-hash key.
    ctx->hash_key = "session-42";
    ctx->hash_key_len = 10;
  };

  EXPECT_EQ(ENVOY_CLIENT_OK,
            envoy_client_set_lb_context_provider(owner->handle, cb, &cap));

  // Trigger a pick — the mock LB returns no host, but the callback must fire.
  owner->cm.thread_local_cluster_.lb_.host_ = nullptr;
  envoy_client_endpoint out{};
  envoy_client_pick_endpoint(owner->handle, "svc", nullptr, &out);

  EXPECT_TRUE(cap.called);
  EXPECT_EQ("svc", cap.seen_cluster);

  envoy_client_destroy(owner->handle);
}

TEST(EnvoyClientApiTest, SetLbContextProviderNullCallbackClearsProvider) {
  auto owner = makeTestHandle();

  // Register a callback then clear it — must not crash on a subsequent pick.
  auto cb = [](const char*, envoy_client_request_context*, void*) {};
  EXPECT_EQ(ENVOY_CLIENT_OK,
            envoy_client_set_lb_context_provider(owner->handle, cb, nullptr));
  EXPECT_EQ(ENVOY_CLIENT_OK,
            envoy_client_set_lb_context_provider(owner->handle, nullptr, nullptr));

  owner->cm.thread_local_cluster_.lb_.host_ = nullptr;
  envoy_client_endpoint out{};
  // With no callback registered, pick must not crash.
  envoy_client_pick_endpoint(owner->handle, "svc", nullptr, &out);

  envoy_client_destroy(owner->handle);
}

} // namespace
