/*
    SPDX-FileCopyrightText: 2026 giri256

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "../historyitem.h"
#include "../historymodel.h"
#include "../historyvault.h"
#include "systemclipboard.h"

#include <functional>

#include <QAbstractItemModelTester>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QImage>
#include <QMimeData>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;

class HistoryVaultTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void testVaultCrypto();
    void testModelVault();
    void cleanupTestCase();

private:
    static bool runVaultOperation(HistoryModel *history, const std::function<void()> &operation);
    static QString text(HistoryModel *history, int row);
    static QByteArray allStoredBytes();
    static QString klipperFolder();
};

void HistoryVaultTest::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
}

void HistoryVaultTest::cleanupTestCase()
{
    // Leave no database behind: klippertest copies its own history into this folder
    QDir(klipperFolder()).removeRecursively();
}

QString HistoryVaultTest::klipperFolder()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + u"/klipper";
}

bool HistoryVaultTest::runVaultOperation(HistoryModel *history, const std::function<void()> &operation)
{
    QSignalSpy spy(history, &HistoryModel::vaultOperationFinished);
    operation();
    if (spy.isEmpty() && !spy.wait(60000)) {
        return false;
    }
    return spy.constFirst().at(1).toBool();
}

QString HistoryVaultTest::text(HistoryModel *history, int row)
{
    return history->index(row).data(Qt::DisplayRole).toString();
}

// Everything Klipper has on disk: the database, its WAL and every data file
QByteArray HistoryVaultTest::allStoredBytes()
{
    QByteArray bytes;
    QDirIterator it(klipperFolder(), QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QFile file(it.next());
        if (file.open(QIODevice::ReadOnly)) {
            bytes += file.readAll();
        }
    }
    return bytes;
}

void HistoryVaultTest::testVaultCrypto()
{
    QSqlDatabase db = QSqlDatabase::addDatabase(u"QSQLITE"_s, u"vaulttest"_s);
    db.setDatabaseName(u":memory:"_s);
    QVERIFY(db.open());

    HistoryVault vault;
    QVERIFY(vault.load(db));
    QVERIFY(!vault.isConfigured());
    QVERIFY(vault.seal("x", "ad").isEmpty()); // nothing to seal to yet

    QElapsedTimer timer;
    timer.start();
    std::optional<HistoryVault::NewKeys> keys = HistoryVault::createKeys("correct horse");
    qInfo() << "Argon2id key derivation took" << timer.elapsed() << "ms";
    QVERIFY(keys);
    QVERIFY(vault.install(db, std::move(*keys)));
    QVERIFY(vault.isConfigured());
    QVERIFY(vault.isUnlocked());

    const QByteArray plain = "top secret clipboard text";
    const QByteArray blob = vault.seal(plain, "row-1");
    QVERIFY(!blob.contains(plain));
    QCOMPARE(vault.open(blob, "row-1"), std::optional<QByteArray>(plain));
    QVERIFY(!vault.open(blob, "row-2")); // bound to its row
    QByteArray tampered = blob;
    tampered[tampered.size() - 3] ^= 1;
    QVERIFY(!vault.open(tampered, "row-1"));
    QVERIFY(vault.seal(plain, "row-1") != blob); // fresh item key and nonce every time

    vault.lock();
    QVERIFY(!vault.isUnlocked());
    QVERIFY(!vault.open(blob, "row-1"));
    const QByteArray sealedWhileLocked = vault.seal("copied while locked", "row-3");
    QVERIFY(!sealedWhileLocked.isEmpty()); // only the public key is needed

    QVERIFY(!HistoryVault::unwrapSecretKey(vault.params(), "wrong horse"));
    HistoryVault::SecretKey key = HistoryVault::unwrapSecretKey(vault.params(), "correct horse");
    QVERIFY(key);
    vault.setSecretKey(key);
    QCOMPARE(vault.open(sealedWhileLocked, "row-3"), std::optional<QByteArray>("copied while locked"));

    // Changing the password re-wraps the same key: existing blobs stay readable
    HistoryVault::Params rewrapped = HistoryVault::rewrap(vault.params(), vault.secretKey(), "battery staple");
    QVERIFY(!rewrapped.wrappedSecretKey.isEmpty());
    QVERIFY(vault.installParams(db, rewrapped));

    HistoryVault reloaded;
    QVERIFY(reloaded.load(db));
    QVERIFY(reloaded.isConfigured());
    QVERIFY(!reloaded.isUnlocked());
    QVERIFY(!HistoryVault::unwrapSecretKey(reloaded.params(), "correct horse"));
    reloaded.setSecretKey(HistoryVault::unwrapSecretKey(reloaded.params(), "battery staple"));
    QVERIFY(reloaded.isUnlocked());
    QCOMPARE(reloaded.open(blob, "row-1"), std::optional<QByteArray>(plain));

    // A swapped public key (items silently sealed to an attacker) is detected at unlock
    HistoryVault::Params swapped = reloaded.params();
    swapped.publicKey[0] = char(swapped.publicKey[0] ^ 1);
    QVERIFY(!HistoryVault::unwrapSecretKey(swapped, "battery staple"));

    QVERIFY(reloaded.reset(db));
    QVERIFY(!reloaded.isConfigured());
    QVERIFY(reloaded.load(db));
    QVERIFY(!reloaded.isConfigured());
}

void HistoryVaultTest::testModelVault()
{
    SystemClipboard::self()->clear();
    std::shared_ptr<HistoryModel> history = HistoryModel::self();
    QAbstractItemModelTester modelTest(history.get());
    QSqlDatabase db = QSqlDatabase::database(u"klipper"_s);
    QVERIFY(db.isOpen());
    history->setMaxSize(50);
    history->clear();
    QTRY_COMPARE(history->pendingJobs(), 0);
    QCOMPARE(history->plaintextItems(), 5);
    QVERIFY(!history->isVaultConfigured());

    // Oldest first: a binary image and a file URL, then 10 text entries.
    // A file copied from a file manager is text/uri-list, not image/png.
    QImage image(10, 7, QImage::Format_RGB32);
    image.fill(Qt::red);
    auto imageData = std::make_unique<QMimeData>();
    imageData->setImageData(image);
    // Screenshot/file producers often attach a filename or text/uri-list next to
    // the binary image. It must still receive the image delegate and preview.
    imageData->setText(u"clipboard-screenshot.png"_s);
    QVERIFY(history->insert(imageData.get()));
    QTRY_COMPARE(history->pendingJobs(), 0);

    QTemporaryDir urlImageDirectory;
    QVERIFY(urlImageDirectory.isValid());
    const QString urlImagePath = urlImageDirectory.filePath(u"copied-file-preview.png"_s);
    QImage urlImage(11, 8, QImage::Format_RGB32);
    urlImage.fill(Qt::blue);
    QVERIFY(urlImage.save(urlImagePath, "PNG"));
    const QUrl urlImageUrl = QUrl::fromLocalFile(urlImagePath);
    QMimeData urlImageData;
    urlImageData.setUrls({urlImageUrl});
    QVERIFY(history->insert(&urlImageData));
    QTRY_COMPARE(history->pendingJobs(), 0);

    for (int i = 0; i <= 9; ++i) {
        QVERIFY(history->insert(u"secret-item-%1"_s.arg(i, 2, 10, QLatin1Char('0'))));
        QTRY_COMPARE(history->pendingJobs(), 0);
    }
    QCOMPARE(history->rowCount(), 12);
    // Image payloads have a format-bearing suffix for tools which consume file URIs.
    // Older extensionless entries should be migrated when history is loaded.
    QCOMPARE(history->index(11).data(HistoryModel::TypeIntRole).toInt(), int(HistoryItemType::Image));
    QVERIFY(text(history.get(), 11).contains(u"10x7"_s));
    const QString plainImageUuid = history->index(11).data(HistoryModel::UuidRole).toString();
    QUrl plainImageUrl = history->index(11).data(HistoryModel::ImageUrlRole).toUrl();
    QVERIFY(plainImageUrl.isLocalFile());
    QVERIFY(plainImageUrl.toLocalFile().endsWith(u".png"));
    QVERIFY(QFile::exists(plainImageUrl.toLocalFile()));
    const QFileInfo plainImageInfo(plainImageUrl.toLocalFile());
    const QString legacyImagePath = plainImageInfo.dir().filePath(plainImageInfo.completeBaseName());
    QVERIFY(QFile::rename(plainImageUrl.toLocalFile(), legacyImagePath));
    QVERIFY(!QFile::exists(plainImageUrl.toLocalFile()));
    QVERIFY(history->loadHistory());
    QCOMPARE(history->index(11).data(HistoryModel::UuidRole).toString(), plainImageUuid);
    plainImageUrl = history->index(11).data(HistoryModel::ImageUrlRole).toUrl();
    QVERIFY(plainImageUrl.toLocalFile().endsWith(u".png"));
    QVERIFY(QFile::exists(plainImageUrl.toLocalFile()));
    QCOMPARE(QImage(plainImageUrl.toLocalFile()).size(), QSize(10, 7));
    const QString urlImageUuid = history->index(10).data(HistoryModel::UuidRole).toString();
    QCOMPARE(history->index(10).data(HistoryModel::TypeIntRole).toInt(), int(HistoryItemType::Url));
    QCOMPARE(text(history.get(), 10), urlImageUrl.toString(QUrl::FullyEncoded));
    // Without a vault nothing is encrypted
    QVERIFY(allStoredBytes().contains("secret-item-00"));

    // --- setup: everything but the newest 5 gets encrypted
    QVERIFY(runVaultOperation(history.get(), [&] {
        history->setupVault(u"pw1"_s);
    }));
    QVERIFY(history->isVaultConfigured());
    QVERIFY(history->isVaultUnlocked());
    QTRY_VERIFY(history->index(11).data(HistoryModel::EncryptedRole).toBool());
    QTRY_COMPARE(history->pendingJobs(), 0);
    QCoreApplication::processEvents(); // let the final VACUUM run
    for (int row = 0; row < 12; ++row) {
        QCOMPARE(history->index(row).data(HistoryModel::EncryptedRole).toBool(), row >= 5);
    }
    // Unlocked: encrypted entries still show their text (and the image its size)
    QCOMPARE(text(history.get(), 5), u"secret-item-04"_s);
    QCOMPARE(text(history.get(), 11), u"▨ 10x7"_s);
    QCOMPARE(history->index(11).data(HistoryModel::ImageSizeRole).toSize(), QSize(10, 7));
    QCOMPARE(history->index(11).data(HistoryModel::TypeIntRole).toInt(), int(HistoryItemType::Image));
    const int encryptedUrlRow = history->indexOf(urlImageUuid);
    QVERIFY(encryptedUrlRow >= 0);
    QVERIFY(history->index(encryptedUrlRow).data(HistoryModel::EncryptedRole).toBool());
    QCOMPARE(history->index(encryptedUrlRow).data(HistoryModel::TypeIntRole).toInt(), int(HistoryItemType::Url));
    QCOMPARE(text(history.get(), encryptedUrlRow), urlImageUrl.toString(QUrl::FullyEncoded));
    const QString encryptedUrlStorageUuid = history->index(encryptedUrlRow).data(HistoryModel::UuidRole).toString();
    const QString encryptedImageUuid = history->index(11).data(HistoryModel::UuidRole).toString();
    QCOMPARE(history->index(11).data(HistoryModel::ImageUrlRole).toUrl().scheme(), u"image"_s);
    QByteArray encryptedImagePreview = history->vaultImagePreviewData(encryptedImageUuid);
    QVERIFY(!encryptedImagePreview.isEmpty());
    QCOMPARE(QImage::fromData(encryptedImagePreview).size(), QSize(10, 7));
    HistoryVault::wipe(encryptedImagePreview);
    const QUrl encryptedDragUrl = history->imageFileUrlForDrag(encryptedImageUuid);
    QVERIFY(encryptedDragUrl.isLocalFile());
    QVERIFY(encryptedDragUrl.toLocalFile().endsWith(u".png"));
    QVERIFY(QFile::exists(encryptedDragUrl.toLocalFile()));
    QCOMPARE(QImage(encryptedDragUrl.toLocalFile()).size(), QSize(10, 7));
    const auto dragPermissions = QFileInfo(encryptedDragUrl.toLocalFile()).permissions();
    QVERIFY(!(dragPermissions & (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup |
                                 QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther)));
    history->releaseImageFileUrlForDrag(encryptedDragUrl);
    QVERIFY(!QFile::exists(encryptedDragUrl.toLocalFile()));

    // --- on disk: no plaintext of encrypted entries anywhere (db, WAL, data/)
    {
        QSqlQuery query(db);
        QVERIFY(query.exec(u"SELECT COUNT(*) FROM main WHERE mimetypes='application/x-kde-klipper-encrypted' AND text IS NULL"_s));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toInt(), 7);
        QVERIFY(query.exec(u"SELECT COUNT(DISTINCT uuid) FROM aux"_s));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toInt(), 5);
        QVERIFY(query.exec(u"SELECT COUNT(*) FROM vault"_s));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toInt(), 7);
    }
    QCOMPARE(QDir(klipperFolder() + u"/data").entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 5);
    const QByteArray onDisk = allStoredBytes();
    for (int i = 0; i <= 9; ++i) {
        const QByteArray needle = u"secret-item-%1"_s.arg(i, 2, 10, QLatin1Char('0')).toUtf8();
        QVERIFY2(onDisk.contains(needle) == (i >= 5), needle.constData()); // items 05..09 are the plain top 5
    }
    QVERIFY(!onDisk.contains(urlImageUrl.toString(QUrl::FullyEncoded).toUtf8())); // URL metadata is encrypted too
    // Encrypted rows don't reveal the content hash either
    QVERIFY(!onDisk.contains(QCryptographicHash::hash("secret-item-00", QCryptographicHash::Sha1).toHex()));
    QCOMPARE(QFileInfo(klipperFolder()).permissions() & (QFileDevice::ReadOther | QFileDevice::ReadGroup), QFileDevice::Permissions());
    QCOMPARE(QFileInfo(db.databaseName()).permissions() & (QFileDevice::ReadOther | QFileDevice::ReadGroup), QFileDevice::Permissions());

    // --- lock: older entries become opaque and can't be pasted
    history->lockVault();
    QVERIFY(!history->isVaultUnlocked());
    QCOMPARE(history->lockedCount(), 7);
    QCOMPARE(history->index(11).data(HistoryModel::TypeIntRole).toInt(), int(HistoryItemType::Text));
    QVERIFY(history->index(11).data(HistoryModel::ImageUrlRole).toUrl().isEmpty());
    QVERIFY(!history->imageFileUrlForDrag(encryptedImageUuid).isValid());
    const int lockedUrlRow = history->indexOf(encryptedUrlStorageUuid);
    QVERIFY(lockedUrlRow >= 0);
    QVERIFY(history->index(lockedUrlRow).data(HistoryModel::LockedRole).toBool());
    QCOMPARE(history->index(lockedUrlRow).data(HistoryModel::TypeIntRole).toInt(), int(HistoryItemType::Text));
    QVERIFY(history->index(8).data(HistoryModel::LockedRole).toBool());
    QCOMPARE(text(history.get(), 8), QString());
    {
        QSignalSpy unlockRequested(history.get(), &HistoryModel::unlockRequested);
        const QString lockedUuid = history->index(8).data(HistoryModel::UuidRole).toString();
        history->moveToTop(lockedUuid);
        QCOMPARE(unlockRequested.size(), 1);
        QCOMPARE(history->index(8).data(HistoryModel::UuidRole).toString(), lockedUuid);
    }

    // New entries are still encrypted while locked (public key only)
    QVERIFY(history->insert(u"secret-item-11"_s));
    QTRY_COMPARE(history->pendingJobs(), 0);
    QTRY_COMPARE(history->lockedCount(), 8);
    QCOMPARE(text(history.get(), 0), u"secret-item-11"_s);
    // Re-copying a locked entry adds a fresh plain copy; the old one is de-duplicated on unlock
    QVERIFY(history->insert(u"secret-item-01"_s));
    QTRY_COMPARE(history->pendingJobs(), 0);
    QCOMPARE(history->rowCount(), 14);
    // Entries pushed out of the recent window later (no VACUUM involved) leave no plaintext either
    QTRY_COMPARE(history->lockedCount(), 9);
    QTRY_COMPARE(history->pendingJobs(), 0);
    {
        const QByteArray bytes = allStoredBytes();
        QVERIFY(bytes.contains("secret-item-11"));
        QVERIFY(!bytes.contains("secret-item-06"));
        QVERIFY(!bytes.contains("secret-item-05"));
    }

    // --- unlock
    QVERIFY(!runVaultOperation(history.get(), [&] {
        history->unlockVault(u"wrong"_s);
    }));
    QVERIFY(!history->isVaultUnlocked());
    QVERIFY(runVaultOperation(history.get(), [&] {
        history->unlockVault(u"pw1"_s);
    }));
    QVERIFY(history->isVaultUnlocked());
    QTRY_COMPARE(history->rowCount(), 13); // duplicate of secret-item-01 removed
    QCOMPARE(history->lockedCount(), 0);
    QCOMPARE(text(history.get(), 0), u"secret-item-01"_s);
    QCOMPARE(text(history.get(), 12), u"▨ 10x7"_s);
    QCOMPARE(history->index(12).data(HistoryModel::TypeIntRole).toInt(), int(HistoryItemType::Image));
    const int unlockedUrlRow = history->indexOf(urlImageUuid);
    QVERIFY(unlockedUrlRow >= 0);
    QCOMPARE(history->index(unlockedUrlRow).data(HistoryModel::EncryptedRole).toBool(), true);
    QCOMPARE(history->index(unlockedUrlRow).data(HistoryModel::TypeIntRole).toInt(), int(HistoryItemType::Url));
    QCOMPARE(text(history.get(), unlockedUrlRow), urlImageUrl.toString(QUrl::FullyEncoded));
    QCOMPARE(history->index(12).data(HistoryModel::ImageUrlRole).toUrl().scheme(), u"image"_s);
    QByteArray unlockedImagePreview = history->vaultImagePreviewData(history->index(12).data(HistoryModel::UuidRole).toString());
    QVERIFY(!unlockedImagePreview.isEmpty());
    QCOMPARE(QImage::fromData(unlockedImagePreview).size(), QSize(10, 7));
    HistoryVault::wipe(unlockedImagePreview);

    // --- using an encrypted entry decrypts it back into a normal one
    const QString imageUuid = history->index(12).data(HistoryModel::UuidRole).toString();
    history->moveToTop(imageUuid);
    QCOMPARE(history->index(0).data(HistoryModel::EncryptedRole).toBool(), false);
    QCOMPARE(history->index(0).data(HistoryModel::TypeIntRole).toInt(), int(HistoryItemType::Image));
    const QUrl imageUrl = history->index(0).data(HistoryModel::ImageUrlRole).toUrl();
    QVERIFY(QFile::exists(imageUrl.toLocalFile()));
    QVERIFY(imageUrl.toLocalFile().endsWith(u".png"));
    QCOMPARE(QImage(imageUrl.toLocalFile()).size(), QSize(10, 7));
    QTRY_VERIFY(history->index(5).data(HistoryModel::EncryptedRole).toBool()); // the displaced entry
    for (int row = 0; row < history->rowCount(); ++row) {
        QCOMPARE(history->index(row).data(HistoryModel::EncryptedRole).toBool(), row >= 5);
    }
    // Copying an encrypted entry's content again while unlocked moves it up instead of duplicating
    QVERIFY(history->insert(u"secret-item-00"_s));
    QTRY_COMPARE(history->pendingJobs(), 0);
    QCOMPARE(history->rowCount(), 13);
    QCOMPARE(text(history.get(), 0), u"secret-item-00"_s);
    QVERIFY(!history->index(0).data(HistoryModel::EncryptedRole).toBool());
    // Encrypted text entries can't be edited in place
    QTRY_VERIFY(history->index(8).data(HistoryModel::EncryptedRole).toBool());
    QVERIFY(!history->setData(history->index(8), u"edited"_s, Qt::DisplayRole));

    // --- password change
    QVERIFY(!runVaultOperation(history.get(), [&] {
        history->changeVaultPassword(u"wrong"_s, u"pw2"_s);
    }));
    QVERIFY(runVaultOperation(history.get(), [&] {
        history->changeVaultPassword(u"pw1"_s, u"pw2"_s);
    }));
    history->lockVault();
    QVERIFY(!runVaultOperation(history.get(), [&] {
        history->unlockVault(u"pw1"_s);
    }));
    QVERIFY(runVaultOperation(history.get(), [&] {
        history->unlockVault(u"pw2"_s);
    }));

    // --- persistence: reloading starts locked
    const int rows = history->rowCount();
    QVERIFY(history->loadHistory());
    QCOMPARE(history->rowCount(), rows);
    QVERIFY(history->isVaultConfigured());
    QVERIFY(!history->isVaultUnlocked());
    QCOMPARE(history->lockedCount(), rows - 5);

    // --- forgotten password: encrypted entries are dropped, the recent ones stay
    history->resetVault();
    QVERIFY(!history->isVaultConfigured());
    QCOMPARE(history->rowCount(), 5);
    QCOMPARE(history->lockedCount(), 0);
    {
        QSqlQuery query(u"SELECT COUNT(*) FROM vault"_s, db);
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toInt(), 0);
    }

    history->clear();
    QTRY_COMPARE(history->pendingJobs(), 0);
    QCOMPARE(history->rowCount(), 0);
}

QTEST_MAIN(HistoryVaultTest)
#include "historyvaulttest.moc"
