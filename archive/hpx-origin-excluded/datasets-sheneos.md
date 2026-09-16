# The `sheneos/` dataset section of `datasets/README.md`

Cut verbatim on 2026-09-16 with the `sheneos_hpx` row; the staged files were
deleted with it.  Re-staging follows this section as written.

## sheneos/ — tabulated nuclear equation of state

Consumed by the EOS-table program as a runtime input (the table file is the
program's `--file` argument). It is a published measurement table, not a
generated fixture: nothing in this tree can produce it, and it is read
identically by every runtime in the comparison, so it is staged by hand and
gated by checksum.

| file | size | role |
|---|---|---|
| `HShenEOS_rho220_temp180_ye65_version_1.1_20120817.h5` | 391,264,384 B | the table the program interpolates (app `--file`) |
| `HShenEOS_rho220_temp180_ye65_version_1.1_20120817.h5.bz2` | 282,075,158 B | the compressed original as downloaded; kept so a re-stage needs no second download |
| `SHA256SUMS` | — | integrity manifest (covers the `.h5`) |

Source: stellarcollapse.org's equation-of-state collection,
`https://stellarcollapse.org/~evanoc/HShenEOS_rho220_temp180_ye65_version_1.1_20120817.h5.bz2`
(H. Shen et al. table, version 1.1, dated 2012-08-17). The file names its own
axis lengths: 220 density points x 180 temperature points x 65 electron
fraction points; the reader takes every dimension from the file, so a
differently sized table of the same layout also loads.

```
3b7c598bf56ec12d734e13a97daf1eeb1f58f59849c5f65c4f9f72dd292b177c  HShenEOS_rho220_temp180_ye65_version_1.1_20120817.h5
```

Verify after staging:

```bash
cd datasets/sheneos && sha256sum -c SHA256SUMS
```
