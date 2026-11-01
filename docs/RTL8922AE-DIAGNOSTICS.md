# RTL8922AE diag1

Pinned base: v0.5.0-BETA, ec3c47f81282d0c7bff5125491855adf5a1efcbc.
Target: PCI 10EC:8922, AMD Ryzen 7 9800X3D, macOS 26.7.2 (25G332).

This is an instrumented upstream driver, not a proven hardware fix.
The diagnostic package matches only 10EC:8922 and uses the existing Ethernet
backend without connecting to AirPortRTW89Front. It does not need legacy
IOSkywalkFamily/IO80211FamilyLegacy. It will not add a native Wi-Fi menu.
The new diagnostic stage appears in IORegistry and the driver's log ring.
Stage properties disappear if driver start fails and the service is destroyed;
log-ring data is volatile and cannot survive a panic or reboot.

Build, in order:

    make fetch-firmware
    make kext
    python3 tools/package_diagnostic.py

Firmware hashes must all pass. `make kext` gates packaging on upstream hosttests
and kernel import checks. Hosttests simulate hardware and do not execute the
IOKit start method or prove real RTL8922AE firmware/radio operation.

First hardware experiment: manually load build/diagnostic/AirPort_RTW89.kext
from a known working boot with the original native macOS network stack.
Do not use tools/load.sh unchanged: it targets build/out, not the diagnostic
package. Do not load concurrently with any other driver claiming this PCI card.
Do not install this experimental package into the boot EFI yet.
Manual loading needs root ownership, permitted kernel-extension policy and may
be rejected by Tahoe. A rejection is not evidence of a hardware driver defect.

The manual loader is `sudo tools/load_diagnostic.sh`. It stages a root-owned
copy under /private/var/tmp and rejects detected conflicting Wi-Fi drivers or
the legacy Wi-Fi stack. It does not install into EFI. Save work first.

The previously pending legacy-stack EFI experiment was reverted byte-for-byte
to the verified working config before preparing this diagnostic.

After a successful manual load:

    build/diagnostic/rtw89ctl status
    sudo build/diagnostic/rtw89ctl log
    ioreg -r -c AirPort_RTW89 -l -w 0 -d 1

Save these before requesting radio-up or scanning. Stop at the first failure.
Never publish full logs without checking network names and hardware addresses.
No connection credentials are embedded in this package.

Known prior local results: full legacy-stack + driver boot failed; another
AirportRTW build previously stalled ControlCenter. Neither isolates the cause.
Existing Bluetooth patches are out of scope for this diagnostic build.

## First hardware result (2026-11-01, Asia/Taipei)

After approving the extension and rebooting, manual loading succeeded.
Observed milestones: PCI device opened, BAR2 mapped (1 MiB), MSI index 1,
firmware rtw8922a_fw-4.bin loaded, probe complete, Ethernet backend registered.
Chip: RTL8922AE cut C, RFE 1, 2T2R. Firmware: 0.35.80.3 (8ef4f0cf).
Interface: en2. Diagnostic stage: start-complete.

Scan passed: 7 BSS entries across 39 channels in 3584 ms, including 2.4 and
5 GHz. No association, DHCP, Internet traffic, sleep/wake or sustained transfer
has been verified yet. Bluetooth remains Off with NULL address after this scan.
This does not establish the cause of the earlier native-stack boot failure.

The original load script queried status before asynchronous start completed,
so it printed "not loaded" and exited 1 although initialization later succeeded.
The loader now polls registration for up to 60 seconds before collecting status.
Local evidence: build/hardware-test/scan.log (contains nearby SSIDs/BSSIDs;
not committed). The diagnostic binary did not change during this test.

### Association and Internet traffic

The user selected the visible WPA2 network and entered its password directly
in Terminal. Association and the WPA2 key handshake completed. DHCP assigned
an IPv4 address to en2. Link observed: 2432 MHz, 802.11ax, 40 MHz, two streams.
The reported 458.8 Mb/s TX PHY rate is not an Internet throughput measurement.
An HTTPS request explicitly bound to en2 returned HTTP 200 over IPv6, with
577 bytes received. A separate IPv4-bound request is recorded in
build/hardware-test/ipv4-https.json. Driver TX/RX counters increased.
No throughput, 5 GHz association, sustained stability, resume, or native AirPort
integration has yet been established. The driver remains temporarily loaded;
no automatic boot installation was performed.

### Continued hardware tests (2026-11-01)

All requests below were explicitly bound to en2. Kept the current 2.4 GHz
802.11ax 40 MHz connection; no kext replacement or EFI changes were made.

- Two 10,000,000-byte HTTPS downloads, IPv4 then IPv6: HTTP 200, exit 0.
  curl transfer rates were 53.86 and 47.37 Mbit/s, respectively.
- A 2,000,000-byte synthetic-zero upload: HTTP 200, exit 0, 44.71 Mbit/s.
  No user files were uploaded. These short WAN samples do not measure maximum
  Wi-Fi throughput, and depend on the remote endpoint and Internet path.
- Scan while connected: passed; HTTPS immediately afterward passed.
- AP liveness probe: passed; HTTPS afterward passed.
- Explicit simulated loss: automatic rejoin succeeded after one attempt,
  approximately 4.82 seconds between loss and reconnection in the driver log.
  HTTPS afterward passed.
- Six more HTTPS requests spaced five seconds apart: all HTTP 200.
- Suite exit code 0; link remained connected afterward.

The final counters include 4 TX drops, 166 RX drops (81 duplicates), and 9478
frames forwarded without a PPDU status report while flush mode was on.
These counters are not equivalent to measured IP packet loss. They should be
tracked in longer tests; this run establishes short functional success only.

The backend join path in src/compat_rtw89/rtw89_glue.c selects the strongest
matching SSID by RSSI. The current rtw89ctl join command has no BSSID/band
selector, so 5 GHz association has not been tested. Native AirPort integration,
Bluetooth repair, real sleep/wake, automatic boot loading and sustained
stability remain unverified.

Evidence (private, uncommitted): transfer-continued.json, upload-test.json,
connection-suite.log, connection-suite-exit.txt, after-connection-suite.txt,
all under build/hardware-test/.

## diag2: explicit BSSID selection

Adds `rtw89ctl join SSID [BSSID]`. The CLI validates a six-octet, nonzero
unicast address and passes six bytes in RTW89BSSID. The kernel independently
checks property type, length and address validity. Password entry remains
hidden in Terminal. It derives the PMK using the existing implementation and
calls the existing rtw89_glue_join_pmk BSSID path. The derived key is cleared
after use. Omitting BSSID preserves the original join path.

The BSSID constrains the initial join. Existing automatic rejoin remembers
SSID/PMK, not BSSID, so a reconnect may choose 2.4 GHz again; inspect frequency
before interpreting measurements. No Linux vendor driver sources changed.
The parser tests run with AddressSanitizer/UBSan, including truncated,
malformed, zero, multicast, uppercase and locally administered addresses.
The full upstream hosttest suite also gates packaging.

A working diag1 copy is retained locally at build/known-good-diag1.

### diag2 validation and loading result

Full upstream hosttests, sanitizer parser tests, kext link/KPI checks and
ad-hoc signature verification passed. The first hot replacement loaded the
cached diag1 UUID despite supplying diag2; the BSSID property was consequently
ignored and association remained 2.4 GHz. This is not a valid diag2/5 GHz test.
A hot reprobe also reported zero efuse MAC and used a random address; cold-boot
reinitialization and repeated-load behavior need investigation before relying
on hot replacement.

The diagnostic package now has CFBundleVersion 0.5.1. The loader checks the
running RTW89 Diagnostic Build property and rejects cached diag1 before any
scan/join. With the new version macOS explicitly requests extension approval.
No diag2 hardware result exists yet. The old driver was unloaded; original
boot EFI is still intact. After approval/reboot, Run-Diag2.command also handles
the case where no diagnostic driver is loaded.

### diag2 / 5 GHz hardware result (2026-11-01, after approval/reboot)

Verified actual loaded binary: com.rtw89.driver 0.5.1,
UUID B6B209B7-384C-3ECF-BE32-9ABC873A6575, runtime marker rtl8922ae-diag2.
Original factory MAC returned after reboot, unlike the earlier hot reprobe.
Explicit BSSID join succeeded on channel 40, 5200 MHz, 802.11ax, 80 MHz,
two streams. WPA2 handshake and DHCP completed on en2.

Comparable short transfer tests, explicitly bound to en2:

| Test | Previous 2.4 GHz diag1 | 5 GHz diag2 |
| --- | --- | --- |
| IPv4 HTTPS download, 10 MB | 53.86 Mbit/s | 116.16 Mbit/s |
| IPv6 HTTPS download, 10 MB | 47.37 Mbit/s | 129.24 Mbit/s |
| IPv4 synthetic upload, 2 MB | 44.71 Mbit/s | 84.39 Mbit/s |

All returned HTTP 200, curl exit 0. Frequency was verified as 5200 MHz before
and after. No automatic rejoin occurred during the transfers. Final counters:
0 TX drops, 87 RX drops (1 undecrypted), no replay/duplicate count; 2282 frames
forwarded without PPDU status report. Counters do not establish IP packet-loss
rate. PHY rates (up to 960.7 Mbit/s) are distinct from measured WAN throughput.
These sequential short WAN tests depend on the Internet path and endpoint;
they do not establish peak LAN throughput or long-term stability.

Evidence: build/hardware-test/diag2-{load,join,driver}.log,
5ghz-transfers.json and 5ghz-{before,after}.txt, kept private and uncommitted.
Still pending: persistent band selection across automatic rejoin, repeated
hot-reload MAC/efuse behavior, native Wi-Fi UI, sleep/wake, boot installation,
long duration traffic and Bluetooth. The current driver remains a temporary
manual load; original EFI boot config was not changed for this experiment.

## Native UI, sleep/wake and Bluetooth follow-up

Native front compiles with additional IORegistry stage markers around
IO80211Controller::start and interface attachment. Symbol checking on the
current native stack cannot validate legacy IO80211FamilyLegacy linkage;
this remains an actual dependency-boot test, not a passed KPI check.
Prepared an isolated stack-only OpenCore config and complete EFI backup under
build/native-experiment. USB recovery folder: Native-WiFi-Isolation-20261101.

Sleep defect found by inspection: sleepRadio used explicit glue_down, which
forgets the proven SSID/PMK. Added separate suspend/resume APIs that preserve
that in-memory state and schedule the existing rejoin worker on resume.
Explicit radio-down still forgets it. Native mode, where automatic backend
rejoin is disabled, leaves reconnection to macOS. Added host assertions for
both remembering on suspend and forgetting on explicit down. Hardware sleep
and native UI are still unverified.

Bluetooth: user confirms power/scanning work, pairing/connection do not.
CORSAIR HARPOON was found as a connectable BLE HID peripheral. Fresh local HCI
trace with working Wi-Fi reproduces the same failure: LE Create Connection
0x200d accepted and LE Connection Complete received; LE Read Remote Features
0x2016 accepted (Command Status 0), but no LE subevent 0x04; LE Connection
Update 0x2013 accepted, but no subevent 0x03. Local disconnect follows about
30 seconds later (HCI reason 0x16). LE event mask 0x1f enables both completion
events, so this capture does not support blaming a disabled event mask.
The current BlueToolFixup already synthesizes the remote-version callback;
that callback is not evidence of an actual controller completion.
No additional speculative bluetoothd bypass was installed. Next investigation
needs a functioning Linux HCI reference or firmware/USB transport analysis.
Raw capture contains device identifiers and remains private under
build/hardware-test/bt-hci-20261101-201008/.

The diag3 build passed the full upstream hosttest suite, including the new
suspend/reconnect and explicit-down assertions, 0 cleanup DMA allocations,
and the kernel dependency checker. Backend package version 0.5.2, marker
rtl8922ae-diag3. Native package is separately signed and contains the front
with stage markers. Hardware sleep/wake and native frontend remain pending.

Activated stage 1 (dependencies only) on the 1401 USB EFI, after verifying
the original config had not changed and the complete EFI backup matched
byte hashes. Only IOSkywalkFamily/IO80211FamilyLegacy injection, Skywalk Block
and -amfipassbeta were added. No Realtek backend/frontend is boot-enabled.
The next required step is a reboot from that USB and confirmation of desktop
startup; stage 1 does not provide a Wi-Fi menu. Hardware sleep/wake on diag3
and frontend activation must follow successful dependency boot verification.
If boot fails, restore with USB Native-WiFi-Isolation-20261101/Restore-Original.ps1
from Windows, or copy its config.original.plist over USB EFI/OC/config.plist.

## Dependency-only boot failure and continued isolation (2026-11-01)

User reported automatic restart before desktop with both legacy families.
Saved failed 22:08 and restored 22:12 OpenCore logs under
build/hardware-test/native-stack-boot-failure-20261101. Failed boot injected
IOSkywalkFamily and IO80211FamilyLegacy successfully, did not inject RTW89,
and ended at EXITBS:START. The successful restored boot also ends its OpenCore
log there, so that last line alone does not locate the fault. No panic report
was found. Current restored config matched original byte-for-byte before
preparing the next experiment.

Both dependency binaries match the exact upstream-recommended OCLP archives.
Fixed check_kpi.py parsing of kmutil's multi-exporter format; the old parser
incorrectly called kdp_set_interface and kdp_unregister_send_receive missing.
Regression tests pass; Skywalk's 911 imports all resolve against current
libraries. Legacy family's two unmatched Skywalk methods must be evaluated
against the replacement Skywalk, not the current native one. Symbol resolution
does not prove ABI/runtime compatibility.

Continued test 1A is active: failed dependency config with IO80211FamilyLegacy
removed. Only IOSkywalk replacement/Exclude and the prior -amfipassbeta delta
remain. No RTW89 backend/frontend is boot-enabled. If this boots, add the legacy
family separately; if it restarts, investigate the Skywalk replacement path
before adding IO80211. The same original-config Windows recovery script remains
available. This is a narrower diagnostic step, not abandonment of native UI.

## Skywalk-only failed; current-stack frontend investigation

The user reported another automatic restart. Saved the 22:20 failed log and
22:24/22:25 recovery logs under build/hardware-test/skywalk-only-boot-failure-20261101.
The failed log confirms IOSkywalkFamily injection with no IO80211FamilyLegacy
or RTW89 boot load. Original config was restored byte-for-byte. No new panic
report was found. No failing dependency configuration remains active.
This isolates the tested failure to the Skywalk replacement/Exclude path plus
its boot-argument delta, but does not establish the precise faulting routine.

AirportRTW 2.2.1 previously created a native interface on this machine using
current IO80211Family/IOSkywalkFamily. Downloaded its current source as a
read-only reference, pinned at 82c78d0b1c300b25a6706c433170c1c635293670.
Tahoe headers and licenses are retained under reference/tahoe-headers.
Prepared a metadata-only RTW89NativeProbe: it matches IOResources, reads the
OSMetaClass sizes of IO80211Controller, IO80211SkywalkInterface and
IO80211InfraProtocol, and compares them with the reference headers.
It does not instantiate Apple network classes or create network interfaces,
open PCI, or call network methods. Matching sizes alone cannot prove vtable
or complete ABI correctness. The module compiles and all 296 imports resolve
against the present kernel libraries. Temporary hardware load is next.

## Native metadata probe passed on hardware

After approval and reboot, RTW89NativeProbe loaded successfully (exit 0).
Runtime sizes matched the Tahoe reference headers: IO80211Controller 296 bytes,
IO80211SkywalkInterface 296 bytes, IO80211InfraProtocol 304 bytes. Evidence:
build/hardware-test/native-probe-success.txt. This validates class sizes only;
it does not validate virtual-call offsets, protocol structures, or a Wi-Fi UI.

Prepared RTW89ModernController as the next controlled bring-up step. It matches
10EC:8922 and uses the current system IO80211Family. It creates a native work
queue and the CoreCapture logger/fault-reporting resources required by the
modern controller, then invokes the native superclass start. It checks native
class sizes again before starting. It does not directly access PCI registers,
open the card, attach a network interface, or connect the RTW89 hardware backend.
Unsupported operations return errors. Startup stage properties are written to
the provider as well as the controller to survive a start failure. Resource
cleanup releases reporters, streams and pipes in reverse order, and ends with
superclass free. This module compiles and all 485 imports resolve against the
current libraries. Actual start validation is still pending.

The modern-controller load attempt returned exit 27: macOS requires extension
approval for com.rtw89.moderncontroller. It has not executed, so no controller
start success is claimed. Opened Privacy & Security for user approval. EFI
remains the original bootable config; this is a temporary manual-load test.

## Modern controller 0.1.0 load crash

User reported a crash/reboot during the approved manual load. The load log was
empty and no panic file/NVRAM panic data was found. Unified logs retain the
kernelmanager load request but no RTW89ModernController startup checkpoints.
It is not yet known whether the fault occurred during module initialization,
CoreCapture setup, superclass start, or failure cleanup. The original EFI was
again verified unchanged; the controller is not loaded after reboot.

Offline inspection mapped the current Boot KC's IO80211Controller vtable against
the compiled object: 462 sampled slots, no mismatches among inherited,
non-reserved symbols; the overridden controller slots occupy matching positions.
This narrows one ABI hypothesis but does not validate CoreCapture behavior,
other class vtables, private object layouts or the whole call path. Saved the
comparison as build/hardware-test/native-vtable-comparison.json.

Blocked reloading 0.1.0 in the loader. Built diagnostic 0.1.1 with 2.5-second
checkpoint pauses before each startup operation. Added a userspace monitor
that polls the PCI provider's stage property and fsyncs changes to a private
per-run JSONL journal, plus pre-load/return markers. It records until the ready
stage or timeout. Owner permissions preserve user access even after a crash.
A mock-command self-test verified stage capture and durable file output without
loading any kernel code. The new module's build, 486-import check and signature
verification pass. This is improved fault localization, not a crash fix.
No new controller load was performed after this crash during investigation.

## Modern controller 0.1.1 freeze localized (2026-11-01)

The user clarified that the screen froze and they forced a restart; this was
not an observed automatic panic reboot. The fsynced journal at
build/hardware-test/controller-journal-20261101-230541/stages.jsonl records
kmutil returning 0, layout-check, workqueue-create, corecapture-create and
native-super-start-enter. It contains no ready checkpoint. This places the
stall after entering the native superclass start path, including a possible
failure/cleanup path; it does not identify an exact faulting instruction.
No new panic report was found. EFI config still matches the original exactly.

Offline disassembly of this machine's current Boot KC retained the superclass
startup and monitor/logging paths under build/hardware-test. These call the
controller's logger, work queue and fault reporter getters. Found a separate
contract mismatch: getLogPipes previously rejected partially requested outputs,
whereas the pinned reference accepts each non-null output independently.
Corrected that behavior, explicitly identify this as a kernel rather than
DriverKit driver, and validate the allocated fault reporter's dynamic class.
None of these changes is yet proven to fix the freeze.

Prepared 0.1.2 with atomic callback counters in preallocated, provider-retained
OSData. Getter instrumentation takes no registry lock, allocates nothing and
never sleeps. The user-space journal snapshots these counters and now records
a checkpoint immediately after superclass start returns, including false, to
separate an internal stall from subsequent cleanup. Before loading, an independent
spindump process starts a 40-second user/kernel stack capture (100 ms interval),
with a 65-second time limit. An early sampler exit cancels the controller load.
If the screen freezes, wait at least 90 seconds before forced restart to allow
capture/save. Stack capture may still fail if the system stops scheduling.

The new controller compiles; all 490 imports resolve and the ad-hoc signature
verifies. Mocked successful and approval-required load paths verified sampler
ordering, callback decoding, durable journal writes and exit status without
loading any kernel code. The loader rejects previous versions. Hardware load
of 0.1.2 remains pending; native Wi-Fi UI, actual sleep/wake and Bluetooth pairing
are not claimed fixed by this change.

0.1.2 approval checkpoint: controller-journal-20261101-231844 shows kmutil
returning 27 before execution. The independent spindump completed successfully
(exit 0), saving a 14 MB baseline report. The user approved the extension and
restarted. After reboot no modern controller is loaded, the package signature
verifies as 0.1.2, and the active EFI config is byte-identical to the original.
Actual approved controller execution remains to be tested.

## Modern controller 0.1.2 freeze; shared provider correction

The user waited 90 seconds before forced restart. Journal
controller-journal-20261101-232232 records successful kmutil load and progress
through native-super-start-enter. The last pre-call snapshot had three
getWorkLoop calls from CoreCapture setup, with no captured logger/workqueue/
fault getter calls yet. Because polling stopped before the call progressed,
those counts do NOT prove which callbacks did or did not execute afterwards.
The sampler progress log is empty and no stack report was saved, unlike its
successful prior baseline. Waiting did not produce a usable runtime stack.
No new panic file was found; the original EFI config remains identical.
The exit 27 file still present at the top-level was stale from the approval
attempt, not this run; the journal's kmutil result is the reliable evidence.
The local Terminal wrapper now writes a pending marker before each attempt.

Found a concrete provider mismatch by following this host's current Boot KC:
- IO80211Controller::start (0xffffff8002297224) dispatches superclass start to
  IONetworkController::start (0xffffff80026f84dc).
- That implementation compares this->getWorkLoop() and provider->getWorkLoop().
  When different, it calls this workloop's getThread() and passes the result
  into ml_thread_policy at 0xffffff80004cd960 (call sequence 0xffffff80026f868e
  through 0xffffff80026f86ab).
- This host's IO80211WorkQueue::getThread at 0xffffff80022f304a returns zero.
  ml_thread_policy passes that unchanged to the internal policy routine at
  0xffffff80003c1700, which computes/acquires a lock from thread + 0x658 without
  a preceding null check. This establishes a dangerous reachable null-thread
  path in the prior direct-PCI design, not a captured runtime fault instruction.
- The pinned reference explicitly supplies IOPCIEDeviceWrapper as provider and
  returns the same native work queue from wrapper and controller. Our minimal
  implementation had omitted that wrapper and matched the controller to PCI.
Saved disassemblies under build/hardware-test/network-super-start.txt,
native-workqueue.txt, ml-thread-policy.txt and thread-policy-internal.txt.

0.1.3 introduces RTW89NativeProvider (IOService), which owns one native queue
and matches only PCI 10EC:8922. RTW89ModernController now matches only this
provider, retains its queue, and refuses to proceed unless provider and child
return the same workloop. No direct PCI/radio access or network-interface
attachment is added. Runtime queue-sharing and dedicated-thread booleans plus
provider version are mirrored to the physical provider and fsynced by the
journal. This restores the reference's queue topology without its HAL/hardware
initialization. Previous controller versions are rejected by the loader.

Build and 507-import KPI checks pass; signature and personality graph checks
pass. Mock journal cases cover successful load, approval failure, early sampler
failure, callback decoding and the true/false queue-routing metadata. Real
0.1.3 loading remains pending. The null-thread path is the current strongest
static explanation for the freeze; hardware validation is needed before
claiming the freeze fixed or native Wi-Fi functional.

## 0.1.3 native controller startup passed on RTL8922AE

After approval and reboot, the actual 23:42 manual load succeeded. Journal
controller-journal-20261101-234224 records provider version 0.1.3,
shared-provider-workloop true and native-queue-has-thread false. The native
superclass start returned true at 23:42:37, followed by the registered
native-controller-ready-no-interface checkpoint. I/O Registry confirms the
controller is registered, matched, active and no longer busy. Its callbacks
were exercised: createWorkLoop 1, createWorkQueue 2, getWorkLoop 6,
getWorkQueue 6, getLogger 3 and getFaultReporter 1. These are observed values
from this run, not coverage of every controller operation.

The loader completed with exit 0. The independent 40-second kernel/user stack
capture also completed with exit 0, saving kernel-stacks.spindump. Saved the
controller registry and focused summaries in the run folder. Subsequent
read-only checks found the same ready controller and a responsive command
session. This is short-term startup validation, not a long-term stability,
sleep/wake or unload test. Source commit bab2716 and the signed 0.1.3 package
are preserved under build/known-good-modern-controller-0.1.3 with SHA256 hashes.

The provider/queue correction therefore passes the previously failing native
startup stage on this machine. No network interface or RTW89 backend was
attached by this test, so native menu, network scanning/joining and Bluetooth
pairing remain outstanding. Next native stage is a minimally initialized
IO80211InfraProtocol/Skywalk interface with explicit unsupported operations,
followed by controlled interface registration before connecting the backend.

## Next stage: unpublished native interface initialization

Prepared a separate com.rtw89.nativeinterface 0.1.0 module so the successful
0.1.3 controller source and binary remain unchanged. Its IOService probe
matches only RTW89ModernController, verifies the shared provider/queue at
runtime, then allocates RTW89UnpublishedInterface (IO80211InfraProtocol).
It performs IO80211InfraInterface::init(),
IO80211SkywalkInterface::init(controller, address), role/id setup and an
IOService-level attach. Each step has a 2.5-second checkpoint mirrored to the
physical card. It deliberately does not invoke native attachInterface,
interface start, BSD publication or packet queues. A diagnostic locally
administered MAC exists only in the unpublished object. No radio/key/backend
operation is performed; 199 protocol operations and the abstract MAC setter
return unsupported. Enable/disable return not-ready.

The new module compiles and all 519 imports resolve. Signature verification
passes. Import inspection found no PCI or RTW89 backend symbols. The inherited
Skywalk vtable does import registerNetworkInterfaceWithLogicalLink; an inherited
slot is not an executed call. Source inspection found no explicit native
attachInterface, network registration, interface start or interface
registerService calls. Its personality only matches the existing controller. Added check_native_vtable.py to compare compiled slots against
this host's current Boot KC. Interface results: 457 inherited matches, 11
identifiable overrides and 200 abstract overrides whose identities cannot be
inferred from a pure parent slot. No identifiable mismatches. Probe results:
260 inherited matches and 6 overrides. Rechecking the successful controller
also passes. Intentionally swapping two inherited slots in a temporary object
produced exactly two mismatches, validating the negative check. This validates
slot placement only, not complete private API compatibility or runtime safety.

The interface loader reuses an already running controller, or verifies all
saved 0.1.3 hashes and loads that controller first after reboot. It never
unloads/replaces the running controller. The durable journal now supports
separate controller/interface stage keys and output folders; six mocked cases
(success, approval error, early sampler exit in both modes) pass. The interface
mode cannot mistake the controller-ready marker for interface completion.
The local Run-Native-Interface.command prompts for sudo in Terminal and records
a pending marker before execution. Root-owned staging stays temporary; EFI
and /Library/Extensions are unchanged. Real hardware interface initialization
is pending, so no new native menu/scan/connect success is claimed.

The first interface-probe load attempt (interface-journal-20261101-235526)
verified the preserved controller package hashes, then kmutil returned 27:
com.rtw89.nativeinterface needs user approval. No interface probe execution
has occurred. Opened Privacy & Security for approval; actual initialization
remains pending after the requested approval/reboot.

## Native interface initialization/IOService attachment passed (2026-11-02)

After interface-extension approval and reboot, Run-Native-Interface first
loaded the unchanged, hash-verified controller 0.1.3. Its native start again
returned true; journal controller-journal-20261102-000133 retains this second
successful controller run and stack capture.

Then interface-journal-20261102-000217 records actual interface probe 0.1.0
loading with kmutil exit 0. Every checkpoint completed: layout check,
allocation, IO80211InfraInterface initialization, Skywalk initialization,
role/id setup, and IOService attach. It reached
native-interface-initialized-attached-not-published at 00:02:34.
The 40-second stack capture completed successfully, saving a nonempty report,
and the complete Terminal wrapper returned 0.

Registry snapshots confirm the probe is registered/matched/active and busy 0.
RTW89UnpublishedInterface is attached and active, busy 0, but !registered and
!matched, with its diagnostic-unpublished property and no BSD Name. This is
the intended boundary: no native logical attachInterface, interface start,
BSD publication, packet queues, scanning, association or backend yet.
Read-only checks after completion found the objects in the expected state
and the system responsive. This does not establish sleep/wake, unload or
long-term stability.

Preserved the signed probe and SHA256 manifest under
build/known-good-native-interface-0.1.0. Registry snapshots, focused success
summary, stack report and Terminal result are in the run folder. EFI remains
byte-identical to the original. The next native stage is logical controller
attachment/registration information and controlled BSD/network publication,
followed by the native API's required state queries before connecting rtw89.
Bluetooth pairing and actual radio sleep/wake remain separate outstanding work.

## Prepared integrated native registration diagnostic 0.2.0 (2026-11-02)

The next step needs both native logical attachment and a BSD bridge. Prepared
a separate com.rtw89.nativeregistration module, keeping the verified controller
0.1.3 and interface probe 0.1.0 source/binaries unchanged. Its renamed base is
copied from the verified controller, with a version change, deferred controller
publication and a prior-test guard. If the physical provider already carries
a test-controller version, the new provider refuses to start until reboot;
there is no hot replacement/unload of the previous tests. The module uses its
own class names and match category, and is manually staged only.

RTW89RegistrationController initializes the shared native work queue and logs,
then an Infra/Skywalk interface, attaches it as an IOService, performs native
IO80211Controller::attachInterface and creates an unregistered BSD adapter.
It prepares both 0x130-byte native registration records only if expansion
pointers are present and no existing records would be overwritten, defers
native BSD attach, starts the native interface, and requests BSD matching.
A gated BSD prepare then publishes the native service and records the BSD name.
Startup/failure checkpoints precede each stage. Async BSD callbacks never
sleep. Runtime class-size guards include the BSD and Skywalk base classes.
The BSD bridge follows the pinned WCL provider/prepare pattern, with credit
retained, while omitting hardware/HAL/data transfer.

The diagnostic uses a locally administered MAC and reports powered-off state,
zero advertised radio capabilities and disconnected link status. Power-on and
radio operations fail explicitly; outgoing packets are freed/dropped. No
Realtek backend/firmware or direct PCI-access imports are present. It provides
basic driver/hardware metadata and accepts off-state requests only. This is a
publication diagnostic, not a working network connection. Country/radio/scan/
association operations still have no backend and are not claimed supported.

Build and 837-import checks pass, with only the two existing reference-header
offsetof warnings. All identifiable virtual slots match the current Boot KC:
controller 433 inherited/18 overrides (12 abstract slots unidentifiable),
native interface 456/12 (200 abstract slots unidentifiable), BSD 334/6,
provider 260/6. Deliberately swapping inherited slots still yields two failures
when the checker handles a local superclass. These checks do not validate full
private-object layouts or runtime API behavior. Ad-hoc signature and scripts
verify. Nine mocked journal paths (success, approval failure, sampler failure
for controller/interface/registration) pass. Registration mode samples stacks
for 60 seconds, versus 40 for earlier probes, to cover the longer startup.

Added a read-only CoreWLAN status executable (no SSID, scan, join or power
changes). Its bounded pre-publication query returned exit 0 and an empty
interface list; saved corewlan-before-registration.json. After actual publication,
repeat this query and inspect BSD/registry state before claiming menu support.
The original EFI remains byte-identical. Hardware loading of registration
0.2.0 and its required new-module approval are pending.

## 0.2.0 native publication and CoreWLAN discovery passed (2026-11-02)

The user approved the new module and rebooted. Prior registration attempt
registration-journal-20261102-002604 ended at kmutil exit 27 without execution.
After reboot no prior test modules were loaded and EFI was original.

The approved actual load is registration-journal-20261102-003100. Runtime
provider/controller version 0.2.0 was verified. All registration stages passed:
layout, native controller, medium, interface init, IOService attach, native
logical attach, BSD object attach, registration records, native start and BSD
publication. It reached native-interface-published-radio-off at 00:31:46.
The wrapper returned 0 and the 60-second stack capture finished with exit 0.
Runtime IOEthernetInterface size is 328, matching the header exactly.

The registered BSD interface is en4, with the intended diagnostic address
02:52:89:22:00:01 and inactive link. networksetup identifies its Hardware Port
as Wi-Fi. CoreWLAN discovery returned in 0.01 seconds with
[{"interface":"en4","powerOn":false}], versus the empty pre-publication
baseline. This verifies native enumeration and a responsive power-state query;
it does not validate scan, join or data traffic.

Opened System Settings through cua_repl and selected Wi-Fi. Its native pane
rendered normally with toggle Value: off, text 已關閉Wi-Fi, and Details/Advanced
controls. No power/network settings were changed. A second CoreWLAN query after
viewing settings still returned en4/power-off. Control Center app AX retrieval
timed out; that retrieval does not establish whether its menu is functional,
so only the actual System Settings page is claimed visually verified.

Preserved signed build and SHA256 manifest under
build/known-good-native-registration-0.2.0. The run folder contains registry
snapshots for all three interface/controller objects, CoreWLAN JSON before/after
viewing settings, networksetup/ifconfig output, UI observation notes, stack
report and a focused success summary. This is a temporary manually loaded
radio-off diagnostic; it is not EFI-installed and must be manually loaded
again after reboot. No long-term, sleep/wake or unload stability is claimed.

Next functional work is connecting the proven RTL8922AE backend to native
power, scan, association/key and packet paths. The current publication module
has no radio backend; Bluetooth pairing and actual radio sleep/wake are still
outstanding. Do not replace this publication baseline until the backend bridge
has separate build/host-test and staged hardware checks.

## Prepared probe-only native bridge 0.3.0 (2026-11-02)

No reboot, no load, no EFI change in this round. 0.2.0 stayed loaded throughout
and its known-good binary hash still matches its manifest. Source commit 0ac86bc.

New module com.rtw89.nativebridge 0.3.0 (src/native_bridge, make nativebridge).
The 0.2.0 module and AirPortRTW89.cpp are untouched, so both baselines stay as
tested. The front and the rtw89 driver are one kext, which needs no rendezvous,
no second bundle and no transport mode in the Ethernet driver.

RTW89HardwareTransport, owned by the provider, is the only owner of the card:
PCI open, D0, BAR 2, MSI, DMA, glue probe. Its platform code is copied from
AirPortRTW89.cpp. The interrupt keeps its own IOWorkLoop, because the native
queue has no thread (getThread() is 0). The provider probes before
registerService(), so the controller never starts without an identity.
rtw89_hw_bridge.h is the plain C seam: size, version, get_info, bind, quiesce
and deferred link/scan events. Teardown order: controller quiesce, then provider
close.

A probe's identity must pass rtw89_hw_mac_publishable() before anything reaches
macOS. eth_random_addr() is the driver's fallback for a failed efuse read and is
always locally administered, which is refused, as are zero and multicast. Real
values from the probe replace the diagnostic MAC in the interface, the
controller and the BSD object. Radio behaviour matches 0.2.0: power reads off,
power-on is refused, capabilities are 0, output is dropped, rx_frame and tx_wake
are not connected.

Guards: the provider marks the card before touching it and refuses if the card
is already marked, so a second probe in one boot cannot happen (efuse read as 0
after a hot re-probe earlier). The loader also refuses any loaded com.rtw89.*.
Failure evidence (stage, error, driver log, identity) goes on the PCI nub
because the provider object is removed after a failed start.

Offline results:
- make nativebridge passed, with the full hosttest as its gate: 0 failures in
  selftest, data path, aggregation, keep, join, SAE and assoc; dead-device probe
  of 10ec:8922 returned -110 with 0 DMA allocations live.
- The first build was stopped by check_kpi: thread_call_cancel_wait is only in
  com.apple.kpi.private. Replaced by an IOLock held across delivery and a count
  of queued runs. The old _nativeRunning pattern was not copied, since it has a
  gap between dispatch and the increment.
- check_kpi: 900 imports, all resolved in declared libraries.
- Vtables: controller 433 inherited/18 overrides (12 abstract unidentifiable),
  interface 456/12 (200), BSD 334/6, provider 259/7. Identifiable slots match the
  Boot KC. This is not proof of layouts or runtime behaviour.
- Binary: no legacy front or AirPort_RTW89 symbols, no diagnostic MAC bytes; no
  IO80211 or Skywalk imports in transport.o.
- kmutil print-diagnostics: Dependencies OK. The ownership and ad-hoc signature
  messages are the same as for 0.2.0, which loaded.
- bridgetest (seam decisions under ASan/UBSan) and 9 verifier cases pass.
- Mocked in scratch copies with fake kmutil, ioreg and spindump: both loader
  refusals, and journal success, approval 27, sampler exit and a stalled stage.

Not verified by anything yet: real probe under this kext, the provider start
path on hardware, BSD publication with the real MAC, and the event and quiesce
path, which no radio-off run exercises. Sleep/wake with a probed card is
untested, and this kext registers no shutdown or sleep handler yet: the radio
stays off, so that belongs with the power round.

Known risk for the first load: NetworkInterfaces.plist still maps
00:e0:4c:12:34:56 to en2, type Ethernet, from the diag runs, while the Wi-Fi
entry is en4 under the diagnostic MAC. The interface may come up as en2 rather
than en4, and network services may need attention. The verifier records the BSD
name and the file before and after. Nothing has been edited there.

The first loader refused while 0.2.0 was loaded, before reaching kmutil, so the
approval prompt could never appear in the boot that holds the earlier test. The
earlier flow was approve first, then restart. With the card marked, the loader
now only runs kmutil load: 27 asks for approval, and an approved kext stops at
the provider's guard before any hardware access. The test itself still needs a
fresh boot. Mocked: approval 27, already approved, other kmutil error, bridge
already loaded, unclaimed diagnostic loaded, legacy family loaded, fresh boot.

## 0.3.0 hardware probe under the native front passed (2026-11-02)

Approval: run in the 0.2.0 boot returned kmutil 27 without executing. The user
approved and restarted (boot 01:20:27). No other test module was loaded.

Run: bridge-journal-20261102-012139, kmutil exit 0, wrapper exit 0, the 120 s
stack capture exit 0 (nonempty, 26.8 MB). bridge-verification.json: passed.

Every stage passed in order, from hardware-pci-open (01:21:41) through
hardware-probe-complete (01:21:51), hardware-identity-verified, the native
controller and interface stages, bridge-hardware-bind, to
native-interface-published-hardware-probed-radio-off at 01:22:44. The native
queue again reports no thread and the shared provider work loop is true.

Measured from the card in this run: MAC 00:e0:4c:12:34:56 from the efuse,
firmware 0.35.80.3 (8ef4f0cf), cut C (CV 2), RFE 1, 2T2R, BAR 2 1 MiB, MSI index
1, CID 71. These match the earlier backend runs. The loaded UUID equals the build,
DACCF2C1-0F13-3DC4-9D59-3CBD35159BC4, so no cached binary was used.

The en2 risk resolved itself. configd reused the entry for this MAC and rewrote
it as IEEE80211, UserDefinedName Wi-Fi, IOBuiltin true, under the bridge path.
networksetup reports Hardware Port Wi-Fi on en2, with address
00:e0:4c:12:34:56, inactive link, power off. CoreWLAN lists en2 with
powerOn=false. A leftover service Wi-Fi 2 on en4 remains from 0.2.0; it was not
edited and is harmless. Using the diagnostic MAC again would bind en4.

Saved under build/known-good-native-bridge-0.3.0 with a SHA256 manifest.

Scope of this result: one real probe under the native front and publication with
the real MAC, in the radio-off state. It does not cover radio power, scan, join,
data, sleep/wake, unload, or long-term stability. The System Settings page and
the menu bar were not visually checked in this run. The event and quiesce paths
were bound but not exercised.

## Prepared native radio power 0.4.0 (2026-11-02)

Same bundle, version 0.4.0, UUID 06E87ED1-E975-3D7A-A4D5-C08FA9089B6B. Not loaded:
0.3.0 stays loaded in this boot. Seam API version 2.

setPOWER no longer refuses. rtw89_glue_up() sleeps for seconds and macOS calls
setPOWER inside its gate, so the request only records the wish and queues a
transport thread call. That thread brings the radio to the latest wish under a
command lock, then raises RTW89_HW_EVENT_POWER. The controller publishes the
state and posts APPLE80211_M_POWER_CHANGED through its command gate, with the
same inGate and not-onThread check as the reference. getPOWER reports the real
radio (glue.up is set only after start completes), so a radio that fails to
start reads as off. isChipInterfaceReady, isInterfaceEnabled and both enable
paths follow it. Capabilities stay 0, scan and join stay unsupported, and no
frame passes.

Added with power, because a running chip must not cross a restart: shutdown and
restart bring it down, sleep suspends it and wake resumes it, ported from
AirPortRTW89.cpp. Backend rejoin is off, since macOS owns joining. Actual sleep
is still untested.

Each of the 199 unsupported interface operations now records its first use and
order as a property, RTW89 Op <name>. The sources are generated at build time
from the shared UnsupportedOperations.inc, which is unchanged. This records what
macOS asks a powered radio for, which the scan round depends on.

The bridge may be powered as soon as it publishes if macOS has Wi-Fi saved as
on, so the verifier no longer expects the radio off and the ready stage is now
native-interface-published-hardware-probed.

Offline results: make nativebridge passed with the full hosttest as its gate
(0 failures), check_kpi 902 imports resolved, signature verified, vtables
controller 433/18, interface 456/12, BSD 334/6, provider 259/7, print-diagnostics
Dependencies OK, no legacy symbols, no private KPI. bridgetest and the verifier
and power test unit tests pass. tools/test_native_power.py was mocked for a good
run, a radio that never starts, and networksetup timing out.

The test cycles power on and off twice through networksetup, recording each step
with an fsync to power.jsonl, then writes power-verification.json and the wifid
and airportd log. Nothing has exercised the power path on hardware yet.

## 0.4.0 native radio power passed (2026-11-02)

The first load returned kmutil 27 in bridge-journal-20261102-015309. A version
bump of the same bundle needs approval again, and cannot request it while the
older version is loaded. The user approved and restarted (boot 01:55:11).

Run: bridge-journal-20261102-015656, wrapper exit 0, kmutil exit 0, 120 s stack
capture finished. bridge-verification.json and power-verification.json both
passed. UUID 06E87ED1-E975-3D7A-A4D5-C08FA9089B6B matches the build.

macOS powered the radio on by itself once the interface published, so this was
already up when the test started. Actual transitions in the driver log, with
timestamps 1793555xxx: radio up: 0 at 880.4 (macOS), down at 956.2, up: 0 at
957.4, down at 968.7. That is two starts and two stops; cycle 0's on request found
the radio already up, so it recorded no transition. A down-to-up took about 1.2 s.
CoreWLAN followed every change, Radio Error stayed 0, POWER_CHANGED posted with
status 0 each time, and setPOWER was called 6 times. getPOWER was called 280
times in about 90 s. The radio ended off, as the test intends. The log shows no
driver error, and there is no wifi, airport or kernel crash report.

What this shows: macOS drives the real backend up and down through the native
switch, and reports the real state. What it does not show: rtw89_glue_up()
returning 0 proves the chip started, not that the RF receives. That is the
scan round's job. A failed start was not exercised on hardware. Sleep/wake and
the shutdown handler are still untested. Nothing was checked visually in
System Settings or the menu bar.

Unsupported operations macOS asked for, in first-use order: setPOWERSAVE,
setINFRA_ENUMERATED, setOS_FEATURE_FLAGS, setUSB_HOST_NOTIFICATION, getOP_MODE,
getCHANNEL, getMAX_NSS_FOR_AP, getMCS, getNSS, getRSSI, getRATE,
getSUPPORTED_CHANNELS, setIE, setBTCOEX_PROFILE. No scan was requested.

Saved under build/known-good-native-bridge-0.4.0 with a SHA256 manifest.

## Prepared native scan 0.5.0 (2026-11-02)

Same bundle, version 0.5.0, UUID F017795D-FF3A-3392-A489-661238405ECC. Not
loaded: 0.4.0 stays loaded in this boot. Seam API version 3.

Why macOS never scanned on 0.4.0: airportd logged Perform scan and then Completed
scan with error 0 in 0.00 s, with no driver call, and countryCode (null). The
likely cause is capabilities of all 0 and failing channel and country queries.
That is an inference until a run shows setWCL_SCAN_REQ arriving.

Tahoe scans through setWCL_SCAN_REQ and answers with messages through
IO80211Controller::postMessage: 201 per network and 237 when done. The request
and beacon layouts are adapted from the pinned reference into
src/native_bridge/rtw89_wcl_scan.h, plain C so that hosttest can feed it malformed
requests. These are observed private layouts, not documented ones.

A request is decoded, checked and queued on the transport thread, because
rtw89_glue_scan() waits for the command mutex. The driver scans every channel;
the front narrows the results to the channels and name requested, inside the
gate, then posts ScanDone, in the order the reference uses. Added: getOP_MODE,
getSUPPORTED_CHANNELS, getCOUNTRY_CODE. Capabilities now declare CCMP, WPA2 and
scan completion events only, since that is what the backend has proven. No WPA3,
no PMF. Hidden networks are not found, because the backend sends wildcard probes
only. Abort tells macOS at once; the chip finishes on its own. There is no scan
timeout: a scan the driver never ends would leave the front busy.

setWCL_ASSOCIATE stays unsupported, and macOS may now try it for a saved network.
That fails explicitly.

Offline results: full hosttest gate 0 failures, check_kpi 905 imports resolved,
signature verified, vtables match as before, Dependencies OK. bridgetest covers
truncated requests, hostile counts and lengths under ASan, 6 GHz and band
mismatches, result filters and beacon packing. Verdict logic and loader paths
are tested with mocks. Nothing has exercised the scan path on hardware.

## 0.5.0 native scan passed (2026-11-02)

Approval could be requested while 0.4.0 was still loaded (kmutil 27), which saved
a restart. The user approved and restarted (boot 02:25:46).

Run: bridge-journal-20261102-022657, wrapper exit 0. bridge, power and scan
verifications all passed. UUID F017795D-FF3A-3392-A489-661238405ECC matches.

The cause inferred from 0.4.0 held: with channels, country and capabilities
answered, macOS called setWCL_SCAN_REQ. Requests reached 4 with 0 rejected, and
every ScanDone posted with status 0. getOP_MODE and getSUPPORTED_CHANNELS left the
unsupported list. The first real scan took 7.18 s by airportd's log; later
CoreWLAN calls came from airportd's cache in 0.00 s.

CoreWLAN returned 9 networks on channels 4, 5, 8, 11 and 40. Those can only have
come from this card, so the RF receives, on both bands. The last request was
narrowed to channel 40 and returned the test network (a4:b1:c2:d3:e4:f5, -54 dBm) and an
unnamed BSS on the same channel. system_profiler reports en2 as a third-party
card, RTW89 native bridge 0.5.0, the real MAC, Status Not Associated, country
code 0, and a list of Local Wi-Fi Networks with PHY modes and WPA2 security,
which macOS can only have parsed from the elements posted here. airportd holds
country code US from its own sources.

macOS did not attempt setWCL_ASSOCIATE in this run. Newly seen unsupported
operations are getCHANNELS_INFO and setWCL_SET_SCAN_HOME_AWAY_TIME.

Not verified: the Wi-Fi menu and System Settings list by eye, 5 GHz beyond channel
40, DFS channels, an aborted scan, and a scan while associated. The hidden-network
limit and the missing scan timeout stand. The radio was left on.

Saved under build/known-good-native-bridge-0.5.0 with a SHA256 manifest.

## Prepared native join, data and sleep 0.6.0 (2026-11-02)

The user asked to merge the remaining work into one round. Join, data and sleep
are one build; Bluetooth stays separate, because it is another device with an
unlocated cause. Same bundle, version 0.6.0, UUID
44BCE62F-03A9-36D6-8265-52C6F202FEF2. Not loaded. Seam API version 4.

The backend does the key handshake: macOS hands over a passphrase or a PMK, and
EAPOL never reaches macOS, so nothing else manages keys or packet numbers. WPA2-PSK
and open networks are accepted; WEP, 802.1X, SAE, SHA-256 AKMs and WPA1 alone are
refused. The secret is copied to the transport, used on the command thread and
wiped on every path, and it is never logged.

setWCL_ASSOCIATE is decoded in src/native_bridge/rtw89_wcl_join.h and hosttested
with hostile lengths. Messages go in the reference's order through the gate:
AssocComplete, LinkStatus up, FirstBeacon, then ConnectComplete. ConnectComplete
waits for authorized and a first beacon, never for associated alone. A failed
join reports the 802.11 status, and a join gets 60 updates at most. A loss posts
LinkStatus down and Deauth, unless macOS asked to leave.

The stack gets the link just before ConnectComplete. Data uses the BSD
IOEthernetInterface in the pull model, as the reference does and as the hardware
proven Ethernet path does: inputPacket through the gate, outputStart with tx_room
back-pressure. New getters: channel, RSSI, rate, MCS, NSS, beacon info, BSS info,
extended BSS info, traffic counters, both channel lists, low latency. Rate and
MCS sets in the extended BSS info are empty. An LQM update goes out per 8 beacons.
Capabilities add WME and HE, both used by the backend. Still no WPA3 or PMF.

Compat: rtw89_glue_beacon() returns the AP's last whole beacon, which Tahoe uses
to tell a live network from a stale one. See docs/COMPAT-CHANGES.md.

Review fix: a join queued and then left would have run, because the command
thread does leave before join. opLeave now drops a waiting join and its secret.

Offline: full hosttest gate 0 failures, including the beacon test, check_kpi 915
imports resolved, signature verified, Dependencies OK, vtables controller 427/24,
interface 456/12, BSD 334/6, provider 259/7, no legacy symbols, no private KPI.
bridgetest and the verdict tests of the verifier, power, scan, join and sleep tools
pass.

Nothing here has run on hardware. Known unknowns: macOS may require setMacAddress
for a private address (still unsupported, and traced), and the Skywalk side may
want more than this reference needed for data. Sleep relies on macOS to rejoin.

## 0.6.0 hardware result and prepared 0.7.0 (2026-11-02)

0.6.0, bridge-journal-20261102-030347: bridge, power and scan passed, and join
failed. Join Requests stayed 0: setWCL_ASSOCIATE never reached the driver.
airportd logged Auto-join aborted, error 37 'driver not available', with
trigger=assoc_failure, so a join was attempted and refused in user space.
setMacAddress also appeared in the operations, answered unsupported. No crash.

Inference, untested: macOS waits for DriverAvailable (message 55), which the
reference posts when its firmware is ready and this driver never sent.

0.7.0, UUID as built: posts ResetInterface and DriverAvailable when the radio comes
up, once more 3 s later because macOS may not be listening at publication, and
unavailable when it goes down. setMacAddress is implemented: the stack changes at
once and the radio on the command thread through rtw89_glue_set_mac(), which runs
before a join queued after it, and a failure restores the old address. The
verifier now accepts a private address on the interface.

Waits shortened at the user's request: stage pause 0.5 s, stack capture 30 s, join
prompt 40 s, address 20 s, driver join timeout 30 s, rejoin after wake 40 s.

Offline: full hosttest gate 0 failures, check_kpi 916 imports resolved, signature,
Dependencies OK, and bridgetest pass. Nothing of 0.7.0 has run on hardware.

## 0.7.0 hardware result and 0.7.1 (2026-11-02)

0.7.0, bridge-journal-20261102-032730: bridge and power passed. The inference held:
with DriverAvailable posted, macOS stopped reporting error 37 and started using
the driver, sending bgscan, roam and PM settings, and setMacAddress worked, with
en2 on a private address. The scan test failed because macOS changes the address
twice right after power on, and the second radio restart aborted a running scan.
Later scans completed. The loader stopped there, so the join test did not run.

The user then joined by hand. airportd logged Will associate to channel 40 at -52
dBm, hasPassword=1, and APPLE80211_IOC_ASSOCIATE returned 0xe00002c2.

The family's own log is in the kernel log under (corecapture) rtw89-native, because
this driver supplies its logger. At boot+722 s it shows the request is fine: open
plus WPA2-PSK, CIPHER_PMK, and a candidate list with B8:FB:B3:C7:A0:22. Then
Fail to Set APPLE80211_IOC_SET_MAC_ADDRESS, 0xe00002c2, "unable to set mac addre".

From the disassembly: WCLJoinManager::handleJoinRequest uses the request's private
address when flag 8 is set, otherwise an address read through a getter.
IO80211MacAddressAgent::setMacAddress rejects only an all-zero or multicast
address. getHW_ADDR answered unsupported in 0.7.0, which gives zeros. Reading the
Boot KC settled this; a run has yet to confirm it.

0.7.1, UUID as built: getHW_ADDR and getHW_SUPPORTED_CHANNELS answered; an address
change during a scan waits for the end of the scan; a scan or join arriving while
the radio restarts for an address is queued after it. Join counters are published
at once, because a stale 0 misled the first reading of this failure. A scan test
failure no longer stops the join test, and the join test saves wcl-log.txt.
tools/test_native_join.py can run alone through Run-Native-Join.command.

Tools for next time: grep "rtw89-native" in the kernel log first. It names the
failing command. Function bytes from the Boot KC can be wrapped with .incbin and
disassembled, since llvm-objdump cannot read the fileset directly.

## 0.7.1 native join and data passed (2026-11-02)

Run: bridge-journal-20261102-035656 (boot 03:55:38). The user joined the test network from
macOS's own Wi-Fi UI. UUID 2DAE09BB-D084-3797-B17C-18451E966B08 is the build. The
getHW_ADDR analysis is confirmed: SET_MAC_ADDRESS passed and setWCL_ASSOCIATE ran.

The first join test reported failure only because the join finished after its
40 s window. Run again on the live link, join-verification.json passed.

| Check | Result |
|---|---|
| Join | Join Requests 1, rejected 0, ConnectComplete status 0, post status 0 |
| Link | state connected, authorized, 5200 MHz, BSSID a4:b1:c2:d3:e4:f5 |
| Security | WPA2-PSK, CIPHER_PMK from macOS, handshake done by the backend |
| Address | DHCP 192.0.2.105, router 192.0.2.1, global IPv6 |
| Traffic | HTTPS 200 over en2; IPv6 HTTPS 200 |
| Download | 219.71 Mbps, 20 MB, one curl to speed.cloudflare.com |
| Upload | 342.3 Mbps, 10 MB, one curl |
| Latency | 1.1.1.1 about 10 ms; 192.0.2.1 about 3.2 ms, no loss |
| Status | PHY 802.11ax, channel 40, tx rate 865, MCS 8 |

These are single transfers bound to en2, and en0 stayed the default route. They
are not a sustained or comparable benchmark. No crash report.

Still open:
- The scan test still failed once right after radio on (status Aborted), although
  macOS then listed networks. The cause in 0.7.1 is not established.
- system_profiler reports 20 MHz for a network the backend joined wider.
- Country code reads 0.
- Sleep/wake has not been run, and neither has the shutdown handler.
- No long run, roaming, reconnect after AP loss, or 2.4 GHz join on this path.
- WPA3, PMF and hidden networks stay unsupported.
- The kext is loaded by hand and is gone after a restart. Nothing is in the EFI.

Saved under build/known-good-native-bridge-0.7.1 with a SHA256 manifest.

## Sleep run on 0.7.1 and 0.7.2 (2026-11-02)

The machine slept at 04:03:28 and woke at 04:04:33 by the power button. The driver
suspended the radio and resumed it with result 0, and macOS brought the radio up,
so real sleep and wake work at that level. No hang and no crash report. The link
did not come back: Join Requests stayed 1.

The family's log shows the cause, and it is this driver's. At 04:04:33.561 the
scan and join managers left HALTED on SYSTEM_POWER_ON, as they should. At
04:04:35.911 the second announcement added in 0.7.0, which repeats the reset
messages about 3 s after power up, put them back in HALTED, with no power on to
follow. Every later SCAN_REQ failed there with 0xe00002bc, 81 times, and the user
saw Wi-Fi on with no scanning. The Aborted scans right after radio on, attributed
to address changes in the 0.7.1 notes, are likely the same reset. The 0.7.1 change
that defers an address change during a scan stays.

0.7.2 announces once per power up. Gate passed. Not run on hardware yet.

## 0.7.2 hardware result (2026-11-02)

Run: bridge-journal-20261102-042113, boot 04:20:08, UUID
0110A82C-E0B7-33CB-AECC-03876854DA48. Bridge, power and scan verifications passed.
The scan test passed for the first time since 0.5.0 (CoreWLAN 5, then 13 networks),
which supports the diagnosis that the repeated reset caused the Aborted scans.

macOS joined the test network by itself, with no click: request at 04:22:53, authorized and
ConnectComplete 0 at 04:22:54. Auto-join works.

First traffic run, with the Wi-Fi menu open: IPv4 was 169.254 for the first 18 s,
then DHCP gave 192.0.2.105; download 33 Mbps, upload timed out, ping 125-465 ms.
WiFiAgent was scanning about 2-4 times per 10 s while associated, and each scan
leaves the channel for about a second. Kept as join-during-menu-scans.jsonl.

Rerun with the link settled: join-verification passed. Download 385.41 Mbps, upload
80.23 Mbps, 1.1.1.1 about 12.7 ms. An earlier check showed 192.0.2.1 at 3.6 ms.
Single transfers vary widely: 0.7.1 gave 219/342.

Scanning costs traffic while associated. The menu effect is inferred from the
timing of the scans and the recovery, not isolated by a controlled comparison.

Sleep with 0.7.2 is not yet tested. Saved under build/known-good-native-bridge-0.7.2.

Sleep test false pass: the first sleep run on 0.7.2 reported passed, but the
machine never slept. The kernel logged System sleep prevented by kPMCPUAssertion at
04:27:13, kern.sleeptime stayed 0, and the test had treated 20 s of wall clock as
sleep. Its sleep-verification.json from that run proves nothing. The test now
requires kern.sleeptime to change. Wake reconnect on 0.7.2 remains unproven.

## 0.7.2 real sleep and reconnect (2026-11-02)

Evidence: sleep-kernel-evidence.txt in bridge-journal-20261102-042113. The machine
slept at 04:29:29 by the kernel log (kern.sleeptime changed) after a CPU assertion
held it off for about 20 s, and woke at 04:29:53 by key. The radio resumed with
result 0 at 04:29:56. macOS set its address twice, and asked to join at 04:30:00,
so Join Requests went from 1 to 2. Backend rejoin stayed off: macOS did this itself.
The link was authorized on 5200 MHz with 192.0.2.105, and HTTPS through en2 gave
200 in 0.03 s. Radio Error 0. No crash report.

sleep-verification.json from this run says "the machine did not sleep". That is a
test bug, not the result: the clock runs through sleep, so the wait loop passed its
deadline and left without checking again. The loop now checks once more after the
deadline. The kernel log above is the evidence for this run.

One sleep cycle of about 24 s. Long sleep, repeated cycles and shutdown or restart
with the radio up are untested.

## Bluetooth analysis of the existing traces (2026-11-02)

Read only; nothing was loaded or patched. tools/parse_pklg.py summarises a trace.

The controller is healthy at the host level. USB re-enumerates at wake and
RealtekBluetoothFirmware sets the configuration. State Off and Address NULL on this
boot come from macOS keeping Bluetooth off since boot, not from sleep.

bt-hci-20261101-201008 and the older hci-mouse-before-fix.pklg agree over three
connections to the CORSAIR HARPOON:
- LE Connection Complete arrives within 40 ms.
- The peripheral's L2CAP parameter update request arrives 5 s later, and the reply
  is acknowledged by Number Of Completed Packets. Both directions of the radio work,
  so shared-antenna coexistence blocking TX is ruled out for this symptom.
- The link holds for 30 s with no supervision timeout, and the host ends it, 0x16.
- Read Remote Features and Connection Update get status 0 and no completion.
- No PHY Update Complete and no Data Length Change appear either. So every
  link-layer control procedure stalls, not only the two recorded earlier.

The mask 1f enables the completions; a sent mask does not explain it.

Hypothesis, unproven: macOS sends LE Set Default PHY 00 02 02, which asks for 2M
only, where Linux asks for 1M and 2M (03). If the firmware starts a PHY update after
connecting and the procedure never ends with this peripheral, the link layer would
hold every later procedure, which matches. Other candidates remain: the firmware
image itself, and host setup Linux does that macOS does not.

The firmware is rtl8922au_fw.bin embedded in RealtekBluetoothFirmware 1.0.2, reported
version 51461 (0xc905). It has not been compared with current linux-firmware.

Discriminating experiment: capture a connection to a second LE device. A device that
supports 2M and completes the procedures points at the PHY; the same stall on every
device points at firmware or setup.

Capture bt-hci-20261102-043925, with Wi-Fi 0.7.2 running: the mouse stalled the same
way (no LE completions, host disconnect 0x16 at 31 s), so running coexistence does
not change it. A classic BR/EDR audio device, <headset address>, connected: Connection
Complete, Read Remote Version Complete and Authentication Complete all arrived, and
system_profiler lists it as Connected. So classic works, and the stall is specific to
LE. This does not test the 2M hypothesis: it needs a second LE device.

Correction to the note above: "classic works" was too strong. The audio device
connected and authenticated, but bluetoothd logged HCI expected event 89 with opcode
0x0413 timedout at 04:41:50, AVDTP connect failed, RFCOMM timed out, and the link
ended at 04:42:54. In the trace, Set Connection Encryption got Encryption Change v1
(0x08) with status 0 at 65 ms. bluetoothd was waiting for v2 (0x59, decimal 89).

Page 2 of the event mask enables 0x59, and it is never sent. The controller's
supported-commands octet 22 is 0x00, so bit 2 (Set Event Mask Page 2) is clear, and
bluetoothd rejects the command locally with status 46. Our BlueToolFixup patch then
skips that failed init step, but bluetoothd still expects v2. That part of the classic
failure is host side: a stack mismatch our patch exposes. How bluetoothd decides to
expect v2 is not established.

The LE stall is separate in kind: the link layer completions are absent from the
controller's output, whichever host waits. It includes Read Remote Version on LE
links (recorded 2026-10-31), which completes on classic links.

Keyboard: not in this capture. At 04:59 the log shows an LE connection to device
694FD4DE pending for 6.5 s and then cancelled, with no Connection Complete, which
differs from the mouse. That the keyboard is that device, and is LE, is unconfirmed.

Firmware compared with linux-firmware on 2026-11-02: the rtl8922au_fw.bin embedded in
RealtekBluetoothFirmware 1.0.2 is byte-identical to upstream (71360 bytes, SHA-256
112290ca9dc3...), which is 0x41C0_C905, the newest in that file's history. It holds a
single patch subsection, and the running controller reports 0xC905, so an old or
partly selected image does not explain the LE stall. The 6-byte config was not
downloaded: a config header alone is 6 bytes, so it can hold no entries.

The classic failure is now explained from bluetoothd's x86_64 code (build 25G332). At
0x100171084, in the Page 2 init step, a controller with HCI version >= 0x0c that
supports opcode 0x0c84 (octet 45 bit 7, set here) makes bluetoothd set event mask
page 2 bit 25, and change the expected event in its command table from 0x08 to 0x59
for 0x0413 and for 0x2019 (entries 16 and 185, offset 0x14). The table change happens
first and is not undone when Page 2 fails. So LE Start Encryption would also wait for
0x59, even once the LE stall is fixed.

The LE event mask 1f is a third-party path: bluetoothd picks 7ffa863ffff30b00 only
for a nonzero chip id, and this controller has none. The 1f mask leaves out
LE Data Length Change and LE PHY Update Complete, so their absence in these traces
proves nothing, and the note above saying they never completed is withdrawn. Read
Remote Features and Connection Update Complete are enabled and are still missing.

Bluetooth raw HCI injection is not available to test the LE stall: IOBluetooth's
BluetoothHCISendRawCommand and BluetoothHCILESetEventMask are empty stubs on this
build, and debugging restrictions block attaching to bluetoothd. A host-side test of
the LE mask or default PHY therefore needs a BlueToolFixup change, which means the EFI.

Keyboard capture, bt-hci-20261102-052753: the keyboard is a CORSAIR K100 AIR, LE only,
random address <keyboard address>, HID service 0x1812, appearance 0x03c1. bluetoothd
ties that address to device 694FD4DE, so it is the 04:59 attempt. The earlier note was
wrong that this attempt left no trace: that capture ended before 04:59, and the
bluetoothd-hci-latest file spans sessions, so its offsets 1188 and 1194 show it now.

In both sessions the keyboard was heard only briefly: 2 reports at 04:59, then 10
reports over 0.15 s in this capture, followed by none for 83 s while other devices
kept reporting, including 1.5 s of active scanning at 50% duty right after the burst.
Its flags are 0x04, which is neither limited nor general discoverable. The 04:59
LE Create Connection came 7 s after the last report and was cancelled at 6 s, and the
retry at 25 s, with no connection. This capture has no connection attempt at all.

So the keyboard never reached the stall seen with the mouse. The likely reading is
that it was not in pairing mode, which its manual shows as a blinking host indicator,
but a scanner fault for this one device is not ruled out. All LE stall evidence so
far comes from one device, the CORSAIR HARPOON, so a second LE device reaching
Connection Complete would test whether the stall is general.

Keyboard retry in pairing mode, bt-hci-20261102-053301: the LE stall is not general.
LE Connection Complete came 637 ms after Create Connection, LE Read Remote Features
Complete 346 ms after the request (F5 71 78 08), and LE Connection Update Complete
286 ms after the request. The link layer works with this device. After that the host
sent nothing: no ATT, no SMP, no encryption, until it disconnected at 29 s.

The cause is on the host side and is read from bluetoothd. After remote features,
0x100209f45 sets the connection's ready state to wait-for-phy (4) when 2M is supported
at both ends and PHY is not ready, or wait-for-length (3) when data length extension
is, and the log shows EDL and 2M both 1 with PHYReady:no. Those states end on
LE PHY Update Complete and LE Data Length Change. The 1f LE mask masks both, so they
cannot arrive. Whether this controller then raises them unprompted is untested.

The CORSAIR HARPOON mouse fails earlier, at Read Remote Features, so its stall is a
different problem and is still unexplained.

BlueToolFixup patch built, not installed: tools/bluetooth/BlueToolFixup-encryption-le-mask.patch,
with the package in build/bluetooth-fix-20261102 (install.sh, restore.sh). It keeps
the existing patches and adds two for Tahoe: the expected event stays 0x08, and the
third-party LE mask gains bits 6 and 11, becoming 5f08000000000000. Both byte patterns
occur once in the x86_64 slice of bluetoothd 25G332. The source used builds the same
__text as the kext now in the EFI. Neither patch has run on hardware.

Patch run on hardware, bt-hci-20261102-054651, with Wi-Fi up (0.7.2, router ping OK).
The controller received LE mask 5f08. The K100 AIR then completed LE PHY Update to 2M,
remote features, connection update, LE Start Encryption with Encryption Change 0x08,
GATT, and vendor and product ID read. macOS shows it as Connected, and hidutil lists
it as a Bluetooth Low Energy keyboard (usage 1/6). No LE Data Length Change appeared,
and the link went ahead anyway, so the PHY event was what the host needed here. Typing
has to be confirmed by the user.

The classic 700 Gen 2 MAX now passes authentication and encryption with no 0x59
timeout, but it is not an audio output. The device itself refuses: AVDTP's L2CAP
connection gets result 4 (no resources), and RFCOMM's channel for hands-free gets DM.
SDP and AVCTP are accepted, so it shows only AVRCP. That the device has its audio
profiles tied to another host is a guess to test. The CORSAIR HARPOON has not been
retried with this patch.

Correction: the 0x59 timeout did not cause the missing audio. The 04:39 trace, before
the patch, has the same AVDTP result 4 and the same RFCOMM DM. The patch removes the
encryption timeout only, so the earlier note that this explained the classic failure
holds for that timeout, not for the audio profiles.

Confirmed by the user: typing on the K100 AIR over LE works on macOS.

Audio after the headset was released from other hosts and paired again: at 05:51:25
A2DP and hands-free both connected, and A2DP streamed from 05:51:45 to 05:51:58. The
user heard it. So the earlier refusal came from the device's other connections.

The K100 AIR dropped twice with reason 708, a supervision timeout (05:49:52, and at
05:52:25 after 13 s). The user switched the keyboard off both times, so these are not link faults.

The HARPOON was retried at 05:52:52 without a trace capture. The log shows Connection
Complete, then no LE PHY Update Complete and no Remote Features for 31 s. On the
keyboard the PHY update completed before remote features, in state conn-complete, and
bluetoothd sent no LE Set PHY, so this controller starts the update unprompted. It
does so because LE Set Default PHY is 00 02 02: bluetoothd allows 2M only.

Hypothesis, unproven: the mouse has no 2M, and the controller's first procedure stalls
the rest. Test build, not installed: build/bluetooth-fix-20261102b and
tools/bluetooth/BlueToolFixup-default-phy.patch. It sets the default to 00 03 03 and
removes the wait for PHY readiness after remote features, which the keyboard would
otherwise hit. Its restore.sh returns to the build that runs now.

Loading Wi-Fi at boot, requested by the user: tools/boot_load, run through
Install-Wifi-Boot-Load.command. A LaunchDaemon runs the same kmutil load of the saved
0.7.2 build (UUID 0110A82C), so the EFI stays as it is. On hand loads macOS powered
the radio by itself within 20 s, so the daemon does not touch power. A marker file
stops the next boot's load if one panics. Not yet run at a real boot.

First boot with both installed, 2026-11-02 06:05. The boot load worked: kmutil returned
0 on the first attempt, the marker was cleared, UUID 0110A82C is loaded, the address
is 192.0.2.105, and the router answers ping, with no hand load.

With the default PHY build the HARPOON reached Remote Features 146 ms after Connection
Complete at 06:07:09, the first time on this controller, and macOS shows it Connected
and hidutil lists it as a Bluetooth Low Energy mouse. This was one run, with no HCI
trace, because no capture was running.

Its features are F5 71 78 08, the same as the keyboard, so it does support 2M and the
guess that it had none was wrong. Allowing 1M did change the outcome, but why the
update stalled this mouse and not the keyboard is unknown. Keyboard and headset have
not been retried with this build.

Confirmed by the user: the cursor moves with the HARPOON over LE on macOS.

All together on the default PHY build at 06:10: K100 AIR and HARPOON are Connected
and listed by hidutil, and the 700 Gen 2 MAX has HFP, AVRCP and A2DP, is the default
output, and streams from 06:10:30, with the router answering ping over Wi-Fi. So the
keyboard did not regress.

The keyboard drop with reason 708 at 06:08:42 was the user switching it off.

Open: the first headset attempt at 06:09:50 failed with 10712 and the next one, 7 s later,
worked. Reconnect after restart, sleep and wake, and long use are untested.

Both drivers in the EFI, on the user's request, 2026-11-02 06:14. Bluetooth was there
already. Wi-Fi: EFI/OC/Kexts/RTW89NativeBridge.kext is the saved 0.7.2 build, verified
against its hashes, and config.plist gained one Kernel Add entry for kernel 25 only.
A parsed comparison shows no other change. All four Wi-Fi dependencies are in the
Boot KC, and the kext has OSBundleRequired Root. Injection has not run yet, so whether
OpenCore links it and whether start() survives early boot are unknown.

The LaunchDaemon stays: if the kext is already loaded it exits, and if injection is
skipped it still loads Wi-Fi. tools/load_native_bridge.sh can no longer test a new
build until the EFI entry and the daemon are removed. Undo: build/efi-wifi-20261102/
remove_from_efi.sh from macOS, or Remove-WiFi-From-Boot.cmd in
/Volumes/1401/WiFi-Boot-Recovery-20261102 from Windows. build/native-experiment/
config.original.plist is no longer the expected EFI config; the current one is
build/efi-wifi-20261102/config.new.plist.

First boot with Wi-Fi in the EFI, 06:17: injection worked. The daemon logged "already
loaded", and the module sits at 0xffffff800461c000 next to the other injected kexts,
where the 06:16 shutdown report has it at 0xffffff7f98907000 after a kmutil load. Stage
and firmware match, radio is up, 192.0.2.105 answers, and HTTPS returns 200, with no
panic report. Headset and HARPOON reconnected unprompted. The K100 AIR had not.

Two things checked and left alone. The kernel log repeats LOW_LATENCY_INFO_STATS failed
every 5 s because that optional query is unsupported; it is noise. Shutdown stall reports
appear at each restart, the latest with a 2 s stall. In the one read, the driver's
threads are idle with under 1 ms CPU and launchd is waiting on user processes, so
nothing points at the driver, though the reports before it were not read.

Open: sleep and wake with both drivers in the EFI, and long use.

Correction: the ping and HTTPS results after the boot load and EFI boots went over
Ethernet. en0 is active at 192.0.2.3 and ranks above Wi-Fi, so an unbound ping does
not test Wi-Fi. Bound to en2 with Wi-Fi 0.7.2 from the EFI, 06:29: IP 192.0.2.22 on
macOS's private address, 3 of 3 pings at 2.8 to 107 ms, HTTPS 200. Throughput since the
EFI change has not been measured. Bind Wi-Fi checks with -b en2 and --interface en2.

AirDrop is not available. awdl0 and llw0 do not exist because the driver answers
setVIRTUAL_IF_CREATE and the P2P queries with unsupported, and bluetoothd reports
LE_InitMultipleAdvertising failed (STATUS 152) at each start. AWDL is not implemented
in this driver.
