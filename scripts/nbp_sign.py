#!/usr/bin/env python3
"""App image signing helpers for nimble-ble-proxy (Secure Boot V2 format, RSA-3072).

The firmware is built with CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT +
CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT (S3/C3/C6). A running app
accepts an OTA image only if signature block 0 of the new image carries the
same public key as block 0 of the running image and the RSA-PSS signature
over the image verifies. See docs/signing.md and the "App image signing"
section of CLAUDE.md.

Subcommands:

    nbp_sign.py fingerprint KEY.pem          sha256 of the SBv2 public key
                                             (what the device reports as
                                             key_sha256 in GET /update)
    nbp_sign.py info IMAGE.bin               signed? by which key?
    nbp_sign.py build-sign --bin B --key K   post-build step (CMake): sign B
                                             in place with K, keep B's
                                             unsigned copy as *-unsigned.bin
    nbp_sign.py selftest-blob ...            test vectors for the QEMU test

Needs the ESP-IDF Python env (cryptography, espsecure, esptool).

Private keys never pass through argv: build-sign reads the key file, the
release script (release_sign.py) reads its key from the macOS Keychain. The
signature is computed in this process and espsecure assembles and checks the
signature block from the public key plus the precomputed signature.
"""
import argparse
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
import zlib

SECTOR = 4096
SIG_BLOCK_SIZE = 1216
SIG_MAGIC = 0xE7
SIG_VERSION_RSA = 0x02
CRC_SIGN_BLOCK_LEN = 1196
# ets_secure_boot_sig_block_t: magic(1) version(1) reserved(2) digest(32),
# then the RSA key {n[384], e u32, rinv[384], mdash u32} = 776 bytes.
KEY_OFF = 36
KEY_LEN = 776

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ---------------------------------------------------------------------------
# Pinned release key. Set by Fab after `scripts/release_sign.py keygen`
# (it prints the line to paste). release_sign.py refuses to sign while this
# is None, and refuses a keys/release.pub.pem whose fingerprint differs.
# The device reports the fingerprint of the key it trusts in GET /update
# (`key_sha256`); a release-flashed device must show exactly this value.
RELEASE_KEY_SHA256 = None
RELEASE_PUBKEY_PEM = os.path.join(REPO, 'keys', 'release.pub.pem')
# ---------------------------------------------------------------------------


def die(msg, code=1):
    print(f'nbp_sign: error: {msg}', file=sys.stderr)
    sys.exit(code)


def _crypto():
    try:
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import padding, rsa, utils
    except ImportError:
        die('python package "cryptography" missing: run inside the ESP-IDF env')
    return hashes, serialization, padding, rsa, utils


def load_private_key(data: bytes):
    _, serialization, _, rsa, _ = _crypto()
    if b'-----BEGIN' in data:
        key = serialization.load_pem_private_key(data, password=None)
    else:
        key = serialization.load_der_private_key(data, password=None)
    if not isinstance(key, rsa.RSAPrivateKey) or key.key_size != 3072:
        die('signing key must be RSA-3072 (Secure Boot V2 RSA scheme)')
    return key


def load_public_key_file(path: str):
    _, serialization, _, rsa, _ = _crypto()
    data = open(path, 'rb').read()
    if b'PRIVATE KEY' in data:
        return load_private_key(data).public_key()
    key = serialization.load_pem_public_key(data)
    if not isinstance(key, rsa.RSAPublicKey) or key.key_size != 3072:
        die(f'{path}: not an RSA-3072 public key')
    return key


def public_pem(pub) -> bytes:
    _, serialization, _, _, _ = _crypto()
    return pub.public_bytes(serialization.Encoding.PEM,
                            serialization.PublicFormat.SubjectPublicKeyInfo)


def key_struct(pub) -> bytes:
    """The 776-byte ets_rsa_pubkey_t the signature block carries (espsecure
    _get_sbv2_rsa_primitives + generate_rsa_signature_block)."""
    nums = pub.public_numbers()
    n, e = nums.n, nums.e
    mdash = (-pow(n, -1, 1 << 32)) & 0xFFFFFFFF
    rinv = (1 << (3072 * 2)) % n
    return (n.to_bytes(384, 'big')[::-1] + struct.pack('<I', e) +
            rinv.to_bytes(384, 'big')[::-1] + struct.pack('<I', mdash))


def fingerprint(pub) -> str:
    """sha256 of the key struct: espsecure digest-sbv2-public-key, and the
    digest the app compares (secure_boot_signatures_app.c)."""
    return hashlib.sha256(key_struct(pub)).hexdigest()


def key_label(fp: str) -> str:
    if RELEASE_KEY_SHA256 and fp == RELEASE_KEY_SHA256:
        return 'RELEASE key'
    return 'dev/test key (not the release key)'


# --- image parsing ----------------------------------------------------------

def image_len(data: bytes) -> int:
    """Length of the ESP app image (header, segments, checksum, appended
    SHA-256), as esp_image_format.c computes it; excludes signature padding."""
    if len(data) < 24 or data[0] != 0xE9:
        raise ValueError('not an ESP app image (magic != 0xE9)')
    nseg = data[1]
    if nseg == 0 or nseg > 16:
        raise ValueError(f'bad segment count {nseg}')
    hash_appended = data[23] == 1
    pos = 24
    for _ in range(nseg):
        if pos + 8 > len(data):
            raise ValueError('truncated segment header')
        _, seg_len = struct.unpack_from('<II', data, pos)
        pos += 8 + seg_len
        if pos > len(data):
            raise ValueError('truncated segment')
    pos = (pos + 1 + 15) & ~15  # checksum byte, 16-byte aligned
    if hash_appended:
        pos += 32
    if pos > len(data):
        raise ValueError('truncated image')
    return pos


def sig_offset(data: bytes) -> int:
    n = image_len(data)
    return (n + SECTOR - 1) & ~(SECTOR - 1)


def parse_block(blk: bytes):
    """Return dict for a valid RSA SBv2 block, else None."""
    if len(blk) < SIG_BLOCK_SIZE or blk[0] != SIG_MAGIC:
        return None
    crc = struct.unpack_from('<I', blk, CRC_SIGN_BLOCK_LEN)[0]
    if zlib.crc32(blk[:CRC_SIGN_BLOCK_LEN]) & 0xFFFFFFFF != crc:
        return None
    if blk[1] != SIG_VERSION_RSA:
        return {'version': blk[1], 'key_sha256': None}
    return {
        'version': blk[1],
        'digest': blk[4:36].hex(),
        'key_sha256': hashlib.sha256(blk[KEY_OFF:KEY_OFF + KEY_LEN]).hexdigest(),
    }


def split_signed(data: bytes):
    """(content, block0) where content is what was (or will be) signed.
    block0 is None for an unsigned image."""
    off = sig_offset(data)
    if len(data) >= off + SIG_BLOCK_SIZE:
        b = parse_block(data[off:off + SIG_BLOCK_SIZE])
        if b is not None:
            return data[:off], b
    return data, None


def pad_sector(content: bytes) -> bytes:
    if len(content) % SECTOR:
        content += b'\xff' * (SECTOR - len(content) % SECTOR)
    return content


def _espsecure():
    return [sys.executable, '-m', 'espsecure']


def sign(content: bytes, priv) -> bytes:
    """Sign `content` (unsigned image) with an in-memory private key. espsecure
    builds the block from the public key + precomputed signature and checks
    the signature itself; the private key never reaches a file."""
    hashes, _, padding, _, utils = _crypto()
    content = pad_sector(content)
    if sig_offset(content) != len(content):
        die('image length and signature offset disagree; refusing to sign')
    digest = hashlib.sha256(content).digest()
    sig = priv.sign(digest,
                    padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=32),
                    utils.Prehashed(hashes.SHA256()))
    with tempfile.TemporaryDirectory(prefix='nbp-sign-') as td:
        src = os.path.join(td, 'unsigned.bin')
        out = os.path.join(td, 'signed.bin')
        pubf = os.path.join(td, 'pub.pem')
        sigf = os.path.join(td, 'sig.bin')
        open(src, 'wb').write(content)
        open(pubf, 'wb').write(public_pem(priv.public_key()))
        open(sigf, 'wb').write(sig)
        r = subprocess.run(_espsecure() + ['sign-data', '--version', '2',
                                           '--pub-key', pubf, '--signature', sigf,
                                           '-o', out, src],
                           capture_output=True, text=True)
        if r.returncode != 0:
            die(f'espsecure sign-data failed:\n{r.stdout}{r.stderr}')
        return open(out, 'rb').read()


def verify(path: str, pub_pem_path: str, expect_fp: str) -> None:
    """Independent checks of a signed image: espsecure verify-signature against
    the public key, and block 0's key equals the expected fingerprint (the
    device only looks at block 0)."""
    r = subprocess.run(_espsecure() + ['verify-signature', '--version', '2',
                                       '--keyfile', pub_pem_path, path],
                       capture_output=True, text=True)
    if r.returncode != 0:
        die(f'{path}: espsecure verify-signature FAILED:\n{r.stdout}{r.stderr}')
    data = open(path, 'rb').read()
    _, b0 = split_signed(data)
    if b0 is None or b0['key_sha256'] != expect_fp:
        die(f'{path}: signature block 0 key is {b0 and b0["key_sha256"]}, '
            f'expected {expect_fp}')


# --- publish gate -------------------------------------------------------------
# An image built with SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT but published
# unsigned locks every device that installs it out of OTA (no trusted key)
# until a USB flash. The Pages workflow and make_updates_manifest.py stage or
# list an image only if check_release_image() returns None.

CHIP_ID_ESP32 = 0  # esp_image_header_t.chip_id; classic ESP32 builds unsigned
PART_TABLE_OFF = 0x8000


def app_in_flash_image(data: bytes) -> bytes:
    """The ota_0 (else factory) app of a merged *-flash.bin, via its
    partition table at 0x8000. Raises ValueError when there is none."""
    for i in range(0, 0xC00, 32):
        e = data[PART_TABLE_OFF + i:PART_TABLE_OFF + i + 32]
        if len(e) < 32 or e[:2] != b'\xaa\x50':
            break
        ptype, subtype, off, size = e[2], e[3], *struct.unpack_from('<II', e, 4)
        if ptype == 0 and subtype in (0x10, 0x00):
            if len(data) <= off or data[off] != 0xE9:
                raise ValueError(f'no app image at 0x{off:x}')
            return data[off:off + size]
    raise ValueError('no ota_0/factory partition in the partition table')


def verify_block_rsa(content: bytes, blk: bytes) -> bool:
    """RSA-PSS check of signature block 0 over `content`, as espsecure
    verify-signature --version 2 does (n and the signature are stored LE)."""
    hashes, _, padding, rsa, utils = _crypto()
    from cryptography.exceptions import InvalidSignature
    digest = hashlib.sha256(content).digest()
    if blk[4:36] != digest:
        return False
    n = int.from_bytes(blk[KEY_OFF:KEY_OFF + 384], 'little')
    e = struct.unpack_from('<I', blk, KEY_OFF + 384)[0]
    sig = blk[KEY_OFF + KEY_LEN:KEY_OFF + KEY_LEN + 384][::-1]
    pub = rsa.RSAPublicNumbers(e, n).public_key()
    try:
        pub.verify(sig, digest,
                   padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=32),
                   utils.Prehashed(hashes.SHA256()))
    except InvalidSignature:
        return False
    return True


def check_release_image(data: bytes, expect_fp) -> 'str | None':
    """None if `data` (an *-ota.bin app image, or a *-flash.bin merged image)
    may be published; else the reason it may not. Fails closed: an unparsable
    image, a missing pinned fingerprint or a missing crypto library refuse."""
    try:
        if data[PART_TABLE_OFF:PART_TABLE_OFF + 2] == b'\xaa\x50':  # merged flash image
            data = app_in_flash_image(data)
        if len(data) < 24 or data[0] != 0xE9:
            return 'not an ESP app image'
        chip_id = struct.unpack_from('<H', data, 12)[0]
        if chip_id == CHIP_ID_ESP32:
            return None  # signing is off on classic ESP32 (sdkconfig.defaults.esp32)
        if not expect_fp:
            return 'no release key pinned (RELEASE_KEY_SHA256 is None)'
        off = sig_offset(data)
        blk = data[off:off + SIG_BLOCK_SIZE]
        b0 = parse_block(blk)
        if b0 is None:
            return 'UNSIGNED (no valid signature block)'
        if b0['key_sha256'] != expect_fp:
            return f'signed with key {b0["key_sha256"]}, not the release key {expect_fp}'
        if not verify_block_rsa(data[:off], blk):
            return 'signature block does not verify (digest or RSA)'
    except ValueError as e:
        return str(e)
    return None


def assert_outside_repo(path: str, what: str):
    p = os.path.realpath(path)
    if p == REPO or p.startswith(REPO + os.sep):
        die(f'{what} {path} lies inside the repository; private keys must live outside it')


# --- subcommands -------------------------------------------------------------

def cmd_fingerprint(a):
    print(fingerprint(load_public_key_file(a.key)))


def cmd_info(a):
    data = open(a.image, 'rb').read()
    content, b0 = split_signed(data)
    print(f'image_len={image_len(data)} sig_offset={sig_offset(data)} file={len(data)}')
    if b0 is None:
        print('UNSIGNED (no valid signature block at the signature offset)')
        return 2
    fp = b0['key_sha256']
    print(f'signed: block0 key_sha256={fp} ({key_label(fp)})')
    if a.pub:
        verify(a.image, a.pub, fingerprint(load_public_key_file(a.pub)))
        print(f'signature verifies against {a.pub}')
    return 0


def cmd_build_sign(a):
    key_path = os.path.expanduser(a.key)
    assert_outside_repo(key_path, 'signing key')
    priv = load_private_key(open(key_path, 'rb').read())
    fp = fingerprint(priv.public_key())
    if RELEASE_KEY_SHA256 and fp == RELEASE_KEY_SHA256:
        die('the build-time key is the RELEASE key. Release images are signed only by '
            'scripts/release_sign.py from the Keychain; keep the release key off disk.')
    data = open(a.bin, 'rb').read()
    content, b0 = split_signed(data)
    if b0 is not None:
        # Rerun without a regenerated .bin (e.g. key changed): strip the old sector.
        print(f'nbp_sign: {a.bin} already signed (key {b0["key_sha256"][:16]}), re-signing')
    unsigned_path = a.bin[:-4] + '-unsigned.bin' if a.bin.endswith('.bin') else a.bin + '.unsigned'
    open(unsigned_path, 'wb').write(content)
    signed = sign(content, priv)
    tmp = a.bin + '.tmp'
    open(tmp, 'wb').write(signed)
    with tempfile.TemporaryDirectory(prefix='nbp-pub-') as td:
        pubf = os.path.join(td, 'pub.pem')
        open(pubf, 'wb').write(public_pem(priv.public_key()))
        verify(tmp, pubf, fp)
    os.replace(tmp, a.bin)
    msg = (f'App image signed: {os.path.basename(a.bin)} key_sha256={fp} ({key_label(fp)}). '
           f'Devices running an image signed by a different key refuse it over OTA; '
           f'flash over USB instead (docs/signing.md).')
    print(msg)
    if a.stamp:
        open(a.stamp, 'w').write(fp + '\n')


def cmd_check_release(a):
    fp = a.fp or RELEASE_KEY_SHA256
    bad = 0
    for path in a.images:
        why = check_release_image(open(path, 'rb').read(), fp)
        print(f'{"OK     " if why is None else "REFUSED"} {path}' + (f': {why}' if why else ''))
        bad += why is not None
    return 1 if bad else 0


def cmd_selftest_blob(a):
    """Test vectors for components/ota/sig_selftest.cpp. Layout (LE):
       0  'NBPSIGT1'
       8  u32 signed_len   (offset of the signature sector in the signed image)
       12 u32 patch_count
       16 patches: u32 off, u32 len, bytes (padded to 4)   -> the tampered image
       4096: signature sector by an untrusted key over the SAME content
    The tampered image is the signed image with one byte of the app
    description flipped and the image checksum and appended SHA-256 fixed up,
    so only the RSA signature can catch it."""
    data = open(a.bin, 'rb').read()
    content, b0 = split_signed(data)
    if b0 is None:
        die(f'{a.bin} is not signed')
    wrong = load_private_key(open(a.wrong_key, 'rb').read())
    if fingerprint(wrong.public_key()) == b0['key_sha256']:
        die('wrong key equals the signing key')
    wrong_signed = sign(content, wrong)
    wrong_sector = wrong_signed[len(content):]
    assert len(wrong_sector) == SECTOR

    n = image_len(data)
    if data[23] != 1:
        die('image has no appended SHA-256; tamper vector needs it')
    # esp_app_desc_t sits at the start of the first segment's data (offset 24+8):
    # magic(4) secure_version(4) reserved(8) version[32]. Flip version[0].
    desc = 24 + 8
    if struct.unpack_from('<I', data, desc)[0] != 0xABCD5432:
        die('app description not where expected')
    tpos = desc + 16
    flip = 0x01
    patched = bytearray(content)
    patched[tpos] ^= flip
    cks = n - 32 - 1
    patched[cks] ^= flip  # XOR checksum over segment data
    patched[n - 32:n] = hashlib.sha256(bytes(patched[:n - 32])).digest()
    patches = []
    for off, ln in ((tpos, 1), (cks, 1), (n - 32, 32)):
        patches.append((off, bytes(patched[off:off + ln])))
    blob = bytearray(b'NBPSIGT1' + struct.pack('<II', len(content), len(patches)))
    for off, b in patches:
        blob += struct.pack('<II', off, len(b)) + b + b'\0' * ((-len(b)) % 4)
    if len(blob) > SECTOR:
        die('blob header too large')
    blob += b'\xff' * (SECTOR - len(blob))
    blob += wrong_sector
    open(a.out, 'wb').write(blob)
    print(f'selftest blob: {a.out} signed_len={len(content)} '
          f'trusted={b0["key_sha256"][:16]} wrong={fingerprint(wrong.public_key())[:16]}')


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('fingerprint')
    s.add_argument('key')
    s.set_defaults(fn=cmd_fingerprint)
    s = sub.add_parser('info')
    s.add_argument('image')
    s.add_argument('--pub', help='public (or private) PEM to verify against')
    s.set_defaults(fn=cmd_info)
    s = sub.add_parser('build-sign')
    s.add_argument('--bin', required=True)
    s.add_argument('--key', required=True)
    s.add_argument('--stamp')
    s.set_defaults(fn=cmd_build_sign)
    s = sub.add_parser('check-release',
                       help='may these images be published? (exit 1 if any may not)')
    s.add_argument('images', nargs='+')
    s.add_argument('--fp', help='expected key fingerprint (default: RELEASE_KEY_SHA256)')
    s.set_defaults(fn=cmd_check_release)
    s = sub.add_parser('selftest-blob')
    s.add_argument('--bin', required=True)
    s.add_argument('--wrong-key', required=True)
    s.add_argument('-o', '--out', required=True)
    s.set_defaults(fn=cmd_selftest_blob)
    a = p.parse_args()
    sys.exit(a.fn(a) or 0)


if __name__ == '__main__':
    main()
