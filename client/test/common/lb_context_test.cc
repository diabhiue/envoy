#include "client/library/common/lb_context.h"

#include "gtest/gtest.h"

namespace Envoy {
namespace Client {
namespace {

// Empty context: all methods return defaults.
TEST(ClientLoadBalancerContextTest, EmptyContext) {
  ClientLoadBalancerContext ctx("", "", false, "", "");

  EXPECT_FALSE(ctx.computeHashKey().has_value());
  EXPECT_EQ(ctx.downstreamHeaders(), nullptr);
  EXPECT_FALSE(ctx.overrideHostToSelect().has_value());
}

// hash_key is hashed deterministically; same input → same output.
TEST(ClientLoadBalancerContextTest, HashKeyDeterministic) {
  ClientLoadBalancerContext ctx1("my-session-id", "", false, "", "");
  ClientLoadBalancerContext ctx2("my-session-id", "", false, "", "");

  ASSERT_TRUE(ctx1.computeHashKey().has_value());
  ASSERT_TRUE(ctx2.computeHashKey().has_value());
  EXPECT_EQ(ctx1.computeHashKey().value(), ctx2.computeHashKey().value());
}

// Different hash keys produce different hashes.
TEST(ClientLoadBalancerContextTest, HashKeyDifferentInputs) {
  ClientLoadBalancerContext ctx1("key-A", "", false, "", "");
  ClientLoadBalancerContext ctx2("key-B", "", false, "", "");

  ASSERT_TRUE(ctx1.computeHashKey().has_value());
  ASSERT_TRUE(ctx2.computeHashKey().has_value());
  EXPECT_NE(ctx1.computeHashKey().value(), ctx2.computeHashKey().value());
}

// Empty hash_key → nullopt (no hash provided).
TEST(ClientLoadBalancerContextTest, EmptyHashKeyIsNullopt) {
  ClientLoadBalancerContext ctx("", "", false, "", "");
  EXPECT_FALSE(ctx.computeHashKey().has_value());
}

// override_host is reflected with strict=false.
TEST(ClientLoadBalancerContextTest, OverrideHostNonStrict) {
  ClientLoadBalancerContext ctx("", "10.0.0.1:8080", false, "", "");

  auto override = ctx.overrideHostToSelect();
  ASSERT_TRUE(override.has_value());
  EXPECT_EQ(override->first, "10.0.0.1:8080");
  EXPECT_FALSE(override->second);
}

// override_host with strict=true.
TEST(ClientLoadBalancerContextTest, OverrideHostStrict) {
  ClientLoadBalancerContext ctx("", "10.0.0.2:9090", true, "", "");

  auto override = ctx.overrideHostToSelect();
  ASSERT_TRUE(override.has_value());
  EXPECT_EQ(override->first, "10.0.0.2:9090");
  EXPECT_TRUE(override->second);
}

// Empty override_host → nullopt.
TEST(ClientLoadBalancerContextTest, EmptyOverrideHostIsNullopt) {
  ClientLoadBalancerContext ctx("", "", true, "", "");
  EXPECT_FALSE(ctx.overrideHostToSelect().has_value());
}

// path only → header map has :path, no :authority.
TEST(ClientLoadBalancerContextTest, PathOnlyPopulatesHeaders) {
  ClientLoadBalancerContext ctx("", "", false, "/api/v1/foo", "");

  const Http::RequestHeaderMap* headers = ctx.downstreamHeaders();
  ASSERT_NE(headers, nullptr);
  EXPECT_EQ(headers->getPathValue(), "/api/v1/foo");
  EXPECT_TRUE(headers->getHostValue().empty());
}

// authority only → header map has :authority, no :path.
TEST(ClientLoadBalancerContextTest, AuthorityOnlyPopulatesHeaders) {
  ClientLoadBalancerContext ctx("", "", false, "", "example.com");

  const Http::RequestHeaderMap* headers = ctx.downstreamHeaders();
  ASSERT_NE(headers, nullptr);
  EXPECT_EQ(headers->getHostValue(), "example.com");
  EXPECT_TRUE(headers->getPathValue().empty());
}

// Both path and authority → both headers set.
TEST(ClientLoadBalancerContextTest, PathAndAuthorityBothSet) {
  ClientLoadBalancerContext ctx("", "", false, "/search", "api.example.com");

  const Http::RequestHeaderMap* headers = ctx.downstreamHeaders();
  ASSERT_NE(headers, nullptr);
  EXPECT_EQ(headers->getPathValue(), "/search");
  EXPECT_EQ(headers->getHostValue(), "api.example.com");
}

// Neither path nor authority → nullptr (no unnecessary allocation).
TEST(ClientLoadBalancerContextTest, NeitherPathNorAuthorityIsNullptr) {
  ClientLoadBalancerContext ctx("hash", "10.0.0.1:80", false, "", "");
  EXPECT_EQ(ctx.downstreamHeaders(), nullptr);
}

// All fields set simultaneously.
TEST(ClientLoadBalancerContextTest, AllFieldsSet) {
  ClientLoadBalancerContext ctx("session-xyz", "10.1.2.3:443", true, "/v2/items",
                                "items.svc.cluster.local");

  EXPECT_TRUE(ctx.computeHashKey().has_value());

  auto override = ctx.overrideHostToSelect();
  ASSERT_TRUE(override.has_value());
  EXPECT_EQ(override->first, "10.1.2.3:443");
  EXPECT_TRUE(override->second);

  const Http::RequestHeaderMap* headers = ctx.downstreamHeaders();
  ASSERT_NE(headers, nullptr);
  EXPECT_EQ(headers->getPathValue(), "/v2/items");
  EXPECT_EQ(headers->getHostValue(), "items.svc.cluster.local");
}

} // namespace
} // namespace Client
} // namespace Envoy
