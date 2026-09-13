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

## Release-candidate validation in CI

The `rc-validate` make target runs the readiness gates locally in a fail-fast order: version-drift, the
workflow + documentation/text checks, the public-surface + install gates, the deterministic
release-artifact verification, a release build, the full test suite, and the sanitizer suite.

In standing CI, a dedicated read-only `Release archive (verify + upload)` job (Ubuntu, pkg-config
required) runs `make check-release-artifacts RELEASE_REQUIRE_PKGCONFIG=1`, builds the archive, and
uploads the tarball + checksum as short-lived workflow artifacts (validation output, not a release
asset; named with the version and the audited commit). Downstream `Archive build` jobs on Ubuntu
(epoll), macOS (kqueue), and Windows (WSAPoll) download that archive, verify its checksum, extract it,
and build/test from the extracted tree. The workflow keeps read-only permissions, uses no release/upload
API, needs no secrets, and keeps a short artifact retention. The `check-workflows` gate lints the
workflow YAML (and actionlint when installed) locally so a malformed workflow edit is caught before push.

## Release-candidate acceptance criteria

A release candidate is accepted for promotion to a final release only when ALL of the following hold:

- The full CI matrix and CodeQL are green on the RC commit.
- The verified-archive matrix is green: `Release archive (verify + upload)` and the Ubuntu, macOS, and
  Windows `Archive build` jobs all pass, so the deterministic source archive builds and tests from its
  extracted tree on every hosted OS.
- There are no open release-blocking security or correctness findings.
- At least seven clean calendar days have elapsed after the final RC tag with no new release-blocking
  issue.

Promotion to the final `3.0.0` then changes only version/prerelease metadata and release documentation;
any functional change requires a new RC (see `docs/v3_release_version_policy_freeze.md`).

## Where this sits in the release sequence

Everything above builds and verifies artifacts; it publishes nothing. The sections below are the
publication step, which is separately authorized every time. The version and maintenance policy is in
`docs/v3_release_version_policy_freeze.md`.

## Choosing the version number

`docs/v3_release_version_policy_freeze.md` is the frozen policy. Within a major version Keel promises
**source** compatibility ("recompile-and-relink against a newer 3.y.z with no source edits") and
explicitly does **not** promise a cross-version binary ABI.

So:

- a new user-visible capability, or **any changed public declaration**, is a MINOR bump
- fixes that change no declaration are a PATCH

3.1.0 was minor for both reasons at once: native MSVC support, and `kl_http_response_file()` changing
from `KlSocketHandle fd` to `int fd`. That change is source-compatible, because C applies the integer
conversion implicitly, but it still changes a declaration, so a patch number would have been wrong.

## Publishing, step 1: the release-prep PR

Strictly no code. The diff should be four files.

```sh
echo "3.1.0" > VERSION
make version-sync        # regenerates include/keel/version.h and sbom.cdx.json; keel.pc at build time
make check-version-drift # VERSION, version.h, keel.pc.in and the SBOM must all agree
```

Then promote the `[Unreleased]` section of `CHANGELOG.md` into a dated-by-tag `[X.Y.Z]` section, and
write upgrade notes for anything a consumer must know before upgrading: changed declarations, changed
build requirements, and explicitly whether existing builds are unaffected.

Open it as its own PR. Do not carry code changes in it.

## Publishing, step 2: tag and publish

Only after step 1 has merged. **This is a separate, explicit authorization**: publishing is
outward-facing and awkward to retract, so it is never bundled with "merge the prep PR".

The verification chain, in order, and every link matters:

```text
audited commit
  -> CI green on that exact SHA, with ZERO non-success jobs (not merely a green run)
  -> the archive from THAT run's artifact, downloaded, never rebuilt locally
  -> its SHA-256 recomputed and matched against the manifest
  -> the VERSION file INSIDE the tarball checked
  -> annotated tag created at the literal SHA, message "Keel X.Y.Z"
  -> the tag dereferenced and confirmed to point at that SHA
  -> published with --verify-tag
```

```sh
gh run view <run-id> --json jobs -q '[.jobs[] | select(.conclusion != "success")] | length'
gh run download <run-id> --dir build/rel        # keel-X.Y.Z-<sha>-source-validation
# verify the checksum and the in-tarball VERSION, then:
git tag -a vX.Y.Z <full-sha> -m "Keel X.Y.Z"
git push origin vX.Y.Z
gh release create vX.Y.Z --title "Keel X.Y.Z" --notes-file <notes> --verify-tag     build/rel/.../keel-X.Y.Z.tar.gz build/rel/.../keel-X.Y.Z.sha256
```

## Why the archive comes from CI

`make release` builds an archive locally, and `check-release-artifacts` proves the build is
deterministic, but the published asset must be the one CI produced from the audited commit. Building
it again on a developer machine would mean publishing bytes nobody verified in their final form, and
the reproducibility guarantee would be a claim rather than a property.

That guarantee is why `.gitattributes` pins `eol=lf`: `git archive` applies `core.autocrlf`, so a
Windows checkout would otherwise produce a different archive from the same commit.

## Tagging the literal SHA

Tag the full commit hash, not `main` or `HEAD`. Between verifying CI and creating the tag, `main` can
move. Tagging a branch name silently releases whatever arrived in the meantime, and the tag would no
longer name the commit whose jobs were checked.
