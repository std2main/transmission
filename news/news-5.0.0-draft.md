# Transmission 5.0.0 (DRAFT PLEASE IGNORE)

## Changelog

- Added an independent quick verify torrent action end to end, with a dedicated `torrent_verify_quick` RPC, Web UI menu entry, API compatibility plumbing, and regression coverage.
- Bumped the advertised RPC version to reflect the new public RPC method so feature detection stays accurate.
- Reworked quick verify into staged file-aware sampling: metadata-gated file checks first, then 32 MiB head/tail windows for files above 100 MiB, an additional rotating 32 MiB middle window for files above 500 MiB, and finally full reads for the remaining small files.
- Fixed quick verify reporting so fallback-enabled quick-verify failures are no longer misreported as “stopped without full fallback”.
- Kept `seed_existing_mode` scoped to the add-time verify flow so it no longer leaks into later manual Verify requests.
- Preserved the simplified manual Verify RPC/UI path while keeping quick verify available through add-time policy and migration flows.
- Switched the default Docker Compose path to use the published `std2main/transmission-seeder:plus-dev` runtime image, while adding a separate local-build override for contributors who want to rebuild from source.
- Added regression coverage for:
  - no-fallback quick verify failures
  - fallback-enabled quick verify failures
  - `seed_existing_mode` lifetime
  - verify.log / daemon log / RPC visibility consistency

## Upgrading from older versions

Please follow the upgrade instructions below to ensure no loss of settings and program state.

### From 4.1.0 or above

Safe to upgrade directly to `5.0.0`.

### From below 4.1.0

1. Upgrade to a version that's at least `4.1.0`, but below `5.0.0` or any of its alphas/betas/RC.
2. Start and stop Transmission.
3. Upgrade to `5.0.0`.
