load("@io_bazel_rules_go//go:def.bzl", "go_test")

licenses(["notice"])  # Apache 2

go_test(
    name = "envoyclient_test",
    srcs = [
        "envoyclient_test.go",
        # Blank-imports linkstamp shim so build_scm_revision / build_scm_status
        # are defined for the CGo test binary.
        "test_setup_test.go",
    ],
    importpath = "github.com/envoyproxy/envoy/client/test/go",
    deps = [
        "//client/library/go:envoyclient",
        "//client/test/go/linkstamp",
    ],
)
