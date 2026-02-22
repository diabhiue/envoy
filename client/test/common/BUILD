load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_test",
    "envoy_package",
)

licenses(["notice"])  # Apache 2

envoy_package()

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
