// Package linkstamp is a CGo shim used exclusively in test binaries.
//
// It provides the build_scm_revision / build_scm_status symbols required by
// version_lib (pulled in transitively through engine_lib → StrippedMainBase →
// server_base_lib → version_lib).  In CC tests these symbols come from
// //test/test_common:test_version_linkstamp via envoy_cc_test; for Go tests we
// need to supply them via a CGo cdep on the test binary.
//
// Import this package with a blank import in any Go test that links against
// the envoyclient library:
//
//	import _ "github.com/envoyproxy/envoy/client/test/go/linkstamp"
package linkstamp

// #include <stdlib.h>
import "C"
