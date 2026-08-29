# Building and verifying Keel release artifacts

This describes how Keel's release artifacts are built and verified. It covers the deterministic source
archive and its checks only; signing, tagging, uploading, and creating a GitHub release are a separate,
explicitly authorized publication step (not part of this mechanism).

## What a release bundle is

Keel ships source-first: a deterministic source archive plus a SHA-256 checksum manifest and the
in-tree SBOM. No portable prebuilt static library is produced or claimed; consumers build and statically
link `libkeel.a` on their platform (see `docs/operations/platform_support.md`). Bring-your-own
dependencies (TLS, HTTP/2, compression backends) stay unvendored; only tracked material already required
to build and test Keel is archived.

## Commands

```
make release                          # build dist/keel-<VERSION>.tar.gz + dist/keel-<VERSION>.sha256
make check-release-artifacts          # untagged: build, verify determinism, extract, build/test/install
                                      #   from the extracted tree, C and C++ consumers, canaries
make check-release-artifacts-strict   # additionally require the Git tag v<VERSION> to point at HEAD
```

The archive is named from the single-source root `VERSION` file (for example `keel-2.9.0.tar.gz`).
`make release` writes to `dist/`, which is gitignored; generated archives are never committed.

## Determinism

Two builds from the same clean commit are byte-identical. The archive is `git archive` (tracked files
only, in stable tree order, with git mode bits, uid/gid 0, and file mtimes derived from the commit, not
wall-clock) piped through `gzip -n` (no name or timestamp in the gzip header). Because only tracked
files are archived, `.git`, untracked files, build output, editor state, local configuration, and the
`dist/` output directory are all excluded automatically.

## What the verification proves

`check-release-artifacts` runs the whole chain and fails closed on any mismatch:

1. refuses a dirty tracked tree and a malformed or drifted `VERSION`;
2. builds the artifacts twice in isolated temp dirs and compares the archives and SHA-256 hashes;
3. extracts into a clean temp dir;
4. confirms the extracted file set equals the tracked release manifest and the required files/dirs are
   present (VERSION, `include/keel/version.h`, `keel.pc.in`, `sbom.cdx.json`, LICENSE, CHANGELOG,
   the migration guide, Makefile, tooling, and the `include`/`src`/`integrations`/`examples`/`tests`
   trees);
5. proves agreement among VERSION, the archived `version.h`, the SBOM component version and purl, the
   archive name, and the checksum manifest;
6. builds and tests from the extracted tree with no reference to the original repo, and checks the
   compiled `kl_version()`;
7. performs a staged install and out-of-tree C and C++ compile-link-run consumers (pkg-config when
   available, else direct include/lib flags);
8. rejects any source-tree, absolute build-path, DESTDIR, or vendor-path leakage in the installed
   metadata;
9. validates the SBOM as JSON and the presence of the license and release documents;
10. self-canaries: a version disagreement, a missing or unexpected archive file, nondeterministic gzip,
    and checksum corruption are each proven to be detected.

## Untagged vs strict mode

- Untagged (`--verify` / `make check-release-artifacts`) verifies the artifacts for any clean commit;
  used during development and release preparation.
- Strict (`--strict` / `make check-release-artifacts-strict`) additionally requires the Git tag
  `v<VERSION>` to exist and point at HEAD; used at the actual release commit.

## Where this sits in the release sequence

This is R3-4: it builds and verifies artifacts but publishes nothing. Enrolling the gate in the
release-candidate matrix is R3-5. Signing (checksum and tag), tagging, uploading, and creating the
GitHub release are the separately authorized R3-6 publication step; the signing method is decided
there. The version and maintenance policy is in `docs/v3_release_version_policy_freeze.md`.
