# Camera Lifecycle and Display Suspension

This archive preserves the completed implementation plan and its physical macOS acceptance checklist. The feature was developed through a standalone fast-feature plan, so it did not have separate `spec.md` or `contracts.md` files. Its requirements, decisions, implementation phases, validation strategy, and results are recorded together in [plan.md](plan.md); the checklist remains separate because it records hardware scenarios that have not been run.

- [Implementation plan and results](plan.md)
- [macOS physical acceptance checklist](validation/macos-acceptance.md)

The checklist was run in part on physical hardware on 2026-10-18: ten of the sixteen scenarios passed with one MacBook Pro and one Dell S3225QC, one passed in part, and five were not run. Automated tests and local packaging alone do not establish camera, display-sleep, DDC or power-consumption behavior, and no claim covers other monitors or connections. Windows remains deferred.
