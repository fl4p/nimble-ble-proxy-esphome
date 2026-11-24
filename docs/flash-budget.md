# App flash budget (branch `auto-update`, 2026-11-24)

Measured on ce15efa (HEAD of `auto-update`) against tag v0.2.8, IDF 6.0.3,
CI configuration (`sdkconfig.defaults`, `-DNBP_VARIANT=<target>-ble-on`,
unsigned, `NBP_SIGNING_KEY=none`). Every number marked *measured* comes from
a build. Numbers marked *estimate* do not.

## Summary

- The OTA slot is `0x1E0000` (1 966 080 B) on every target
  (`partitions.csv`, the only partition table; flash size 4 MB).
- The ESP32-C6 BLE-on image is the only one near the limit. Its headroom fell
  from **143 616 B** (v0.2.8 release) to **18 324 B** (HEAD built like CI),
  a loss of 125 292 B:
  - **65 536 B: the signing layout.** A signed image is padded to a 64 KiB
    boundary and followed by a 4 KiB signature sector, so its content must end
    at or below `0x1D0000`, not `0x1E0000`.
  - **59 756 B: content growth.** About half of it (30 979 B) is the larger
    uncompressed dashboard. The rest is rollback, signing code and the
    switch output.
- The ~28 KB headroom figure came from a build in the main checkout. That
  checkout's untracked `dependencies.lock` pins `espressif/mdns` 1.11.1. CI
  has no lock file and resolves mdns 1.15.1, which is 7 376 B larger on the
  C6. A CI-equivalent build has **18.3 KB** left, not 28 KB.
- The growth is the same on every target: +59 831 B on the S3, +60 095 B on
  the C6. Only the C6 is near the limit, because its base image is 365 KB
  larger than the S3's. Most of that difference is the BLE controller
  (`libble_app.a` 239 KB in flash, against 98 KB for `libbtdm_app.a` on the
  S3) and the Wi-Fi 6 libraries. IEEE 802.15.4, OpenThread and Zigbee are
  **not** linked: turning `CONFIG_IEEE802154_ENABLED` off changed the image
  by 0 B (measured).
- The largest cheap trims are compiling with `-Os` instead of the current
  `-Og` (−157 548 B on the C6, measured), sending the dashboard gzipped
  (−81 502 B, computed exactly from file sizes) and dropping the 64 KiB
  signing pad (+61 452 B of headroom, measured). With these three the C6
  would have more headroom than v0.2.8 had.

## Headroom per target (HEAD, CI-equivalent)

"Content" is the unpadded image: the `.bin` minus esptool's PADDING segment.
A signed image cannot have content past `0x1D0000` (1 900 544 B), because the
next 64 KiB step (`0x1E0000`) plus the 4 KiB signature sector does not fit in
the slot. The classic ESP32 does not sign, so its limit is the slot size.

| target, BLE on | content (B) | limit (B) | headroom (B) | source |
|---|---:|---:|---:|---|
| esp32c6 | 1 882 220 | 1 900 544 | **18 324** | measured, fresh worktree (mdns 1.15.1) |
| esp32c6, main-checkout lock (mdns 1.11.1) | 1 874 844 | 1 900 544 | 25 700 | measured |
| esp32s3 | 1 529 008 | 1 900 544 | 371 536 | measured, fresh worktree |
| esp32c3 | ~1 583 892 | 1 900 544 | ~316 600 | `/tmp/nbp-chk` build, main-checkout lock |
| esp32 (no signing) | 1 510 624 | 1 966 080 | 455 456 | `/tmp/nbp-chk` build, main-checkout lock |
| esp32c6 **v0.2.8** release | 1 822 464 | 1 966 080 | 143 616 | release asset `-ota.bin` |

## v0.2.8 vs HEAD, ESP32-C6 BLE on

Both trees were built locally the same way, each in a fresh git worktree, so
both resolved mdns 1.15.1 and esp-nimble-cpp 2.6.0. Bytes are image bytes per
archive from `idf.py size-components`: Flash Code plus `.data` and IRAM
`.text`, without `.bss`/`.noinit`.

| archive | v0.2.8 | HEAD | Δ | what |
|---|---:|---:|---:|---|
| libapi_server.a | 129 256 | 160 265 | **+31 009** | `index.html.S.obj` +30 979 (dashboard 88 787 → 119 766 B, embedded raw) |
| libota.a | 1 404 | 11 035 | **+9 631** | `ota_verify.cpp` +6 882 (rollback), `ota_sig.cpp` +2 519 (signing), `ota.cpp` +246 |
| libesp_stdio.a | 153 407 | 160 487 | **+7 080** | the linker's merged string pool (see below): new log, format and assert strings |
| libbootloader_support.a | 6 118 | 9 860 | +3 742 | Secure Boot V2 RSA verify in the app (`secure_boot_signatures_app.c` +1 492, `secure_boot_rsa_signature.c` +1 026, `esp_image_format.c` +838) |
| libswitch_out.a | 0 | 3 296 | +3 296 | switch output, now in every image |
| libesp_driver_gpio.a | 4 208 | 6 574 | +2 366 | `gpio.c` hold/sleep API, pulled in by switch_out |
| libapp_update.a | 2 500 | 3 768 | +1 268 | rollback and state API in `esp_ota_ops.c` |
| libefuse.a | 2 301 | 2 999 | +698 | key digest reads (signing) |
| libmain.a | 1 257 | 1 523 | +266 | wiring |
| liblwip.a | 126 086 | 126 263 | +177 | `etharp.c` (rollback's ARP uplink proof) |
| other | | | +532 | gpio hal, hw_support, crash_capture, ... |
| **total** | 1 818 656 | 1 878 751 | **+60 095** | |

No mbedTLS/PSA code was added. `libtfpsacrypto.a` was already 118 KB in
v0.2.8 because WPA2/WPA3 (SAE) uses it. The RSA-PSS verify reuses it: −58 B
when signing is turned off.

**The `libesp_stdio.a` bucket is the string pool, not stdio.** Its 160 KB is
`.rodata.console_open.str1.4`. The linker merges every mergeable string
section (`ESP_LOGx` formats, assert expressions, `__func__`, `esp_err` names)
into the first input section of that name, and that section belongs to
`stdio_vfs.c.obj`. Changes in logging and assertions show up in this row.

### Attribution by feature (C6)

| feature | Δ image (B) | how |
|---|---:|---|
| Dashboard growth (update check, SHA-256, signature parsing, UI) | +30 979 | measured (embedded file size) |
| Switch output built in by default | +6 960 | measured: HEAD with `CONFIG_NBP_SWITCH_OUT=n` |
| Signing: IDF verify code and its strings | +6 988 | measured: HEAD with `SECURE_SIGNED_APPS_NO_SECURE_BOOT=n` / `SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT=n` |
| Signing: `ota_sig.cpp` (classification, `GET /update`) | +2 519 | measured (object size; compiled whether signing is on or off) |
| Rollback: `ota_verify.cpp`, `esp_ota_ops`, ARP, strings | ~+12 600 | *estimate*: the remainder. A build with `BOOTLOADER_APP_ROLLBACK_ENABLE=n` does not compile (see "Findings") |
| **content growth** | **+60 095** | |
| Signing layout: content limit moves from `0x1E0000` to `0x1D0000` | −65 536 of headroom | from the image layout |

The signing layout cost does not show up in the image's size. In the current
build the padding is only 18 316 B, plus the 4 096 B signature sector. What it
costs is the last 64 KiB of the slot, which a signed image can never use.

## Same check on the ESP32-S3

| archive | v0.2.8 | HEAD | Δ |
|---|---:|---:|---:|
| libapi_server.a | 122 616 | 153 615 | +30 999 |
| libota.a | 1 248 | 9 060 | +7 812 |
| libesp_stdio.a (strings) | 144 063 | 151 355 | +7 292 |
| libesp_driver_gpio.a | 2 538 | 6 728 | +4 190 |
| libbootloader_support.a | 5 019 | 8 275 | +3 256 |
| libswitch_out.a | 0 | 3 085 | +3 085 |
| libapp_update.a | 2 178 | 3 215 | +1 037 |
| other | | | +1 160 |
| **total** | 1 454 070 | 1 513 901 | **+59 831** |

The cause is the same on every target. The C6 is only the first to run out.

### Why the C6 base is larger (HEAD, C6 minus S3, image bytes)

| archive | S3 | C6 | Δ |
|---|---:|---:|---:|
| BLE controller (`libbtdm_app.a` on S3, `libble_app.a` on C6) | 97 756 | 238 733 | +140 977 |
| libpp.a + libnet80211.a (Wi-Fi, C6 is Wi-Fi 6) | 203 861 | 295 737 | +91 876 |
| libbt.a (NimBLE host) | 126 946 | 148 038 | +21 092 |
| liblwip.a | 106 157 | 126 263 | +20 106 |
| libtfpsacrypto.a | 98 039 | 117 839 | +19 800 |
| libesp_hw_support.a | 39 174 | 52 892 | +13 718 |
| libwpa_supplicant.a | 78 198 | 89 792 | +11 594 |
| total | 1 513 925 | 1 878 775 | +364 850 |

lwIP, crypto and the NimBLE host are the same source on both targets, so their
deltas come from RISC-V code density and the S3's ROM. The controller and
Wi-Fi rows are the C6's own binary libraries. The controller is ~31 % of the
gap. Nothing 802.15.4-related is linked.

## Trim candidates, ranked

Deltas are content bytes on the C6 BLE-on image relative to HEAD
(1 882 220 B), each change built alone in its own build directory, unless
marked as an estimate.

| # | change | Δ (B) | status | risk |
|---:|---|---:|---|---|
| 1 | `CONFIG_COMPILER_OPTIMIZATION_SIZE=y` (now `OPTIMIZATION_DEBUG`, `-Og`) | **−157 548** | measured (S3: −131 368) | Low to medium. The build fails as is: `-Werror=format-truncation` at `wifi_sta.cpp:543` (`compose_ap_ssid` snprintf). The measurement downgraded that warning to a non-error. Fix the buffer size instead. Timing and stack use change, so rerun the QEMU self-tests. A side effect is −9 KB DIRAM (238 327 → 229 271 B), which means more heap. |
| 2 | Serve `web/index.html` gzipped (`Content-Encoding: gzip`, gzip at build time) | **−81 502** | computed exactly: 119 766 → 38 264 B (`gzip -9`). `EMBED_FILES` stores the file's bytes 1:1 | Low. Every browser accepts gzip over plain HTTP. Brotli is not an option because browsers accept it only over HTTPS. `stats.cpp::root_get` is the only consumer (the BLE transport does not serve the page). Needs a CMake step and a header. The `bulk_ok` 503 guard still applies, and the transfer gets 3× shorter. |
| 3 | `CONFIG_SECURE_BOOT_ALLOW_SHORT_APP_PARTITION=y` (no `--secure-pad-v2`, pad to 4 KiB only) | **+61 452 of headroom** (content unchanged) | measured: `.bin` 1 882 208 B, no PADDING segment. `espsecure sign-data --version 2` signed it (1 884 160 B + 4 KiB sector) and `verify-signature` passed with a throwaway key | Low to medium. The 64 KiB pad protects against data appended after the image being cache-mapped, which only matters under secure boot, and this project has none. The app-side verifier (`esp_image_format.c:1047-1052`) only requires the signature at a 4 KiB boundary. **Not tested on a device or in QEMU**: run the `NBP_SIG_SELFTEST` QEMU test. `scripts/nbp_sign.py:315` uses "length is a multiple of 64 KiB" to tell signing builds from pre-signing images. That heuristic must change first, or check-release would refuse signed builds. |
| 4 | `CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_SILENT=y` | **−72 868** | measured | Medium. Assertions still abort, but without file, line or expression text. crash_capture still records PC, backtrace and the ELF SHA, so `crash_decode.py` recovers the location. The `/crash` "assert text" field goes empty. |
| 5 | `CONFIG_BT_NIMBLE_LOG_LEVEL_WARNING=y` (now INFO) | **−9 144** | measured | Low. NimBLE info chatter goes; warnings and errors stay. |
| 5b | `CONFIG_BT_NIMBLE_LOG_LEVEL_NONE=y` | −36 084 | measured | Medium. Also removes NimBLE errors from `/log`. |
| 6 | `CONFIG_MDNS_ENABLE_BROWSE=n`, `CONFIG_MDNS_ENABLE_CONSOLE_CLI=n` | **−11 668** | measured | Low. No code calls browse, query or the console (`grep mdns_browse\|mdns_query\|mdns_console`: none). Also **commit `dependencies.lock`**, or pin `espressif/mdns` in `components/mdns_announce/idf_component.yml`. Today CI floats to the newest 1.x, which grew 7.4 KB between 1.11.1 and 1.15.1 with nobody noticing. |
| 7 | `CONFIG_ESP_ERR_TO_NAME_LOOKUP=n` | −7 888 | measured | Low to medium. Logs show `0x101` instead of `ESP_ERR_NO_MEM`. Many of this project's log lines print `esp_err_to_name()`. |
| 8 | `CONFIG_LOG_MAXIMUM_LEVEL_WARN` (with default WARN) | −53 868 | measured | **High.** Compiles out every INFO line, and the `/log` console and the CLAUDE.md debugging recipes depend on them (`wifi_ev`, `ota_verify`, `crash`, `send queue full`...). Lowering only `LOG_DEFAULT_LEVEL` saves 8 B (measured), because `LOG_MAXIMUM_LEVEL_INFO` is pinned in `sdkconfig.defaults`. |
| 9 | IPv6 off (`CONFIG_LWIP_IPV6=n`) | ~−22 000 to −30 000 | *estimate*: the lwIP IPv6 objects in the map total 22 039 B (`nd6` 13 103, `ip6` 4 618, `mld6` 2 070, ...), plus mdns and esp_netif IPv6 paths | Medium. Does not compile as is: `ota.cpp:51` uses `sockaddr_in6` for the STA-side check (switch_out and modbus do the same). The dual-stack listener logic needs `#if LWIP_IPV6` guards. |
| 10 | SoftAP + WPA3-SAE out of a non-NAT build | ~−35 000 (hostap/wpa_auth) and ~−21 000 (SAE) | *estimate* from the map | High. The SoftAP is the provisioning fallback (`NBP_AP_FALLBACK`) and the NAT router. SAE is needed to join WPA3 networks. Listed for completeness only. |
| — | Switch output back to opt-in | −6 960 | measured | It was made default on purpose (a stock image keeps the switch). Not recommended. |
| — | 802.15.4 / OpenThread / Zigbee off | 0 | measured (`CONFIG_IEEE802154_ENABLED=n`: −8 B) | Nothing is linked. |
| — | newlib nano printf | n/a | | IDF 6.0.3 uses picolibc (`CONFIG_LIBC_PICOLIBC=y`), and `libc.a` is only 10 462 B. There is no `NEWLIB_NANO_FORMAT` to turn on. |
| — | Unused mbedTLS features | small | not measured | `libtfpsacrypto.a` is 117 839 B. Only `aes.c` (14 240), `psa_crypto.c` (10 512) and the SAE EC code are large, and WPA2/WPA3 and signing use them. TLS (`libmbedtls.a`) is 54 B, so nothing to gain there. |
| — | ECDSA-V2 instead of RSA-3072 | ~0 | from the format | The V2 signature block is a 4 KiB sector and `--secure-pad-v2` applies to ECDSA-V2 too. Changing the key type is a USB reflash of every device (`docs/signing.md`). No flash gained. |

Combined builds (measured, C6):

| set | content (B) | headroom to `0x1D0000` | Δ |
|---|---:|---:|---:|
| HEAD | 1 882 220 | 18 324 | |
| `-Os` + mdns browse/CLI off + NimBLE log WARNING (low risk) | 1 705 580 | 194 964 | −176 640 |
| `-Os` + silent asserts + NimBLE log NONE + no err-to-name + mdns off | 1 606 696 | 293 848 | −275 524 |

Gzipping the dashboard (−81 502 B) and the short pad (+61 452 B of
headroom) add to either row. The low-risk set plus those two gives about
**338 KB** of headroom (*estimate*: the sum of measured parts, not built
together).

### A larger slot

- The 4 MB layout is already as large as it gets for two OTA slots and a core
  dump: `0x20000 + 2 × 0x1E0000 + 0x20000 (coredump) = 0x400000`. Dropping the
  128 KB coredump partition would give `0x1F0000` slots, 64 KiB more each.
- **An app OTA cannot change it.** The table at `0x8000` is only written by a
  serial flash. Rewriting it from the app would need
  `SPI_FLASH_DANGEROUS_WRITE`, a power cut in the middle bricks the board, and
  `ota_1` would move under a running or rollback image. Every field device
  would have to be reflashed over USB, the same as for a key rotation. Not
  a lever for devices in the field. New installs could use it, but then
  two layouts are in the field.
- Larger-flash C6 modules (8 MB parts) would need a second partition table and
  release variant. Same objection.

## Findings beyond size

1. **CI does not pin component versions.** `dependencies.lock` is untracked,
   so every CI run resolves `espressif/mdns ^1.3` and `esp-nimble-cpp` afresh.
   The release C6 image is 7 376 B larger than the image the main checkout
   builds. Builds are not reproducible from the tag.
2. **A build with `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=n` does not
   compile.** `ota_verify.cpp:266` uses `CONFIG_NBP_OTA_VERIFY_MAX_BOOTS`,
   whose Kconfig menu `depends on BOOTLOADER_APP_ROLLBACK_ENABLE`.
3. **`-Os` breaks the build** with `-Werror=format-truncation` in
   `wifi_sta.cpp:543` (`compose_ap_ssid`: GCC reports 12..42 bytes of output
   into `char ssid[33]`, a long hostname plus `-setup-XXXX`). Under `-Og` GCC
   does not run the range analysis that finds it. A long hostname truncates
   the SSID today. An SSID is at most 32 bytes anyway, so the fix is to cut
   the hostname part on purpose (for example `%.20s`). That keeps the
   per-device suffix and silences the warning without muting it.

## Commands

Environment (each script sources it):

```bash
export IDF_PYTHON_ENV_PATH=~/.espressif/python_env/idf6.0_py3.14_env
. ~/dev/esp/idf6.0.3/export.sh
export NBP_SIGNING_KEY=none
```

Trees (worktrees, so that the component manager resolves dependencies afresh,
as CI does; nanopb and the `etc/idf-devtools` submodule symlinked from the
main checkout):

```bash
git worktree add --detach /tmp/nbp-fb/v028 v0.2.8
git worktree add --detach /tmp/nbp-fb/headwt HEAD   # ce15efa
ln -s ~/dev/ha/nimble-ble-proxy/components/api_proto/nanopb <tree>/components/api_proto/nanopb
rmdir <tree>/etc/idf-devtools && ln -s ~/dev/ha/nimble-ble-proxy/etc/idf-devtools <tree>/etc/idf-devtools
```

Build and size (`/tmp/nbp-fb/scripts/b.sh SRC TARGET NAME [FRAGMENT]`):

```bash
d=/tmp/nbp-fb/$NAME
defs="sdkconfig.defaults"            # + ";$FRAGMENT" for experiments
variant=$TARGET-ble-on               # "custom" for experiments (stock-variant check)
idf.py -B $d -DIDF_TARGET=$TARGET -DSDKCONFIG=$d/sdkconfig \
  -DSDKCONFIG_DEFAULTS="$defs" -DNBP_VARIANT=$variant reconfigure
ninja -C $d -j 4 all
idf.py -B $d -DSDKCONFIG=$d/sdkconfig size
idf.py -B $d -DSDKCONFIG=$d/sdkconfig size-components --format csv --output-file $d.comp.csv
idf.py -B $d -DSDKCONFIG=$d/sdkconfig size-files      --format csv --output-file $d.files.csv
```

The `-Os` builds also passed
`-DCMAKE_C_FLAGS=-Wno-error=format-truncation -DCMAKE_CXX_FLAGS=-Wno-error=format-truncation`
(finding 3).

Unpadded content (`/tmp/nbp-fb/scripts/end.sh`): `.bin` size minus the
PADDING segment and its 8-byte header, from
`python -m esptool --chip <t> image-info <bin>`.

Per-archive diff: `/tmp/nbp-fb/scripts/cmp.py OLD.comp.csv NEW.comp.csv`
(image bytes = total − `.bss` − `.noinit` − `.rtc_noinit` − `.rtc_reserved`).

Experiment fragments (`/tmp/nbp-fb/frag/`), each added after
`sdkconfig.defaults`:

| name | content |
|---|---|
| nosign | `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT=n`, `CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT=n` |
| norb | `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=n` (does not compile) |
| nosw | `CONFIG_NBP_SWITCH_OUT=n` |
| os | `CONFIG_COMPILER_OPTIMIZATION_SIZE=y` |
| asilent | `CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_SILENT=y` |
| nimblewarn / nimblelog | `CONFIG_BT_NIMBLE_LOG_LEVEL_WARNING=y` / `_NONE=y` |
| errname | `CONFIG_ESP_ERR_TO_NAME_LOOKUP=n` |
| logwarn | `CONFIG_LOG_DEFAULT_LEVEL_WARN=y` |
| logmaxwarn | `CONFIG_LOG_DEFAULT_LEVEL_WARN=y`, `CONFIG_LOG_MAXIMUM_LEVEL_WARN=y` |
| no154 | `CONFIG_IEEE802154_ENABLED=n` |
| mdns | `CONFIG_MDNS_ENABLE_BROWSE=n`, `CONFIG_MDNS_ENABLE_CONSOLE_CLI=n` |
| noipv6 | `CONFIG_LWIP_IPV6=n` (does not compile) |
| short | `CONFIG_SECURE_BOOT_ALLOW_SHORT_APP_PARTITION=y` |
| rec | os + mdns + nimblewarn |
| combo | os + asilent + nimblelog + errname + mdns |

Dashboard sizes: `wc -c web/index.html`, `gzip -9 -c web/index.html | wc -c`,
`git show v0.2.8:web/index.html | wc -c`.

Signing with the short pad (throwaway key, deleted afterwards):
`espsecure sign-data --version 2 --keyfile k.pem --output s.bin <bin>`, then
`espsecure verify-signature --version 2 --keyfile k.pem s.bin`.
