// Package envoyclient_test - test binary linkage setup.
//
// The blank import below pulls in the test version linkstamp (via a CGo
// wrapper library) so that build_scm_revision / build_scm_status are defined
// at link time.  These symbols are required by version_lib, which is
// transitively linked through the engine stack.
package envoyclient_test

import _ "github.com/envoyproxy/envoy/client/test/go/linkstamp"
