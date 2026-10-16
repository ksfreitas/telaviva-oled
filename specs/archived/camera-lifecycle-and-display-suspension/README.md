# Camera Lifecycle and Display Suspension

This archive preserves the completed implementation plan and its physical macOS acceptance checklist. The feature was developed through a standalone fast-feature plan, so it did not have separate `spec.md` or `contracts.md` files. Its requirements, decisions, implementation phases, validation strategy, and results are recorded together in [plan.md](plan.md); the checklist remains separate because it records hardware scenarios that have not been run.

- [Implementation plan and results](plan.md)
- [macOS physical acceptance checklist](validation/macos-acceptance.md)

The checklist is still unverified. Automated tests and local packaging do not establish camera, display-sleep, DDC write/restoration, or power-consumption behavior on physical hardware. Windows remains deferred.
