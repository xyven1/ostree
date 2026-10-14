#include <ostree.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static void count (int *socks, int *tmps) {
  *socks = *tmps = 0; DIR *d = opendir ("/proc/self/fd"); struct dirent *e;
  while ((e = readdir (d))) { char p[64], t[512]; snprintf (p, sizeof p, "/proc/self/fd/%s", e->d_name);
    ssize_t n = readlink (p, t, sizeof t - 1); if (n < 0) continue; t[n] = 0;
    if (!strncmp (t, "socket:", 7)) (*socks)++; if (strstr (t, "(deleted)")) (*tmps)++; }
  closedir (d);
}
int main (int argc, char **argv) {
  const char *work = argv[1], *port = argv[2]; int n = atoi (argv[3]), s, t;
  count (&s, &t); printf ("before: sockets=%d deleted-tmpfiles=%d\n", s, t);
  for (int i = 0; i < n; i++) {
    g_autofree char *path = g_strdup_printf ("%s/leakc_%d", work, i);
    g_autofree char *cmd = g_strdup_printf ("rm -rf %s", path); system (cmd);
    g_autoptr(GFile) f = g_file_new_for_path (path);
    g_autoptr(OstreeRepo) repo = ostree_repo_new (f);
    g_autoptr(GError) err = NULL;
    g_assert (ostree_repo_create (repo, OSTREE_REPO_MODE_ARCHIVE, NULL, &err));
    g_autofree char *url = g_strdup_printf ("s3://bkt/repo?endpoint=http://127.0.0.1:%s", port);
    GVariantBuilder rb; g_variant_builder_init (&rb, G_VARIANT_TYPE ("a{sv}"));
    g_variant_builder_add (&rb, "{sv}", "gpg-verify", g_variant_new_boolean (FALSE));
    g_assert (ostree_repo_remote_add (repo, "origin", url, g_variant_builder_end (&rb), NULL, &err));
    const char *refs[] = { "main", NULL };
    GVariantBuilder ob; g_variant_builder_init (&ob, G_VARIANT_TYPE ("a{sv}"));
    g_variant_builder_add (&ob, "{sv}", "refs", g_variant_new_strv (refs, -1));
    g_variant_builder_add (&ob, "{sv}", "disable-static-deltas", g_variant_new_boolean (TRUE));
    g_variant_builder_add (&ob, "{sv}", "n-network-retries", g_variant_new_uint32 (0));
    gboolean ok = ostree_repo_pull_with_options (repo, "origin", g_variant_builder_end (&ob), NULL, NULL, &err);
    sleep (3);
    count (&s, &t);
    printf ("pull %d: %s -> sockets=%d deleted-tmpfiles=%d\n", i, ok ? "ok" : err->message, s, t);
  }
  return 0;
}
