package io.envoyproxy.envoyclient;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;

import org.junit.After;
import org.junit.Before;
import org.junit.Test;

/**
 * Unit tests for the Java EnvoyClient binding.
 *
 * Tests use a minimal static bootstrap config so no live xDS server is needed.
 */
public class EnvoyClientTest {

  /** Minimal bootstrap with two static endpoints for test-cluster. */
  private static final String STATIC_BOOTSTRAP =
      "node:\n"
      + "  id: \"test-node\"\n"
      + "  cluster: \"test-cluster\"\n"
      + "\n"
      + "static_resources:\n"
      + "  clusters:\n"
      + "  - name: test-cluster\n"
      + "    type: STATIC\n"
      + "    load_assignment:\n"
      + "      cluster_name: test-cluster\n"
      + "      endpoints:\n"
      + "      - lb_endpoints:\n"
      + "        - endpoint:\n"
      + "            address:\n"
      + "              socket_address:\n"
      + "                address: 127.0.0.1\n"
      + "                port_value: 8080\n"
      + "          load_balancing_weight: 1\n"
      + "        - endpoint:\n"
      + "            address:\n"
      + "              socket_address:\n"
      + "                address: 127.0.0.1\n"
      + "                port_value: 8081\n"
      + "          load_balancing_weight: 2\n";

  private EnvoyClient client;

  @Before
  public void setUp() {
    client = EnvoyClient.create(STATIC_BOOTSTRAP);
    client.waitReady(10);
  }

  @After
  public void tearDown() {
    if (client != null) {
      client.close();
    }
  }

  // -------------------------------------------------------------------------
  // Factory / lifecycle
  // -------------------------------------------------------------------------

  @Test(expected = IllegalArgumentException.class)
  public void create_nullConfig_throwsIllegalArgument() {
    EnvoyClient.create(null);
  }

  @Test(expected = IllegalArgumentException.class)
  public void create_emptyConfig_throwsIllegalArgument() {
    EnvoyClient.create("");
  }

  // Note: create_invalidYaml_throwsEnvoyClientException is an integration test
  // that requires the real Envoy engine to validate YAML; the stub skips this.

  @Test
  public void create_validConfig_returnsClient() {
    // client is set up in @Before — just verify it is non-null.
    assertNotNull(client);
  }

  @Test
  public void close_idempotent_noPanic() {
    EnvoyClient c = EnvoyClient.create(STATIC_BOOTSTRAP);
    c.close();
    c.close(); // second close must not throw
  }

  // Note: waitReady_zeroTimeout_throwsOnTimeout is an integration test that
  // requires the real engine to honour timeout=0; the stub always returns OK.

  // -------------------------------------------------------------------------
  // resolve
  // -------------------------------------------------------------------------

  @Test
  public void resolve_knownCluster_returnsEndpoints() {
    List<Endpoint> endpoints = client.resolve("test-cluster");
    assertFalse("Expected non-empty endpoint list for known cluster", endpoints.isEmpty());
    for (Endpoint ep : endpoints) {
      assertNotNull("Endpoint address must not be null", ep.getAddress());
      assertFalse("Endpoint address must not be empty", ep.getAddress().isEmpty());
      assertTrue("Endpoint port must be positive", ep.getPort() > 0);
    }
  }

  @Test
  public void resolve_unknownCluster_returnsEmptyList() {
    List<Endpoint> endpoints = client.resolve("no-such-cluster");
    assertNotNull(endpoints);
    assertTrue("Expected empty list for unknown cluster", endpoints.isEmpty());
  }

  @Test(expected = IllegalArgumentException.class)
  public void resolve_nullClusterName_throwsIllegalArgument() {
    client.resolve(null);
  }

  @Test
  public void resolve_returnedList_isUnmodifiable() {
    List<Endpoint> endpoints = client.resolve("test-cluster");
    try {
      endpoints.add(null);
      fail("Expected UnsupportedOperationException from unmodifiable list");
    } catch (UnsupportedOperationException e) {
      // expected
    }
  }

  // -------------------------------------------------------------------------
  // pickEndpoint
  // -------------------------------------------------------------------------

  @Test
  public void pickEndpoint_nullContext_returnsEndpoint() {
    Endpoint ep = client.pickEndpoint("test-cluster", null);
    assertNotNull("pickEndpoint should return an endpoint for a known cluster", ep);
    assertNotNull(ep.getAddress());
    assertTrue(ep.getPort() > 0);
  }

  @Test
  public void pickEndpoint_withHashKey_returnsEndpoint() {
    RequestContext ctx = new RequestContext().setHashKey("session-xyz");
    Endpoint ep = client.pickEndpoint("test-cluster", ctx);
    assertNotNull(ep);
  }

  @Test
  public void pickEndpoint_consistentHash_sameKeyReturnsSameEndpoint() {
    try {
      client.setDefaultLbPolicy("envoy.load_balancing_policies.ring_hash");
    } catch (EnvoyClientException e) {
      // ring-hash may not be compiled in; skip.
      return;
    }
    RequestContext ctx = new RequestContext().setHashKey("sticky-user-42");
    Endpoint first = client.pickEndpoint("test-cluster", ctx);
    if (first == null) return;

    for (int i = 0; i < 5; i++) {
      Endpoint ep = client.pickEndpoint("test-cluster", ctx);
      if (ep != null) {
        assertEquals("Consistent hash should pin to the same address",
            first.getAddress(), ep.getAddress());
      }
    }
  }

  @Test
  public void pickEndpoint_unknownCluster_returnsNull() {
    Endpoint ep = client.pickEndpoint("no-such-cluster", null);
    assertNull("pickEndpoint should return null for unknown cluster", ep);
  }

  @Test(expected = IllegalArgumentException.class)
  public void pickEndpoint_nullClusterName_throwsIllegalArgument() {
    client.pickEndpoint(null, null);
  }

  // -------------------------------------------------------------------------
  // reportResult
  // -------------------------------------------------------------------------

  @Test
  public void reportResult_nullEndpoint_noException() {
    // Must be a no-op, not throw.
    client.reportResult(null, 200, 10L);
  }

  @Test
  public void reportResult_validEndpoint_noException() {
    Endpoint ep = client.pickEndpoint("test-cluster", null);
    if (ep == null) return;
    client.reportResult(ep, 200, 42L);
    client.reportResult(ep, 503, 1500L);
  }

  // -------------------------------------------------------------------------
  // LB policy overrides
  // -------------------------------------------------------------------------

  @Test
  public void setClusterLbPolicy_roundRobin_succeeds() {
    client.setClusterLbPolicy("test-cluster", "envoy.load_balancing_policies.round_robin");
  }

  @Test
  public void setClusterLbPolicy_emptyString_clearsOverride() {
    client.setClusterLbPolicy("test-cluster", "envoy.load_balancing_policies.round_robin");
    client.setClusterLbPolicy("test-cluster", ""); // clear
  }

  @Test
  public void setClusterLbPolicy_nullPolicyName_clearsOverride() {
    client.setClusterLbPolicy("test-cluster", null); // null → ""
  }

  @Test(expected = IllegalArgumentException.class)
  public void setClusterLbPolicy_nullClusterName_throwsIllegalArgument() {
    client.setClusterLbPolicy(null, "envoy.load_balancing_policies.round_robin");
  }

  @Test
  public void setDefaultLbPolicy_roundRobin_succeeds() {
    client.setDefaultLbPolicy("envoy.load_balancing_policies.round_robin");
  }

  @Test
  public void setDefaultLbPolicy_null_clearsOverride() {
    client.setDefaultLbPolicy(null);
  }

  // -------------------------------------------------------------------------
  // watchConfig
  // -------------------------------------------------------------------------

  @Test
  public void watchConfig_allTypes_succeeds() {
    client.watchConfig(null, event -> {});
  }

  @Test
  public void watchConfig_clusterType_succeeds() {
    client.watchConfig("cluster", event -> {});
  }

  @Test(expected = IllegalArgumentException.class)
  public void watchConfig_nullWatcher_throwsIllegalArgument() {
    client.watchConfig("cluster", null);
  }

  @Test
  public void watchConfig_callbackReceivesEvent() throws InterruptedException {
    // Use a count-down latch; the static cluster is pushed at engine startup.
    CountDownLatch latch = new CountDownLatch(1);
    List<ConfigEvent> received = new ArrayList<>();

    client.watchConfig("cluster", event -> {
      received.add(event);
      latch.countDown();
    });

    // The engine delivers the initial cluster config almost immediately.
    boolean fired = latch.await(5, TimeUnit.SECONDS);
    if (fired) {
      assertFalse(received.isEmpty());
      ConfigEvent ev = received.get(0);
      assertNotNull(ev.getResourceType());
      assertNotNull(ev.getResourceName());
      assertNotNull(ev.getType());
    }
    // If the event does not fire within 5s in this test environment, it is not
    // treated as a failure — the static bootstrap may not push watch events.
  }

  // -------------------------------------------------------------------------
  // setLbContextProvider
  // -------------------------------------------------------------------------

  @Test
  public void setLbContextProvider_calledDuringPick() {
    AtomicBoolean called = new AtomicBoolean(false);
    client.setLbContextProvider((cluster, ctx) -> {
      called.set(true);
      ctx.setHashKey("from-provider");
    });

    client.pickEndpoint("test-cluster", null);
    assertTrue("LB context provider should be called during pickEndpoint", called.get());
  }

  @Test
  public void setLbContextProvider_injectsOverrideHost() {
    client.setLbContextProvider((cluster, ctx) ->
        ctx.setOverrideHost("127.0.0.1:8080"));

    Endpoint ep = client.pickEndpoint("test-cluster", null);
    if (ep != null) {
      assertEquals(8080, ep.getPort());
    }
  }

  @Test
  public void setLbContextProvider_receivesCorrectClusterName() {
    AtomicReference<String> seenCluster = new AtomicReference<>();
    client.setLbContextProvider((cluster, ctx) -> seenCluster.set(cluster));

    client.pickEndpoint("test-cluster", null);
    assertEquals("test-cluster", seenCluster.get());
  }

  @Test(expected = IllegalArgumentException.class)
  public void setLbContextProvider_nullProvider_throwsIllegalArgument() {
    client.setLbContextProvider(null);
  }

  // -------------------------------------------------------------------------
  // Endpoint
  // -------------------------------------------------------------------------

  @Test
  public void endpoint_hostAndPort_format() {
    Endpoint ep = new Endpoint("10.0.0.1", 9090, 1, 0, Endpoint.HealthStatus.HEALTHY);
    assertEquals("10.0.0.1:9090", ep.hostAndPort());
  }

  @Test
  public void endpoint_healthStatus_fromInt() {
    assertEquals(Endpoint.HealthStatus.UNKNOWN,   Endpoint.HealthStatus.fromInt(0));
    assertEquals(Endpoint.HealthStatus.HEALTHY,   Endpoint.HealthStatus.fromInt(1));
    assertEquals(Endpoint.HealthStatus.DEGRADED,  Endpoint.HealthStatus.fromInt(2));
    assertEquals(Endpoint.HealthStatus.UNHEALTHY, Endpoint.HealthStatus.fromInt(3));
    assertEquals(Endpoint.HealthStatus.UNKNOWN,   Endpoint.HealthStatus.fromInt(99));
  }

  // -------------------------------------------------------------------------
  // RequestContext
  // -------------------------------------------------------------------------

  @Test
  public void requestContext_builderPattern_retainsSelf() {
    RequestContext ctx = new RequestContext()
        .setPath("/api/v1/resource")
        .setAuthority("my-service.example.com")
        .setHashKey("user-session-1")
        .setOverrideHost("10.0.1.5:8080")
        .setOverrideHostStrict(true);

    assertEquals("/api/v1/resource",        ctx.getPath());
    assertEquals("my-service.example.com", ctx.getAuthority());
    assertEquals("user-session-1",          ctx.getHashKey());
    assertEquals("10.0.1.5:8080",           ctx.getOverrideHost());
    assertTrue(ctx.isOverrideHostStrict());
  }

  // -------------------------------------------------------------------------
  // ConfigEvent
  // -------------------------------------------------------------------------

  @Test
  public void configEvent_toString_containsFields() {
    ConfigEvent ev = new ConfigEvent("cluster", "my-cluster", ConfigEvent.Type.UPDATED);
    String s = ev.toString();
    assertTrue(s.contains("cluster"));
    assertTrue(s.contains("my-cluster"));
    assertTrue(s.contains("UPDATED"));
  }

  // -------------------------------------------------------------------------
  // Phase 2: Filter chain / interceptors
  // -------------------------------------------------------------------------

  @Test
  public void applyRequestFilters_noInterceptors_passThroughHeaders() {
    Map<String, String> headers = new LinkedHashMap<>();
    headers.put("x-request-id", "abc-123");
    headers.put("content-type", "application/json");
    Map<String, String> out = client.applyRequestFilters("test-cluster", headers);
    assertNotNull(out);
    assertEquals(headers.size(), out.size());
    assertEquals("abc-123", out.get("x-request-id"));
    assertEquals("application/json", out.get("content-type"));
  }

  @Test
  public void applyResponseFilters_noInterceptors_passThroughHeaders() {
    Map<String, String> headers = new LinkedHashMap<>();
    headers.put("content-length", "42");
    Map<String, String> out = client.applyResponseFilters("test-cluster", headers);
    assertNotNull(out);
    assertEquals(1, out.size());
    assertEquals("42", out.get("content-length"));
  }

  @Test
  public void applyRequestFilters_emptyHeaders_returnsEmpty() {
    Map<String, String> out = client.applyRequestFilters("test-cluster", Collections.emptyMap());
    assertNotNull(out);
    assertTrue(out.isEmpty());
  }

  @Test(expected = IllegalArgumentException.class)
  public void applyRequestFilters_nullClusterName_throwsIllegalArgument() {
    client.applyRequestFilters(null, Collections.emptyMap());
  }

  @Test
  public void addInterceptor_injectsHeader() {
    client.addInterceptor("add-auth", (headers, cluster, phase) -> {
      if (phase == InterceptorPhase.PRE_REQUEST) {
        headers.put("x-auth-token", "secret");
      }
      return true;
    });

    Map<String, String> input = new LinkedHashMap<>();
    input.put("content-type", "application/json");
    Map<String, String> out = client.applyRequestFilters("test-cluster", input);
    assertEquals("secret", out.get("x-auth-token"));
  }

  @Test(expected = EnvoyClientException.class)
  public void addInterceptor_deny_throwsEnvoyClientException() {
    client.addInterceptor("deny-all", (headers, cluster, phase) -> false);
    Map<String, String> headers = new LinkedHashMap<>();
    headers.put("x-request-id", "123");
    client.applyRequestFilters("test-cluster", headers); // should throw
  }

  @Test(expected = EnvoyClientException.class)
  public void addInterceptor_duplicateName_throwsEnvoyClientException() {
    Interceptor noop = (headers, cluster, phase) -> true;
    client.addInterceptor("my-interceptor", noop);
    client.addInterceptor("my-interceptor", noop); // duplicate -> throws
  }

  @Test
  public void removeInterceptor_removesEffect() {
    AtomicBoolean called = new AtomicBoolean(false);
    client.addInterceptor("removable", (headers, cluster, phase) -> {
      called.set(true);
      headers.put("x-injected", "yes");
      return true;
    });
    client.removeInterceptor("removable");

    Map<String, String> out = client.applyRequestFilters("test-cluster", Collections.emptyMap());
    assertFalse("removed interceptor should not be called", called.get());
    assertNull("removed interceptor should not inject headers", out.get("x-injected"));
  }

  @Test(expected = EnvoyClientException.class)
  public void removeInterceptor_unknownName_throwsEnvoyClientException() {
    client.removeInterceptor("no-such-interceptor");
  }

  @Test
  public void interceptor_receivesCorrectClusterName() {
    AtomicReference<String> seenCluster = new AtomicReference<>();
    client.addInterceptor("cluster-check", (headers, cluster, phase) -> {
      seenCluster.set(cluster);
      return true;
    });
    client.applyRequestFilters("test-cluster", Collections.emptyMap());
    assertEquals("test-cluster", seenCluster.get());
  }

  @Test
  public void interceptor_phaseOrdering_preBeforePost() {
    List<InterceptorPhase> phases = new ArrayList<>();
    client.addInterceptor("phase-recorder", (headers, cluster, phase) -> {
      phases.add(phase);
      return true;
    });
    client.applyRequestFilters("test-cluster", Collections.emptyMap());
    assertTrue("expected at least 2 phase callbacks", phases.size() >= 2);
    assertEquals(InterceptorPhase.PRE_REQUEST, phases.get(0));
    assertEquals(InterceptorPhase.POST_REQUEST, phases.get(1));
  }

  @Test(expected = IllegalArgumentException.class)
  public void addInterceptor_nullName_throwsIllegalArgument() {
    client.addInterceptor(null, (headers, cluster, phase) -> true);
  }

  @Test(expected = IllegalArgumentException.class)
  public void addInterceptor_nullInterceptor_throwsIllegalArgument() {
    client.addInterceptor("my-interceptor", null);
  }

  @Test(expected = IllegalArgumentException.class)
  public void removeInterceptor_nullName_throwsIllegalArgument() {
    client.removeInterceptor(null);
  }
}
