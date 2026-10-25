# Benchmarks

| Tool | What it measures |
| --- | --- |
| `bench_cmp.c` | Capture overhead vs raw SQLite: same schema, same pragmas, prepared statements, exact percentiles. Arkilian's outbox is drained outside the timed region (a flush thread that keeps up). |
| `minio/minio_bench.c` | End-to-end against an S3-compatible server: write throughput with live shipping, time until all changes are durable, snapshot cycle, clean-shutdown publish, restore with byte-level verification, optional storage outage. |
| `minio/run.sh` | One command that builds both, runs the local comparison, starts a throwaway MinIO container, runs the end-to-end benchmark (pausing MinIO for 15 s mid-run) and writes `minio/results-<timestamp>.txt`. |

```bash
bash bench/minio/run.sh          # needs docker with a minio image, cc, libcurl
```

The container uses throwaway local credentials and is removed when the run ends.
