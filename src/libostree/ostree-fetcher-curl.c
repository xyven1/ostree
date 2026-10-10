/*
 * Copyright (C) 2016 Colin Walters <walters@verbum.org>
 * Copyright (C) 2022 Igalia S.L.
 *
 * SPDX-License-Identifier: LGPL-2.0+
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <https://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <curl/curl.h>
#include <gio/gfiledescriptorbased.h>
#include <gio/gunixoutputstream.h>
#include <glib-unix.h>
#include <stdbool.h>

/* These macros came from 7.43.0, but we want to check
 * for versions a bit earlier than that (to work on CentOS 7),
 * so define them here if we're using an older version.
 */
#ifndef CURL_VERSION_BITS
#define CURL_VERSION_BITS(x, y, z) ((x) << 16 | (y) << 8 | z)
#endif
#ifndef CURL_AT_LEAST_VERSION
#define CURL_AT_LEAST_VERSION(x, y, z) (LIBCURL_VERSION_NUM >= CURL_VERSION_BITS (x, y, z))
#endif

/* Cargo culted from https://github.com/curl/curl/blob/curl-7_53_0/docs/examples/http2-download.c
 */
#ifndef CURLPIPE_MULTIPLEX
/* This little trick will just make sure that we don't enable pipelining for
   libcurls old enough to not have this symbol. It is _not_ defined to zero in
   a recent libcurl header. */
#define CURLPIPE_MULTIPLEX 0
#endif

#include "ostree-date-utils-private.h"
#include "ostree-enumtypes.h"
#include "ostree-fetcher-util.h"
#include "ostree-fetcher.h"
#include "ostree-repo-private.h"
#include "otutil.h"

#ifdef HAVE_AWS
#include "ostree-aws.h"
#include "ostree-fetcher-s3.h"

/* How much of a non-2xx S3 response body to keep for error reporting. */
#define S3_ERROR_BODY_MAX (64 * 1024)
/* Signing region for ?endpoint= remotes that resolve none; S3-compatible
 * servers generally accept any. */
#define S3_ENDPOINT_FALLBACK_REGION "us-east-1"
#endif

typedef struct FetcherRequest FetcherRequest;
typedef struct SockInfo SockInfo;

static int sock_cb (CURL *e, curl_socket_t s, int what, void *cbp, void *sockp);
static gboolean timer_cb (gpointer data);
static void sock_unref (SockInfo *f);
static int update_timeout_cb (CURLM *multi, long timeout_ms, void *userp);
static void request_unref (FetcherRequest *req);
static void initiate_next_curl_request (FetcherRequest *req, GTask *task);
static void destroy_and_unref_source (GSource *source);

#ifdef HAVE_AWS
typedef struct S3Remote S3Remote;
typedef struct S3CredState S3CredState;
static void s3_remote_free (S3Remote *remote);
static void s3_cred_state_free (S3CredState *cs);
#endif

struct OstreeFetcher
{
  GObject parent_instance;

  OstreeFetcherConfigFlags config_flags;
  char *remote_name;
  char *tls_ca_db_path;
  char *tls_client_cert_path;
  char *tls_client_key_path;
  char *cookie_jar_path;
  char *proxy;
  struct curl_slist *extra_headers;
  int tmpdir_dfd;
  bool force_anonymous;
  bool finalizing; // Set if we're in the process of teardown
  char *custom_user_agent;
  guint32 opt_low_speed_limit;
  guint32 opt_low_speed_time;
  gboolean opt_retry_all;
  guint32 opt_max_outstanding_fetcher_requests;

  GMainContext *mainctx;
  CURLM *multi;
  GSource *timer_event;
  int curl_running;
  GHashTable *outstanding_requests; /* Set<GTask> */
  GHashTable *sockets;              /* Set<SockInfo> */

  guint64 bytes_transferred;

#ifdef HAVE_AWS
  GHashTable *s3_remotes; /* base URI string -> S3Remote* */
  GHashTable *s3_creds;   /* "profile\nregion" -> S3CredState* */
#endif
};

/* Information associated with a request */
struct FetcherRequest
{
  guint refcount;
  GPtrArray *mirrorlist;
  guint idx;

  char *filename;
  guint64 current_size;
  guint64 max_size;
  OstreeFetcherRequestFlags flags;
  struct curl_slist *req_headers;
  char *if_none_match;       /* request ETag */
  guint64 if_modified_since; /* seconds since the epoch */
  gboolean is_membuf;
  GError *caught_write_error;
  GLnxTmpfile tmpf;
  GString *output_buf;
  gboolean out_not_modified; /* TRUE if the server gave a HTTP 304 Not Modified response, which we
                                don’t propagate as an error */
  char *out_etag;            /* response ETag */
  guint64 out_last_modified; /* response Last-Modified, seconds since the epoch */

  CURL *easy;
  char error[CURL_ERROR_SIZE];

  OstreeFetcher *fetcher;

#ifdef HAVE_AWS
  gboolean is_s3;                 /* current attempt targets an s3:// mirror */
  gboolean s3_retried_expired;    /* already retried once after an expired-token error */
  GString *s3_error_body;         /* body of a non-2xx response, capped */
  char *s3_bucket_region;         /* x-amz-bucket-region response header */
  const char *s3_creds_key;       /* owned by the S3Remote */
  OstreeAwsCredentials *s3_creds; /* what the current attempt was signed with */
#endif
};

/* Information associated with a specific socket */
struct SockInfo
{
  guint refcount;
  curl_socket_t sockfd;
  int action;
  long timeout;
  GSource *ch;
  OstreeFetcher *fetcher;
};

enum
{
  PROP_0,
  PROP_CONFIG_FLAGS
};

G_DEFINE_TYPE (OstreeFetcher, _ostree_fetcher, G_TYPE_OBJECT)

static void
_ostree_fetcher_set_property (GObject *object, guint prop_id, const GValue *value,
                              GParamSpec *pspec)
{
  OstreeFetcher *self = OSTREE_FETCHER (object);

  switch (prop_id)
    {
    case PROP_CONFIG_FLAGS:
      self->config_flags = g_value_get_flags (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
_ostree_fetcher_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  OstreeFetcher *self = OSTREE_FETCHER (object);

  switch (prop_id)
    {
    case PROP_CONFIG_FLAGS:
      g_value_set_flags (value, self->config_flags);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
_ostree_fetcher_finalize (GObject *object)
{
  OstreeFetcher *self = OSTREE_FETCHER (object);

  // Because curl_multi_cleanup may invoke callbacks, we effectively have
  // some circular references going on here. See discussion in
  // https://github.com/curl/curl/issues/14860
  // Basically what we do is make most callbacks libcurl may invoke into no-ops when
  // we detect we're finalizing. The data structures are owned by this object and
  // not by the callbacks, and will be destroyed below. Note that
  // e.g. g_hash_table_unref() may itself invoke callbacks, which is where
  // some data is cleaned up.
  self->finalizing = true;
  curl_multi_cleanup (self->multi);
  g_free (self->remote_name);
  g_free (self->tls_ca_db_path);
  g_free (self->tls_client_cert_path);
  g_free (self->tls_client_key_path);
  g_free (self->cookie_jar_path);
  g_free (self->proxy);
  g_assert_cmpint (g_hash_table_size (self->outstanding_requests), ==, 0);
  g_clear_pointer (&self->extra_headers, curl_slist_free_all);
  g_hash_table_unref (self->outstanding_requests);
  g_hash_table_unref (self->sockets);
#ifdef HAVE_AWS
  g_hash_table_unref (self->s3_remotes);
  g_hash_table_unref (self->s3_creds);
#endif
  g_clear_pointer (&self->timer_event, destroy_and_unref_source);
  if (self->mainctx)
    g_main_context_unref (self->mainctx);
  g_clear_pointer (&self->custom_user_agent, g_free);

  G_OBJECT_CLASS (_ostree_fetcher_parent_class)->finalize (object);
}

static void
_ostree_fetcher_constructed (GObject *object)
{
  // OstreeFetcher *self = OSTREE_FETCHER (object);

  G_OBJECT_CLASS (_ostree_fetcher_parent_class)->constructed (object);
}

static void
_ostree_fetcher_class_init (OstreeFetcherClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->set_property = _ostree_fetcher_set_property;
  gobject_class->get_property = _ostree_fetcher_get_property;
  gobject_class->finalize = _ostree_fetcher_finalize;
  gobject_class->constructed = _ostree_fetcher_constructed;

  g_object_class_install_property (
      gobject_class, PROP_CONFIG_FLAGS,
      g_param_spec_flags ("config-flags", "", "", OSTREE_TYPE_FETCHER_CONFIG_FLAGS,
                          OSTREE_FETCHER_FLAGS_NONE,
                          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

static void
_ostree_fetcher_init (OstreeFetcher *self)
{
  CURLMcode rc;
  self->multi = curl_multi_init ();
  self->outstanding_requests
      = g_hash_table_new_full (NULL, NULL, (GDestroyNotify)g_object_unref, NULL);
  self->sockets = g_hash_table_new_full (NULL, NULL, (GDestroyNotify)sock_unref, NULL);
#ifdef HAVE_AWS
  self->s3_remotes
      = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify)s3_remote_free);
  self->s3_creds = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                          (GDestroyNotify)s3_cred_state_free);
#endif
  rc = curl_multi_setopt (self->multi, CURLMOPT_SOCKETFUNCTION, sock_cb);
  g_assert_cmpint (rc, ==, CURLM_OK);
  rc = curl_multi_setopt (self->multi, CURLMOPT_SOCKETDATA, self);
  g_assert_cmpint (rc, ==, CURLM_OK);
  rc = curl_multi_setopt (self->multi, CURLMOPT_TIMERFUNCTION, update_timeout_cb);
  g_assert_cmpint (rc, ==, CURLM_OK);
  rc = curl_multi_setopt (self->multi, CURLMOPT_TIMERDATA, self);
  g_assert_cmpint (rc, ==, CURLM_OK);
#if CURL_AT_LEAST_VERSION(7, 30, 0)
  /* Let's do something reasonable here. */
  rc = curl_multi_setopt (self->multi, CURLMOPT_MAX_TOTAL_CONNECTIONS, 8);
  g_assert_cmpint (rc, ==, CURLM_OK);
#endif
  /* This version mirrors the version at which we're enabling HTTP2 support.
   * See also https://github.com/curl/curl/blob/curl-7_53_0/docs/examples/http2-download.c
   */
#if CURL_AT_LEAST_VERSION(7, 51, 0)
  rc = curl_multi_setopt (self->multi, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
  g_assert_cmpint (rc, ==, CURLM_OK);
#endif
}

OstreeFetcher *
_ostree_fetcher_new (int tmpdir_dfd, const char *remote_name, OstreeFetcherConfigFlags flags)
{
  OstreeFetcher *fetcher = g_object_new (OSTREE_TYPE_FETCHER, "config-flags", flags, NULL);
  fetcher->remote_name = g_strdup (remote_name);
  fetcher->tmpdir_dfd = tmpdir_dfd;
  return fetcher;
}

void
_ostree_fetcher_set_force_anonymous_tmpfiles (OstreeFetcher *self)
{
  self->force_anonymous = true;
}

static void
destroy_and_unref_source (GSource *source)
{
  g_source_destroy (source);
  g_source_unref (source);
}

static char *
request_get_uri (FetcherRequest *req, GUri *baseuri)
{
  if (!req->filename)
    return g_uri_to_string_partial (baseuri, G_URI_HIDE_PASSWORD);
  {
    g_autofree char *uristr = g_uri_to_string_partial (baseuri, G_URI_HIDE_PASSWORD);
    return g_build_filename (uristr, req->filename, NULL);
  }
}

static gboolean
ensure_tmpfile (FetcherRequest *req, GError **error)
{
  if (req->tmpf.initialized)
    return TRUE;
  if (req->fetcher->force_anonymous)
    return glnx_open_anonymous_tmpfile (O_RDWR | O_CLOEXEC, &req->tmpf, error);
  else
    return _ostree_fetcher_tmpf (req->fetcher->tmpdir_dfd, &req->tmpf, error);
}

#ifdef HAVE_AWS
/* One s3:// base URI as configured on a remote. */
struct S3Remote
{
  OstreeS3Location *loc;
  char *region;    /* resolved signing region */
  char *creds_key; /* key into OstreeFetcher.s3_creds */
};

/* Credentials for one (profile, region), shared by every request using it. */
struct S3CredState
{
  OstreeAwsCredentials *creds; /* NULL until fetched, or after being dropped as expired */
  guint64 fetched_at;          /* wall-clock seconds */
  gboolean fetching;           /* a fetch is in flight */
  gboolean force_next;         /* the next fetch bypasses the process-wide cache */
  GQueue waiting;              /* GTask*, borrowed: outstanding_requests owns them */
};

static void
s3_remote_free (S3Remote *remote)
{
  _ostree_s3_location_free (remote->loc);
  g_free (remote->region);
  g_free (remote->creds_key);
  g_free (remote);
}

static void
s3_cred_state_free (S3CredState *cs)
{
  g_clear_pointer (&cs->creds, _ostree_aws_credentials_unref);
  g_queue_clear (&cs->waiting);
  g_free (cs);
}

static guint64
s3_now (void)
{
  return g_get_real_time () / G_USEC_PER_SEC;
}

/* Parse @baseuri and resolve its region, once per fetcher. */
static S3Remote *
s3_get_remote (OstreeFetcher *self, GUri *baseuri, GError **error)
{
  g_autofree char *key = g_uri_to_string_partial (baseuri, G_URI_HIDE_PASSWORD);
  S3Remote *remote = g_hash_table_lookup (self->s3_remotes, key);
  if (remote != NULL)
    return remote;

  g_autoptr (OstreeS3Location) loc = _ostree_s3_location_parse (baseuri, error);
  if (loc == NULL)
    return NULL;

  g_autoptr (GError) region_error = NULL;
  g_autofree char *region
      = _ostree_aws_resolve_region (loc->region, loc->profile, NULL, &region_error);
  if (region == NULL)
    {
      if (loc->endpoint == NULL)
        {
          g_propagate_error (error, g_steal_pointer (&region_error));
          return NULL;
        }
      region = g_strdup (S3_ENDPOINT_FALLBACK_REGION);
    }

  remote = g_new0 (S3Remote, 1);
  remote->creds_key = g_strdup_printf ("%s\n%s", loc->profile ?: "", region);
  remote->region = g_steal_pointer (&region);
  remote->loc = g_steal_pointer (&loc);
  g_hash_table_insert (self->s3_remotes, g_steal_pointer (&key), remote);
  return remote;
}

typedef struct
{
  GTask *task; /* owned */
  GError *error;
} S3FailData;

static void
s3_fail_data_free (gpointer user_data)
{
  S3FailData *data = user_data;
  g_clear_error (&data->error);
  g_object_unref (data->task);
  g_free (data);
}

static gboolean
s3_fail_idle_cb (gpointer user_data)
{
  S3FailData *data = user_data;
  OstreeFetcher *self = g_task_get_source_object (data->task);

  g_task_return_error (data->task, g_steal_pointer (&data->error));
  /* Same bookkeeping as the end of check_multi_info(). */
  g_hash_table_remove (self->outstanding_requests, data->task);
  if (g_hash_table_size (self->outstanding_requests) == 0)
    g_clear_pointer (&self->mainctx, g_main_context_unref);
  return G_SOURCE_REMOVE;
}

/* Fail a request that never reached curl. This is deferred to an idle so
 * it behaves the same whether we were called from
 * _ostree_fetcher_request_async() (which adds the task to
 * outstanding_requests only after we return) or from a callback.
 * Takes ownership of @error. */
static void
s3_fail_request (OstreeFetcher *self, GTask *task, GError *error)
{
  S3FailData *data = g_new0 (S3FailData, 1);
  data->task = g_object_ref (task);
  data->error = error;

  GSource *source = g_idle_source_new ();
  g_source_set_callback (source, s3_fail_idle_cb, data, s3_fail_data_free);
  g_source_attach (source, self->mainctx);
  g_source_unref (source);
}

typedef struct
{
  OstreeFetcher *self; /* owned */
  char *creds_key;
} S3CredsFetch;

static void
s3_on_credentials (GObject *source, GAsyncResult *result, gpointer user_data)
{
  S3CredsFetch *fetch = user_data;
  OstreeFetcher *self = fetch->self;
  S3CredState *cs = g_hash_table_lookup (self->s3_creds, fetch->creds_key);
  g_autoptr (GError) error = NULL;
  g_autoptr (OstreeAwsCredentials) creds = _ostree_aws_get_credentials_finish (result, &error);
  const guint64 now = s3_now ();

  g_assert (cs != NULL);
  cs->fetching = FALSE;

  /* Credentials that are already expired would only be refetched again
   * immediately; turn that into an error instead of a loop. */
  if (creds != NULL && _ostree_aws_credentials_get_expiry (creds) <= now)
    {
      g_clear_pointer (&creds, _ostree_aws_credentials_unref);
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                   "AWS credential provider returned credentials that have already expired");
    }
  if (creds != NULL)
    {
      g_clear_pointer (&cs->creds, _ostree_aws_credentials_unref);
      cs->creds = g_steal_pointer (&creds);
      cs->fetched_at = now;
    }

  /* Take the list first: resuming a request can park it again. */
  GQueue waiting = cs->waiting;
  g_queue_init (&cs->waiting);
  GTask *task;
  while ((task = g_queue_pop_head (&waiting)) != NULL)
    {
      if (error == NULL)
        initiate_next_curl_request (g_task_get_task_data (task), task);
      else
        s3_fail_request (self, task, g_error_copy (error));
    }

  g_object_unref (fetch->self);
  g_free (fetch->creds_key);
  g_free (fetch);
}

static void
s3_start_credentials_fetch (OstreeFetcher *self, S3Remote *remote, S3CredState *cs)
{
  S3CredsFetch *fetch = g_new0 (S3CredsFetch, 1);
  fetch->self = g_object_ref (self);
  fetch->creds_key = g_strdup (remote->creds_key);

  /* Replacing credentials we already hold means they are stale; the
   * process-wide cache would just hand the same ones back. */
  const gboolean force = cs->force_next || cs->creds != NULL;
  cs->force_next = FALSE;
  cs->fetching = TRUE;

  /* The result is delivered on the thread-default context; make it ours. */
  g_main_context_push_thread_default (self->mainctx);
  _ostree_aws_get_credentials_async (remote->loc->profile, remote->region, force, NULL,
                                     s3_on_credentials, fetch);
  g_main_context_pop_thread_default (self->mainctx);
}

/* Prepare one attempt at an s3:// mirror.
 *
 * On success either *out_parked is TRUE (the request waits for
 * credentials and will be re-initiated), or *out_url is the URL to
 * request and the signed headers have been appended to req->req_headers.
 */
static gboolean
s3_prepare_request (OstreeFetcher *self, FetcherRequest *req, GTask *task, GUri *baseuri,
                    char **out_url, gboolean *out_parked, GError **error)
{
  S3Remote *remote = s3_get_remote (self, baseuri, error);
  if (remote == NULL)
    return FALSE;

  S3CredState *cs = g_hash_table_lookup (self->s3_creds, remote->creds_key);
  if (cs == NULL)
    {
      cs = g_new0 (S3CredState, 1);
      g_queue_init (&cs->waiting);
      g_hash_table_insert (self->s3_creds, g_strdup (remote->creds_key), cs);
    }

  if (cs->creds == NULL
      || !_ostree_s3_credentials_usable (_ostree_aws_credentials_get_expiry (cs->creds),
                                         cs->fetched_at, s3_now ()))
    {
      g_queue_push_tail (&cs->waiting, task);
      if (!cs->fetching)
        s3_start_credentials_fetch (self, remote, cs);
      *out_parked = TRUE;
      return TRUE;
    }

  g_autofree char *url
      = _ostree_s3_location_to_request_url (remote->loc, remote->region, req->filename);
  g_autoptr (GPtrArray) headers = g_ptr_array_new_with_free_func (g_free);
  if (!_ostree_aws_sign_get (cs->creds, url, remote->region, headers, error))
    return FALSE;
  for (guint i = 0; i < headers->len; i++)
    req->req_headers = curl_slist_append (req->req_headers, headers->pdata[i]);

  req->s3_creds_key = remote->creds_key;
  req->s3_creds = _ostree_aws_credentials_ref (cs->creds);
  *out_url = g_steal_pointer (&url);
  return TRUE;
}

/* Handle a non-2xx, non-304 response to an s3:// request. Exactly one of
 * these happens: *out_retry is set (re-sign and retry the same mirror),
 * *out_next_mirror is set, or @task is completed with an error. */
static void
s3_complete_error (OstreeFetcher *self, FetcherRequest *req, GTask *task, const char *eff_url,
                   long response, gboolean retry_all, gboolean *out_next_mirror,
                   gboolean *out_retry)
{
  g_autofree char *code = NULL;
  g_autofree char *message = NULL;
  g_autofree char *body_region = NULL;
  if (req->s3_error_body != NULL)
    (void)_ostree_s3_parse_error_body (req->s3_error_body->str, req->s3_error_body->len, &code,
                                       &message, &body_region);

  /* S3 reports ExpiredToken with HTTP 400 and RequestExpired with 403, so
   * go by the error code, not the status. */
  const gboolean expired
      = code != NULL
        && (g_str_equal (code, "ExpiredToken") || g_str_equal (code, "TokenRefreshRequired")
            || g_str_equal (code, "RequestExpired"));
  if (expired && !req->s3_retried_expired)
    {
      S3CredState *cs = g_hash_table_lookup (self->s3_creds, req->s3_creds_key);
      /* Only drop the credentials this request was signed with: when many
       * requests fail together, the first one may already have replaced them. */
      if (cs != NULL && cs->creds == req->s3_creds)
        {
          g_clear_pointer (&cs->creds, _ostree_aws_credentials_unref);
          cs->force_next = TRUE;
        }
      req->s3_retried_expired = TRUE;
      *out_retry = TRUE;
      return;
    }

  if (req->idx + 1 < req->mirrorlist->len)
    {
      *out_next_mirror = TRUE;
      return;
    }

  const char *bucket_region = req->s3_bucket_region ?: body_region;
  const gboolean wrong_region
      = response == 301 || response == 307
        || (code != NULL && g_str_equal (code, "AuthorizationHeaderMalformed")
            && body_region != NULL);
  GIOErrorEnum giocode;
  g_autofree char *msg = NULL;

  if (response == 404)
    {
      giocode = G_IO_ERROR_NOT_FOUND;
      msg = g_strdup_printf ("While fetching %s: Server returned HTTP %ld", eff_url, response);
    }
  else if (wrong_region)
    {
      giocode = G_IO_ERROR_FAILED;
      if (bucket_region != NULL)
        msg = g_strdup_printf ("While fetching %s: bucket is in region %s; add ?region=%s to "
                               "the s3:// URL",
                               eff_url, bucket_region, bucket_region);
      else
        msg = g_strdup_printf ("While fetching %s: bucket is in a different region (HTTP %ld); "
                               "set ?region= on the s3:// URL",
                               eff_url, response);
    }
  else if (response == 403 || expired)
    {
      giocode = G_IO_ERROR_PERMISSION_DENIED;
      msg = g_strdup_printf ("While fetching %s: S3 returned HTTP %ld %s: %s", eff_url, response,
                             code ?: "(no error code)", message ?: "(no message)");
    }
  else
    {
      giocode = _ostree_fetcher_http_status_code_to_io_error (response, retry_all);
      if (code != NULL)
        msg = g_strdup_printf ("While fetching %s: S3 returned HTTP %ld %s: %s", eff_url, response,
                               code, message ?: "(no message)");
      else
        msg = g_strdup_printf ("While fetching %s: Server returned HTTP %ld", eff_url, response);
    }

  g_task_return_new_error (task, G_IO_ERROR, giocode, "%s", msg);
  if (self->remote_name
      && !((req->flags & OSTREE_FETCHER_REQUEST_OPTIONAL_CONTENT) > 0
           && giocode == G_IO_ERROR_NOT_FOUND))
    _ostree_fetcher_journal_failure (self->remote_name, eff_url, msg);
}
#endif /* HAVE_AWS */

/* Check for completed transfers, and remove their easy handles */
static void
check_multi_info (OstreeFetcher *fetcher)
{
  CURLMcode rc;
  CURLMsg *msg;
  int msgs_left;

  while ((msg = curl_multi_info_read (fetcher->multi, &msgs_left)) != NULL)
    {
      long response;
      CURL *easy = msg->easy_handle;
      CURLcode curlres = msg->data.result;
      GTask *task;
      FetcherRequest *req;
      const char *eff_url;
      gboolean is_file;
      gboolean continued_request = FALSE;
      gboolean s3_retry = FALSE;

      if (msg->msg != CURLMSG_DONE)
        continue;

      rc = curl_easy_getinfo (easy, CURLINFO_PRIVATE, &task);
      g_assert_cmpint (rc, ==, CURLM_OK);
      rc = curl_easy_getinfo (easy, CURLINFO_EFFECTIVE_URL, &eff_url);
      g_assert_cmpint (rc, ==, CURLM_OK);
      /* We should have limited the protocols; this is what
       * curl's tool_operate.c does.
       */
      is_file = g_str_has_prefix (eff_url, "file:");
      g_assert (is_file || g_str_has_prefix (eff_url, "http"));

      req = g_task_get_task_data (task);

      gboolean retry_all = (!is_file && req->fetcher->opt_retry_all);

      if (req->caught_write_error)
        g_task_return_error (task, g_steal_pointer (&req->caught_write_error));
      else if (curlres != CURLE_OK)
        {
          if (is_file && curlres == CURLE_FILE_COULDNT_READ_FILE)
            {
              /* Handle file not found */
              g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "%s",
                                       curl_easy_strerror (curlres));
            }
          else
            {
              /* When it is not a file, we want to retry the request.
               * We accomplish that by using G_IO_ERROR_TIMED_OUT.
               */
              g_task_return_new_error (
                  task, G_IO_ERROR, retry_all ? G_IO_ERROR_TIMED_OUT : G_IO_ERROR,
                  "While fetching %s: [%u] %s", eff_url, curlres, curl_easy_strerror (curlres));
              _ostree_fetcher_journal_failure (req->fetcher->remote_name, eff_url,
                                               curl_easy_strerror (curlres));
            }
        }
      else
        {
          rc = curl_easy_getinfo (easy, CURLINFO_RESPONSE_CODE, &response);
          g_assert_cmpint (rc, ==, CURLM_OK);

          if (!is_file && response == 304
              && (req->if_none_match != NULL || req->if_modified_since > 0))
            {
              /* Version on the server is unchanged from the version we have
               * cached locally; report this as an out-argument, a zero-length
               * response buffer, and no error. */
              req->out_not_modified = TRUE;
            }

#ifdef HAVE_AWS
          const gboolean s3_failed
              = req->is_s3 && !(response >= 200 && response < 300) && response != 304;
#else
          const gboolean s3_failed = FALSE;
#endif
          if (s3_failed)
            {
#ifdef HAVE_AWS
              s3_complete_error (fetcher, req, task, eff_url, response, retry_all,
                                 &continued_request, &s3_retry);
#endif
            }
          else if (!is_file && !(response >= 200 && response < 300) && response != 304)
            {
              GIOErrorEnum giocode
                  = _ostree_fetcher_http_status_code_to_io_error (response, retry_all);

              if (req->idx + 1 == req->mirrorlist->len)
                {
                  g_autofree char *response_msg = g_strdup_printf (
                      "While fetching %s: Server returned HTTP %lu", eff_url, response);
                  g_task_return_new_error (task, G_IO_ERROR, giocode, "%s", response_msg);
                  if (req->fetcher->remote_name
                      && !((req->flags & OSTREE_FETCHER_REQUEST_OPTIONAL_CONTENT) > 0
                           && giocode == G_IO_ERROR_NOT_FOUND))
                    _ostree_fetcher_journal_failure (req->fetcher->remote_name, eff_url,
                                                     response_msg);
                }
              else
                {
                  continued_request = TRUE;
                }
            }
          else if (req->is_membuf)
            {
              GBytes *ret;
              if ((req->flags & OSTREE_FETCHER_REQUEST_NUL_TERMINATION) > 0)
                g_string_append_c (req->output_buf, '\0');
              ret = g_string_free_to_bytes (req->output_buf);
              req->output_buf = NULL;
              g_task_return_pointer (task, ret, (GDestroyNotify)g_bytes_unref);
            }
          else
            {
              g_autoptr (GError) local_error = NULL;
              GError **error = &local_error;

              if (!ensure_tmpfile (req, error))
                {
                  g_task_return_error (task, g_steal_pointer (&local_error));
                }
              else if (lseek (req->tmpf.fd, 0, SEEK_SET) < 0)
                {
                  glnx_set_error_from_errno (error);
                  g_task_return_error (task, g_steal_pointer (&local_error));
                }
              else
                {
                  /* We return the tmpfile in the _finish wrapper */
                  g_task_return_boolean (task, TRUE);
                }
            }
        }

      rc = curl_multi_remove_handle (fetcher->multi, easy);
      g_assert_cmpint (rc, ==, CURLM_OK);
      if (continued_request)
        {
          req->idx++;
          initiate_next_curl_request (req, task);
        }
      else if (s3_retry)
        {
          initiate_next_curl_request (req, task);
        }
      else
        {
          g_hash_table_remove (fetcher->outstanding_requests, task);
          if (g_hash_table_size (fetcher->outstanding_requests) == 0)
            {
              g_clear_pointer (&fetcher->mainctx, g_main_context_unref);
            }
        }
    }
}

/* Called by glib when our timeout expires */
static gboolean
timer_cb (gpointer data)
{
  CURLMcode rc;
  OstreeFetcher *fetcher = data;
  g_clear_pointer (&fetcher->timer_event, destroy_and_unref_source);
  rc = curl_multi_socket_action (fetcher->multi, CURL_SOCKET_TIMEOUT, 0, &fetcher->curl_running);
  g_assert_cmpint (rc, ==, CURLM_OK);
  check_multi_info (fetcher);

  return G_SOURCE_REMOVE;
}

/* Update the event timer after curl_multi library calls */
static int
update_timeout_cb (CURLM *multi, long timeout_ms, void *userp)
{
  OstreeFetcher *fetcher = userp;

  g_clear_pointer (&fetcher->timer_event, destroy_and_unref_source);

  if (timeout_ms != -1)
    {
      fetcher->timer_event = g_timeout_source_new (timeout_ms);
      g_source_set_callback (fetcher->timer_event, timer_cb, fetcher, NULL);
      g_source_attach (fetcher->timer_event, fetcher->mainctx);
    }

  return 0;
}

/* Called by glib when we get action on a multi socket */
static gboolean
event_cb (int fd, GIOCondition condition, gpointer data)
{
  OstreeFetcher *fetcher = data;
  CURLMcode rc;

  int action
      = (condition & G_IO_IN ? CURL_CSELECT_IN : 0) | (condition & G_IO_OUT ? CURL_CSELECT_OUT : 0);

  rc = curl_multi_socket_action (fetcher->multi, fd, action, &fetcher->curl_running);
  g_assert_cmpint (rc, ==, CURLM_OK);
  check_multi_info (fetcher);
  if (fetcher->curl_running > 0)
    {
      return TRUE;
    }
  else
    {
      return FALSE;
    }
}

/* Clean up the SockInfo structure */
static void
sock_unref (SockInfo *f)
{
  if (!f)
    return;
  if (--f->refcount != 0)
    return;
  g_clear_pointer (&f->ch, destroy_and_unref_source);
  g_free (f);
}

/* Assign information to a SockInfo structure */
static void
setsock (SockInfo *f, curl_socket_t s, int act, OstreeFetcher *fetcher)
{
  GIOCondition kind = (act & CURL_POLL_IN ? G_IO_IN : 0) | (act & CURL_POLL_OUT ? G_IO_OUT : 0);

  f->sockfd = s;
  f->action = act;
  g_clear_pointer (&f->ch, destroy_and_unref_source);
  /* TODO - investigate new g_source_modify_unix_fd() so changing the poll
   * flags involves less allocation.
   */
  f->ch = g_unix_fd_source_new (f->sockfd, kind);
  g_source_set_callback (f->ch, (GSourceFunc)event_cb, fetcher, NULL);
  g_source_attach (f->ch, fetcher->mainctx);
}

/* Initialize a new SockInfo structure */
static void
addsock (curl_socket_t s, CURL *easy, int action, OstreeFetcher *fetcher)
{
  SockInfo *fdp = g_new0 (SockInfo, 1);

  fdp->refcount = 1;
  fdp->fetcher = fetcher;
  setsock (fdp, s, action, fetcher);
  CURLMcode rc = curl_multi_assign (fetcher->multi, s, fdp);
  g_assert_cmpint (rc, ==, CURLM_OK);
  g_hash_table_add (fetcher->sockets, fdp);
}

/* CURLMOPT_SOCKETFUNCTION */
static int
sock_cb (CURL *easy, curl_socket_t s, int what, void *cbp, void *sockp)
{
  OstreeFetcher *fetcher = cbp;
  SockInfo *fdp = (SockInfo *)sockp;

  // We do nothing if we're in the process of teardown; see below.
  if (fetcher->finalizing)
    return 0;

  if (what == CURL_POLL_REMOVE)
    {
      if (!g_hash_table_remove (fetcher->sockets, fdp))
        g_assert_not_reached ();
    }
  else
    {
      if (!fdp)
        {
          addsock (s, easy, what, fetcher);
        }
      else
        {
          setsock (fdp, s, what, fetcher);
        }
    }
  return 0;
}

/* CURLOPT_WRITEFUNCTION */
static size_t
write_cb (void *ptr, size_t size, size_t nmemb, void *data)
{
  CURLMcode rc;
  const size_t realsize = size * nmemb;
  GTask *task = data;
  FetcherRequest *req;

  req = g_task_get_task_data (task);

  if (req->caught_write_error)
    return -1;

#ifdef HAVE_AWS
  if (req->is_s3)
    {
      long response = 0;
      (void)curl_easy_getinfo (req->easy, CURLINFO_RESPONSE_CODE, &response);
      if (!(response >= 200 && response < 300))
        {
          /* An error document, not content: keep a bounded copy for the
           * error message and never let it reach the output. */
          if (req->s3_error_body == NULL)
            req->s3_error_body = g_string_new (NULL);
          if (req->s3_error_body->len < S3_ERROR_BODY_MAX)
            g_string_append_len (req->s3_error_body, ptr,
                                 MIN (realsize, S3_ERROR_BODY_MAX - req->s3_error_body->len));
          return realsize;
        }
    }
#endif

  if (req->max_size > 0)
    {
      if (realsize > req->max_size || (realsize + req->current_size) > req->max_size)
        {
          const char *eff_url;
          rc = curl_easy_getinfo (req->easy, CURLINFO_EFFECTIVE_URL, &eff_url);
          g_assert_cmpint (rc, ==, CURLM_OK);
          req->caught_write_error
              = g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED,
                             "URI %s exceeded maximum size of %" G_GUINT64_FORMAT " bytes", eff_url,
                             req->max_size);
          return -1;
        }
    }

  if (req->is_membuf)
    g_string_append_len (req->output_buf, ptr, realsize);
  else
    {
      if (!ensure_tmpfile (req, &req->caught_write_error))
        return -1;
      g_assert (req->tmpf.fd >= 0);
      if (glnx_loop_write (req->tmpf.fd, ptr, realsize) < 0)
        {
          glnx_set_error_from_errno (&req->caught_write_error);
          return -1;
        }
    }

  req->current_size += realsize;
  req->fetcher->bytes_transferred += realsize;

  return realsize;
}

/* CURLOPT_HEADERFUNCTION */
static size_t
response_header_cb (const char *buffer, size_t size, size_t n_items, void *user_data)
{
  const size_t real_size = size * n_items;
  GTask *task = G_TASK (user_data);
  FetcherRequest *req;

  /* libcurl says that @size is always 1, but let’s check
   * See https://curl.haxx.se/libcurl/c/CURLOPT_HEADERFUNCTION.html */
  g_assert (size == 1);

  req = g_task_get_task_data (task);

  /* libcurl doesn't NUL-terminate header lines; copy once so everything
   * below works on an ordinary C string and can't read past real_size. */
  g_autofree char *line = g_strndup (buffer, real_size);
  const char *etag_header = "ETag:";
  const char *last_modified_header = "Last-Modified:";

  if (g_ascii_strncasecmp (line, etag_header, strlen (etag_header)) == 0)
    {
      g_free (req->out_etag);
      req->out_etag = g_strdup (g_strstrip (line + strlen (etag_header)));
    }
  else if (g_ascii_strncasecmp (line, last_modified_header, strlen (last_modified_header)) == 0)
    {
      const char *lm = g_strstrip (line + strlen (last_modified_header));
      g_autoptr (GDateTime) dt = _ostree_parse_rfc2616_date_time (lm, strlen (lm));
      req->out_last_modified = (dt != NULL) ? g_date_time_to_unix (dt) : 0;
    }
#ifdef HAVE_AWS
  else if (req->is_s3 && g_ascii_strncasecmp (line, "x-amz-bucket-region:", 20) == 0)
    {
      g_free (req->s3_bucket_region);
      req->s3_bucket_region = g_strdup (g_strstrip (line + 20));
    }
#endif

  return real_size;
}

/* CURLOPT_PROGRESSFUNCTION */
static int
prog_cb (void *p, double dltotal, double dlnow, double ult, double uln)
{
  CURLMcode rc;
  GTask *task = p;
  FetcherRequest *req;
  char *eff_url;
  req = g_task_get_task_data (task);
  rc = curl_easy_getinfo (req->easy, CURLINFO_EFFECTIVE_URL, &eff_url);
  g_assert_cmpint (rc, ==, CURLM_OK);
  g_printerr ("Progress: %s (%g/%g)\n", eff_url, dlnow, dltotal);
  return 0;
}

static void
request_unref (FetcherRequest *req)
{
  if (--req->refcount != 0)
    return;

  g_ptr_array_unref (req->mirrorlist);
  g_free (req->filename);
  g_clear_error (&req->caught_write_error);
  glnx_tmpfile_clear (&req->tmpf);
  if (req->output_buf)
    g_string_free (req->output_buf, TRUE);
  g_free (req->if_none_match);
  g_free (req->out_etag);
  g_clear_pointer (&req->req_headers, curl_slist_free_all);
  curl_easy_cleanup (req->easy);
#ifdef HAVE_AWS
  if (req->s3_error_body)
    g_string_free (req->s3_error_body, TRUE);
  g_free (req->s3_bucket_region);
  g_clear_pointer (&req->s3_creds, _ostree_aws_credentials_unref);
#endif

  g_free (req);
}

int
_ostree_fetcher_get_dfd (OstreeFetcher *fetcher)
{
  return fetcher->tmpdir_dfd;
}

void
_ostree_fetcher_set_proxy (OstreeFetcher *self, const char *http_proxy)
{
  g_free (self->proxy);
  self->proxy = g_strdup (http_proxy);
}

void
_ostree_fetcher_set_low_speed_time (OstreeFetcher *self, guint32 opt_low_speed_time)
{
  self->opt_low_speed_time = opt_low_speed_time;
}

void
_ostree_fetcher_set_low_speed_limit (OstreeFetcher *self, guint32 opt_low_speed_limit)
{
  self->opt_low_speed_limit = opt_low_speed_limit;
}

void
_ostree_fetcher_set_retry_all (OstreeFetcher *self, gboolean opt_retry_all)
{
  self->opt_retry_all = opt_retry_all;
}

void
_ostree_fetcher_set_max_outstanding_fetcher_requests (OstreeFetcher *self,
                                                      guint32 opt_max_outstanding_fetcher_requests)
{
  self->opt_max_outstanding_fetcher_requests = opt_max_outstanding_fetcher_requests;
#if CURL_AT_LEAST_VERSION(7, 30, 0)
  /* Servers that only speak HTTP/1.1 (S3 among them) get their concurrency
   * from parallel connections, so don't cap those below the request limit. */
  CURLMcode rc = curl_multi_setopt (self->multi, CURLMOPT_MAX_TOTAL_CONNECTIONS,
                                    (long)MAX (8, opt_max_outstanding_fetcher_requests));
  g_assert_cmpint (rc, ==, CURLM_OK);
#endif
}

void
_ostree_fetcher_set_cookie_jar (OstreeFetcher *self, const char *jar_path)
{
  g_free (self->cookie_jar_path);
  self->cookie_jar_path = g_strdup (jar_path);
}

void
_ostree_fetcher_set_client_cert (OstreeFetcher *self, const char *cert_path, const char *key_path)
{
  g_assert ((cert_path == NULL && key_path == NULL) || (cert_path != NULL && key_path != NULL));
  g_free (self->tls_client_cert_path);
  self->tls_client_cert_path = g_strdup (cert_path);
  g_free (self->tls_client_key_path);
  self->tls_client_key_path = g_strdup (key_path);
}

void
_ostree_fetcher_set_tls_database (OstreeFetcher *self, const char *dbpath)
{
  g_free (self->tls_ca_db_path);
  self->tls_ca_db_path = g_strdup (dbpath);
}

void
_ostree_fetcher_set_extra_headers (OstreeFetcher *self, GVariant *extra_headers)
{
  GVariantIter viter;
  const char *key;
  const char *value;

  g_clear_pointer (&self->extra_headers, curl_slist_free_all);

  g_variant_iter_init (&viter, extra_headers);
  while (g_variant_iter_loop (&viter, "(&s&s)", &key, &value))
    {
      g_autofree char *header = g_strdup_printf ("%s: %s", key, value);
      self->extra_headers = curl_slist_append (self->extra_headers, header);
    }
}

void
_ostree_fetcher_set_extra_user_agent (OstreeFetcher *self, const char *extra_user_agent)
{
  g_clear_pointer (&self->custom_user_agent, g_free);
  if (extra_user_agent)
    {
      self->custom_user_agent
          = g_strdup_printf ("%s %s", OSTREE_FETCHER_USERAGENT_STRING, extra_user_agent);
    }
}

/* Re-bind all of the outstanding curl items to our new main context */
static void
adopt_steal_mainctx (OstreeFetcher *self, GMainContext *mainctx)
{
  g_assert (self->mainctx == NULL);
  self->mainctx = mainctx; /* Transfer */

  if (self->timer_event != NULL)
    {
      guint64 readytime = g_source_get_ready_time (self->timer_event);
      guint64 curtime = g_source_get_time (self->timer_event);
      guint64 timeout_micros = curtime - readytime;
      if (curtime < readytime)
        timeout_micros = 0;
      update_timeout_cb (self->multi, timeout_micros / 1000, self);
    }

  GLNX_HASH_TABLE_FOREACH (self->sockets, SockInfo *, fdp)
    setsock (fdp, fdp->sockfd, fdp->action, self);
}

static void
initiate_next_curl_request (FetcherRequest *req, GTask *task)
{
  CURLcode rc;
  OstreeFetcher *self = req->fetcher;

  /* The old handle must go before anything it points at is freed below. */
  g_clear_pointer (&req->easy, curl_easy_cleanup);

  g_assert_cmpint (req->idx, <, req->mirrorlist->len);

  GUri *baseuri = req->mirrorlist->pdata[req->idx];
  g_autofree char *s3_url = NULL;

#ifdef HAVE_AWS
  req->is_s3 = g_str_equal (g_uri_get_scheme (baseuri), "s3");
  if (req->is_s3)
    {
      g_autoptr (GError) s3_error = NULL;
      gboolean parked = FALSE;

      /* Every attempt is signed afresh; never resend a stale signature. */
      g_clear_pointer (&req->req_headers, curl_slist_free_all);
      g_clear_pointer (&req->s3_creds, _ostree_aws_credentials_unref);
      g_clear_pointer (&req->s3_bucket_region, g_free);
      if (req->s3_error_body)
        g_string_truncate (req->s3_error_body, 0);

      if (!s3_prepare_request (self, req, task, baseuri, &s3_url, &parked, &s3_error))
        {
          s3_fail_request (self, task, g_steal_pointer (&s3_error));
          return;
        }
      if (parked)
        return;
    }
#endif

  req->easy = curl_easy_init ();
  g_assert (req->easy);

  {
    g_autofree char *uri = s3_url ? g_strdup (s3_url) : request_get_uri (req, baseuri);
    rc = curl_easy_setopt (req->easy, CURLOPT_URL, uri);
    g_assert_cmpint (rc, ==, CURLM_OK);
  }

  rc = curl_easy_setopt (req->easy, CURLOPT_USERAGENT,
                         self->custom_user_agent ?: OSTREE_FETCHER_USERAGENT_STRING);
  g_assert_cmpint (rc, ==, CURLM_OK);

  /* Set caching request headers */
  if (req->if_none_match != NULL)
    {
      g_autofree char *if_none_match = g_strconcat ("If-None-Match: ", req->if_none_match, NULL);
      req->req_headers = curl_slist_append (req->req_headers, if_none_match);
    }

  if (req->if_modified_since > 0)
    {
      g_autoptr (GDateTime) date_time = g_date_time_new_from_unix_utc (req->if_modified_since);
      g_autofree char *mod_date
          = g_date_time_format (date_time, "If-Modified-Since: %a, %d %b %Y %H:%M:%S %Z");

      req->req_headers = curl_slist_append (req->req_headers, mod_date);
    }

  /* Append a copy of @extra_headers to @req_headers, as the former could change
   * between requests or while a request is in flight */
  for (const struct curl_slist *l = self->extra_headers; l != NULL; l = l->next)
    req->req_headers = curl_slist_append (req->req_headers, l->data);

  if (req->req_headers != NULL)
    {
      rc = curl_easy_setopt (req->easy, CURLOPT_HTTPHEADER, req->req_headers);
      g_assert_cmpint (rc, ==, CURLM_OK);
    }

  if (self->cookie_jar_path)
    {
      rc = curl_easy_setopt (req->easy, CURLOPT_COOKIEFILE, self->cookie_jar_path);
      g_assert_cmpint (rc, ==, CURLM_OK);
      rc = curl_easy_setopt (req->easy, CURLOPT_COOKIELIST, "RELOAD");
      g_assert_cmpint (rc, ==, CURLM_OK);
    }

  if (self->proxy)
    {
      rc = curl_easy_setopt (req->easy, CURLOPT_PROXY, self->proxy);
      g_assert_cmpint (rc, ==, CURLM_OK);
    }

  if (self->tls_ca_db_path)
    {
      rc = curl_easy_setopt (req->easy, CURLOPT_CAINFO, self->tls_ca_db_path);
      g_assert_cmpint (rc, ==, CURLM_OK);
    }

  if ((self->config_flags & OSTREE_FETCHER_FLAGS_TLS_PERMISSIVE) > 0)
    {
      rc = curl_easy_setopt (req->easy, CURLOPT_SSL_VERIFYPEER, 0L);
      g_assert_cmpint (rc, ==, CURLM_OK);
    }

  if (self->tls_client_cert_path)
    {
      /* Support for pkcs11:
       * https://github.com/ostreedev/ostree/pull/1183
       * This will be used by https://github.com/advancedtelematic/aktualizr
       * at least to fetch certificates.  No test coverage at the moment
       * though. See https://gitlab.com/gnutls/gnutls/tree/master/tests/pkcs11
       * and https://github.com/opendnssec/SoftHSMv2 and
       * https://github.com/p11-glue/p11-kit/tree/master/p11-kit for
       * possible ideas there.
       */
      if (g_str_has_prefix (self->tls_client_key_path, "pkcs11:"))
        {
          rc = curl_easy_setopt (req->easy, CURLOPT_SSLENGINE, "pkcs11");
          g_assert_cmpint (rc, ==, CURLM_OK);
          rc = curl_easy_setopt (req->easy, CURLOPT_SSLENGINE_DEFAULT, 1L);
          g_assert_cmpint (rc, ==, CURLM_OK);
          rc = curl_easy_setopt (req->easy, CURLOPT_SSLKEYTYPE, "ENG");
          g_assert_cmpint (rc, ==, CURLM_OK);
        }
      if (g_str_has_prefix (self->tls_client_cert_path, "pkcs11:"))
        {
          rc = curl_easy_setopt (req->easy, CURLOPT_SSLCERTTYPE, "ENG");
          g_assert_cmpint (rc, ==, CURLM_OK);
        }

      rc = curl_easy_setopt (req->easy, CURLOPT_SSLCERT, self->tls_client_cert_path);
      g_assert_cmpint (rc, ==, CURLM_OK);

      rc = curl_easy_setopt (req->easy, CURLOPT_SSLKEY, self->tls_client_key_path);
      g_assert_cmpint (rc, ==, CURLM_OK);
    }

  if ((self->config_flags & OSTREE_FETCHER_FLAGS_TRANSFER_GZIP) > 0)
    {
      rc = curl_easy_setopt (req->easy, CURLOPT_ACCEPT_ENCODING, "");
      g_assert_cmpint (rc, ==, CURLM_OK);
    }

  /* If we have e.g. basic auth in the URL string, let's honor that */
  const char *username = g_uri_get_user (baseuri);
  rc = curl_easy_setopt (req->easy, CURLOPT_USERNAME, username);
  g_assert_cmpint (rc, ==, CURLM_OK);
  const char *password = g_uri_get_password (baseuri);
  rc = curl_easy_setopt (req->easy, CURLOPT_PASSWORD, password);
  g_assert_cmpint (rc, ==, CURLM_OK);

  /* We should only speak HTTP; TODO: only enable file if specified */
  rc = curl_easy_setopt (req->easy, CURLOPT_PROTOCOLS,
                         (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS | CURLPROTO_FILE));
  g_assert_cmpint (rc, ==, CURLM_OK);
  /* Picked the current version in F25 as of 20170127, since
   * there are numerous HTTP/2 fixes since the original version in
   * libcurl 7.43.0.
   */
  if (!(self->config_flags & OSTREE_FETCHER_FLAGS_DISABLE_HTTP2))
    {
#if CURL_AT_LEAST_VERSION(7, 51, 0)
      if ((curl_version_info (CURLVERSION_NOW))->features & CURL_VERSION_HTTP2)
        {
          rc = curl_easy_setopt (req->easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_0);
          g_assert_cmpint (rc, ==, CURLM_OK);
        }
#endif
        /* https://github.com/curl/curl/blob/curl-7_53_0/docs/examples/http2-download.c */
#if (CURLPIPE_MULTIPLEX > 0)
      /* wait for pipe connection to confirm */
      rc = curl_easy_setopt (req->easy, CURLOPT_PIPEWAIT, 1L);
      g_assert_cmpint (rc, ==, CURLM_OK);
#endif
    }

  if (self->config_flags & OSTREE_FETCHER_FLAGS_DISABLE_HTTP2)
    {
      rc = curl_easy_setopt (req->easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
      g_assert_cmpint (rc, ==, CURLM_OK);
    }

  rc = curl_easy_setopt (req->easy, CURLOPT_WRITEFUNCTION, write_cb);
  g_assert_cmpint (rc, ==, CURLM_OK);
  rc = curl_easy_setopt (req->easy, CURLOPT_HEADERFUNCTION, response_header_cb);
  g_assert_cmpint (rc, ==, CURLM_OK);
  if (g_getenv ("OSTREE_DEBUG_HTTP"))
    {
      rc = curl_easy_setopt (req->easy, CURLOPT_VERBOSE, 1L);
      g_assert_cmpint (rc, ==, CURLM_OK);
    }
  rc = curl_easy_setopt (req->easy, CURLOPT_ERRORBUFFER, req->error);
  g_assert_cmpint (rc, ==, CURLM_OK);
  /* Note that the "easy" object's privdata is the task */
  rc = curl_easy_setopt (req->easy, CURLOPT_NOPROGRESS, 1L);
  g_assert_cmpint (rc, ==, CURLM_OK);
  rc = curl_easy_setopt (req->easy, CURLOPT_PROGRESSFUNCTION, prog_cb);
  g_assert_cmpint (rc, ==, CURLM_OK);
  rc = curl_easy_setopt (req->easy, CURLOPT_FOLLOWLOCATION, 1L);
  g_assert_cmpint (rc, ==, CURLM_OK);
#ifdef HAVE_AWS
  if (req->is_s3)
    {
      /* curl forwards custom headers to redirect targets, which would hand
       * the signature and session token to another host. An S3 redirect
       * means "wrong region"; s3_complete_error() reports it. */
      rc = curl_easy_setopt (req->easy, CURLOPT_FOLLOWLOCATION, 0L);
      g_assert_cmpint (rc, ==, CURLM_OK);
    }
#endif
  rc = curl_easy_setopt (req->easy, CURLOPT_CONNECTTIMEOUT, 30L);
  g_assert_cmpint (rc, ==, CURLM_OK);
  rc = curl_easy_setopt (req->easy, CURLOPT_LOW_SPEED_LIMIT, req->fetcher->opt_low_speed_limit);
  g_assert_cmpint (rc, ==, CURLM_OK);
  rc = curl_easy_setopt (req->easy, CURLOPT_LOW_SPEED_TIME, req->fetcher->opt_low_speed_time);
  g_assert_cmpint (rc, ==, CURLM_OK);
  /* closure bindings -> task */
  rc = curl_easy_setopt (req->easy, CURLOPT_PRIVATE, task);
  g_assert_cmpint (rc, ==, CURLM_OK);
  rc = curl_easy_setopt (req->easy, CURLOPT_WRITEDATA, task);
  g_assert_cmpint (rc, ==, CURLM_OK);
  rc = curl_easy_setopt (req->easy, CURLOPT_HEADERDATA, task);
  g_assert_cmpint (rc, ==, CURLM_OK);
  rc = curl_easy_setopt (req->easy, CURLOPT_PROGRESSDATA, task);
  g_assert_cmpint (rc, ==, CURLM_OK);

  CURLMcode multi_rc = curl_multi_add_handle (self->multi, req->easy);
  g_assert (multi_rc == CURLM_OK);
}

static void
_ostree_fetcher_request_async (OstreeFetcher *self, GPtrArray *mirrorlist, const char *filename,
                               OstreeFetcherRequestFlags flags, const char *if_none_match,
                               guint64 if_modified_since, gboolean is_membuf, guint64 max_size,
                               int priority, GCancellable *cancellable,
                               GAsyncReadyCallback callback, gpointer user_data)
{
  g_autoptr (GTask) task = NULL;
  FetcherRequest *req;
  g_autoptr (GMainContext) mainctx = g_main_context_ref_thread_default ();

  /* We don't support multiple concurrent main contexts; take
   * a ref to the first one, and require that later invocations
   * share it.
   */
  if (g_hash_table_size (self->outstanding_requests) == 0 && mainctx != self->mainctx)
    {
      adopt_steal_mainctx (self, g_steal_pointer (&mainctx));
    }
  else
    {
      g_assert (self->mainctx == mainctx);
    }

  req = g_new0 (FetcherRequest, 1);
  req->refcount = 1;
  req->error[0] = '\0';
  req->fetcher = self;
  req->mirrorlist = g_ptr_array_ref (mirrorlist);
  req->filename = g_strdup (filename);
  req->max_size = max_size;
  req->flags = flags;
  req->if_none_match = g_strdup (if_none_match);
  req->if_modified_since = if_modified_since;
  req->is_membuf = is_membuf;
  /* We'll allocate the tmpfile on demand, so we handle
   * file I/O errors just in the write func.
   */
  if (req->is_membuf)
    req->output_buf = g_string_new ("");

  task = g_task_new (self, cancellable, callback, user_data);
  /* We'll use the GTask priority for our own priority queue. */
  g_task_set_priority (task, priority);
  g_task_set_source_tag (task, _ostree_fetcher_request_async);
  g_task_set_task_data (task, req, (GDestroyNotify)request_unref);

  initiate_next_curl_request (req, task);

  g_hash_table_add (self->outstanding_requests, g_steal_pointer (&task));
}

void
_ostree_fetcher_request_to_tmpfile (OstreeFetcher *self, GPtrArray *mirrorlist,
                                    const char *filename, OstreeFetcherRequestFlags flags,
                                    const char *if_none_match, guint64 if_modified_since,
                                    guint64 max_size, int priority, GCancellable *cancellable,
                                    GAsyncReadyCallback callback, gpointer user_data)
{
  _ostree_fetcher_request_async (self, mirrorlist, filename, flags, if_none_match,
                                 if_modified_since, FALSE, max_size, priority, cancellable,
                                 callback, user_data);
}

gboolean
_ostree_fetcher_request_to_tmpfile_finish (OstreeFetcher *self, GAsyncResult *result,
                                           GLnxTmpfile *out_tmpf, gboolean *out_not_modified,
                                           char **out_etag, guint64 *out_last_modified,
                                           GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, self), FALSE);
  g_return_val_if_fail (g_async_result_is_tagged (result, _ostree_fetcher_request_async), FALSE);

  GTask *task = (GTask *)result;
  FetcherRequest *req = g_task_get_task_data (task);

  if (!g_task_propagate_boolean (task, error))
    return FALSE;

  g_assert (!req->is_membuf);
  *out_tmpf = req->tmpf;
  req->tmpf.initialized = FALSE; /* Transfer ownership */

  if (out_not_modified != NULL)
    *out_not_modified = req->out_not_modified;
  if (out_etag != NULL)
    *out_etag = g_steal_pointer (&req->out_etag);
  if (out_last_modified != NULL)
    *out_last_modified = req->out_last_modified;

  return TRUE;
}

void
_ostree_fetcher_request_to_membuf (OstreeFetcher *self, GPtrArray *mirrorlist, const char *filename,
                                   OstreeFetcherRequestFlags flags, const char *if_none_match,
                                   guint64 if_modified_since, guint64 max_size, int priority,
                                   GCancellable *cancellable, GAsyncReadyCallback callback,
                                   gpointer user_data)
{
  _ostree_fetcher_request_async (self, mirrorlist, filename, flags, if_none_match,
                                 if_modified_since, TRUE, max_size, priority, cancellable, callback,
                                 user_data);
}

gboolean
_ostree_fetcher_request_to_membuf_finish (OstreeFetcher *self, GAsyncResult *result,
                                          GBytes **out_buf, gboolean *out_not_modified,
                                          char **out_etag, guint64 *out_last_modified,
                                          GError **error)
{
  GTask *task;
  FetcherRequest *req;
  gpointer ret;

  g_return_val_if_fail (g_task_is_valid (result, self), FALSE);
  g_return_val_if_fail (g_async_result_is_tagged (result, _ostree_fetcher_request_async), FALSE);

  task = (GTask *)result;
  req = g_task_get_task_data (task);

  ret = g_task_propagate_pointer (task, error);
  if (!ret)
    return FALSE;

  g_assert (req->is_membuf);
  g_assert (out_buf);
  *out_buf = ret;

  if (out_not_modified != NULL)
    *out_not_modified = req->out_not_modified;
  if (out_etag != NULL)
    *out_etag = g_steal_pointer (&req->out_etag);
  if (out_last_modified != NULL)
    *out_last_modified = req->out_last_modified;

  return TRUE;
}

guint64
_ostree_fetcher_bytes_transferred (OstreeFetcher *self)
{
  return self->bytes_transferred;
}
