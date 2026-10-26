# Signed-boot trust anchor

`boot-signing.json` is the **development** RSA-2048 private key used by
`tools/scos_sign.py` when `tools/makedisk.py` packs an image. The matching
public modulus is compiled into the UEFI loader as
`boot/uefi/trust_anchor.h`.

**This private key is published in the repository on purpose.** Without it a
fresh clone could not produce a bootable signed image. Its signature therefore
proves *integrity* (the boot files have not changed since they were packed),
not *publisher identity*. Anyone can re-sign a modified image with this key.

For a real deployment:

```sh
python3 tools/scos_keys.py --generate   # new keypair, rewrites trust_anchor.h + this file
python3 tools/scos_keys.py --verify
# move boot-signing.json off the repository; sign on a trusted machine:
export SCOS_SIGN_KEY=/secure/path/boot-signing.json
make -j4 && make gpu-modules && make -j4 && make dist
# enrol the resulting BOOTX64.EFI in firmware Secure Boot
```

Keep the private key out of any public tree, keep a recovery copy, and remember
that rotating it invalidates every previously signed image. Firmware Secure
Boot enrolment is what authenticates the loader that holds the public key;
without it an attacker can replace the loader itself.

See [the security design](../docs/migration/SECURITY.md) for the full threat
model.
