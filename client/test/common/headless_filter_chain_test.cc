#include "client/library/common/headless_filter_chain.h"

#include "source/common/event/real_time_system.h"
#include "source/common/http/header_map_impl.h"
#include "source/extensions/filters/http/common/pass_through_filter.h"

#include "test/mocks/event/mocks.h"
#include "test/mocks/upstream/cluster_manager.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using ::testing::NiceMock;

namespace Envoy {
namespace Client {
namespace {

// ---------------------------------------------------------------------------
// Minimal inline filter implementations.
// PassThroughDecoderFilter is in namespace Envoy::Http.
// ---------------------------------------------------------------------------

// A filter that always passes through and stamps a "x-pass: 1" header.
class SyncPassFilter : public Http::PassThroughDecoderFilter {
public:
  Http::FilterHeadersStatus decodeHeaders(Http::RequestHeaderMap& headers,
                                          bool end_stream) override {
    headers.addCopy(Http::LowerCaseString("x-pass"), "1");
    return Http::PassThroughDecoderFilter::decodeHeaders(headers, end_stream);
  }
};

// A filter that denies by calling sendLocalReply(403).
class SyncDenyFilter : public Http::PassThroughDecoderFilter {
public:
  Http::FilterHeadersStatus decodeHeaders(Http::RequestHeaderMap&, bool) override {
    decoder_callbacks_->sendLocalReply(Http::Code::Forbidden, "denied", nullptr, absl::nullopt,
                                       "test_deny");
    return Http::FilterHeadersStatus::StopIteration;
  }
};

// A filter that pauses on headers and can be resumed externally.
class AsyncPassFilter : public Http::PassThroughDecoderFilter {
public:
  Http::FilterHeadersStatus decodeHeaders(Http::RequestHeaderMap&, bool) override {
    paused_ = true;
    return Http::FilterHeadersStatus::StopIteration;
  }

  void resume() {
    paused_ = false;
    decoder_callbacks_->continueDecoding();
  }

  bool paused() const { return paused_; }

private:
  bool paused_{false};
};

// A filter that appends a string to "x-tag" header.
class TagFilter : public Http::PassThroughDecoderFilter {
public:
  explicit TagFilter(std::string tag) : tag_(std::move(tag)) {}

  Http::FilterHeadersStatus decodeHeaders(Http::RequestHeaderMap& headers,
                                          bool end_stream) override {
    headers.addCopy(Http::LowerCaseString("x-tag"), tag_);
    return Http::PassThroughDecoderFilter::decodeHeaders(headers, end_stream);
  }

private:
  const std::string tag_;
};

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class HeadlessFilterChainTest : public testing::Test {
public:
  HeadlessFilterChainTest() : chain_(dispatcher_, cm_, time_system_) {}

  Http::RequestHeaderMapPtr makeRequestHeaders() {
    auto headers = Http::RequestHeaderMapImpl::create();
    headers->setMethod("GET");
    headers->setPath("/test");
    headers->setHost("example.com");
    return headers;
  }

  Http::ResponseHeaderMapPtr makeResponseHeaders() {
    auto headers = Http::ResponseHeaderMapImpl::create();
    headers->setStatus("200");
    return headers;
  }

  // Synchronously invoke applyRequestFilters and capture the result.
  struct Result {
    FilterChainResult chain_result;
    Http::RequestHeaderMapPtr headers;
    bool called{false};
  };

  Result applyRequest(const std::string& cluster = "test_cluster") {
    Result r;
    chain_.applyRequestFilters(cluster, makeRequestHeaders(),
                               [&r](FilterChainResult res, Http::RequestHeaderMapPtr hdrs) {
                                 r.chain_result = res;
                                 r.headers = std::move(hdrs);
                                 r.called = true;
                               });
    return r;
  }

  NiceMock<Event::MockDispatcher> dispatcher_;
  NiceMock<Upstream::MockClusterManager> cm_;
  Event::RealTimeSystem time_system_;
  HeadlessFilterChain chain_;
};

// ---------------------------------------------------------------------------
// Empty chain tests
// ---------------------------------------------------------------------------

TEST_F(HeadlessFilterChainTest, EmptyChainAllows) {
  auto r = applyRequest();
  EXPECT_TRUE(r.called);
  EXPECT_EQ(r.chain_result.status, FilterChainResult::Status::Allow);
  EXPECT_NE(r.headers, nullptr);
}

TEST_F(HeadlessFilterChainTest, EmptyChainPreservesHeaders) {
  auto r = applyRequest();
  ASSERT_NE(r.headers, nullptr);
  EXPECT_EQ(r.headers->getMethodValue(), "GET");
  EXPECT_EQ(r.headers->getPathValue(), "/test");
  EXPECT_EQ(r.headers->getHostValue(), "example.com");
}

// ---------------------------------------------------------------------------
// Native filter factory tests
// ---------------------------------------------------------------------------

TEST_F(HeadlessFilterChainTest, SinglePassFilterAllows) {
  chain_.addFilterFactory("pass", [](Http::FilterChainFactoryCallbacks& cbs) {
    cbs.addStreamDecoderFilter(std::make_shared<SyncPassFilter>());
  });

  auto r = applyRequest();
  EXPECT_EQ(r.chain_result.status, FilterChainResult::Status::Allow);
  ASSERT_NE(r.headers, nullptr);
  auto vals = r.headers->get(Http::LowerCaseString("x-pass"));
  ASSERT_EQ(vals.size(), 1u);
  EXPECT_EQ(vals[0]->value().getStringView(), "1");
}

TEST_F(HeadlessFilterChainTest, SingleDenyFilterDenies) {
  chain_.addFilterFactory("deny", [](Http::FilterChainFactoryCallbacks& cbs) {
    cbs.addStreamDecoderFilter(std::make_shared<SyncDenyFilter>());
  });

  auto r = applyRequest();
  EXPECT_EQ(r.chain_result.status, FilterChainResult::Status::Deny);
  EXPECT_EQ(r.chain_result.deny_code, 403u);
}

TEST_F(HeadlessFilterChainTest, DenyFilterShortCircuits) {
  // pass runs first (adds x-pass), then deny stops the chain.
  chain_.addFilterFactory("pass", [](Http::FilterChainFactoryCallbacks& cbs) {
    cbs.addStreamDecoderFilter(std::make_shared<SyncPassFilter>());
  });
  chain_.addFilterFactory("deny", [](Http::FilterChainFactoryCallbacks& cbs) {
    cbs.addStreamDecoderFilter(std::make_shared<SyncDenyFilter>());
  });

  auto r = applyRequest();
  EXPECT_EQ(r.chain_result.status, FilterChainResult::Status::Deny);
  EXPECT_EQ(r.chain_result.deny_code, 403u);
}

TEST_F(HeadlessFilterChainTest, MultiplePassFiltersAccumulateHeaders) {
  chain_.addFilterFactory("tag_a", [](Http::FilterChainFactoryCallbacks& cbs) {
    cbs.addStreamDecoderFilter(std::make_shared<TagFilter>("a"));
  });
  chain_.addFilterFactory("tag_b", [](Http::FilterChainFactoryCallbacks& cbs) {
    cbs.addStreamDecoderFilter(std::make_shared<TagFilter>("b"));
  });

  auto r = applyRequest();
  EXPECT_EQ(r.chain_result.status, FilterChainResult::Status::Allow);
  ASSERT_NE(r.headers, nullptr);
  auto tags = r.headers->get(Http::LowerCaseString("x-tag"));
  ASSERT_EQ(tags.size(), 2u);
  EXPECT_EQ(tags[0]->value().getStringView(), "a");
  EXPECT_EQ(tags[1]->value().getStringView(), "b");
}

TEST_F(HeadlessFilterChainTest, FilterFactoryCountReflectsRegistrations) {
  EXPECT_EQ(chain_.filterFactoryCount(), 0u);
  chain_.addFilterFactory("f1", [](Http::FilterChainFactoryCallbacks& cbs) {
    cbs.addStreamDecoderFilter(std::make_shared<SyncPassFilter>());
  });
  EXPECT_EQ(chain_.filterFactoryCount(), 1u);
  chain_.addFilterFactory("f2", [](Http::FilterChainFactoryCallbacks& cbs) {
    cbs.addStreamDecoderFilter(std::make_shared<SyncPassFilter>());
  });
  EXPECT_EQ(chain_.filterFactoryCount(), 2u);
}

// ---------------------------------------------------------------------------
// Async filter tests
// ---------------------------------------------------------------------------

TEST_F(HeadlessFilterChainTest, AsyncFilterResumesAndAllows) {
  std::shared_ptr<AsyncPassFilter> async_filter;

  chain_.addFilterFactory("async", [&async_filter](Http::FilterChainFactoryCallbacks& cbs) {
    auto f = std::make_shared<AsyncPassFilter>();
    async_filter = f;
    cbs.addStreamDecoderFilter(f);
  });

  bool cb_called = false;
  FilterChainResult result;
  Http::RequestHeaderMapPtr out_headers;

  chain_.applyRequestFilters("cluster", makeRequestHeaders(),
                             [&](FilterChainResult r, Http::RequestHeaderMapPtr h) {
                               cb_called = true;
                               result = r;
                               out_headers = std::move(h);
                             });

  // Callback must NOT have been fired yet — filter is paused.
  EXPECT_FALSE(cb_called);
  ASSERT_NE(async_filter, nullptr);
  EXPECT_TRUE(async_filter->paused());

  // Simulate async work completing.
  async_filter->resume();

  EXPECT_TRUE(cb_called);
  EXPECT_EQ(result.status, FilterChainResult::Status::Allow);
  EXPECT_NE(out_headers, nullptr);
}

TEST_F(HeadlessFilterChainTest, AsyncFilterFollowedBySyncPassAllows) {
  std::shared_ptr<AsyncPassFilter> async_filter;

  chain_.addFilterFactory("async", [&async_filter](Http::FilterChainFactoryCallbacks& cbs) {
    auto f = std::make_shared<AsyncPassFilter>();
    async_filter = f;
    cbs.addStreamDecoderFilter(f);
  });
  chain_.addFilterFactory("tag", [](Http::FilterChainFactoryCallbacks& cbs) {
    cbs.addStreamDecoderFilter(std::make_shared<TagFilter>("after_async"));
  });

  bool cb_called = false;
  FilterChainResult result;
  Http::RequestHeaderMapPtr out_headers;

  chain_.applyRequestFilters("cluster", makeRequestHeaders(),
                             [&](FilterChainResult r, Http::RequestHeaderMapPtr h) {
                               cb_called = true;
                               result = r;
                               out_headers = std::move(h);
                             });

  EXPECT_FALSE(cb_called);
  async_filter->resume();

  EXPECT_TRUE(cb_called);
  EXPECT_EQ(result.status, FilterChainResult::Status::Allow);
  ASSERT_NE(out_headers, nullptr);
  auto tags = out_headers->get(Http::LowerCaseString("x-tag"));
  ASSERT_EQ(tags.size(), 1u);
  EXPECT_EQ(tags[0]->value().getStringView(), "after_async");
}

// ---------------------------------------------------------------------------
// Interceptor tests
// ---------------------------------------------------------------------------

TEST_F(HeadlessFilterChainTest, PreRequestInterceptorDenyShortCircuits) {
  chain_.addInterceptor(
      {"deny_all", [](Http::RequestHeaderMap&, const std::string&, InterceptorPhase) {
         return false;
       }, nullptr});

  auto r = applyRequest();
  EXPECT_EQ(r.chain_result.status, FilterChainResult::Status::Deny);
  EXPECT_EQ(r.chain_result.deny_code, 403u);
}

TEST_F(HeadlessFilterChainTest, PreRequestInterceptorAllowContinues) {
  chain_.addInterceptor(
      {"allow_all",
       [](Http::RequestHeaderMap& headers, const std::string&, InterceptorPhase phase) {
         if (phase == InterceptorPhase::PreRequest) {
           headers.addCopy(Http::LowerCaseString("x-intercepted"), "yes");
         }
         return true;
       },
       nullptr});

  auto r = applyRequest();
  EXPECT_EQ(r.chain_result.status, FilterChainResult::Status::Allow);
  ASSERT_NE(r.headers, nullptr);
  auto vals = r.headers->get(Http::LowerCaseString("x-intercepted"));
  ASSERT_EQ(vals.size(), 1u);
  EXPECT_EQ(vals[0]->value().getStringView(), "yes");
}

TEST_F(HeadlessFilterChainTest, PostRequestInterceptorRunsAfterNativeFilters) {
  chain_.addFilterFactory("tag", [](Http::FilterChainFactoryCallbacks& cbs) {
    cbs.addStreamDecoderFilter(std::make_shared<TagFilter>("native"));
  });

  std::vector<std::string> call_order;
  chain_.addInterceptor(
      {"interceptor",
       [&call_order](Http::RequestHeaderMap&, const std::string&, InterceptorPhase phase) {
         if (phase == InterceptorPhase::PreRequest) {
           call_order.push_back("pre");
         } else if (phase == InterceptorPhase::PostRequest) {
           call_order.push_back("post");
         }
         return true;
       },
       nullptr});

  auto r = applyRequest();
  EXPECT_EQ(r.chain_result.status, FilterChainResult::Status::Allow);
  ASSERT_EQ(call_order.size(), 2u);
  EXPECT_EQ(call_order[0], "pre");
  EXPECT_EQ(call_order[1], "post");
  ASSERT_NE(r.headers, nullptr);
  EXPECT_FALSE(r.headers->get(Http::LowerCaseString("x-tag")).empty());
}

TEST_F(HeadlessFilterChainTest, PostRequestInterceptorDenies) {
  chain_.addFilterFactory("pass", [](Http::FilterChainFactoryCallbacks& cbs) {
    cbs.addStreamDecoderFilter(std::make_shared<SyncPassFilter>());
  });
  chain_.addInterceptor(
      {"post_deny",
       [](Http::RequestHeaderMap&, const std::string&, InterceptorPhase phase) {
         return phase != InterceptorPhase::PostRequest;
       },
       nullptr});

  auto r = applyRequest();
  EXPECT_EQ(r.chain_result.status, FilterChainResult::Status::Deny);
}

TEST_F(HeadlessFilterChainTest, AddInterceptorReplacesByName) {
  int call_count = 0;
  chain_.addInterceptor(
      {"counter",
       [&call_count](Http::RequestHeaderMap&, const std::string&, InterceptorPhase) {
         ++call_count;
         return true;
       },
       nullptr});
  chain_.addInterceptor(
      {"counter",
       [&call_count](Http::RequestHeaderMap&, const std::string&, InterceptorPhase) {
         call_count += 10;
         return true;
       },
       nullptr});

  EXPECT_EQ(chain_.interceptorCount(), 1u);
  applyRequest();
  // Only the replacement ran: pre=+10, post=+10 → 20 (not 1+10=11).
  EXPECT_EQ(call_count, 20);
}

TEST_F(HeadlessFilterChainTest, RemoveInterceptorByName) {
  chain_.addInterceptor(
      {"to_remove",
       [](Http::RequestHeaderMap&, const std::string&, InterceptorPhase) {
         return false; // would deny if still registered
       },
       nullptr});
  EXPECT_EQ(chain_.interceptorCount(), 1u);

  chain_.removeInterceptor("to_remove");
  EXPECT_EQ(chain_.interceptorCount(), 0u);

  auto r = applyRequest();
  EXPECT_EQ(r.chain_result.status, FilterChainResult::Status::Allow);
}

TEST_F(HeadlessFilterChainTest, RemoveNonexistentInterceptorIsNoop) {
  EXPECT_EQ(chain_.interceptorCount(), 0u);
  EXPECT_NO_THROW(chain_.removeInterceptor("does_not_exist"));
  EXPECT_EQ(chain_.interceptorCount(), 0u);
}

// ---------------------------------------------------------------------------
// Response filter pipeline tests
// ---------------------------------------------------------------------------

TEST_F(HeadlessFilterChainTest, EmptyResponseChainAllows) {
  bool cb_called = false;
  FilterChainResult result;
  chain_.applyResponseFilters("cluster", makeResponseHeaders(),
                              [&](FilterChainResult r, Http::ResponseHeaderMapPtr) {
                                cb_called = true;
                                result = r;
                              });
  EXPECT_TRUE(cb_called);
  EXPECT_EQ(result.status, FilterChainResult::Status::Allow);
}

TEST_F(HeadlessFilterChainTest, ResponseInterceptorDenies) {
  chain_.addInterceptor(
      {"resp_deny", nullptr,
       [](Http::ResponseHeaderMap&, const std::string&, InterceptorPhase) { return false; }});

  bool cb_called = false;
  FilterChainResult result;
  chain_.applyResponseFilters("cluster", makeResponseHeaders(),
                              [&](FilterChainResult r, Http::ResponseHeaderMapPtr) {
                                cb_called = true;
                                result = r;
                              });
  EXPECT_TRUE(cb_called);
  EXPECT_EQ(result.status, FilterChainResult::Status::Deny);
  EXPECT_EQ(result.deny_code, 403u);
}

TEST_F(HeadlessFilterChainTest, ResponseInterceptorAllowsAndModifiesHeaders) {
  chain_.addInterceptor(
      {"resp_allow", nullptr,
       [](Http::ResponseHeaderMap& h, const std::string&, InterceptorPhase phase) {
         if (phase == InterceptorPhase::PreResponse) {
           h.addCopy(Http::LowerCaseString("x-resp"), "ok");
         }
         return true;
       }});

  bool cb_called = false;
  Http::ResponseHeaderMapPtr out;
  chain_.applyResponseFilters("cluster", makeResponseHeaders(),
                              [&](FilterChainResult r, Http::ResponseHeaderMapPtr h) {
                                cb_called = true;
                                EXPECT_EQ(r.status, FilterChainResult::Status::Allow);
                                out = std::move(h);
                              });
  EXPECT_TRUE(cb_called);
  ASSERT_NE(out, nullptr);
  auto vals = out->get(Http::LowerCaseString("x-resp"));
  ASSERT_EQ(vals.size(), 1u);
  EXPECT_EQ(vals[0]->value().getStringView(), "ok");
}

} // namespace
} // namespace Client
} // namespace Envoy
