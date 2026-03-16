# Security Policy

## Supported Versions

| Version | Supported |
|---------|-----------|
| 1.0.x   | Yes       |

## Reporting a Vulnerability

**Please do not open public issues for security vulnerabilities.**

Report vulnerabilities via [GitHub Security Advisories](https://github.com/artalis-io/keel/security/advisories/new) (private disclosure). You will receive an initial response within 72 hours.

Include:
- Description of the vulnerability
- Steps to reproduce
- Affected versions
- Impact assessment (if known)

## Security Measures

Keel employs multiple layers of security testing:

- **Compiler hardening**: `-Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Werror -fstack-protector-strong`
- **AddressSanitizer + UndefinedBehaviorSanitizer**: CI runs all tests under ASan/UBSan (`make debug-test`)
- **Fuzz testing**: libFuzzer targets for HTTP parser, multipart parser, WebSocket parser, and response parser
- **Static analysis**: Clang scan-build and cppcheck in CI
- **CodeQL**: GitHub SAST scanning on every push and PR

## Vendored Dependencies

All vendored code is tracked in [`vendor/MANIFEST.json`](vendor/MANIFEST.json) with version pins and checksums. A CycloneDX SBOM is available at [`sbom.cdx.json`](sbom.cdx.json).

## Supply Chain Recommendations

For production deployments, we recommend:
- Verify signed commits on releases
- Enable 2FA on accounts with write access
- Use SSH keys for repository access
- Enable branch protection on `main`
