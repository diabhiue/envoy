load("@io_bazel_rules_go//go:def.bzl", "go_library")

licenses(["notice"])  # Apache 2

# CGo wrapper that pulls test_version_linkstamp into Go test binaries.
# See linkstamp.go for rationale.
go_library(
    name = "linkstamp",
    srcs = ["linkstamp.go"],
    cgo = True,
    cdeps = [
        # Provides build_scm_revision / build_scm_status for version_lib.
        "//test/test_common:test_version_linkstamp",
    ],
    importpath = "github.com/envoyproxy/envoy/client/test/go/linkstamp",
    visibility = ["//client/test/go:__pkg__"],
)
