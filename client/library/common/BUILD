load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_library",
    "envoy_package",
)

licenses(["notice"])  # Apache 2

envoy_package()

envoy_cc_library(
    name = "engine_interface_lib",
    hdrs = ["engine_interface.h"],
    repository = "@envoy",
    deps = [
        ":config_store_lib",
        "@com_google_absl//absl/time",
    ],
)

envoy_cc_library(
    name = "config_store_lib",
    srcs = ["config_store.cc"],
    hdrs = ["config_store.h"],
    repository = "@envoy",
    deps = [
        "//envoy/upstream:cluster_manager_interface",
        "//envoy/upstream:load_balancer_interface",
        "//envoy/upstream:thread_local_cluster_interface",
        "//source/common/common:logger_lib",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/synchronization",
    ],
)

envoy_cc_library(
    name = "lb_context_lib",
    srcs = ["lb_context.cc"],
    hdrs = ["lb_context.h"],
    repository = "@envoy",
    deps = [
        "//envoy/upstream:load_balancer_interface",
        "//source/common/common:hash_lib",
        "//source/common/http:header_map_lib",
        "//source/common/upstream:load_balancer_context_base_lib",
    ],
)

envoy_cc_library(
    name = "filter_chain_manager_lib",
    srcs = ["filter_chain_manager.cc"],
    hdrs = ["filter_chain_manager.h"],
    repository = "@envoy",
    visibility = ["//visibility:public"],
    deps = [
        "//client/library/c_api:envoy_client_lib",
    ],
)

envoy_cc_library(
    name = "engine_lib",
    srcs = ["engine.cc"],
    hdrs = ["engine.h"],
    repository = "@envoy",
    deps = [
        ":config_store_lib",
        ":engine_interface_lib",
        "//envoy/server:instance_interface",
        "//source/common/common:logger_lib",
        "//source/common/common:thread_lib",
        "//source/common/common:random_generator_lib",
        "//source/common/event:real_time_system_lib",
        "//source/common/runtime:runtime_lib",
        "//source/exe:platform_impl_lib",
        "//source/exe:stripped_main_base_lib",
        "//source/server:listener_hooks_lib",
        "//source/server:null_overload_manager_lib",
        "//source/server:options_base",
        "@com_google_absl//absl/synchronization",
    ],
)
