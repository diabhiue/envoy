load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_test",
    "envoy_package",
)

licenses(["notice"])  # Apache 2

envoy_package()

envoy_cc_test(
    name = "client_test",
    srcs = ["client_test.cc"],
    repository = "@envoy",
    deps = [
        "//client/library/cc:client_lib",
        "//client/library/common:config_store_lib",
        "//client/library/common:engine_interface_lib",
        "//client/library/common:headless_filter_chain_lib",
        "//source/common/event:real_time_system_lib",
        "//source/common/http:header_map_lib",
        "//source/common/network:utility_lib",
        "//source/extensions/filters/http/common:pass_through_filter_lib",
        "//test/mocks/event:event_mocks",
        "//test/mocks/upstream:cluster_manager_mocks",
        "//test/mocks/upstream:host_mocks",
        "//test/mocks/upstream:host_set_mocks",
        "//test/mocks/upstream:priority_set_mocks",
        "//test/mocks/upstream:thread_local_cluster_mocks",
    ],
)
