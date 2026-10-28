# reSA-Coop Diagnostic System — component scope

Version: **0.2.37-3p** (updated snapshot of the same fourteen files covered in 0.2.36-3p). Terms: [Diagnostic System Attribution License 1.0](DIAGNOSTICS-LICENSE.txt).

The separate terms apply only to protected original diagnostic material in the
following files and to copies, modified versions and substantial portions of that
material. Listing a file does not create exclusive rights in functional APIs,
facts, native addresses, algorithms as such, or short mechanical helpers.
Hashes identify this release snapshot; they do not restrict permitted modifications.

| File | SHA-256 |
| --- | --- |
| `src/common/lib/diagnostics/FlightRecorder.cpp` | `e3c1aad9b358b4b263d217f4104ba2b611d382793ac8761c1c1a24881f0e65dc` |
| `src/common/lib/diagnostics/FlightRecorder.h` | `b1726e55258df5257eed477abe70717bd84c833cc2dccc08282696cdc3c4443e` |
| `src/common/lib/diagnostics/PassengerTrace.h` | `840644ccefdd861863e2eea02953a52854245aa92e2602f59ebb45bd0a171434` |
| `src/client/app/crash-handler/FlightTail.h` | `fb5fec1dda0f5f7e1c14f14f505ddfbea63ab855bd22f13b3934520149dc7a45` |
| `src/client/app/crash-handler/ExceptionAccess.h` | `083f2713961059da12581e094c8ab19c28744829f65ccd817d5822904f0ba834` |
| `src/client/shared/lib/memory/HookRegistry.cpp` | `296a8829da81293cfcf799bb37aed2cbea2258239d2cd2292dbf187540a7d6b0` |
| `src/client/shared/lib/memory/HookRegistry.h` | `93f61c366174290b6f5ac34a60a6e809c680a843896597cba4ca1baf5fb89100` |
| `src/client/shared/lib/memory/HookInspector.cpp` | `c1b2ea6069181285734d2000ce92d10fb597c3489559069a5c0b0b270f02fe9d` |
| `src/client/shared/lib/memory/HookInspector.h` | `ef72b15effd19dc835053a5614de7461e9f29a16ce130364fa275fd29662e186` |
| `src/client/widgets/overlay/DiagnosticModel.h` | `afc80977a03dab40da637ad09a9d5ebb38b6b9d3136247dcbc81569046e8d26a` |
| `src/client/widgets/overlay/DiagnosticNative.h` | `2af8b44edc970abac83073d6bd4e6cbda9a756a723cc92bb09f8cf4daa2b996d` |
| `src/client/widgets/overlay/DiagnosticNative.cpp` | `68c3ee41e98dd1e7f6525497a2f12a936671c013f917be01b120c99d2be10b53` |
| `src/client/widgets/overlay/DiagnosticPanel.h` | `1dc8eccafee1ea5c7b420e48be8f2cf6044499435f1d89fe2f445ee5ca8ffff8` |
| `src/client/widgets/overlay/DiagnosticPanel.cpp` | `0ff33a6c649ba9f91cd7dae020afaac5e7a5dd5ca6421dfa67c9164e1f2598b3` |

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
