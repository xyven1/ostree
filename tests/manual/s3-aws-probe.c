/*
 * SPDX-License-Identifier: LGPL-2.0+
 *
 * Standalone check of the two aws-c-auth behaviours the s3:// fetcher
 * design depends on. Not built by the normal build; see
 * docs/superpowers/specs/2026-10-09-s3-fetcher-design.md section 11.
 *
 *   gcc -o probe s3-aws-probe.c -laws-c-auth -laws-c-http -laws-c-io -laws-c-common
 *
 * Run with AWS_CONFIG_FILE pointing at a config whose [default] profile
 * has no SSO settings. Exits 0 only if both behaviours hold.
 */
#include <aws/auth/auth.h>
#include <aws/auth/credentials.h>
#include <aws/auth/signable.h>
#include <aws/auth/signing.h>
#include <aws/auth/signing_config.h>
#include <aws/auth/signing_result.h>
#include <aws/common/date_time.h>
#include <aws/common/string.h>
#include <aws/common/thread.h>
#include <aws/http/request_response.h>
#include <aws/io/channel_bootstrap.h>
#include <aws/io/event_loop.h>
#include <aws/io/host_resolver.h>
#include <aws/io/tls_channel_handler.h>
#include <stdio.h>

static int sign_cb_called = 0;
static aws_thread_id_t sign_cb_thread;

static void
on_sign (struct aws_signing_result *result, int error_code, void *user_data)
{
  sign_cb_called = 1;
  sign_cb_thread = aws_thread_current_thread_id ();
  if (error_code != 0)
    {
      printf ("sign error: %s\n", aws_error_debug_str (error_code));
      return;
    }
  struct aws_array_list *headers = NULL;
  aws_signing_result_get_property_list (result, g_aws_http_headers_property_list_name, &headers);
  const size_t n = headers ? aws_array_list_length (headers) : 0;
  for (size_t i = 0; i < n; i++)
    {
      struct aws_signing_result_property p;
      aws_array_list_get_at (headers, &p, i);
      printf ("HDR %s: %s\n", aws_string_c_str (p.name), aws_string_c_str (p.value));
    }
}

int
main (void)
{
  struct aws_allocator *a = aws_default_allocator ();
  aws_auth_library_init (a);

  /* Assumption 1: signing with explicit credentials completes inline. */
  struct aws_credentials *creds
      = aws_credentials_new (a, aws_byte_cursor_from_c_str ("AKIAIOSFODNN7EXAMPLE"),
                             aws_byte_cursor_from_c_str ("wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"),
                             (struct aws_byte_cursor){ 0 }, UINT64_MAX);
  struct aws_http_message *msg = aws_http_message_new_request (a);
  aws_http_message_set_request_method (msg, aws_byte_cursor_from_c_str ("GET"));
  aws_http_message_set_request_path (
      msg, aws_byte_cursor_from_c_str ("/repo/deltas/ab/c%2Bd/superblock"));
  struct aws_http_header host = { .name = aws_byte_cursor_from_c_str ("host"),
                                  .value = aws_byte_cursor_from_c_str (
                                      "examplebucket.s3.amazonaws.com") };
  aws_http_message_add_header (msg, host);
  struct aws_signable *signable = aws_signable_new_http_request (a, msg);

  struct aws_signing_config_aws cfg;
  AWS_ZERO_STRUCT (cfg);
  cfg.config_type = AWS_SIGNING_CONFIG_AWS;
  cfg.algorithm = AWS_SIGNING_ALGORITHM_V4;
  cfg.signature_type = AWS_ST_HTTP_REQUEST_HEADERS;
  cfg.region = aws_byte_cursor_from_c_str ("us-east-1");
  cfg.service = aws_byte_cursor_from_c_str ("s3");
  aws_date_time_init_epoch_secs (&cfg.date, 1369353600.0); /* 20130524T000000Z */
  cfg.flags.use_double_uri_encode = false;
  cfg.flags.should_normalize_uri_path = false;
  cfg.signed_body_value = g_aws_signed_body_value_unsigned_payload;
  cfg.signed_body_header = AWS_SBHT_X_AMZ_CONTENT_SHA256;
  cfg.credentials = creds;

  const int rc
      = aws_sign_request_aws (a, signable, (struct aws_signing_config_base *)&cfg, on_sign, NULL);
  const int same_thread
      = sign_cb_called
        && aws_thread_thread_id_equal (sign_cb_thread, aws_thread_current_thread_id ());
  printf ("SIGN rc=%d callback_called_before_return=%d same_thread=%d\n", rc, sign_cb_called,
          same_thread);
  const int sign_ok = rc == 0 && sign_cb_called && same_thread;

  /* Assumption 2: no SSO provider when the profile has no SSO settings. */
  struct aws_event_loop_group_options elg_opts = { .loop_count = 1 };
  struct aws_event_loop_group *elg = aws_event_loop_group_new (a, &elg_opts);
  struct aws_host_resolver_default_options res_opts = { .max_entries = 8, .el_group = elg };
  struct aws_host_resolver *resolver = aws_host_resolver_new_default (a, &res_opts);
  struct aws_client_bootstrap_options bs_opts
      = { .event_loop_group = elg, .host_resolver = resolver };
  struct aws_client_bootstrap *bootstrap = aws_client_bootstrap_new (a, &bs_opts);
  struct aws_tls_ctx_options tls_opts;
  aws_tls_ctx_options_init_default_client (&tls_opts, a);
  struct aws_tls_ctx *tls = aws_tls_client_ctx_new (a, &tls_opts);
  aws_tls_ctx_options_clean_up (&tls_opts);

  struct aws_credentials_provider_sso_options sso_opts;
  AWS_ZERO_STRUCT (sso_opts);
  sso_opts.bootstrap = bootstrap;
  sso_opts.tls_ctx = tls;
  struct aws_credentials_provider *sso = aws_credentials_provider_new_sso (a, &sso_opts);
  printf ("SSO provider=%s\n", sso ? "non-NULL" : "NULL");

  return (sign_ok && sso == NULL) ? 0 : 1;
}
