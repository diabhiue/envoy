/*
 * C trampolines that bridge the const-correct callback typedef signatures in
 * envoy_client.h to the non-const char* signatures that CGo generates for
 * //export-ed Go functions.
 *
 * This must be a separate .c file (not part of the CGo preamble) because CGo
 * requires that preambles contain only declarations when //export is used.
 */

#include "client/library/c_api/envoy_client.h"

/* Forward-declare the Go-exported functions with the signatures CGo generates
 * (non-const char*). */
extern void goConfigCB(char* resource_type, char* resource_name,
                       envoy_client_config_event event, void* context);
extern void goLbContextCB(char* cluster_name,
                          envoy_client_request_context* ctx, void* context);

void configCBTrampoline(const char* rt, const char* rn,
                        envoy_client_config_event event, void* ctx) {
  goConfigCB((char*)rt, (char*)rn, event, ctx);
}

void lbContextCBTrampoline(const char* cluster,
                           envoy_client_request_context* ctx, void* userCtx) {
  goLbContextCB((char*)cluster, ctx, userCtx);
}
