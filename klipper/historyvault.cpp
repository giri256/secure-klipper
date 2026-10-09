/*
    SPDX-FileCopyrightText: 2026 giri256

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "historyvault.h"

#include <sodium.h>

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include "klipper_debug.h"

using namespace Qt::StringLiterals;

namespace
{
constexpr char s_blobVersion = 1;
constexpr QByteArrayView s_blobContext = "org.kde.klipper.vault.v1.item";
constexpr QByteArrayView s_wrapContext = "org.kde.klipper.vault.v1.secretkey";
constexpr int s_sealedKeyBytes = HistoryVault::KeyBytes + crypto_box_SEALBYTES;
constexpr int s_nonceBytes = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;

// Associated data for the wrapped secret key: binds it to the public key and KDF parameters,
// so a swapped public key (items silently sealed to someone else) is detected at unlock.
QByteArray wrapAd(const HistoryVault::Params &params)
{
    QByteArray ad(s_wrapContext.data(), s_wrapContext.size());
    ad += '\0' + params.publicKey + params.salt + QByteArray::number(params.opsLimit) + ':' + QByteArray::number(params.memLimit);
    return ad;
}

QByteArray blobAd(QByteArrayView ad)
{
    QByteArray full(s_blobContext.data(), s_blobContext.size());
    full += '\0';
    full += ad;
    return full;
}

bool sodiumReady()
{
    static const bool ok = sodium_init() >= 0;
    return ok;
}
}

HistoryVault::HistoryVault()
{
    sodiumReady();
}

HistoryVault::~HistoryVault() = default;

HistoryVault::SecretKey HistoryVault::allocateSecretKey()
{
    auto *p = static_cast<unsigned char *>(sodium_malloc(crypto_box_SECRETKEYBYTES));
    if (!p) {
        return {};
    }
    return SecretKey(p, [](unsigned char *k) {
        sodium_free(k); // zeroes before freeing
    });
}

void HistoryVault::wipe(QByteArray &data)
{
    if (!data.isEmpty()) {
        sodium_memzero(data.data(), data.size()); // data() detaches, so a shared copy is not affected
    }
    data.clear();
}

bool HistoryVault::load(QSqlDatabase &db)
{
    if (!sodiumReady()) {
        qCCritical(KLIPPER_LOG) << "libsodium failed to initialise; history encryption unavailable";
        return false;
    }
    QSqlQuery query(db);
    if (!query.exec(u"CREATE TABLE IF NOT EXISTS vault_meta (key TEXT PRIMARY KEY, value BLOB NOT NULL)"_s)
        || !query.exec(u"CREATE TABLE IF NOT EXISTS vault (uuid char(40) PRIMARY KEY, meta BLOB NOT NULL, payload BLOB NOT NULL)"_s)) {
        qCWarning(KLIPPER_LOG) << "Failed to create vault tables" << query.lastError().text();
        return false;
    }
    m_params = {};
    m_secretKey.reset();
    if (!query.exec(u"SELECT key, value FROM vault_meta"_s)) {
        return false;
    }
    while (query.next()) {
        const QString key = query.value(0).toString();
        const QByteArray value = query.value(1).toByteArray();
        if (key == "salt"_L1) {
            m_params.salt = value;
        } else if (key == "opslimit"_L1) {
            m_params.opsLimit = value.toULongLong();
        } else if (key == "memlimit"_L1) {
            m_params.memLimit = value.toULongLong();
        } else if (key == "public_key"_L1) {
            m_params.publicKey = value;
        } else if (key == "wrapped_secret_key"_L1) {
            m_params.wrappedSecretKey = value;
        }
    }
    if (m_params.publicKey.size() != crypto_box_PUBLICKEYBYTES || m_params.salt.size() != crypto_pwhash_SALTBYTES
        || m_params.wrappedSecretKey.size() != s_nonceBytes + int(crypto_box_SECRETKEYBYTES + crypto_aead_xchacha20poly1305_ietf_ABYTES)) {
        if (!m_params.publicKey.isEmpty()) {
            qCWarning(KLIPPER_LOG) << "Vault parameters are malformed; treating vault as not configured";
        }
        m_params = {};
    }
    return true;
}

bool HistoryVault::isConfigured() const
{
    return !m_params.publicKey.isEmpty();
}

bool HistoryVault::isUnlocked() const
{
    return bool(m_secretKey);
}

const HistoryVault::Params &HistoryVault::params() const
{
    return m_params;
}

QByteArray HistoryVault::deriveAndWrap(const Params &params, const unsigned char *secretKey, QByteArray password)
{
    unsigned char kek[KeyBytes];
    sodium_mlock(kek, sizeof kek);
    const int rc = crypto_pwhash(kek,
                                 sizeof kek,
                                 password.constData(),
                                 password.size(),
                                 reinterpret_cast<const unsigned char *>(params.salt.constData()),
                                 params.opsLimit,
                                 params.memLimit,
                                 crypto_pwhash_ALG_ARGON2ID13);
    wipe(password);
    if (rc != 0) {
        sodium_munlock(kek, sizeof kek);
        return {};
    }
    QByteArray wrapped(s_nonceBytes + crypto_box_SECRETKEYBYTES + crypto_aead_xchacha20poly1305_ietf_ABYTES, Qt::Uninitialized);
    auto *nonce = reinterpret_cast<unsigned char *>(wrapped.data());
    randombytes_buf(nonce, s_nonceBytes);
    const QByteArray ad = wrapAd(params);
    crypto_aead_xchacha20poly1305_ietf_encrypt(nonce + s_nonceBytes,
                                               nullptr,
                                               secretKey,
                                               crypto_box_SECRETKEYBYTES,
                                               reinterpret_cast<const unsigned char *>(ad.constData()),
                                               ad.size(),
                                               nullptr,
                                               nonce,
                                               kek);
    sodium_munlock(kek, sizeof kek); // also zeroes
    return wrapped;
}

bool HistoryVault::store(QSqlDatabase &db, const Params &params)
{
    if (!db.transaction()) {
        return false;
    }
    QSqlQuery query(db);
    bool ok = query.exec(u"DELETE FROM vault_meta"_s);
    const std::pair<QString, QByteArray> rows[] = {
        {u"version"_s, QByteArray::number(s_blobVersion)},
        {u"salt"_s, params.salt},
        {u"opslimit"_s, QByteArray::number(params.opsLimit)},
        {u"memlimit"_s, QByteArray::number(params.memLimit)},
        {u"public_key"_s, params.publicKey},
        {u"wrapped_secret_key"_s, params.wrappedSecretKey},
    };
    for (const auto &[key, value] : rows) {
        if (!ok) {
            break;
        }
        query.prepare(u"INSERT INTO vault_meta (key, value) VALUES (?, ?)"_s);
        query.addBindValue(key);
        query.addBindValue(value);
        ok = query.exec();
    }
    if (!ok) {
        qCWarning(KLIPPER_LOG) << "Failed to store vault parameters" << query.lastError().text();
        db.rollback();
        return false;
    }
    return db.commit();
}

std::optional<HistoryVault::NewKeys> HistoryVault::createKeys(QByteArray password)
{
    if (!sodiumReady() || password.isEmpty()) {
        wipe(password);
        return std::nullopt;
    }
    NewKeys keys;
    keys.secretKey = allocateSecretKey();
    if (!keys.secretKey) {
        wipe(password);
        return std::nullopt;
    }
    Params &params = keys.params;
    params.publicKey.resize(crypto_box_PUBLICKEYBYTES);
    crypto_box_keypair(reinterpret_cast<unsigned char *>(params.publicKey.data()), keys.secretKey.get());
    params.salt.resize(crypto_pwhash_SALTBYTES);
    randombytes_buf(params.salt.data(), params.salt.size());
    // ~0.5-1 s and 256 MiB on a desktop CPU; callers run this off the GUI thread
    params.opsLimit = crypto_pwhash_OPSLIMIT_MODERATE;
    params.memLimit = crypto_pwhash_MEMLIMIT_MODERATE;
    params.wrappedSecretKey = deriveAndWrap(params, keys.secretKey.get(), std::move(password));
    if (params.wrappedSecretKey.isEmpty()) {
        return std::nullopt;
    }
    return keys;
}

bool HistoryVault::install(QSqlDatabase &db, NewKeys keys)
{
    if (!keys.secretKey || !store(db, keys.params)) {
        return false;
    }
    m_params = std::move(keys.params);
    m_secretKey = std::move(keys.secretKey);
    return true;
}

HistoryVault::SecretKey HistoryVault::unwrapSecretKey(const Params &params, QByteArray password)
{
    if (!sodiumReady() || params.publicKey.isEmpty()) {
        wipe(password);
        return {};
    }
    unsigned char kek[KeyBytes];
    sodium_mlock(kek, sizeof kek);
    const int rc = crypto_pwhash(kek,
                                 sizeof kek,
                                 password.constData(),
                                 password.size(),
                                 reinterpret_cast<const unsigned char *>(params.salt.constData()),
                                 params.opsLimit,
                                 params.memLimit,
                                 crypto_pwhash_ALG_ARGON2ID13);
    wipe(password);
    if (rc != 0) {
        sodium_munlock(kek, sizeof kek);
        return {};
    }
    SecretKey secretKey = allocateSecretKey();
    const QByteArray ad = wrapAd(params);
    const auto *nonce = reinterpret_cast<const unsigned char *>(params.wrappedSecretKey.constData());
    unsigned long long outLen = 0;
    const int openRc = !secretKey ? -1
                                  : crypto_aead_xchacha20poly1305_ietf_decrypt(secretKey.get(),
                                                                               &outLen,
                                                                               nullptr,
                                                                               nonce + s_nonceBytes,
                                                                               params.wrappedSecretKey.size() - s_nonceBytes,
                                                                               reinterpret_cast<const unsigned char *>(ad.constData()),
                                                                               ad.size(),
                                                                               nonce,
                                                                               kek);
    sodium_munlock(kek, sizeof kek);
    if (openRc != 0 || outLen != crypto_box_SECRETKEYBYTES) {
        return {}; // wrong password (or tampered parameters)
    }
    // Defence in depth: the secret key must match the stored public key
    unsigned char derivedPublic[crypto_box_PUBLICKEYBYTES];
    crypto_scalarmult_base(derivedPublic, secretKey.get());
    if (sodium_memcmp(derivedPublic, params.publicKey.constData(), sizeof derivedPublic) != 0) {
        qCWarning(KLIPPER_LOG) << "Vault public key does not match its secret key";
        return {};
    }
    return secretKey;
}

void HistoryVault::setSecretKey(SecretKey key)
{
    m_secretKey = std::move(key);
}

HistoryVault::SecretKey HistoryVault::secretKey() const
{
    return m_secretKey;
}

void HistoryVault::lock()
{
    m_secretKey.reset();
}

HistoryVault::Params HistoryVault::rewrap(const Params &params, const SecretKey &secretKey, QByteArray newPassword)
{
    if (!secretKey || newPassword.isEmpty()) {
        wipe(newPassword);
        return {};
    }
    Params newParams = params;
    randombytes_buf(newParams.salt.data(), newParams.salt.size());
    newParams.wrappedSecretKey = deriveAndWrap(newParams, secretKey.get(), std::move(newPassword));
    if (newParams.wrappedSecretKey.isEmpty()) {
        return {};
    }
    return newParams;
}

bool HistoryVault::installParams(QSqlDatabase &db, Params params)
{
    if (params.publicKey != m_params.publicKey || params.wrappedSecretKey.isEmpty() || !store(db, params)) {
        return false;
    }
    m_params = std::move(params);
    return true;
}

bool HistoryVault::reset(QSqlDatabase &db)
{
    m_secretKey.reset();
    m_params = {};
    QSqlQuery query(db);
    return query.exec(u"DELETE FROM vault_meta"_s);
}

QByteArray HistoryVault::seal(QByteArrayView plain, QByteArrayView ad) const
{
    if (!isConfigured()) {
        return {};
    }
    // Layout: version(1) | sealed item key(80) | nonce(24) | ciphertext+tag
    QByteArray blob(1 + s_sealedKeyBytes + s_nonceBytes + plain.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES, Qt::Uninitialized);
    auto *out = reinterpret_cast<unsigned char *>(blob.data());
    out[0] = s_blobVersion;
    unsigned char itemKey[KeyBytes];
    crypto_aead_xchacha20poly1305_ietf_keygen(itemKey);
    crypto_box_seal(out + 1, itemKey, sizeof itemKey, reinterpret_cast<const unsigned char *>(m_params.publicKey.constData()));
    unsigned char *nonce = out + 1 + s_sealedKeyBytes;
    randombytes_buf(nonce, s_nonceBytes);
    const QByteArray fullAd = blobAd(ad);
    crypto_aead_xchacha20poly1305_ietf_encrypt(nonce + s_nonceBytes,
                                               nullptr,
                                               reinterpret_cast<const unsigned char *>(plain.data()),
                                               plain.size(),
                                               reinterpret_cast<const unsigned char *>(fullAd.constData()),
                                               fullAd.size(),
                                               nullptr,
                                               nonce,
                                               itemKey);
    sodium_memzero(itemKey, sizeof itemKey);
    return blob;
}

std::optional<QByteArray> HistoryVault::open(QByteArrayView blob, QByteArrayView ad) const
{
    return open(m_secretKey, m_params.publicKey, blob, ad);
}

std::optional<QByteArray> HistoryVault::open(const SecretKey &secretKey, const QByteArray &publicKey, QByteArrayView blob, QByteArrayView ad)
{
    constexpr qsizetype header = 1 + s_sealedKeyBytes + s_nonceBytes;
    if (!secretKey || publicKey.size() != crypto_box_PUBLICKEYBYTES || blob.size() < header + qsizetype(crypto_aead_xchacha20poly1305_ietf_ABYTES)
        || blob[0] != s_blobVersion) {
        return std::nullopt;
    }
    const auto *in = reinterpret_cast<const unsigned char *>(blob.data());
    unsigned char itemKey[KeyBytes];
    if (crypto_box_seal_open(itemKey, in + 1, s_sealedKeyBytes, reinterpret_cast<const unsigned char *>(publicKey.constData()), secretKey.get()) != 0) {
        return std::nullopt;
    }
    const unsigned char *nonce = in + 1 + s_sealedKeyBytes;
    QByteArray plain(blob.size() - header - crypto_aead_xchacha20poly1305_ietf_ABYTES, Qt::Uninitialized);
    const QByteArray fullAd = blobAd(ad);
    const int rc = crypto_aead_xchacha20poly1305_ietf_decrypt(reinterpret_cast<unsigned char *>(plain.data()),
                                                              nullptr,
                                                              nullptr,
                                                              nonce + s_nonceBytes,
                                                              blob.size() - header,
                                                              reinterpret_cast<const unsigned char *>(fullAd.constData()),
                                                              fullAd.size(),
                                                              nonce,
                                                              itemKey);
    sodium_memzero(itemKey, sizeof itemKey);
    if (rc != 0) {
        return std::nullopt;
    }
    return plain;
}
