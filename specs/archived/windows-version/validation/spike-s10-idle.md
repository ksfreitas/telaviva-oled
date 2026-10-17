# Spike S10: idle cost of the WinUI 3 shell

Date: 2026-10-17. Machine: Windows 11 build 26300, AMD Ryzen 7 9800X3D (16 logical cores), desktop.

**Result: provisional go.** The skeleton shell (tray host, hidden platform window, XAML initialized, no window open, no core running) idles at a small but not yet macOS-level CPU cost. The Win32 resident agent fallback is not needed yet, but SC-003 stays open until the full app (core, camera and monitor watchers) is measured.

## Method

`TelaViva.exe` was started without arguments, left for 5 s to settle, then its processor time, working set, private bytes, threads and handles were sampled over a 60 s window (`Get-Process`, run by the coordinator, not Task Manager). A second launch was started to confirm the single-instance behavior. Release x64 build, self-contained Windows App SDK 2.5.1, no presence protection and no monitors attached to the app.

## Measurement

| Metric | Value |
|--------|-------|
| CPU time used in 60 s | 47 ms |
| CPU | 0.078% of one core (0.0049% of 16 logical cores) |
| Working set | 57.6 MB |
| Private bytes | 59.4 MB |
| Threads | 25 |
| Handles | 516 |
| Main window handle | none (no window created) |

Single instance: the second launch exited by itself with code 0 and the first instance stayed alive.

## Against SC-003

SC-003 asks for steady-state CPU and memory at or below the measured macOS idle figures (about 0.02% of one core and about 85 MiB RSS) or a Windows budget set in design.

- Memory: 57.6 MB working set is below the macOS figure.
- CPU: 0.078% of one core is above the macOS figure (about 0.017%). Over a 60 s window 47 ms is a coarse number and likely includes residual start-up work, so a longer window (10 minutes or more) is needed before concluding.

## Not measured

- GPU use and wake-ups per second (Task Manager GPU column and Windows Performance Recorder).
- Behavior with EcoQoS on (the efficiency controller is not implemented yet) and in Windows Efficiency mode.
- The cost with the core, DDC watcher, camera and event sources running.
- Memory after a flyout or Settings window has been opened and closed.

## Decision

No change to the shell choice. Re-measure after Phase 3 (flyout) and Phase 5 (presence), and against a hidden-window baseline for the flyout lifecycle (create on first click, destroy after a hidden timeout). If steady-state CPU stays above budget after the efficiency work, revisit the architecture risk row for a Win32 resident agent.
