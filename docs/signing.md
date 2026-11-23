# Firmware signing: release checklist

A device accepts an app image over OTA (`POST /update`) only if the image is
signed with the same key as the image it is running. CI builds unsigned
images; Fab signs releases on his Mac with a key that lives in the macOS
Keychain. The mechanism, its limits and the source citations are in
CLAUDE.md, section "App image signing".

| Key | Where it lives | Who uses it |
| --- | --- | --- |
| Release key (RSA-3072) | macOS Keychain, service `nbp-release-signing-key`; offline backup | `scripts/release_sign.py` |
| Release public key | `keys/release.pub.pem` (in the repo), fingerprint pinned as `RELEASE_KEY_SHA256` in `scripts/nbp_sign.py` | release script, to verify |
| Dev key (RSA-3072) | `~/.config/nbp-signing/dev-key.pem` (per developer, never in the repo) | every local `idf.py build` |

A device reports the key it trusts: `curl http://<device>/update` returns
`key_sha256`. A release device shows `RELEASE_KEY_SHA256`; a dev device shows
the dev key's fingerprint (`scripts/nbp_sign.py fingerprint ~/.config/nbp-signing/dev-key.pem`).

All commands run inside the ESP-IDF 6.0.3 Python environment:

```sh
export IDF_PYTHON_ENV_PATH=~/.espressif/python_env/idf6.0_py3.14_env
. ~/dev/esp/idf6.0.3/export.sh
```

## 1. Generate the release key (once)

```sh
scripts/release_sign.py keygen
```

This stores the key in the login Keychain (via `security -i`, so the key never
appears in a process argument list), writes `keys/release.pub.pem` and prints
a line like `RELEASE_KEY_SHA256 = '…'`. Then:

1. Paste that line into `scripts/nbp_sign.py`.
2. Commit `keys/release.pub.pem` and the pinned fingerprint.
3. Make the offline backup (next section) before you sign anything.

The first read from the Keychain shows a macOS prompt for `security`. Choose
"Allow" for one run, or "Always Allow" if you accept that any process running
as you can then read the key without a prompt.

## 2. Back the key up

```sh
scripts/release_sign.py export-key --out /Volumes/<USB>/nbp-release-key.pem
```

The script refuses a path inside the repository. Keep the file offline (USB
stick in a drawer, or a password manager's file attachment). If the key is
lost, release devices cannot be updated over OTA again; each one then needs a
USB flash (section 6).

## 3. Sign and upload a release

1. Bump `VERSION` in `CMakeLists.txt`, then push the tag `vX.Y.Z`. CI builds
   and creates a **draft** release with the **unsigned** assets (job
   `release` in `build.yml`). A draft is not public.
2. Sign them once CI has finished. Pages and the web flasher keep serving
   the previous release until step 4 publishes this one.
3. Dry run (downloads, signs, verifies, uploads nothing):

   ```sh
   scripts/release_sign.py sign vX.Y.Z
   ```

   For every `esp32s3`, `esp32c3` and `esp32c6` asset it:
   - checks that merging the CI parts (bootloader, partition table, otadata,
     `flash_args`) with the unsigned app reproduces the CI `*-flash.bin` byte
     for byte;
   - signs `*-ota.bin` with the Keychain key;
   - verifies it with `espsecure verify-signature` against `keys/release.pub.pem`;
   - rebuilds `*-flash.bin` with the signed app and checks that the app region
     is the signed app.

   `esp32` (classic) assets are skipped: that target does not check signatures
   (section 7).
4. Upload. The script prints the plan, then asks you to type the tag:

   ```sh
   scripts/release_sign.py sign vX.Y.Z --upload
   ```

   It runs `gh release upload vX.Y.Z … --clobber`, publishes the draft
   (`gh release edit vX.Y.Z --draft=false`), then `gh workflow run pages.yml`.
   Write the release notes on the draft before this step or after it.
5. Pages publishes the web flasher images and the dashboard's `updates.json`.
   It no longer runs after a tag build. Its gate (`nbp_sign.py check-release`)
   drops every image that is not signed with the pinned release key (classic
   ESP32 excepted), so a run before step 4 publishes nothing for S3/C3/C6.
   Check the run log for `REFUSED` lines.
6. Check one device: after the OTA, `curl http://<device>/update` shows the
   release fingerprint.

Running `sign` again is safe: assets already signed with the release key are
skipped. An asset signed with another key stops the script.

### Hazard: unsigned images in the window before signing

A device that installs an **unsigned** image from this firmware onward has no
trusted key. It refuses every OTA (`403`, `no_trusted_key`) until it is flashed
over USB. This affects:

- the unsigned CI `*-ota.bin`, downloaded from the release page and OTA'd
  by hand before step 4 (the dashboard only offers what Pages lists, and
  Pages lists only signed images);
- the unsigned CI `*-flash.bin`, downloaded from the release page and
  flashed by hand (the web flasher only serves what the Pages gate passed);
- a local build made without a signing key (the build prints `UNSIGNED`).

Devices still running firmware from before signing (0.2.8 and older) check no
signatures. They accept whichever image they get next, signed or not. Give
them the **signed** release; from then on they accept only images signed with
that key.

## 4. Local development

`idf.py build` signs `build/nimble_ble_proxy.bin` with the dev key and keeps
`nimble_ble_proxy-unsigned.bin` next to it. `idf.py flash` and the usual OTA
work unchanged on dev devices:

```sh
curl -H 'X-NBP-Confirm: yes' --data-binary @build/nimble_ble_proxy.bin http://<device>/update
```

Create the dev key once per machine:

```sh
mkdir -p ~/.config/nbp-signing && chmod 700 ~/.config/nbp-signing
espsecure generate-signing-key --version 2 --scheme rsa3072 ~/.config/nbp-signing/dev-key.pem
```

Use another key with `-DNBP_SIGNING_KEY=<pem>` or the environment variable
`NBP_SIGNING_KEY`. `none` leaves the image unsigned. The build refuses the
release key.

## 5. Moving a device between dev and release keys

A device trusts the key of the image it runs. A release device refuses a dev
build (`403 untrusted_key`), and a dev device refuses a release. To switch
keys, flash over USB (section 6). OTA cannot switch keys.

## 6. Recover a device that refuses images

The bootloader does not check signatures and no eFuse is involved, so USB
always works:

- Dev build: `idf.py -p /dev/cu.usbmodemXXXX flash` (after a flash, reboot and
  check the compile time, see CLAUDE.md).
- Release, keeping settings: write the signed app to `ota_0` and reset
  otadata so the bootloader picks `ota_0`:
  `esptool --chip esp32s3 write-flash 0xf000 ota_data_initial-<t>-<v>.bin 0x20000 nimble_ble_proxy-<t>-<v>-ota.bin`.
- Release, from scratch: the signed `*-flash.bin` with the web flasher, or
  `esptool --chip esp32s3 write-flash 0x0 nimble_ble_proxy-esp32s3-ble-on-flash.bin`.
  The merged image covers 0x0 up to the end of the app with the gaps filled
  with 0xFF, so it **erases NVS** (WiFi credentials and all settings).

`GET /update` and the `OTA REFUSED: …` line in `/log` say why an image was
refused: `unsigned`, `untrusted_key` (image key vs trusted key, first 8 bytes
of each), `bad_signature` (modified after signing), `no_trusted_key` (the
device runs an unsigned image), `corrupt` (hash mismatch) or `not_an_image`.

## 7. Rotate the release key

In this mode (`CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT`) a device trusts only
the key in signature block 0 of its running image, and IDF compares block 0
only. An image signed with a new key is refused by every device that runs an
image signed with the old one. **Rotation therefore means a USB flash of every
release device.** Steps:

1. `scripts/release_sign.py keygen --force` (overwrites the Keychain item and
   `keys/release.pub.pem`; back up the old key first if devices still use it).
2. Pin the new fingerprint, commit, back it up.
3. Sign the next release with it; USB-flash each device with that release.

If the old key leaked, anyone holding it can still update devices that run
old-key images until they are reflashed.

## Classic ESP32

Signing is off for `esp32` (sdkconfig.defaults.esp32). Its RSA scheme needs
chip revision 3 or later, and raising the minimum revision would stop the
image from booting on older chips. The remaining scheme (Secure Boot V1, ECDSA
P-256) embeds a fixed public key in the app at build time: CI would have to
embed the release public key, dev builds the dev key, and the release script
would need a second (ECDSA) release key and `sign-data --version 1`. Not done
yet.
