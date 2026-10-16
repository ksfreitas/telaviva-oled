# macOS process resource sampling

`tools/validation/sample_process.py` samples an already-running process. It never launches TelaViva, accesses the camera, or sends monitor commands. Run it only after building and launching the release app yourself.

Find the PID and executable path for the release app, then pass both explicitly. For example, after substituting the PID and actual app bundle path:

```sh
tools/validation/sample_process.py 12345 \
  --expected-executable "/Applications/TelaViva.app/Contents/MacOS/TelaViva" \
  --interval 1 --duration 300 --output baseline.jsonl
```

The sampler verifies the PID's executable path against the expected path before sampling and rechecks it throughout the run. It fails closed if the PID exited, cannot be inspected, or belongs to another executable. Output is JSON Lines: one metadata record (PID, executable, macOS version, architecture, Mac model, interval and metric definitions), followed by timestamped samples. CPU records distinguish cumulative CPU seconds since process start from sampled average CPU percent between samples; 100% means one fully used logical core. RSS is reported in bytes, converted from `ps` kilobytes.

Take separate runs and output files for baseline and each protection state, and record the state in the filename or accompanying notes. Use the same release build, Mac, display setup, and sampling duration when comparing runs. These samples describe only the target process and do not establish whole-system energy use or camera energy impact. Energy measurements require a separate `powermetrics` run; do not infer energy from CPU or RSS. This harness does not invoke `powermetrics` or use `sudo`.

The script has no third-party dependencies and is intended for macOS, where it uses `libproc` to verify process identity and `ps` for cumulative CPU time and RSS. Use `--help` for options. A short initial baseline is recorded below; active-camera and energy acceptance remain unverified.

## Initial release idle measurement

On 2026-10-16, the optimized native bundle (Rust release static libraries and Swift `-O`) was launched with no saved settings and both protections disabled. After five seconds of warm-up, the sampler collected 60 one-second samples over 60.025 seconds on the M4 Pro / macOS 27.0.1 host. The app could read the connected Dell's capabilities; no camera or Set VCP action was requested.

| Metric | Observed |
|---|---:|
| RSS minimum | 84.31 MiB |
| RSS maximum | 84.42 MiB |
| Mean sampled CPU | approximately 0.017% of one logical core |

The CPU estimate uses quantized `ps` cumulative CPU time and is a short baseline, not an energy measurement or a multi-minute acceptance run. Raw local records are in `build/validation/idle-release.jsonl` (ignored build artifact). The owned test process was stopped by the harness after sampling; this does not validate normal application quit restoration. An initial concurrent-build sample failed executable identity validation and was discarded before this successful frozen-bundle run.

The idle observation is below the planned 100 MiB / 0.2% budgets for this short run only. Battery-suspended state, active and absent camera modes, 30-second detector release, energy, wakeups, and return latency still need separate measurements. The isolated detector's earlier 47.6 MiB peak is a different short-process measurement and must not be added to or substituted for whole-app active memory.
