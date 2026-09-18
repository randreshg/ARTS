# coherence-wrf-val — retired 2026-09-18

The `WRF_VAL` arm (`ARTS_MEMORY_MODEL=DB_WRF × VAL × WT`) was the validation
family carried under a model that needs no validation: it kept buffer
versions, a per-rank served-version ledger with header-only replies, read
request combining, a publish write-combining flight and a snapshot reorder
buffer. Its successor is the `FLUSH` arm under the `DB_WRF` memory model
(`libs/src/core/coherence/flush/`), which fetches the whole payload at every
remote acquire and writes it back at every remote RW release. The nine tests
here exercised WRF_VAL-only mechanics (ledger dedup, version watermarks) and
have no counterpart to migrate.
