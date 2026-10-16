# Monitor compatibility matrix

Date: 2026-10-16. This matrix records observed results, not a model-wide compatibility promise.

| Host / OS | Display | Transport | Brightness read | Volume read | Writes / restoration |
|---|---|---|---|---|---|
| Apple M4 Pro / macOS 27.0.1 (26A434) | DELL S3225QC | `dispext1`, IOAV matched through `IOMobileFramebufferShim` and the proxy's direct provider interface | VCP 0x10: 10 / 100 | VCP 0x62: 0 / 100 | Not exercised |
| Same host | Built-in Color LCD | Excluded from external monitor controls | Not applicable | Not applicable | Not applicable |

The final release bundle's `--diagnostics` and `--list-displays` were executed locally. Both brightness and volume report supported. The cable/dock model was not recorded, so this result must not be generalized to another connection. The earlier implementation incorrectly discarded the Dell when transport lookup failed; discovery now keeps external displays visible with unavailable controls, and provider matching was corrected for the observed topology.

These probes send read-only Get VCP requests. No Set VCP request, webcam capture, two-external-monitor test, M1 test, cable/dock comparison, or restore lifecycle test has been performed. Volume read support does not prove that every input/output path exposes monitor speakers. A zero brightness setting is not a panel-off guarantee.
