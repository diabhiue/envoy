load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_library",
    "envoy_package",
)

licenses(["notice"])  # Apache 2

envoy_package()

# JNI shared library loaded by EnvoyClient.java via System.loadLibrary("envoy_client_jni").
envoy_cc_library(
    name = "envoy_client_jni",
    srcs = ["envoy_client_jni.cc"],
    linkopts = ["-shared"],
    repository = "@envoy",
    visibility = ["//visibility:public"],
    deps = [
        "//client/library/c_api:envoy_client_lib",
    ],
)
