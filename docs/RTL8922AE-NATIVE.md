# RTL8922AE on macOS 26: native Wi-Fi and Bluetooth

This fork adds `RTW89NativeBridge`, a driver for the RTL8922AE that uses macOS 26's own
Wi-Fi stack (`IO80211Family`, `IOSkywalkFamily`), so the old-stack kexts are not needed.
It also patches `BlueToolFixup` so that the card's Bluetooth works with `bluetoothd`.

It is experimental and has run on one machine. Keep a working EFI backup and a second way
to boot before you try it. Nothing here is supported by Apple or Realtek.

## Tested setup

| | |
|---|---|
| macOS | 26.7.2 (25G332), x86_64 |
| Machine | Ryzen 9800X3D with OpenCore, SMBIOS MacPro7,1 |
| Card | RTL8922AE, Wi-Fi PCI `10EC:8922`, Bluetooth USB `0BDA:8922` |
| Bluetooth firmware | RealtekBluetoothFirmware 1.0.2 (not included here), 0xC905 |

Other macOS builds are untested. The Bluetooth patches match exact bytes in `bluetoothd`
25G332, so on any other build they find nothing and change nothing. Intel machines are
untested.

## What works

| Feature | State |
|---|---|
| Wi-Fi menu, scanning, joining, DHCP, IPv4, IPv6 | Works |
| WPA2-Personal | Works |
| 5 GHz | Works: up to about 385 Mbps down and 80 Mbps up in one test |
| Sleep and wake | One real cycle worked, with Wi-Fi only |
| Loading from OpenCore | Works, booted once |
| Bluetooth LE keyboard (CORSAIR K100 AIR) | Works, typing confirmed |
| Bluetooth LE mouse (CORSAIR HARPOON) | Works, cursor confirmed |
| Classic Bluetooth audio (A2DP, hands-free) | Works, one headset tried |
| Reconnect after restart | Worked once for the mouse and headset |

| Feature | State |
|---|---|
| AirDrop and other features that need AWDL | **Not available**: AWDL is not implemented |
| WPA3, PMF, hidden networks | Not implemented |
| 2.4 GHz, roaming, reconnect after the AP drops | Untested |
| Bluetooth after sleep, long use | Untested |
| Wi-Fi throughput when loaded from OpenCore | Not measured |

## Install

You need Lilu and RealtekBluetoothFirmware already working, and to know how to restore
your EFI. Use the files from the Release, or build them as below.

**Wi-Fi.** Put `RTW89NativeBridge.kext` in `EFI/OC/Kexts` and add it to `Kernel > Add`:

| Key | Value |
|---|---|
| `Arch` | `x86_64` |
| `BundlePath` | `RTW89NativeBridge.kext` |
| `Enabled` | `true` |
| `ExecutablePath` | `Contents/MacOS/RTW89NativeBridge` |
| `MinKernel` | `25.0.0` |
| `MaxKernel` | `25.99.99` |
| `PlistPath` | `Contents/Info.plist` |

Do not also enable `IOSkywalkFamily` or `IO80211FamilyLegacy`, or block the system's copy.

**Without touching the EFI.** `sudo tools/boot_load/install_boot_load.sh path/to/kext`
loads the driver at each boot through a LaunchDaemon, and macOS asks you to approve the
extension the first time. `tools/boot_load/remove_boot_load.sh` undoes it. If a load
panics, the next boot skips it until you delete the marker file it names in
`/var/log/rtw89-boot-load.log`.

**Bluetooth.** Replace `EFI/OC/Kexts/BlueToolFixup.kext` with the patched one.

Restart after either change. A wrong EFI can stop the machine booting, so test with a
copy on a USB stick first if you can.

## Build

```bash
make nativebridge
```

The output is `build/native-bridge/RTW89NativeBridge.kext`. `make nativebridge` runs the
host tests first, and needs `firmware/rtw8922a_fw-4.bin` (see `firmware/SHA256SUMS`).

BlueToolFixup comes from Acidanthera's
[BrcmPatchRAM](https://github.com/acidanthera/BrcmPatchRAM), commit
`65389020769837e92e9fb939bac34bd9a3e25bb2`, with
`tools/bluetooth/BlueToolFixup-RTL8922-Tahoe.patch` applied (`patch -p1`).

## What the Bluetooth patches do

| Patch | Why |
|---|---|
| Skip the failed Set Event Mask Page 2 step | The controller does not offer it, and `bluetoothd` stops initialising |
| Do not restart on a failed LE scan, and use the public LE address | `bluetoothd` never sets a random address for third-party controllers |
| Skip Read Remote Version on LE links | It does not complete, and GATT waits for it |
| Keep Encryption Change v1 as the expected event | The controller reports HCI 5.3, so `bluetoothd` waits for v2, which needs Page 2 |
| Add LE Data Length Change and PHY Update Complete to the LE event mask | The third-party mask `1f` disables events that `bluetoothd` waits for |
| Allow 1M and 2M as default PHYs, and do not wait for PHY readiness | With 2M only, the controller's early PHY update stalled the HARPOON |

Why that update stalled the HARPOON but not the K100 AIR is not known. Both report the
same LE features, and the mouse supports 2M. Allowing 1M changed the result, and nothing more
is established. See `docs/RTL8922AE-DIAGNOSTICS.md` for the evidence.

## Credits

The chip code is Linux rtw89, and the port builds on AirPort_RTW89, AirPort_RTW88 and
others listed in `CREDITS.md`. This is licensed GPL-2.0 like the code it comes from.
