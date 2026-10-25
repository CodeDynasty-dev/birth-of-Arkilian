// Arkilian end-to-end benchmark against a real S3-compatible server (MinIO).
//
//   minio_bench <endpoint> <bucket> <access-key> <secret-key> [rows] [--outage SECONDS]
//
// Phases (each line of output is also appended as CSV to results.csv):
//   1. Baseline: the same write workloads on raw SQLite (same pragmas).
//   2. Arkilian with live shipping to MinIO: single-row autocommit INSERTs,
//      batched INSERTs, UPDATEs — throughput and exact latency percentiles.
//   3. Time until every captured change is durable in MinIO (queue drained).
//   4. Snapshot cycle (copy + upload + manifest publish) and DB size.
//   5. Clean shutdown (final drain + manifest publish).
//   6. Restore into a fresh file with arkilian_hydrate_s3 and verify the
//      restored table is byte-identical to the source (ordered dump hash).
//   7. Optional --outage N: steady writes for N seconds while the driver
//      script pauses MinIO; prints queue depth / health once per second and
//      the recovery time, then restores and verifies again.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <curl/curl.h>
#include "class.h"
#include "hydration.h"
#include "sha256.h"
#include "sqlite3.h"

static const char *g_ep, *g_bucket, *g_ak, *g_sk;
static FILE *g_csv;
static char g_prefix[128];

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static void rmdb(const char *p) {
  char t[512]; const char *s[] = {"", "-wal", "-shm", ".arkddlqueue", "-journal"};
  for (int i = 0; i < 5; i++) { snprintf(t, sizeof t, "%s%s", p, s[i]); remove(t); }
}

static void emit(const char *phase, const char *metric, double value, const char *unit) {
  printf("  %-26s %-30s %14.1f %s\n", phase, metric, value, unit);
  if (g_csv) { fprintf(g_csv, "%s,%s,%.3f,%s\n", phase, metric, value, unit); fflush(g_csv); }
  fflush(stdout);
}

// ── Minimal SigV4 query presign (path-style) to create the bucket ───────
static void hex(const uint8_t *in, size_t n, char *out) { for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", in[i]); }
static void uenc(const char *s, char *o) {
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') *o++ = (char)c;
    else { sprintf(o, "%%%02X", c); o += 3; }
  }
  *o = 0;
}
static int create_bucket(void) {
  const char *host = g_ep; if (!strncmp(host, "http://", 7)) host += 7; else if (!strncmp(host, "https://", 8)) host += 8;
  char hostc[256]; snprintf(hostc, sizeof hostc, "%s", host); size_t hl = strlen(hostc); while (hl && hostc[hl - 1] == '/') hostc[--hl] = 0;
  time_t now = time(NULL); struct tm g; gmtime_r(&now, &g);
  char date[9], amz[17]; strftime(date, sizeof date, "%Y%m%d", &g); strftime(amz, sizeof amz, "%Y%m%dT%H%M%SZ", &g);
  char cred[256], credenc[512]; snprintf(cred, sizeof cred, "%s/%s/us-east-1/s3/aws4_request", g_ak, date); uenc(cred, credenc);
  char query[1024]; snprintf(query, sizeof query,
    "X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=%s&X-Amz-Date=%s&X-Amz-Expires=600&X-Amz-SignedHeaders=host", credenc, amz);
  char canon[2048]; snprintf(canon, sizeof canon, "PUT\n/%s\n%s\nhost:%s\n\nhost\nUNSIGNED-PAYLOAD", g_bucket, query, hostc);
  char ch[65]; ark_sha256_hex(canon, strlen(canon), ch);
  char sts[512]; snprintf(sts, sizeof sts, "AWS4-HMAC-SHA256\n%s\n%s/us-east-1/s3/aws4_request\n%s", amz, date, ch);
  char k0[256]; snprintf(k0, sizeof k0, "AWS4%s", g_sk);
  uint8_t k1[32], k2[32], k3[32], k4[32], sig[32];
  ark_hmac_sha256((const uint8_t *)k0, strlen(k0), date, strlen(date), k1);
  ark_hmac_sha256(k1, 32, "us-east-1", 9, k2);
  ark_hmac_sha256(k2, 32, "s3", 2, k3);
  ark_hmac_sha256(k3, 32, "aws4_request", 12, k4);
  ark_hmac_sha256(k4, 32, sts, strlen(sts), sig);
  char sighex[65]; hex(sig, 32, sighex);
  char url[2048]; snprintf(url, sizeof url, "%s/%s?%s&X-Amz-Signature=%s", g_ep, g_bucket, query, sighex);
  CURL *c = curl_easy_init(); if (!c) return -1;
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "PUT");
  curl_easy_setopt(c, CURLOPT_POSTFIELDS, "");
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
  int rc = curl_easy_perform(c); long code = 0; curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code); curl_easy_cleanup(c);
  if (rc != CURLE_OK) { fprintf(stderr, "bucket create: cannot reach %s (%s)\n", g_ep, curl_easy_strerror(rc)); return -1; }
  if (code == 200 || code == 409) return 0;  // created, or already exists
  fprintf(stderr, "bucket create: HTTP %ld\n", code); return -1;
}

// ── Workloads ─────────────────────────────────────────────────────────
#define SCHEMA "CREATE TABLE bench(id INTEGER PRIMARY KEY, name TEXT, email TEXT, qty INTEGER, price REAL, note TEXT)"
static unsigned rng = 42; static unsigned xr(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static int g_id = 0;
static void bind_row(sqlite3_stmt *s, int id) {
  char name[32], email[48]; snprintf(name, sizeof name, "user-%d", id); snprintf(email, sizeof email, "user%d@example.com", id);
  sqlite3_bind_int(s, 1, id); sqlite3_bind_text(s, 2, name, -1, SQLITE_TRANSIENT); sqlite3_bind_text(s, 3, email, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(s, 4, (int)(xr() % 100)); sqlite3_bind_double(s, 5, (xr() % 10000) / 100.0); sqlite3_bind_text(s, 6, "standard order", -1, SQLITE_STATIC);
}
static void workloads(const char *phase, sqlite3 *h, int n) {
  sqlite3_stmt *ins, *upd;
  sqlite3_prepare_v2(h, "INSERT INTO bench VALUES(?,?,?,?,?,?)", -1, &ins, 0);
  sqlite3_prepare_v2(h, "UPDATE bench SET qty=?, price=? WHERE id=?", -1, &upd, 0);
  double *lat = malloc(sizeof(double) * (size_t)n), tot = 0;
  for (int i = 0; i < n; i++) { bind_row(ins, ++g_id); double t = now_s(); sqlite3_step(ins); sqlite3_reset(ins); lat[i] = now_s() - t; tot += lat[i]; }
  qsort(lat, (size_t)n, sizeof(double), cmpd);
  emit(phase, "insert_autocommit_ops_s", n / tot, "ops/s");
  emit(phase, "insert_autocommit_p50", lat[n / 2] * 1e6, "us");
  emit(phase, "insert_autocommit_p99", lat[(int)(n * 0.99)] * 1e6, "us");
  int batches = 50; double t0 = now_s();
  for (int b = 0; b < batches; b++) {
    sqlite3_exec(h, "BEGIN", 0, 0, 0);
    for (int j = 0; j < 1000; j++) { bind_row(ins, ++g_id); sqlite3_step(ins); sqlite3_reset(ins); }
    sqlite3_exec(h, "COMMIT", 0, 0, 0);
  }
  emit(phase, "insert_batch1000_rows_s", batches * 1000 / (now_s() - t0), "rows/s");
  tot = 0;
  for (int i = 0; i < n; i++) {
    sqlite3_bind_int(upd, 1, (int)(xr() % 100)); sqlite3_bind_double(upd, 2, (xr() % 10000) / 100.0); sqlite3_bind_int(upd, 3, 1 + (int)(xr() % (unsigned)g_id));
    double t = now_s(); sqlite3_step(upd); sqlite3_reset(upd); lat[i] = now_s() - t; tot += lat[i];
  }
  qsort(lat, (size_t)n, sizeof(double), cmpd);
  emit(phase, "update_autocommit_ops_s", n / tot, "ops/s");
  emit(phase, "update_autocommit_p99", lat[(int)(n * 0.99)] * 1e6, "us");
  free(lat); sqlite3_finalize(ins); sqlite3_finalize(upd);
}

// Ordered dump hash of the bench table: equal hash == identical content.
static int table_hash(const char *path, char out[65], long long *rows) {
  sqlite3 *h; if (sqlite3_open_v2(path, &h, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) return -1;
  sqlite3_stmt *s; *rows = 0;
  if (sqlite3_prepare_v2(h, "SELECT id, name, email, qty, quote(price), note FROM bench ORDER BY id", -1, &s, 0) != SQLITE_OK) { sqlite3_close(h); return -1; }
  size_t cap = 1 << 20, len = 0; char *buf = malloc(cap);
  while (sqlite3_step(s) == SQLITE_ROW) {
    char line[256];
    int n = snprintf(line, sizeof line, "%lld|%s|%s|%d|%s|%s\n", (long long)sqlite3_column_int64(s, 0), sqlite3_column_text(s, 1),
                     sqlite3_column_text(s, 2), sqlite3_column_int(s, 3), sqlite3_column_text(s, 4), sqlite3_column_text(s, 5));
    if (len + (size_t)n + 1 > cap) { cap *= 2; buf = realloc(buf, cap); }
    memcpy(buf + len, line, (size_t)n); len += (size_t)n; (*rows)++;
  }
  ark_sha256_hex(buf, len, out); free(buf); sqlite3_finalize(s); sqlite3_close(h); return 0;
}

static int wait_drained(arkilian *db, double limit_s) {
  double t0 = now_s();
  while (now_s() - t0 < limit_s) { if (db_backup_queue_depth(db) == 0) return 0; db_wal_flush(db); usleep(20000); }
  return -1;
}

static int restore_and_verify(const char *phase, const char *src, const char *dst) {
  rmdb(dst);
  double t0 = now_s();
  int rc = arkilian_hydrate_s3(dst, g_ep, g_bucket, "us-east-1", g_ak, g_sk, g_prefix, NULL, NULL);
  double dt = now_s() - t0;
  if (rc != HYDRATION_OK) { printf("  %-26s RESTORE FAILED rc=%d\n", phase, rc); return 1; }
  emit(phase, "restore_seconds", dt, "s");
  char ha[65], hb[65]; long long ra = 0, rb = 0;
  table_hash(src, ha, &ra); table_hash(dst, hb, &rb);
  emit(phase, "rows_source", (double)ra, "rows");
  emit(phase, "rows_restored", (double)rb, "rows");
  int same = strcmp(ha, hb) == 0 && ra == rb;
  printf("  %-26s %-30s %14s\n", phase, "restored_equals_source", same ? "YES" : "NO");
  if (g_csv) fprintf(g_csv, "%s,restored_equals_source,%d,bool\n", phase, same);
  return same ? 0 : 1;
}

int main(int argc, char **argv) {
  if (argc < 5) { fprintf(stderr, "usage: %s <endpoint> <bucket> <access-key> <secret-key> [rows] [--outage SECONDS]\n", argv[0]); return 2; }
  g_ep = argv[1]; g_bucket = argv[2]; g_ak = argv[3]; g_sk = argv[4];
  int n = 5000, outage = 0;
  for (int i = 5; i < argc; i++) {
    if (!strcmp(argv[i], "--outage") && i + 1 < argc) outage = atoi(argv[++i]);
    else n = atoi(argv[i]);
  }
  curl_global_init(CURL_GLOBAL_DEFAULT);
  g_csv = fopen("results.csv", "a");
  snprintf(g_prefix, sizeof g_prefix, "arkbench-%ld", (long)time(NULL));
  printf("Arkilian end-to-end benchmark\n  endpoint=%s bucket=%s prefix=%s rows=%d\n\n", g_ep, g_bucket, g_prefix, n);
  if (create_bucket() != 0) return 1;

  // 1. Baseline: raw SQLite, same pragmas as Arkilian's defaults.
  {
    rmdb("mb_raw.db"); sqlite3 *h; sqlite3_open("mb_raw.db", &h);
    sqlite3_exec(h, "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA busy_timeout=5000; PRAGMA foreign_keys=ON;", 0, 0, 0);
    sqlite3_exec(h, SCHEMA, 0, 0, 0);
    workloads("raw_sqlite_sync_full", h, n);
    sqlite3_close(h); rmdb("mb_raw.db");
  }
  g_id = 0;

  // 2. Arkilian with live shipping (defaults: synchronous=FULL outbox).
  setenv("ARKILIAN_S3_ENDPOINT", g_ep, 1); setenv("ARKILIAN_S3_BUCKET", g_bucket, 1);
  setenv("ARKILIAN_S3_REGION", "us-east-1", 1); setenv("ARKILIAN_S3_ACCESS_KEY", g_ak, 1);
  setenv("ARKILIAN_S3_SECRET_KEY", g_sk, 1); setenv("ARKILIAN_S3_PREFIX", g_prefix, 1);
  setenv("ARKILIAN_MANIFEST_HMAC_KEY", "arkbench-local-hmac-key-0123456789abcdef", 1);
  setenv("ARKILIAN_ENABLE_BACKUP", "1", 1); setenv("ARKILIAN_CHUNK_INTERVAL_SEC", "1", 1);
  setenv("ARKILIAN_MANIFEST_INTERVAL_SEC", "1", 1); setenv("ARKILIAN_BACKUP_INTERVAL", "999999", 1);
  setenv("ARKILIAN_BACKUP_PATH", "mb_backup.sqlite", 1);
  rmdb("mb_src.db");
  arkilian *db = NULL;
  if (db_init(&db, "mb_src.db") != 0) { fprintf(stderr, "db_init failed\n"); return 1; }
  db_exec(db, SCHEMA);
  for (int i = 0; i < 300 && !(db_backup_health_flags(db) & ARK_HF_MANIFEST_RESOLVED); i++) usleep(50000);
  if (!(db_backup_health_flags(db) & ARK_HF_MANIFEST_RESOLVED)) { fprintf(stderr, "storage not reachable (manifest read unresolved)\n"); return 1; }
  sleep(2);  // let the startup snapshot publish
  sqlite3 *h = db_get_handle(db);
  workloads("arkilian_live_shipping", h, n);

  // 3. Time until every change is durable in MinIO.
  double t0 = now_s();
  if (wait_drained(db, 120) != 0) printf("  queue did not drain within 120 s\n");
  emit("arkilian_live_shipping", "drain_to_minio_seconds", now_s() - t0, "s");
  emit("arkilian_live_shipping", "chunks_uploaded", db_backup_chunk_count(db), "chunks");

  // 4. Snapshot cycle.
  t0 = now_s();
  int src = arkilian_run_snapshot_cycle(db);
  emit("snapshot", "snapshot_cycle_seconds", now_s() - t0, "s");
  { sqlite3_stmt *s; sqlite3_prepare_v2(h, "SELECT page_count * page_size FROM pragma_page_count, pragma_page_size", -1, &s, 0);
    if (sqlite3_step(s) == SQLITE_ROW) emit("snapshot", "database_size", sqlite3_column_int64(s, 0) / 1048576.0, "MiB");
    sqlite3_finalize(s); }
  if (src != 0) printf("  snapshot cycle rc=%d\n", src);

  // 5. A few more changes after the snapshot, then a clean shutdown.
  sqlite3_exec(h, "UPDATE bench SET qty = qty + 1 WHERE id % 7 = 0; DELETE FROM bench WHERE id % 97 = 0;", 0, 0, 0);
  printf("  healthy before shutdown: %d (flags 0x%x)\n", db_backup_is_healthy(db), db_backup_health_flags(db));
  t0 = now_s(); db_close(db); db = NULL;
  emit("shutdown", "close_with_final_publish_s", now_s() - t0, "s");

  // 6. Restore + verify.
  int bad = restore_and_verify("restore", "mb_src.db", "mb_restored.db");

  // 7. Optional outage run (driver pauses MinIO while this writes).
  if (outage > 0) {
    if (db_init(&db, "mb_src.db") != 0) { fprintf(stderr, "reopen failed\n"); return 1; }
    for (int i = 0; i < 300 && !(db_backup_health_flags(db) & ARK_HF_MANIFEST_RESOLVED); i++) usleep(50000);
    h = db_get_handle(db);
    sqlite3_stmt *ins; sqlite3_prepare_v2(h, "INSERT INTO bench VALUES(?,?,?,?,?,?)", -1, &ins, 0);
    { sqlite3_stmt *m; sqlite3_prepare_v2(h, "SELECT coalesce(max(id),0) FROM bench", -1, &m, 0); if (sqlite3_step(m) == SQLITE_ROW) g_id = sqlite3_column_int(m, 0); sqlite3_finalize(m); }
    printf("\n  outage run: %d s of steady writes (~500 rows/s); MinIO is paused by the driver\n", outage);
    printf("  %6s %10s %8s %10s\n", "t(s)", "queue", "healthy", "write_ms");
    double start = now_s(), worst = 0; int sec = -1, peak = 0;
    while (now_s() - start < outage) {
      double t = now_s(); bind_row(ins, ++g_id); sqlite3_step(ins); sqlite3_reset(ins); double d = now_s() - t; if (d > worst) worst = d;
      int s = (int)(now_s() - start);
      if (s != sec) { sec = s; int q = db_backup_queue_depth(db); if (q > peak) peak = q;
        printf("  %6d %10d %8d %10.2f\n", s, q, db_backup_is_healthy(db), worst * 1e3); fflush(stdout); worst = 0; }
      usleep(2000);
    }
    emit("outage", "peak_queue_depth", peak, "rows");
    t0 = now_s();
    if (wait_drained(db, 300) != 0) printf("  queue did not drain within 300 s after the outage\n");
    emit("outage", "recovery_drain_seconds", now_s() - t0, "s");
    sqlite3_finalize(ins);
    db_close(db);
    bad |= restore_and_verify("outage_restore", "mb_src.db", "mb_restored2.db");
  }
  if (g_csv) fclose(g_csv);
  printf("\n%s\n", bad ? "RESULT: restore verification FAILED" : "RESULT: all restores verified identical to source");
  return bad;
}
