# km-init -- userspace keymaster TA bootstrap (fake re-lock for iceland)

Reports a stock **locked / GREEN** verified-boot state to the keymaster TA
in QTEE, entirely from Linux userspace, so that hardware key attestation
later reports the same values a never-unlocked OnePlus Pad 4 would
produce. Companion research doc: the ABL-side protocol this mirrors lives
in `src/bootloader/edk2/QcomModulePkg/Library/avb/KeymasterClient.c`
(`uefi.lnx.6.0.r49-rel`).

## Why this works

The ESP boot chain (stock ABL -> `BOOTAA64.EFI`) skips ABL's
`BootLinux()` verified-boot path, so the boot-state notification that ABL
normally sends to the keymaster TA before jumping to the kernel never
happens -- no `SET_ROT`, no `SET_BOOT_STATE`, no `SET_VBH`, no milestone.
Nothing in TZ independently knows what booted: the reporter of these
values is ordinary non-secure code, so userspace may as well be the
reporter. The kernel side is already in place: `CONFIG_QCOMTEE=y`
(upstream qcomtee TEE driver).

## What it does

```
/dev/teeN -> libqcomtee root object
  -> registerAsClient(credentials)          [IClientEnv]
  -> IClientEnv.open(3)                     [IAppLoader]
  -> IAppLoader.loadFromBuffer(keymaster.img)
  -> IAppController.getAppObject()
  -> IOpener.open(150)                      [IKMHal]
  -> IKMHal.sendCmd():
       0x200 GET_VERSION    handshake, Major >= 2
       0x201 SET_ROT        SHA256(avb_public_key || 0x00)
       0x208 SET_BOOT_STATE {IsUnlocked=0, PublicKey=SHA256(avb_public_key),
                             Color=GREEN(0), SystemVersion, SecurityPatch}
       0x211 SET_VBH         stock vbmeta-chain digest
       0x204 MILESTONE_CALL  seal this boot's state
```

`keymaster.img` is the **stock-signed** TA (from `repos/iceland-fw`,
SHA256-pinned in `build.sh`); QTEE verifies its OEM signature on load, so
the blob cannot be tampered with. The daemon then stays alive holding the
object references, keeping the TA resident for later clients (Waydroid's
keymint HAL attaches to the same TA).

## The values (`/etc/km-init/values.conf`)

Generated offline from the stock partition dumps:

```
values/km-extract-values.py <iceland-fw dir> -o values.conf \
    [--digest-from-cmdline <androidboot.vbmeta.digest hex>]
```

- `avb_public_key`: raw key blob from the root vbmeta header (1032 B
  RSA-4096 on iceland).
- `vbmeta_digest`: SHA256 over the vbmeta blobs in avb_slot_verify order
  (`vbmeta -> boot -> dtbo -> recovery -> vbmeta_system -> vbmeta_vendor`;
  chained partitions are read through their AVB footer). This equals
  stock `androidboot.vbmeta.digest`. **Cross-check it against a stock
  boot's `/proc/cmdline`** via `--digest-from-cmdline` -- the script then
  picks the matching full-vs-trimmed hashing variant (partition content
  vs header+auth+aux) and warns on mismatch.
- `os_version_packed` / `security_patch_packed`: from the
  `com.android.build.boot.*` properties in the boot partition's embedded
  vbmeta, packed exactly like ABL's `ParseFooterOsVersion` /
  `ParseFooterSecPatch` (`(maj<<14)|(min<<7)|sub`,
  `(day<<11)|((y-2000)<<4)|month`).

## Build / use

```
./build.sh                    # -> ../../build/userspace/debs/km-init_*_arm64.deb
sudo ./build.sh --addition-presents keymaster    # userspace rootfs preset
```

On-device manual test (before trusting it):

```
km-init -t                                   # parse + print derived values
km-init --oneshot --no-milestone -v          # dry-run the sequence
journalctl -u km-init                        # each step logs SET_ROT/...
```

The sequence is per-boot volatile TA state; it is idempotent until the
milestone is sent, and nothing irreversible is reachable from it (no ARB
fuses, no tamper fuses, no RPMB writes).

## Validation checklist (attestation side)

1. `km-init` logs all five commands OK.
2. Decode an attestation cert (e.g. `keytool` in the container, or a
   KeyMint `attestKey` via libqcomtee): extension
   `1.3.6.1.4.1.11129.2.1.17` RootOfTrust must show
   `verifiedBootState=Verified`, `deviceLocked=true`,
   `verifiedBootKey=8d897f62...` (SHA256 of the stock AVB key) and
   `verifiedBootHash=027c526d...` (the stock VBH).
3. Waydroid: keymint HAL in a vendor image built from the stock
   `vendor/odm` EROFS dumps; property spoofing to the stock fingerprint
   (`com.android.build.boot.fingerprint` in the same embedded vbmeta,
   `qti/canoe/canoe:16/BP2A.250605.015/...`). Stock HALs expect
   `/dev/smcinvoke` (not the upstream qcomtee TEE device), so the
   smcinvoke driver still needs porting for the container -- that is
   follow-up work, independent of this daemon.

## Files

- `src/km-init.c` -- the daemon (protocol mirrors
  `KeymasterClient.{c,h}` + `SmciInvokeUtils.h` verbatim).
- `src/sha256.{c,h}` -- minimal SHA-256, keeps the deb libc-only.
- `values/km-extract-values.py` -- offline value extractor.
- `rootfs/` -- values.conf (generated), systemd unit.
- `build.sh` -- one-shot arm64 container build, pinned inputs
  (quic-teec `736419e2`, keymaster.img SHA256), self-test acceptance.

## Notes / caveats

- If `IOpener.open(150)` is refused, km-init falls back to using the app
  object directly; both shapes are logged.
- `KEYMASTER_MILESTONE_CALL` mirrors ABL's `VBSendMilestone()`; skip it
  with `--no-milestone` while experimenting (the TA then still accepts a
  re-run of the whole sequence within the same boot).
- Widevine L1 additionally needs the oemcrypto TA + drm HALs in the
  container; gatekeeper rides the same keymaster TA (`GK_CMD_ID=0x1000`).
- Research use on your own device only: this misrepresents the boot state
  to attestation services, which violates Google Play / Widevine terms.
