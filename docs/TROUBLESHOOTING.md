# Troubleshooting

| Symptom | Likely cause | What to do |
|---|---|---|
| XSOverlay exits ~10 s after launch, Steam console shows `NullReferenceException` in `XSOApplicationContext.ShowMessageBox` | The Process Manager launcher crashed (it always does under Wine) | Use the wrapper (`XSO_DIRECT` default) or Steam's "Old Launch Method" entry |
| Log says `Interface Not Found (105)` / `Connection to SteamVR failed!` repeatedly, no overlays | SteamVR was not running when XSOverlay started (Proton Experimental does not auto-start it) | Start SteamVR first, then XSOverlay |
| Overlays transparent / black, `/dev/shm/xso-uwc.log` says `bridge file ... not available` | Helper not running or different `XSO_BRIDGE_SHM` | Check `xso-bridge.log`; run `./build/xso-bridge -v` manually; the wrapper only starts it if `pgrep -x xso-bridge` finds none |
| `uwc` log: `bridge file has unexpected size` | Helper and DLL built from different `xso_shm.h` | `./build.sh` and re-run the wrapper (it reinstalls the DLL) |
| `uwc` log: `helper stopped responding` | Helper crashed/was killed | Look at `xso-bridge.log`; restart it; file an issue with the log |
| Window list empty in the VR UI | `WindowAdded` messages not consumed, or windows filtered (`isAltTabWindow`, `isChild`, `isBackground`), or no icon | `XSO_UWC_DEBUG=1`; check `windows=` in stats and that icons are non-zero in `./build/shmtest` output |
| XSOverlay freezes after the UI appears, later "Fatal error in GC" | No desktop entry in XSOverlay's window list (see INTERNALS "Icons are mandatory") | Make sure the helper build includes synthesized icons; `XSO_UWC_TRACE=1` dumps the last calls |
| No UI at all (toolbar/settings never show) when launched by hand | Vuplex Chromium GPU process fails outside Steam's runtime container | Launch through Steam, or via `_v2-entry-point` as in docs/TESTING.md |
| Capture is slow (few fps) | `XShmGetImage` readback contends with other GPU work | Capture smaller windows, lower XSOverlay's per-overlay FPS, close GPU-heavy apps; see README limitations |
| Clicks/keys from VR don't reach the desktop | Hooks disabled (`XSO_INPUT=0`), helper without XTest, or Wayland | `uwc` stats `input=` should grow; `xso-bridge.log` reports `xtest=1` at start |
| Keys produce wrong characters | VK→keysym table / keyboard layout mismatch | Extend `vk_to_keysym()` in `src/helper/xso-bridge.c` |
| Layout saving fails | `Process.GetProcessById` for a window pid | Already handled (own PID is returned); report if it recurs |
| Steam "verify integrity" restored the original DLL | Expected | The wrapper reinstalls `build/uWindowCapture.dll` on the next launch |

Useful one-liners:

```bash
strings /dev/shm/xso-uwc.log | grep stats | tail -3          # plugin statistics
tail -f xso-bridge.log                                        # helper
ls -t ~/.local/share/Steam/steamapps/compatdata/1173510/pfx/drive_c/users/steamuser/AppData/LocalLow/Xiexe/XSOverlay/output_logs | head -1
grep -c "GPU process launch failed" ~/.local/share/Steam/steamapps/common/XSOverlay_Beta/XSOverlay_Data/Plugins/x86_64/VuplexWebViewChromium/log-chromium.txt
```
