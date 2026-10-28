# reSA-Coop Diagnostic System — component scope

Version: **0.2.36-3p**. Terms: [Diagnostic System Attribution License 1.0](DIAGNOSTICS-LICENSE.txt).

The separate terms apply only to protected original diagnostic material in the
following files and to copies, modified versions and substantial portions of that
material. Listing a file does not create exclusive rights in functional APIs,
facts, native addresses, algorithms as such, or short mechanical helpers.
Hashes identify this release snapshot; they do not restrict permitted modifications.

| File | SHA-256 |
| --- | --- |
| `src/common/lib/diagnostics/FlightRecorder.cpp` | `e334f4278838442dd4538ae6362182155b2dcd7fb3408867bf1ee306a8f75ef0` |
| `src/common/lib/diagnostics/FlightRecorder.h` | `3086a12fdd3fb2010605000330dd887b5a656e6f0bcb925b3a3020d6e00f1af3` |
| `src/common/lib/diagnostics/PassengerTrace.h` | `840644ccefdd861863e2eea02953a52854245aa92e2602f59ebb45bd0a171434` |
| `src/client/app/crash-handler/FlightTail.h` | `96120994118eb6f45e84f2197f9e84d9672d3cde8be962edb675d910e87b8387` |
| `src/client/app/crash-handler/ExceptionAccess.h` | `083f2713961059da12581e094c8ab19c28744829f65ccd817d5822904f0ba834` |
| `src/client/shared/lib/memory/HookRegistry.cpp` | `296a8829da81293cfcf799bb37aed2cbea2258239d2cd2292dbf187540a7d6b0` |
| `src/client/shared/lib/memory/HookRegistry.h` | `93f61c366174290b6f5ac34a60a6e809c680a843896597cba4ca1baf5fb89100` |
| `src/client/shared/lib/memory/HookInspector.cpp` | `c1b2ea6069181285734d2000ce92d10fb597c3489559069a5c0b0b270f02fe9d` |
| `src/client/shared/lib/memory/HookInspector.h` | `ef72b15effd19dc835053a5614de7461e9f29a16ce130364fa275fd29662e186` |
| `src/client/widgets/overlay/DiagnosticModel.h` | `ad6db65ea1b9961b4f8c20f14ad8ec3aa3d2535f27fd850b39047e0e4cd9334e` |
| `src/client/widgets/overlay/DiagnosticNative.h` | `b1f9451a490ba8b1d1f28af3dddc83095335c452859c6096d7597c5d76c428ec` |
| `src/client/widgets/overlay/DiagnosticNative.cpp` | `8ade337944eab8173396f2f809370f3b9a5bbe88dc41bd15d69800adde3d046f` |
| `src/client/widgets/overlay/DiagnosticPanel.h` | `9a500e324f808be045c98ea3810b3f09b5f03067bccbfee0cacc409c5f0e88d1` |
| `src/client/widgets/overlay/DiagnosticPanel.cpp` | `2cf6f85a6981ab3fc70d14471061d6f6fc984125fe633391c6475770c9b02fec` |

## Existing licenses remain applicable

Existing MIT code, including SyncDiagnostics.cpp, SyncTrace.h, SyncAudit.h,
Memory.cpp/.h, Core.cpp, Patches.cpp, CrashHandler.cpp/.h, Log.cpp/.h,
Overlay.cpp/.h, ConnectMenu.cpp and network integration files, is outside this
separate scope. Calling the diagnostic component does not relicense those files.
Gameplay code and third-party components retain their existing terms. No earlier
MIT grant is withdrawn or restricted, including if an earlier MIT distribution
of material listed above is established.

SA:COOP is by Karimov-Akbar and contributors; reSA-Coop development is by opiahex.
ENet, MinHook and other dependencies retain their own notices and licenses.
