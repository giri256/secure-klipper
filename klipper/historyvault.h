/*
    SPDX-FileCopyrightText: 2026 giri256

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include <memory>
#include <optional>

#include <QByteArray>
#include <QByteArrayView>

#include "klipper_export.h"

class QSqlDatabase;

/**
 * Password-protected storage for clipboard history items older than the
 * "recent" window (see HistoryModel). All crypto is libsodium.
 *
 * Key hierarchy:
 *   password --Argon2id(salt, opslimit, memlimit)--> KEK (32 bytes, never stored)
 *   KEK --XChaCha20-Poly1305--> wraps the vault X25519 secret key (stored wrapped)
 *   vault X25519 public key (stored in clear) --sealed box--> per-item random 32-byte key
 *   per-item key --XChaCha20-Poly1305(AD = blob purpose + row id)--> item data
 *
 * Sealing only needs the public key, so items can be encrypted while the vault
 * is locked; opening needs the secret key, i.e. the password.
 */
class KLIPPER_EXPORT HistoryVault
{
public:
    /// Secret key bytes in sodium_malloc'd (mlocked, guard-paged) memory; freed with sodium_free (which zeroes).
    using SecretKey = std::shared_ptr<unsigned char>;

    struct Params {
        QByteArray salt;
        quint64 opsLimit = 0;
        quint64 memLimit = 0;
        QByteArray publicKey;
        QByteArray wrappedSecretKey; // nonce || ciphertext
    };

    static constexpr int KeyBytes = 32;

    HistoryVault();
    ~HistoryVault();

    /// Creates the vault tables if needed and loads parameters. Returns false on a DB or sodium error.
    bool load(QSqlDatabase &db);

    bool isConfigured() const;
    bool isUnlocked() const;
    const Params &params() const;

    struct NewKeys {
        Params params;
        SecretKey secretKey;
    };
    /// Slow (Argon2id), thread-safe. First-time setup: new keypair wrapped under @p password. Wipes @p password.
    static std::optional<NewKeys> createKeys(QByteArray password);
    /// Stores @p keys and adopts them (vault becomes configured and unlocked).
    bool install(QSqlDatabase &db, NewKeys keys);

    /**
     * Slow (Argon2id). Thread-safe: touches no instance state.
     * Returns the secret key if @p password opens @p params, else null. Wipes @p password.
     */
    static SecretKey unwrapSecretKey(const Params &params, QByteArray password);

    /// Adopts a key returned by unwrapSecretKey().
    void setSecretKey(SecretKey key);
    SecretKey secretKey() const;
    void lock();

    /// Slow (Argon2id), thread-safe. Re-wraps @p secretKey under @p newPassword with a new salt. Empty params on failure.
    static Params rewrap(const Params &params, const SecretKey &secretKey, QByteArray newPassword);
    /// Stores re-wrapped params from rewrap(). The public key must be unchanged.
    bool installParams(QSqlDatabase &db, Params params);

    /// Forgets the keys and deletes the vault parameters (encrypted rows become unreadable garbage).
    bool reset(QSqlDatabase &db);

    /// Encrypts @p plain for this vault, bound to @p ad. Works while locked.
    QByteArray seal(QByteArrayView plain, QByteArrayView ad) const;
    /// Decrypts a seal() blob. Needs the vault unlocked. nullopt on wrong key, wrong AD or tampering.
    std::optional<QByteArray> open(QByteArrayView blob, QByteArrayView ad) const;
    /// Thread-safe variant for worker threads.
    static std::optional<QByteArray> open(const SecretKey &secretKey, const QByteArray &publicKey, QByteArrayView blob, QByteArrayView ad);

    /// Zeroes the bytes of @p data (detaching first) and clears it.
    static void wipe(QByteArray &data);

private:
    static SecretKey allocateSecretKey();
    static QByteArray deriveAndWrap(const Params &params, const unsigned char *secretKey, QByteArray password);
    static bool store(QSqlDatabase &db, const Params &params);

    Params m_params;
    SecretKey m_secretKey;
};
