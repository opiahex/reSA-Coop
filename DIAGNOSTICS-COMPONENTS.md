# reSA-Coop Diagnostic System — component scope

Version: **0.2.39-3p** (the original fourteen files plus ten new original task-observation files). Terms: [Diagnostic System Attribution License 1.0](DIAGNOSTICS-LICENSE.txt).

The separate terms apply only to protected original diagnostic material in the
following files and to copies, modified versions and substantial portions of that
material. Listing a file does not create exclusive rights in functional APIs,
facts, native addresses, algorithms as such, or short mechanical helpers.
Hashes identify this source snapshot with LF-normalized line endings; they do not restrict permitted modifications.

| File | SHA-256 |
| --- | --- |
| `src/common/lib/diagnostics/FlightRecorder.cpp` | `6b15932dbd3b0d9d7920a0893d17509ebb2c14f9e150406eccdbc0b928bf7333` |
| `src/common/lib/diagnostics/FlightRecorder.h` | `c7ac2bbdb7736c5b275cdb9807b7d765fad37b01b7cd134430b442088adafe2e` |
| `src/common/lib/diagnostics/PassengerTrace.h` | `b0002261d86d595b135e5b4565509df4df081794d3d455ba927afc27b5793128` |
| `src/client/app/crash-handler/FlightTail.h` | `ac40c7dda692137e2865f72196d5cf45f11f2fd7dd859550403556952347071f` |
| `src/client/app/crash-handler/ExceptionAccess.h` | `32a19ccb2ba11b91d341e52841aafcc0e934edcc0976e15e21aaca94815880b9` |
| `src/client/shared/lib/memory/HookRegistry.cpp` | `15cba6f48bb4f4431631feec45253f3eb493b1d744d6515f5b265567c0fea6c1` |
| `src/client/shared/lib/memory/HookRegistry.h` | `d25d7ebbd75c8c2e670ffb90ecff5abd0e4d2527fe72c9c6ba9f9cc7e57d7a00` |
| `src/client/shared/lib/memory/HookInspector.cpp` | `bc60018651d06897afc25952340839546a5dab9abf3ae6b94f04955c964733a5` |
| `src/client/shared/lib/memory/HookInspector.h` | `67059845ff7c4eabf5c357b7a7609ee31ab3ec86363744c356bef3090f38763f` |
| `src/client/widgets/overlay/DiagnosticModel.h` | `fda6f4a04d5cb0a02f2fe7f3f3cc83d9dca8806c27fc5798720ae1e8ecee43f1` |
| `src/client/widgets/overlay/DiagnosticNative.h` | `669084a3c9fd263c789c57bb261b4879ccaa72c6c7a0a39453883df63a0a15a7` |
| `src/client/widgets/overlay/DiagnosticNative.cpp` | `2c6b181981ce26da3b6c6c1316553b2f4f9645baf5cf5a0e436f8c016185c922` |
| `src/client/widgets/overlay/DiagnosticPanel.h` | `fd0bbe5ae976c955c159e6ce47691f0428f146fe567a9d525f88296323eef047` |
| `src/client/widgets/overlay/DiagnosticPanel.cpp` | `c830f97fb08dd6a4ebd1e1c41b0a6c3ac06ff1df7183c338ddb62e93bbf09803` |
| `src/client/features/sync-diagnostics/TaskLifecycle.cpp` | `0f96be1e642b9b60160fbe8e5de62cc235f51d74efd1c548704d452a451c16af` |
| `src/client/features/sync-diagnostics/TaskLifecycle.h` | `77046801c30fa3a4d33b78c63adf71a1034609d07a783c0ad6fee437a1300e1f` |
| `src/client/features/sync-diagnostics/TaskLifecycleNative.cpp` | `3413e7fcefed14ed4a5393a40b157fd38cffe37d896037653fcf50bf5705ddfb` |
| `src/client/features/sync-diagnostics/TaskLifecycleNative.h` | `f939ac497e9f892e50d5cea02d0d3f5e473fc68e099a9ff59d53f29246f26bbc` |
| `src/client/features/sync-diagnostics/TaskTreeSnapshot.cpp` | `0ee570dbb7f564c601720f978bea1d1e6ecbc462ada0741bff80930e4050808c` |
| `src/client/features/sync-diagnostics/TaskTreeSnapshot.h` | `fd11e2c56892e816eecf681839c2c9d37913f998da833144f8844804550e06a3` |
| `src/client/features/sync-diagnostics/TaskTreeTrace.cpp` | `d63a41f5bbe592e78f695bd674e921aa3a08b7ee658a0fb6e4c816ff6d3517db` |
| `src/client/features/sync-diagnostics/TaskTreeTrace.h` | `5a60b58d31bf2c9889916d6e5ac499df706c6ad0dc6878a60459ef1df02ea290` |
| `src/client/widgets/overlay/DiagnosticTaskBridge.cpp` | `43a5d9d11f43875374556828a5beff0f17f902269d8a988238fcffb1a18f5c8d` |
| `src/client/widgets/overlay/DiagnosticTaskBridge.h` | `623d8e6a19c42303969264858dd9c4a61fcf5ef70122a1a5399e332c9943921a` |

## Existing licenses remain applicable

Existing MIT code, including SyncDiagnostics.cpp, SyncTrace.h, SyncAudit.h,
Memory.cpp/.h, Core.cpp, Patches.cpp, CrashHandler.cpp/.h, Log.cpp/.h,
Overlay.cpp/.h, ConnectMenu.cpp and network integration files, is outside this
separate scope. Calling the diagnostic component does not relicense those files.
Gameplay code, functional shared API declarations and third-party components retain their existing terms. No earlier
MIT grant is withdrawn or restricted, including if an earlier MIT distribution
of material listed above is established.

SA:COOP is by Karimov-Akbar and contributors; reSA-Coop development is by opiahex.
ENet, MinHook and other dependencies retain their own notices and licenses.
