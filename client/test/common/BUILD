load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_test",
    "envoy_package",
)

licenses(["notice"])  # Apache 2

envoy_package()

envoy_cc_test(
    name = "engine_test",
    srcs = ["engine_test.cc"],
    repository = "@envoy",
    deps = [
        "//client/library/common:engine_lib",
        "//source/server:options_base",
    ],
)

envoy_cc_test(
    name = "config_store_test",
    srcs = ["config_store_test.cc"],
    repository = "@envoy",
    deps = [
        "//client/library/common:config_store_lib",
        "//source/common/network:utility_lib",
        "//test/mocks/upstream:cluster_manager_mocks",
        "//test/mocks/upstream:host_mocks",
        "//test/mocks/upstream:host_set_mocks",
        "//test/mocks/upstream:priority_set_mocks",
        "//test/mocks/upstream:thread_local_cluster_mocks",
    ],
)

envoy_cc_test(
    name = "lb_context_test",
    srcs = ["lb_context_test.cc"],
    repository = "@envoy",
    deps = [
        "//client/library/common:lb_context_lib",
        "//source/common/http:header_map_lib",
    ],
)
