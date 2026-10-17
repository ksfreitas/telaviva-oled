# Spike S2b: audio endpoint to monitor match

Run on 2026-10-17, read-only (PnP queries; nothing was changed). Machine: Windows 11 Pro build 26300, one external DELL S3225QC over HDMI, NVIDIA RTX 4080 SUPER and AMD Radeon iGPU, JBL Quantum 650 wireless headset.

## What was listed

| Device | Class | Status | Container id |
|--------|-------|--------|--------------|
| Monitor `DELL_S3225QC(HDMI)` (`DISPLAY\DELD1B8\...`) | Monitor | OK | `{2DC20509-22D8-54A7-8A80-B5F8EF3EC06E}` |
| Render endpoint `Dell 32 Plus QD-OLED (NVIDIA High Definition Audio)` | AudioEndpoint | Unknown (not the active output) | `{2DC20509-22D8-54A7-8A80-B5F8EF3EC06E}` |
| Render endpoint `Alto-falantes (JBL Quantum 650 Wireless Game)` | AudioEndpoint | OK (current default) | `{3C4D8B1D-E5CF-52D4-A940-2B7422F979F9}` |
| Render endpoint `Fone de ouvido do headset (JBL ... Chat)` | AudioEndpoint | OK | `{3C4D8B1D-E5CF-52D4-A940-2B7422F979F9}` |
| Render endpoint `Alto-falantes (iPhone ... Hands-Free)` | AudioEndpoint | Unknown | `{ACD10902-1B05-5A67-97B2-5D4E83AE851F}` |

## Findings

- **NVIDIA over HDMI: the match works.** The monitor's HDMI audio endpoint carries the same container id as the monitor node. Container-id equality (the only rule in the design, no name fallback) identifies this monitor as the owner whenever that endpoint is the default render device.
- The endpoint is not the default here (audio goes to the JBL headset), so on this machine the volume keys pass through, which is the expected K3 behavior.
- The AMD High Definition Audio device exists but exposes no endpoint for a connected display on this setup, so the AMD path is **not exercised**. DisplayPort, USB-C and dock paths are **not exercised** either (one monitor, HDMI only).
- The endpoint name embeds the monitor's EDID name ("Dell 32 Plus QD-OLED"), which differs from the PnP monitor name (`DELL_S3225QC`). This confirms that a name-based match would fail and container ids are the right key.

## Not yet verified

- Making the Dell endpoint the default and observing the key route flip to owner (needs the owner to change the default output; planned with T114).
- Behavior with a second monitor or other GPU vendors (hardware rows K1 to K4 stay "Not run").
