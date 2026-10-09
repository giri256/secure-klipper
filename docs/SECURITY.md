# Security design

This document describes what Secure Klipper encrypts, how, and where the limits are. The code is in [`klipper/historyvault.cpp`](../klipper/historyvault.cpp) (cryptography) and [`klipper/historymodel.cpp`](../klipper/historymodel.cpp) (which entries are encrypted, and when).

## Goal and threat model

Clipboard history quietly collects secrets: passwords pasted from a manager, API tokens, one-time codes, recovery keys, private messages. Stock Klipper stores all of it in plaintext in `~/.local/share/klipper/history3.sqlite`, usually world-readable.

**Secure Klipper protects history at rest.** Someone who gets a copy of your home folder should not be able to read anything but the newest few entries. That copy could come from a stolen or decommissioned disk, a backup, a synced or shared home folder, or another local user.

**It does not protect a running, unlocked session** from malware running as your user (see [Limits](#limits)).

## Behaviour

- Encryption is off until you choose a password in the popup.
- After that, only the newest `PlaintextHistoryItems` entries (default 5) are stored readable. When a new copy pushes an entry out of that window, it is encrypted on disk immediately, **including while the vault is locked**.
- **Locked:** encrypted entries are hidden, and the popup shows how many there are plus a password field.
- **Unlocked:** encrypted entries are decrypted **in memory only**; on disk they stay encrypted. Selecting one moves it to the top, so it becomes plaintext again as a recent entry, and the entry it displaces is encrypted.
- **The vault locks:**
  - on demand;
  - `VaultAutoLockMinutes` after unlocking (default 10);
  - when the screen locks (`org.freedesktop.ScreenSaver.ActiveChanged`);
  - whenever plasmashell starts.
- Encrypted entries are never returned by Klipper's D-Bus history API, can't be edited, and are skipped by the history cycler while locked.

## Key hierarchy

Everything uses libsodium. There are no custom primitives.

```
password ──Argon2id──────────────────────────────▶ KEK                            (never stored)
            crypto_pwhash, 16-byte random salt,
            OPSLIMIT/MEMLIMIT_MODERATE (3 passes, 256 MiB, ~1 s)

KEK ──XChaCha20-Poly1305──────────────────────────▶ wrapped X25519 secret key     (stored)
       random nonce, AD = "…secretkey" ‖ pk ‖ salt ‖ KDF limits

X25519 public key pk                                                               (stored in clear)

pk ──crypto_box_seal──────────────────────────────▶ sealed per-entry key          (stored per blob)

per-entry key ──XChaCha20-Poly1305────────────────▶ entry data                    (stored)
                 random 24-byte nonce, AD = "…item" ‖ purpose ‖ row id
```

### Why it is built this way

- **Public-key sealing.** Entries age out at any moment, usually while the vault is locked. A symmetric data key would have to stay in memory while locked, which defeats locking. Sealing to a public key needs no secret; only opening needs the secret key, which exists in usable form only while unlocked.
- **A random key per entry.** Each blob is independent. Changing the password only re-wraps the secret key, so it's instant regardless of history size.
- **Argon2id at 256 MiB.** The password is the only secret. With a stolen database, an attacker's only option is guessing offline, and each guess costs about a second and 256 MiB of memory. The derivation runs on a worker thread, so the popup stays responsive.
- **Integrity and binding.**
  - Every blob is authenticated with Poly1305.
  - The associated data binds each blob to its purpose (`meta` or `payload`) and its row id, so blobs can't be swapped between entries or roles.
  - The wrapped secret key's AD binds the public key and the KDF parameters.
  - On unlock, `crypto_scalarmult_base(sk) == pk` is checked. An attacker who replaces the stored public key, so that future entries get sealed to *their* key, is detected.
- **Memory hygiene.** The secret key lives in `sodium_malloc` memory: guard pages, `mlock`ed so it isn't swapped. It is zeroed on lock. The password bytes the vault code owns are wiped with `sodium_memzero` after use.

## What is stored

| Field | Plaintext entry (newest N) | Encrypted entry |
|---|---|---|
| `main.uuid` | SHA-1 of the content | **Random** 160-bit id. A content hash would let anyone holding the database confirm a guess ("was X copied?") |
| `main.mimetypes` | e.g. `text/plain` | `application/x-kde-klipper-encrypted` |
| `main.text` | The text | `NULL` |
| `main.added_time`, `last_used_time`, `starred` | Kept | Kept: ordering, starring and eviction need them |
| `aux` rows, `data/<uuid>/` files | The payloads (text, HTML, images…) | Deleted |
| `vault.meta` | — | Sealed: real content id, MIME types, text, image size |
| `vault.payload` | — | Sealed: list of (MIME type, bytes) |
| `vault_meta` | — | Salt, KDF limits, public key, wrapped secret key, format version |

The blob format is `version byte | sealed key | nonce | ciphertext`.

## Removing the old plaintext

Encrypting a row is not enough if old bytes survive elsewhere in the files.

- `PRAGMA secure_delete = ON`: SQLite zero-fills freed pages and cells.
- `PRAGMA wal_checkpoint(TRUNCATE)` after every batch of entries is encrypted. The write-ahead log otherwise keeps old page images. **This is the step that matters**; see the test below.
- "Clear history" and "Forgot password" run `VACUUM`.
- Payload folders of encrypted entries are deleted.
- The history folder and `data/` are `0700`; the database, `-wal` and `-shm` are `0600`.

### How this is tested

`klipper/autotests/historyvaulttest.cpp` (`testModelVault`):

1. Copies 12 entries (11 texts and an image) and sets up a vault.
2. Reads **every byte of every file** under the history folder: database, WAL, shared memory, payload files.
3. Asserts that the content and SHA-1 ids of the 7 older entries appear nowhere, and the 5 recent ones do.
4. Lets more entries age out, without `VACUUM`, and scans again.

**Mutation check:** with the WAL truncation removed, the test fails, because the plaintext is found in `history3.sqlite-wal`. So the scan genuinely detects leaks. With truncation on and `secure_delete` off it still passes: `secure_delete` is defence in depth here, not the load-bearing step.

`testVaultCrypto` covers wrong passwords, sealing while locked, password change, tampered blobs, blobs moved to another row, and a replaced public key.

## Limits

- **Metadata is visible:** how many entries exist, when each was copied and last used, which are starred, and roughly how large each is (ciphertext length).
- **The newest N entries are plaintext by design.** The current clipboard is readable by every running application, as on any desktop.
- **A running, unlocked session is not protected.**
  - Code running as your user can read plasmashell's memory, where unlocked entries are decrypted, or log your password as you type it.
  - Locking reduces the time window; it doesn't change this.
- **Not every copy of the password is wiped.** It passes through QML and `QString`, which Qt doesn't let applications wipe. Only the copies the vault code owns are wiped.
- **Copy-on-write filesystems and SSDs.**
  - On btrfs, ZFS, or any SSD with wear levelling, rewriting a file doesn't overwrite the old physical blocks.
  - An entry's plaintext, written before it aged out, can survive in free space until it's reused. No application can fix this at file level.
  - Use **full-disk encryption** (e.g. LUKS) and keep swap encrypted or in zram.
- **No hardware binding.** There is no TPM or keyring integration, so the password alone protects the history. Choose a strong one.
- **Forgetting the password means losing the encrypted entries.** There's no recovery mechanism, by design.

## Reporting a vulnerability

Please open a [GitHub security advisory](https://github.com/giri256/secure-klipper/security/advisories/new) instead of a public issue.
