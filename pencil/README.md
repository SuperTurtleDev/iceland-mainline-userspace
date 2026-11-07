# Iceland Pencil bridge

Native C daemon using the existing GLib/GIO and BlueZ system bus. No Python,
custom Bluetooth stack, frequency table or pressure polling. Normal coordinates
continue through the kernel's evdev interface. This daemon handles the IC control
reports that the kernel does not decode.

```
make
make check
sudo make install
sudo systemctl enable --now iceland-pencild
journalctl -u iceland-pencild
```

Installation also supplies a libinput quirk and udev rule for
`NVTCapacitivePen`. Earlier kernel modules lack BTN_STYLUS capability; the
compatibility quirk supplies it without synthesizing button events. The udev
rule corrects the native portrait ranges to 24000 x 33920 and
sets 124 units/mm, derived from the panel's 193 x 274 mm native active area.
Its calibration matrix swaps XY and inverts display Y to match the landscape
display. The kernel continues to report native pen coordinates.
Restart the graphical session or reboot after installation: an existing
libinput context caches its quirk database, so a synthetic device hotplug alone
does not reload it. No kernel rebuild is needed.

Pair the pencil with BlueZ first. At startup the daemon reads MT5806's
`ble_mac_addr`; if the detached pencil cleared that value, it recovers a unique
paired device advertising the known pencil control service. More than one
candidate requires `--address MAC`; it never chooses an arbitrary device.
Power-supply uevents and BlueZ object/property changes drive setup and reconnect.

`--debug` logs raw payloads; `--duration SECONDS` runs a bounded test. There is no
periodic status query. After a successful firmware reload, the touchscreen
emits a kernel `KOBJ_CHANGE` uevent with `NVT_FIRMWARE=ready`. The daemon then
replays pen setup and queries geometry again. This covers panel blank/unblank,
greeter-to-session modesets, system resume and watchdog firmware recovery;
configuration starts when the IC accepts commands, without a fixed delay.

## Debian package

Run `./pencil/build.sh` from the userspace repository to build the ARM64
`iceland-pencild` deb in an Ubuntu container. The package installs the bridge,
`nvtctl`, the systemd service, and the udev/libinput configuration. Debian's
systemd helpers enable and start the service on install and manage upgrades.
The matching kernel must supply the `nt36536` Generic Netlink family.

The rootfs builder includes it with `--addition-presents pencil`. The deb can
also be included alongside the kernel debs in a packages-only USB deploy blob.

## Interface and diagnostics

`nt36536.h` is copied from the kernel UAPI `include/uapi/linux/nt36536.h`.
The Generic Netlink family is `nt36536`, multicast group `events`. A raw event
contains 300 bytes without the SPI address. A wholly handled touch/pen report
is not multicast; a mixed frame containing unknown data retains its full bytes.

```
nvtctl monitor 30               # public raw event subscription
sudo nvtctl send 7f 10 01      # OEM OPP enable / request timing
sudo nvtctl send 7f 11         # OEM OPP disable
sudo tests/capture 60          # simultaneous physical evdev + Netlink capture
```

SEND requires CAP_SYS_ADMIN, accepts 1–15 payload bytes and returns the actual
SPI result, except for driver-local pen downlinks below. An ACK is not a firmware
ACK. Suspended/unready firmware rejects a command with EHOSTDOWN. Events are best
effort and carry no replay guarantee.

### Driver-local pen downlinks

SEND payloads beginning with `4e 56 54 00` (`NVT\0`) are consumed by the driver
without an SPI write. Unknown opcodes, bad lengths, out-of-range values and
unknown bits are rejected. Following the prefix:

| Opcode | Payload | Meaning |
| --- | --- | --- |
| `01` | u16 little-endian | Pressure, 0–16384 |
| `02` | u8 bitmap | Side buttons: bit 0 BTN_STYLUS, bit 1 BTN_STYLUS2 |
| `03` | u8 bitmap | Sources: bit 0 pressure, bit 1 BTN_STYLUS, bit 2 BTN_STYLUS2 |

Each source bit defaults to zero (IC). Setting it to one selects userspace and
filters IC updates for that property. Injection into an IC-owned property is
rejected. Coordinates, tilt and proximity continue to come from the IC. Pressure
and tip contact require an IC-reported pen position; userspace-owned side buttons
are emitted even outside that range. Leaving range clears pressure and IC-owned
buttons, retaining injected buttons and the last position. Suspend and firmware
reload release all input state and clear cached values, retaining source selection.
Changing a source bit clears the injected value for that property.

These are evdev semantics. libinput 1.31.1 suppresses tablet-tool events outside
proximity and releases side buttons on proximity-out. Off-screen button actions
in a desktop therefore need separate userspace mapping; retaining coordinates
alone does not make libinput deliver those button events.

```
sudo nvtctl send 4e 56 54 00 03 01       # pen pressure, IC buttons
sudo nvtctl send 4e 56 54 00 01 00 10    # pressure 4096
sudo nvtctl send 4e 56 54 00 01 00 00    # release tip
sudo nvtctl send 4e 56 54 00 03 07       # all three properties from userspace
sudo nvtctl send 4e 56 54 00 02 01       # first side button down
sudo nvtctl send 4e 56 54 00 02 00       # side buttons up
sudo nvtctl send 4e 56 54 00 03 00       # return all properties to IC
```

The daemon selects pen pressure and forwards real BLE pressure values, including
zero. It also retains the OEM IC pressure-presence transitions. When the dedicated
BLE button subscription succeeds it selects userspace for BTN_STYLUS as well.
The OEM click characteristic `180938ac-2a69-11ec-8d3d-0242ac130003` reports
`02` down, `03`/`04` up and `ff` cancel; cancel releases the button. Its `01`
double-click gesture is not treated as a second physical button. BTN_STYLUS2
remains IC-owned. A BLE disconnect or normal daemon shutdown releases injected
pressure and the side button.

The click mapping follows the extracted OEM `OplusExSystemService` method
`z7.a.S` and `IPeManager` method `com.oplus.ipemanager.btadsorb.ble.s0.e`.
GATT control and click notifications are subscribed in sequence before normal
initialization writes. Both callbacks run on the same GLib main loop; pressure
and button injection update independent cached kernel fields under the same
device mutex.

## Protocol sources

The source of the chip command mapping is:

`repos/android_kernel_modules_and_devicetree_oneplus_sm8850/vendor/oplus/kernel/touchpanel/oplus_touchscreen_v2/Novatek/NT36536_noflash/nvt_drivers_nt36536_noflash.c`

Relevant functions are `nvt_get_pen_uplink_timing`, `nvt_opp_pen_handle`,
`nvt_set_pen_downlink` and `nvt_multi_cmd_store`. Android transport mapping comes
from the extracted `/odm/bin/touchDaemon` (`touch_pen_set_cmd_map`,
`touch_pen_bt_uplink_package`), and `libbluetooth_jni.so`
(`oplus_write_pen_press_char`, `oplus_handle_pen_press_notify`). Analysis is in
`build/pen-ghidra/` outside this userspace tree.

IC function page 6 type 0 becomes BLE AA0201; type 1 becomes AA0202. The exact
IC-supplied timing bytes are forwarded. Ordinary control writes use UUID ending
5cbb; the OEM empty PDU AA020000 uses 5cbc once per six pressure notifications.
Only pressure presence transitions are written to the IC, as in the OEM driver;
the full value is injected into pen evdev through the local command above.

The actual size response has six payload bytes. The OEM downstream ioctl buffer
is zero-initialised, while the chip driver reads six bytes after its first byte.
The translation preserves that zero padding explicitly instead of reading beyond
the received BLE packet. Unknown packets are logged at debug level and never
interpreted as commands. Kernel command injection itself remains unrestricted by
opcode, permitting independent protocol experiments with `nvtctl`.

Unit fixtures include actual IC timing and actual pen geometry replies; they
also check truncated packets and unknown commands. They establish translation
and bounds, not real pen-coordinate accuracy or sleep/resume reliability.
