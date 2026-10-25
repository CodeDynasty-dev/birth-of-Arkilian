# Changelog

## 1.6.0 — replication correctness release

Every fix below closes a defect that was reproduced against 1.5.0 and is now
covered by `tests/test_p0_replication.c` (or an updated existing suite).

### Fixed — data correctness (P0)

- **Restore → keep writing → restore again failed.** Hydration reset the outbox
  sequence, so a restored node restarted LSNs at 1 and published an overlapping
  manifest that every later restore rejected, while health stayed green. The
  restored node now continues the prefix's LSN sequence (verified across four
  failover generations, including a snapshot taken on a restored node, which
  previously also failed with a stale `last_applied_lsn`).
- **An UPDATE that changed a PRIMARY KEY left a ghost row** in the replica
  (INTEGER, TEXT and composite WITHOUT ROWID keys). The update trigger now ships
  a `DELETE` of the old key before the `REPLACE` of the new one.
- **DDL near a snapshot made the restore fail.** The snapshot baseline sat below
  the image's real position, so `CREATE TABLE` / `ALTER TABLE` / `CREATE INDEX`
  were replayed onto a schema that already had them. Snapshots now record their
  exact LSN inside the copy's read transaction, chunk statements carry LSN tags,
  hydration skips exactly the statements the image contains, and wrapped DDL
  commits atomically with its capture row.
- **A full outbox silently dropped part of a transaction.** Overflow now writes
  a durable capture-gap marker in the same transaction; health turns RED
  immediately and no manifest is published until a snapshot rebases. New strict
  mode `ARKILIAN_CAPTURE_OVERFLOW=abort` fails the write instead.

### Fixed — reliability

- **Split-brain guard:** a node whose LSN head is behind the remote manifest
  (a second writer, or a node not restored from the prefix) freezes shipping and
  publishing with a clear error instead of interleaving histories. Chunk and
  snapshot keys carry a random per-process writer id, so writers can never
  overwrite each other's objects. `ARKILIAN_MANIFEST_REBASE=1` is the explicit
  operator override.
- **Snapshots use immutable per-cycle keys**
  (`{prefix}/snapshots/<lsn>_<writer>.backup.sqlite`). Overwriting a fixed
  `backup.sqlite` broke the previous manifest whenever a new publish failed.
- **Snapshot staging is unique per cycle.** Instances sharing the default
  `./backup.sqlite` staging path overwrote each other's image mid-upload.
- **Clean shutdown publishes its tail.** Final uploads were aborted by the
  shutdown flag (`CURLE_ABORTED_BY_CALLBACK`), so the last chunks never reached
  the manifest. `db_close()` now drains the whole outbox in full chunks within
  `ARKILIAN_SHUTDOWN_DRAIN_SEC` (default 10 s) and publishes immediately
  (found by the end-to-end benchmark: a large final backlog was cut short).
- Shipping waits for the startup manifest read, so adopted and new chunk
  records stay in LSN order; an endpoint refused by the SSRF guard is reported
  at startup.
- The startup snapshot is retried on a short backoff until it publishes,
  instead of waiting a full `ARKILIAN_BACKUP_INTERVAL`.

### Performance

Measured on the same machine against raw SQLite with prepared statements
(`bench/bench_cmp.c`, `synchronous=NORMAL`, median of 3):

| Workload | 1.5.0 | 1.6.0 |
| --- | --- | --- |
| Single-row INSERT (autocommit) | 7.3x slower than SQLite | 3.2x |
| INSERT, 1000 rows per transaction | 57x | 12.5x |
| UPDATE by primary key (autocommit) | 4.6x | 2.5x |
| Point SELECT | 1.1x | 1.1x |

- The flush thread is no longer woken on every changed row (the update hook
  fired per row, before commit, so the thread spun on empty drains in
  lock-step with the writer). Wake-ups are coalesced to at most one per
  100 ms and the thread polls every 100 ms while writes flow.
- The outbox cap is no longer measured on every captured row. A guard
  trigger on the outbox checks occupancy every M-th row (M = min(256,
  cap/4)) with M rows of headroom, so the cap stays exact while capture
  triggers test a single flag row; overflow still records a capture gap in
  the same transaction (or fails the write in strict mode).
- Write cost no longer grows with the unshipped backlog (1.5.0 ran a
  `COUNT(*)` over the outbox per row: about 10x slower at 90k pending rows).
- The `created_at` outbox index is dropped from the capture hot path.
- Remaining overhead is inherent to trigger-based row capture: every captured
  row also writes an outbox row with its replay SQL.

### Build, tooling and docs

- `bench/bench_cmp.c`: fair local benchmark against raw SQLite (prepared
  statements, identical pragmas, exact latency percentiles).
- `bench/minio/run.sh`: one-command end-to-end benchmark against a local MinIO
  container: live-shipping throughput, drain time, snapshot, shutdown
  publish, restore with byte-level verification, and a storage outage.
- A data race in the `test_kill_resilience` mock server (read while its
  thread was still recording) is fixed.

- The documented default build works on a fresh clone (the example target
  pointed at a missing `examples/main.c`).
- README guarantees rewritten to match behavior, including the recovery-point
  objective. `BENCHMARK.md` figures are marked superseded pending a GCE + GCS
  benchmark.

### Upgrade notes

- New chunk and snapshot object keys; manifests written by 1.5.0 remain
  readable and restorable.
- A node that starts against a prefix whose manifest is ahead of its local
  database now refuses to ship. Restore it with `arkilian_hydrate_s3()` first,
  use a different `ARKILIAN_S3_PREFIX`, or set `ARKILIAN_MANIFEST_REBASE=1` if
  this database is knowingly the authority.
- Deployments that were restored under 1.5.0 and kept writing have a reset LSN
  sequence and will hit that guard after upgrading: their remote history was
  already unrestorable. Set `ARKILIAN_MANIFEST_REBASE=1` once on the live writer;
  the next snapshot republishes a clean restore point.
