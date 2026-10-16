/*
    SPDX-FileCopyrightText: 2014 Martin Gräßlin <mgraesslin@kde.org>
    SPDX-FileCopyrightText: 2024 Fushan Wen <qydwhotmail@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "historymodel.h"

#include <algorithm>
#include <chrono>
#include <zlib.h>

#include <QBuffer>
#include <QCryptographicHash>
#include <QDBusConnection>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QImageReader>
#include <QMimeData>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QtConcurrentRun>

#include <KIO/DeleteJob>
#include <KLocalizedString>
#include <KMessageBox>

#include "config-klipper.h"
#include "historyitem.h"
#include "klipper_debug.h"
#include "klippersettings.h"
#include "systemclipboard.h"
#include "updateclipboardjob.h"

using namespace std::chrono_literals;
using namespace Qt::StringLiterals;

namespace
{
struct TransactionGuard {
    TransactionGuard(QSqlDatabase *db)
        : db(db)
        , inited(db->transaction())
    {
        if (!inited) {
            qCWarning(KLIPPER_LOG) << "A transaction didn't start:" << db->lastError().text();
        }
    }

    ~TransactionGuard()
    {
        if (!inited) {
            return;
        }
        if (!committed) {
            qCWarning(KLIPPER_LOG) << "Transaction failed:" << db->lastError().text();
            db->rollback();
        }
        db->commit();
    }

    bool exec(QSqlQuery &query)
    {
        if (!inited || !committed) {
            return false;
        }
        committed = query.exec();
        if (!committed) {
            qCWarning(KLIPPER_LOG).nospace().noquote() << "Query \"" << query.lastQuery() << "\" failed: " << query.lastError().text();
        }
        return committed;
    }

    bool exec(const QString &query)
    {
        QSqlQuery sqlQuery(query, *db);
        return exec(sqlQuery);
    }

    QSqlDatabase *db;
    bool inited = false;
    bool committed = true;
};

QString computeUuid(const QMimeData *data)
{
    QCryptographicHash hash(QCryptographicHash::Sha1);
    if (data->hasText()) {
        hash.addData(data->text().toUtf8());
    }
    if (data->hasUrls()) {
        for (const auto urls = data->urls(); const QUrl &url : urls) {
            hash.addData(url.toEncoded());
        }
    }
    if (data->hasImage()) {
        const auto image = data->imageData().value<QImage>();
        hash.addData(QByteArrayView(reinterpret_cast<const char *>(image.constBits()), image.sizeInBytes()));
    }
    return QString::fromLatin1(hash.result().toHex());
}

// Storage id of an encrypted row. Random, so it does not reveal the content hash.
QString randomUuid()
{
    quint32 words[5];
    QRandomGenerator::system()->fillRange(words);
    return QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(words), sizeof words).toHex());
}

// Associated data binding each sealed blob to its row and purpose, so blobs can't be swapped between rows
QByteArray metaAd(const QString &uuid)
{
    return "meta|" + uuid.toLatin1();
}

QByteArray payloadAd(const QString &uuid)
{
    return "payload|" + uuid.toLatin1();
}

constexpr quint8 s_vaultFormat = 1;

// What the UI needs of an encrypted item; decrypted on unlock
struct VaultMeta {
    QString contentUuid;
    QString mimeTypes;
    QString text;
    QSize imageSize;
};

QByteArray serializeMeta(const VaultMeta &meta)
{
    QByteArray data;
    QDataStream out(&data, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_5);
    out << s_vaultFormat << meta.contentUuid << meta.mimeTypes << meta.text << meta.imageSize;
    return data;
}

std::optional<VaultMeta> parseMeta(const QByteArray &data)
{
    QDataStream in(data);
    in.setVersion(QDataStream::Qt_6_5);
    quint8 format = 0;
    VaultMeta meta;
    in >> format >> meta.contentUuid >> meta.mimeTypes >> meta.text >> meta.imageSize;
    if (format != s_vaultFormat || in.status() != QDataStream::Ok || meta.contentUuid.size() != 40) {
        return std::nullopt;
    }
    return meta;
}

// The item's data: aux rows (mimetype -> data_uuid) and the data files' bytes (data_uuid -> bytes)
using AuxRows = QList<std::pair<QString, QString>>;
using Blobs = QHash<QString, QByteArray>;

void restartIfNeeded(QTimer &timer, int minutes)
{
    if (minutes > 0) {
        timer.start(std::chrono::minutes(minutes));
    } else {
        timer.stop();
    }
}
}

std::shared_ptr<HistoryModel> HistoryModel::self()
{
    static std::weak_ptr<HistoryModel> instance;
    if (instance.expired()) {
        struct make_shared_enabler : public HistoryModel {
        };
        std::shared_ptr<HistoryModel> ptr = std::make_shared<make_shared_enabler>();
        instance = ptr;
        return ptr;
    }
    return instance.lock();
}

HistoryModel::HistoryModel()
    : QAbstractListModel(nullptr)
    , m_clip(SystemClipboard::self())
    , m_displayImages(true)
{
    if (!QSqlDatabase::isDriverAvailable(u"QSQLITE"_s)) {
        qCCritical(KLIPPER_LOG) << "SQLITE driver isn't available";
        return;
    }

    loadSettings();
    if (!loadHistory()) [[unlikely]] {
        return;
    }

    // Only connect to this signal after loading the history, to avoid the action of loading triggering a save
    auto modelChanged = [this](const QModelIndex & /*parent*/, int first, int /*last*/) {
        changed(first == 0);
    };
    connect(this, &HistoryModel::rowsInserted, this, modelChanged);
    connect(this, &HistoryModel::rowsRemoved, this, modelChanged);
    connect(this,
            &HistoryModel::rowsMoved,
            this,
            [this](const QModelIndex & /*sourceParent*/, int sourceStart, int /*sourceEnd*/, const QModelIndex & /*destinationParent*/, int destinationRow) {
                Q_EMIT changed(sourceStart == 0 || destinationRow == 0);
            });
    connect(this, &HistoryModel::dataChanged, this, [this](const QModelIndex &topLeft, const QModelIndex &, const QList<int> &roles) {
        if (roles.contains(UuidRole)) { // Avoid overriding the current clipboard with async-loaded image data
            Q_EMIT changed(topLeft.row() == 0); // BUG 494031 the first item does not trigger rowsMoved
        }
    });
    connect(this, &HistoryModel::modelReset, this, [this] {
        Q_EMIT changed(true);
    });

    connect(this, &HistoryModel::changed, this, [this](bool isTop) {
        if (m_items.empty()) {
            m_clip->clear(SystemClipboard::SelectionMode(SystemClipboard::Selection | SystemClipboard::Clipboard));
            return;
        }
        if (!isTop || m_items[0]->isEncrypted()) {
            return;
        }

        QSqlQuery query(m_db);
        query.prepare(u"UPDATE main SET last_used_time=? WHERE uuid='%1'"_s.arg(m_items[0]->uuid()));
        query.addBindValue(QDateTime::currentMSecsSinceEpoch() / 1000.0);
        if (query.exec()) {
            if (m_clip->isLocked(QClipboard::Selection) || m_clip->isLocked(QClipboard::Clipboard)) {
                return;
            }
            m_clip->setMimeData(m_items[0], SystemClipboard::SelectionMode(SystemClipboard::Clipboard | SystemClipboard::Selection));
        }
    });

    connect(m_clip.get(), &SystemClipboard::ignored, this, &HistoryModel::slotIgnored);
    connect(m_clip.get(), &SystemClipboard::newClipData, this, &HistoryModel::checkClipData);

    m_autoLockTimer.setSingleShot(true);
    connect(&m_autoLockTimer, &QTimer::timeout, this, &HistoryModel::lockVault);
    // Lock the vault together with the session. Empty service: accept the signal from whichever screen locker owns the name.
    QDBusConnection::sessionBus().connect(QString(),
                                          u"/ScreenSaver"_s,
                                          u"org.freedesktop.ScreenSaver"_s,
                                          u"ActiveChanged"_s,
                                          this,
                                          SLOT(slotScreenSaverActiveChanged(bool)));
}

HistoryModel::~HistoryModel()
{
    if (!m_bKeepContents) {
        m_db.close();
        QFile(m_db.databaseName()).remove();
        QDir(m_dbFolder + u"/data").removeRecursively();
    }
}

void HistoryModel::clear()
{
    if (!m_db.isOpen()) {
        return;
    }
    if (TransactionGuard transaction(&m_db);
        !transaction.exec(u"DELETE FROM main"_s) || !transaction.exec(u"DELETE FROM aux"_s) || !transaction.exec(u"DELETE FROM vault"_s)) {
        return;
    }
    // Move the whole data folder aside and delete it in the background:
    // KIO::del of a few thousand item folders used to take minutes.
    const QString dataPath = m_dbFolder + u"/data";
    const QString trashPath = m_dbFolder + u"/data.trash-" + randomUuid().left(8);
    if (QDir().rename(dataPath, trashPath)) {
        QDir().mkpath(dataPath);
        QFile::setPermissions(dataPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        removeFolders({trashPath});
    } else {
        QStringList folders;
        for (const auto &item : std::as_const(m_items)) {
            folders.append(dataFolder(item->uuid()));
        }
        removeFolders(folders);
    }
    QSqlQuery(u"VACUUM"_s, m_db).exec();
    QSqlQuery(u"PRAGMA wal_checkpoint(TRUNCATE)"_s, m_db).exec();
    if (!m_items.empty()) { // Is empty when m_bKeepContents is false
        beginResetModel();
        m_items.clear();
        m_starredCount = 0;
        endResetModel();
    }
    m_clip->clear(SystemClipboard::SelectionMode(SystemClipboard::Selection | SystemClipboard::Clipboard));
}

void HistoryModel::clearNonStarredHistory()
{
    if (!m_db.isOpen()) {
        return;
    }

    // Get UUIDs of all non-starred items
    QStringList nonStarredUuids;
    QSqlQuery query(m_db);
    query.exec(u"SELECT uuid FROM main WHERE starred = 0 OR starred IS NULL"_s);
    while (query.next()) {
        nonStarredUuids.append(query.value(0).toString());
    }

    if (nonStarredUuids.isEmpty()) {
        return; // No non-starred items to remove
    }

    // Delete non-starred items from database
    {
        TransactionGuard transaction(&m_db);
        QStringList quotedUuids;
        for (const QString &uuid : std::as_const(nonStarredUuids)) {
            quotedUuids.append(u'\'' + uuid + u'\'');
        }
        const QString uuidList = quotedUuids.join(u',');

        if (!transaction.exec(u"DELETE FROM main WHERE uuid IN (%1)"_s.arg(uuidList))) {
            return;
        }
        if (!transaction.exec(u"DELETE FROM aux WHERE uuid IN (%1)"_s.arg(uuidList))) {
            return;
        }
        if (!transaction.exec(u"DELETE FROM vault WHERE uuid IN (%1)"_s.arg(uuidList))) {
            return;
        }
    }

    // Delete associated data folders
    QStringList deletedDataFolders;
    deletedDataFolders.reserve(nonStarredUuids.size());
    for (const QString &uuid : std::as_const(nonStarredUuids)) {
        deletedDataFolders.append(dataFolder(uuid));
    }
    removeFolders(deletedDataFolders);

    // Removing items from a list one by one is quadratic; look them up in a set
    const QSet<QString> removedUuids(nonStarredUuids.cbegin(), nonStarredUuids.cend());

    // Remove items from the model that were deleted
    // We need to work backwards to maintain correct indices
    beginResetModel();
    for (qsizetype i = m_items.size() - 1; i >= 0; --i) {
        if (removedUuids.contains(m_items[i]->uuid())) {
            m_items.removeAt(i);
        }
    }
    endResetModel();

    QSqlQuery(u"VACUUM"_s, m_db).exec();
    QSqlQuery(u"PRAGMA wal_checkpoint(TRUNCATE)"_s, m_db).exec();
    m_clip->clear(SystemClipboard::SelectionMode(SystemClipboard::Selection | SystemClipboard::Clipboard));
    Q_EMIT vaultStateChanged();
}

void HistoryModel::clearHistory()
{
    // No starred items, show normal confirmation
    // TODO: Consider adding a "Reset 'Don't ask again' dialogs" button in Klipper settings
    // to help users recover from accidentally dismissed confirmations
    int clearHist = KMessageBox::warningContinueCancel(nullptr,
                                                       i18n("Do you really want to clear and delete the entire clipboard history?"),
                                                       i18n("Clear Clipboard History"),
                                                       KStandardGuiItem::del(),
                                                       KStandardGuiItem::cancel(),
                                                       QStringLiteral("klipperClearHistoryAskAgain"),
                                                       KMessageBox::Dangerous);
    if (clearHist != KMessageBox::Continue) {
        return; // User cancelled
    }

    clearNonStarredHistory();
}

qsizetype HistoryModel::maxSize() const
{
    return m_maxSize;
}

void HistoryModel::setMaxSize(qsizetype size)
{
    if (m_maxSize == size) {
        return;
    }
    m_maxSize = size;
    if (m_items.size() > m_maxSize) {
        // Remove non-starred items from the end until we're within the size limit
        // or until no more non-starred items are available. Starred items are protected,
        // so the history may exceed maxSize.
        // Batched: one starred lookup, then one removeRows() per contiguous run instead of
        // per-row SQL + per-row KIO jobs (shrinking 5000 -> 20 took over a minute).
        QSet<QString> starred;
        if (QSqlQuery query(m_db); query.exec(u"SELECT uuid FROM main WHERE starred = 1"_s)) {
            while (query.next()) {
                starred.insert(query.value(0).toString());
            }
        }
        qsizetype toRemove = m_items.size() - m_maxSize;
        qsizetype i = m_items.size() - 1;
        while (i >= 0 && toRemove > 0) {
            if (starred.contains(m_items[i]->uuid())) {
                --i;
                continue;
            }
            const qsizetype last = i;
            while (i >= 0 && toRemove > 0 && !starred.contains(m_items[i]->uuid())) {
                --i;
                --toRemove;
            }
            removeRows(i + 1, last - i);
        }
    }
}

int HistoryModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return m_items.size();
}

QBindable<int> HistoryModel::bindableStarredCount() const
{
    return &m_starredCount;
}

QVariant HistoryModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_items.size() || index.column() != 0) {
        return {};
    }

    Q_ASSERT_X(m_db.isOpen(), Q_FUNC_INFO, qPrintable(m_db.lastError().text()));
    const std::shared_ptr<HistoryItem> &item = m_items.at(index.row());

    if (item->isEncrypted()) {
        switch (role) {
        case Qt::DisplayRole:
            if (item->isLocked()) {
                return QString();
            }
            if (const QSize size = item->imageSize(); size.isValid() && item->text().isEmpty()) {
                return QString(u"▨ " + i18nc("@info:tooltip width x height", "%1x%2", QString::number(size.width()), QString::number(size.height())));
            }
            return item->text();
        case ImageUrlRole:
            return QUrl(); // never written to disk in plaintext
        case ImageSizeRole:
            return item->imageSize();
        case TypeRole:
            return QVariant::fromValue<HistoryItemType>(HistoryItemType::Text);
        case TypeIntRole:
            return int(HistoryItemType::Text);
        default:
            break;
        }
    }

    switch (role) {
    case Qt::DisplayRole: {
        if (item->type() == HistoryItemType::Image) {
            QSqlQuery query(u"SELECT data_uuid FROM aux WHERE uuid='%1' AND (mimetype='%2')"_s.arg(item->uuid(), s_imageFormat), m_db);
            if (query.exec() && query.isSelect() && query.next()) {
                QSize size = QImageReader(QString(m_dbFolder + u"/data/" + item->uuid() + u'/' + query.value(0).toString())).size();
                return QString(u"▨ " + i18nc("@info:tooltip width x height", "%1x%2", QString::number(size.width()), QString::number(size.height())));
            }
        }
        return item->text();
    }
    case ImageUrlRole: {
        if (!(item->allTypes() & HistoryItemType::Image)) {
            return QUrl();
        }
        QSqlQuery query(u"SELECT data_uuid FROM aux WHERE uuid='%1' AND (mimetype='%2')"_s.arg(item->uuid(), s_imageFormat), m_db);
        if (query.exec() && query.isSelect() && query.next()) {
            return QUrl::fromLocalFile(m_dbFolder + u"/data/" + item->uuid() + u'/' + query.value(0).toString());
        }
        return QUrl();
    }
    case ImageSizeRole: {
        if (!(item->allTypes() & HistoryItemType::Image)) {
            return QSize();
        }
        QSqlQuery query(u"SELECT data_uuid FROM aux WHERE uuid='%1' AND (mimetype='%2')"_s.arg(item->uuid(), s_imageFormat), m_db);
        if (query.exec() && query.isSelect() && query.next()) {
            return QImageReader(m_dbFolder + u"/data/" + item->uuid() + u'/' + query.value(0).toString()).size();
        }
        return QSize();
    }
    case HistoryItemConstPtrRole:
        return QVariant::fromValue<HistoryItemConstPtr>(std::const_pointer_cast<const HistoryItem>(item));
    case UuidRole:
        return item->uuid();
    case TypeRole:
        return QVariant::fromValue<HistoryItemType>(item->type());
    case TypeIntRole:
        return int(item->type());
    case StarredRole:
        // TODO: Consider adding QHash<QString, bool> cache for starred status to avoid
        // frequent database queries if performance becomes an issue with large histories
        return isItemStarred(item->uuid());
    case EncryptedRole:
        return item->isEncrypted();
    case LockedRole:
        return item->isLocked();
    }
    return {};
}

bool HistoryModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    Q_ASSERT_X(m_db.isOpen(), Q_FUNC_INFO, qPrintable(m_db.lastError().text()));
    if (!checkIndex(index, CheckIndexOption::IndexIsValid)) {
        return false;
    }

    switch (auto &item = m_items[index.row()]; role) {
    case Qt::DisplayRole: {
        if (item->isEncrypted() || item->type() != HistoryItemType::Text || !value.canConvert<QString>()) [[unlikely]] {
            return false;
        }
        QString text = value.toString();
        if (text == item->text()) {
            return false;
        }
        QStringList mimetypes{u"text/plain"_s, u"text/plain;charset=utf-8"_s};
        QString newUuid = QString::fromLatin1(QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha1).toHex());
        {
            TransactionGuard transaction(&m_db);
            {
                QSqlQuery query(m_db);
                query.prepare(u"UPDATE main SET (uuid, mimetypes, text)=(?, ?, ?) WHERE uuid='%1'"_s.arg(item->uuid()));
                query.addBindValue(newUuid);
                query.addBindValue(mimetypes.join(u','));
                query.addBindValue(text);
                // last_used_time is updated in the signal slot
                if (!transaction.exec(query) || !transaction.exec(u"DELETE FROM aux WHERE uuid='%1'"_s.arg(item->uuid()))) {
                    return false;
                }
            }

            for (const QString &mimetype : std::as_const(mimetypes)) {
                QSqlQuery query(m_db);
                query.prepare(u"INSERT INTO aux (uuid, mimetype, data_uuid) VALUES (?, ?, ?)"_s);
                query.addBindValue(newUuid);
                query.addBindValue(mimetype);
                query.addBindValue(newUuid);
                if (!transaction.exec(query)) {
                    return false;
                }
            }
        }

        KIO::del(QUrl::fromLocalFile(m_dbFolder + u"/data/" + item->uuid() + u'/'), KIO::HideProgressInfo);
        saveToFile(m_dbFolder, text.toUtf8(), newUuid, newUuid); // Must be synchronous so the clipboard can be updated immediately

        item = std::make_shared<HistoryItem>(std::move(newUuid), std::move(mimetypes), std::move(text));
        Q_EMIT dataChanged(index, index, {Qt::DisplayRole, UuidRole});
        return true;
    }
    case StarredRole: {
        bool newValue = value.toBool();
        if (newValue == isItemStarred(item->uuid())) {
            return false;
        }

        QSqlQuery query(m_db);
        // Use prepared statement for safety
        query.prepare(u"UPDATE main SET starred = ? WHERE uuid = ?"_s);
        query.addBindValue(newValue);
        query.addBindValue(item->uuid());

        if (query.exec()) {
            // Notify views that this specific role has changed for the item
            m_starredCount = m_starredCount + (newValue ? 1 : -1);
            Q_EMIT dataChanged(index, index, {StarredRole});
            return true;
        }
        // TODO: Add user feedback/notification when starring/unstarring fails
        // Currently users have no indication if the star operation was unsuccessful
        break;
    }
    }

    return false;
}

bool HistoryModel::removeRows(int row, int count, const QModelIndex &parent)
{
    if (parent.isValid()) {
        return false;
    }
    if (qsizetype(row + count) > m_items.size()) {
        return false;
    }

    auto first = std::next(m_items.cbegin(), row);
    auto last = std::next(m_items.cbegin(), row + count);
    const int newStarredCount = m_starredCount - std::accumulate(first, last, 0, [this](int count, const std::shared_ptr<HistoryItem> &item) {
                                    return isItemStarred(item->uuid()) ? (count + 1) : count;
                                });

    {
        TransactionGuard transaction(&m_db);
        QStringList uuidList;
        for (int i = 0; i < count; ++i) {
            uuidList.append(u'\'' + m_items[row + i]->uuid() + u'\'');
        }
        const QString uuids = uuidList.join(u',');
        if (!transaction.exec(u"DELETE FROM main WHERE uuid IN (%1)"_s.arg(uuids))) {
            return false;
        }
        if (!transaction.exec(u"DELETE FROM aux WHERE uuid IN (%1)"_s.arg(uuids))) {
            return false;
        }
        if (!transaction.exec(u"DELETE FROM vault WHERE uuid IN (%1)"_s.arg(uuids))) {
            return false;
        }
    }

    QStringList deletedDataFolders;
    deletedDataFolders.reserve(count);
    for (int i = 0; i < count; ++i) {
        if (const auto &item = m_items[row + i]; !item->isEncrypted()) { // encrypted items have no data folder
            deletedDataFolders.append(dataFolder(item->uuid()));
        }
    }
    removeFolders(deletedDataFolders);

    beginRemoveRows(QModelIndex(), row, row + count - 1);
    m_items.erase(first, last);
    m_starredCount = newStarredCount;
    endRemoveRows();
    // Older entries moved up; some may now belong in the plaintext window
    scheduleEnforceVault();
    Q_EMIT vaultStateChanged(); // lockedCount may have changed
    return true;
}

bool HistoryModel::remove(const QString &uuid)
{
    const int index = indexOf(uuid);
    if (index < 0) {
        return false;
    }

    // Check if the item is starred before removing
    bool isStarred = isItemStarred(uuid);

    // Show confirmation dialog for starred items
    if (isStarred) {
        int result = KMessageBox::warningContinueCancel(nullptr,
                                                       i18n("This item is starred. Do you really want to remove it from history?"),
                                                       i18n("Remove Starred Item"),
                                                       KStandardGuiItem::del(),
                                                       KStandardGuiItem::cancel(),
                                                       QStringLiteral("klipperRemoveStarredItemAskAgain"));
        if (result != KMessageBox::Continue) {
            return false; // User cancelled deletion
        }
    }

    return removeRow(index, QModelIndex());
}

int HistoryModel::indexOf(const QString &uuid) const
{
    // Also match the content hash of unlocked encrypted items, so a re-copied entry is found
    auto it = std::ranges::find_if(m_items, [&uuid](const auto &item) {
        return item->uuid() == uuid || item->contentUuid() == uuid;
    });
    return it == m_items.cend() ? -1 : std::distance(m_items.cbegin(), it);
}

int HistoryModel::indexOf(const HistoryItem *item) const
{
    if (!item) [[unlikely]] {
        return -1;
    }
    return indexOf(item->uuid());
}

HistoryItemConstPtr HistoryModel::first() const
{
    if (m_items.empty() || m_items[0]->isEncrypted()) {
        return {};
    }
    return m_items[0];
}

bool HistoryModel::insert(const QMimeData *mimeData, qreal timestamp)
{
    if (m_maxSize == 0) {
        // special case - cannot insert any items
        return false;
    }
    QString uuid = computeUuid(mimeData);
    if (uuid.size() != 40 /*SHA1*/) [[unlikely]] {
        return false;
    }
    if (const int existingItemIndex = indexOf(uuid); existingItemIndex >= 0) {
        // move to top
        moveToTop(existingItemIndex);
        return true;
    }

    QStringList formats = mimeData->formats();
    if (formats.empty() || formats.size() > 50) [[unlikely]] {
        return false;
    }

    QString text;
    if (mimeData->hasUrls()) {
        QStringList urlText;
        for (const QList<QUrl> urls = mimeData->urls(); const QUrl &url : urls) {
            urlText.append(url.toString(QUrl::FullyEncoded));
        }
        text = urlText.join(u' ');
    } else {
        text = mimeData->text();
    }

    auto item = std::make_shared<HistoryItem>(uuid, formats, text);
    auto updateJob = UpdateDatabaseJob::updateClipboard(this, &m_db, m_dbFolder, uuid, text, mimeData, timestamp);
    if (item->type() == HistoryItemType::Image) {
        connect(updateJob, &KJob::finished, this, [this, uuid](KJob *job) {
            if (job->error()) {
                return;
            }
            if (int row = indexOf(uuid); row >= 0) {
                Q_EMIT dataChanged(index(row), index(row), {Qt::DisplayRole, ImageUrlRole, ImageSizeRole});
            }
        });
    }

    beginInsertRows(QModelIndex(), 0, 0);
    m_items.prepend(std::move(item));
    endInsertRows();

    // BUG 417590: Remove only after an item is inserted to avoid clearing clipboard
    if (m_items.size() > m_maxSize) {
        // Find the first non-starred item from the end to remove, skipping starred items
        int itemToRemove = -1;
        for (qsizetype i = m_items.size() - 1; i >= 0; --i) {
            // Check if item is starred
            QSqlQuery query(m_db);
            query.prepare(u"SELECT starred FROM main WHERE uuid = ?"_s);
            query.addBindValue(m_items[i]->uuid());
            bool isStarred = false;
            if (query.exec() && query.isSelect() && query.next()) {
                isStarred = query.value(0).toBool();
            }

            if (!isStarred) {
                itemToRemove = i;
                break;
            }
        }

        if (itemToRemove >= 0) {
            removeRow(itemToRemove);
        }
        // If no non-starred items found, we don't remove anything
        // This means starred items can cause the history to exceed maxSize
        // TODO: Consider implementing a separate starred item limit or warning mechanism
    }

    ++m_pendingJobs;
    m_pendingWrites.insert(uuid);
    connect(updateJob, &KJob::finished, this, [this, uuid] {
        --m_pendingJobs;
        m_pendingWrites.remove(uuid);
        scheduleEnforceVault(); // the item can only be encrypted once it is fully written
    });
    updateJob->start();
    scheduleEnforceVault();

    return true;
}

bool HistoryModel::insert(const QString &text)
{
    auto data = std::make_unique<QMimeData>();
    data->setText(text);
    return insert(data.get());
}

bool HistoryModel::loadHistory()
{
    constexpr const char *failedLoadWarning = "Failed to load history resource. Clipboard history cannot be read.";
    // don't use "appdata", klipper is also a kicker applet
    if (qEnvironmentVariableIsSet("KLIPPER_DATABASE")) {
        m_dbFolder = QFileInfo(qEnvironmentVariable("KLIPPER_DATABASE")).absoluteDir().absolutePath();
    } else {
        m_dbFolder = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + u"/klipper";
    }
    QDir dataDir(m_dbFolder + u"/data");
    dataDir.mkpath(dataDir.absolutePath());
    // Clipboard contents are private: owner-only access (stock Klipper leaves 0755/0644)
    constexpr auto privateDir = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
    if (!qEnvironmentVariableIsSet("KLIPPER_DATABASE")) {
        QFile::setPermissions(m_dbFolder, privateDir);
    }
    QFile::setPermissions(dataDir.absolutePath(), privateDir);
    // Leftovers of an interrupted clear()
    for (const QString &trash : QDir(m_dbFolder).entryList({u"data.trash-*"_s}, QDir::Dirs | QDir::NoDotAndDotDot)) {
        QDir(m_dbFolder + u'/' + trash).removeRecursively();
    }
    // removeFolders() retires individual payload directories before deleting them asynchronously.
    // Clean up any such retired directories left behind by a crash or forced process exit.
    for (const QString &trash : dataDir.entryList({u"vault-trash-*"_s}, QDir::Dirs | QDir::NoDotAndDotDot)) {
        QDir(dataDir.filePath(trash)).removeRecursively();
    }

    // Try to reuse the previous connection
    m_db = QSqlDatabase::database(u"klipper"_s);
    if (!m_db.isValid()) [[likely]] {
        m_db = QSqlDatabase::addDatabase(u"QSQLITE"_s, u"klipper"_s);
        m_db.setHostName(u"localhost"_s);
        if (qEnvironmentVariableIsSet("KLIPPER_DATABASE")) {
            m_db.setDatabaseName(qEnvironmentVariable("KLIPPER_DATABASE"));
        } else {
            m_db.setDatabaseName(m_dbFolder + u"/history3.sqlite");
        }
    }
    if (!m_db.isOpen() && !m_db.open()) {
        qCWarning(KLIPPER_LOG) << failedLoadWarning << m_db.lastError().text();
        return false;
    }
    for (const QString &suffix : {u""_s, u"-wal"_s, u"-shm"_s}) {
        if (QFile::exists(m_db.databaseName() + suffix)) {
            QFile::setPermissions(m_db.databaseName() + suffix, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        }
    }

    QSqlQuery query(m_db);
    query.exec(u"PRAGMA journal_mode=WAL"_s);
    // Overwrite deleted content with zeros, so plaintext of encrypted or removed entries does not linger in free pages
    query.exec(u"PRAGMA secure_delete=ON"_s);
    // The main table only stores text data
    query.exec(
        u"CREATE TABLE IF NOT EXISTS main (uuid char(40) PRIMARY KEY, added_time REAL NOT NULL CHECK (added_time > 0), last_used_time REAL CHECK (last_used_time > 0), mimetypes TEXT NOT NULL, text NTEXT, starred BOOLEAN)"_s);
    // The aux table stores data index
    query.exec(u"CREATE TABLE IF NOT EXISTS aux (uuid char(40) NOT NULL, mimetype TEXT NOT NULL, data_uuid char(40) NOT NULL, PRIMARY KEY (uuid, mimetype))"_s);
    // Save the latest version number
    query.exec(u"CREATE TABLE IF NOT EXISTS version (db_version INT NOT NULL)"_s);
    constexpr int currentDBVersion = 3;
    if (query.exec(u"SELECT db_version FROM version"_s) && query.isSelect() && query.next() /* has a record */) {
        if (query.value(0).toInt() != currentDBVersion) {
            return false;
        }
    } else if (!query.exec(u"INSERT INTO version (db_version) VALUES (%1)"_s.arg(QString::number(currentDBVersion)))) {
        qCWarning(KLIPPER_LOG) << failedLoadWarning << m_db.lastError().text();
        return false;
    }
    // Vault tables are additive (db_version stays 3), so stock Klipper still opens the database;
    // it then shows encrypted entries as empty text items.
    if (!m_vault.load(m_db)) {
        qCWarning(KLIPPER_LOG) << "History encryption is unavailable";
    }
    m_autoLockTimer.stop();

    if (!m_bKeepContents) {
        QSqlQuery clearQuery(m_db);
        clearQuery.exec(u"DELETE FROM main"_s);
        clearQuery.exec(u"DELETE FROM aux"_s);
        clearQuery.exec(u"DELETE FROM vault"_s);
        clearQuery.exec(u"VACUUM"_s);
        if (dataDir.exists()) {
            dataDir.removeRecursively();
            dataDir.mkpath(dataDir.absolutePath());
        }
    }

    if (m_maxSize == 0) {
        return true;
    }

    // The last row is either items.size() - 1 or m_maxSize - 1.
    decltype(m_items) items;
    int starredCount = 0;
    if (query.exec(u"SELECT * FROM main ORDER BY last_used_time DESC, added_time DESC LIMIT %1"_s.arg(QString::number(m_maxSize))) && query.isSelect()) {
        items.reserve(std::max(query.size(), 1));
        while (query.next()) {
            if (HistoryItemPtr item = HistoryItem::create(query)) {
                items.emplace_back(std::move(item));
                starredCount += query.value(u"starred"_s).toBool() ? 1 : 0;
            }
        }
    }

    if (items.empty()) {
        // special case - nothing to insert, but don't keep showing stale items from a previous load
        if (!m_items.empty()) {
            beginResetModel();
            m_items.clear();
            m_starredCount = 0;
            endResetModel();
        }
        Q_EMIT vaultStateChanged();
        return true;
    }

    beginResetModel();
    m_items = std::move(items);
    m_starredCount = starredCount;
    endResetModel();

    if (!m_items[0]->isEncrypted()) {
        m_clip->setMimeData(m_items[0], SystemClipboard::SelectionMode(SystemClipboard::Clipboard | SystemClipboard::Selection));
    }
    Q_EMIT vaultStateChanged();
    // e.g. entries written by stock Klipper while this build wasn't running
    scheduleEnforceVault();

    return true;
}

bool HistoryModel::saveClipboardHistory()
{
    QSqlQuery query(u"PRAGMA wal_checkpoint"_s, m_db);
    return query.exec();
}

void HistoryModel::loadSettings()
{
    setMaxSize(KlipperSettings::maxClipItems());
    m_displayImages = !KlipperSettings::ignoreImages();
    m_bNoNullClipboard = KlipperSettings::preventEmptyClipboard();
    // 0 is the id of "Ignore selection" radiobutton
    m_bIgnoreSelection = KlipperSettings::ignoreSelection();
    m_bKeepContents = KlipperSettings::keepClipboardContents();
    m_bSynchronize = KlipperSettings::syncClipboards();
    m_bSelectionTextOnly = KlipperSettings::selectionTextOnly();
    if (const qsizetype plaintextItems = KlipperSettings::plaintextHistoryItems(); plaintextItems != m_plaintextItems) {
        m_plaintextItems = plaintextItems;
        scheduleEnforceVault();
    }
    if (m_vault.isUnlocked()) {
        restartAutoLock();
    }

    if (m_bNoNullClipboard) {
        connect(m_clip.get(), &SystemClipboard::receivedEmptyClipboard, this, &HistoryModel::slotReceivedEmptyClipboard, Qt::UniqueConnection);
    } else {
        disconnect(m_clip.get(), &SystemClipboard::receivedEmptyClipboard, this, &HistoryModel::slotReceivedEmptyClipboard);
    }

    // BUG: 142882
    // Security: If user has save clipboard turned off, old data should be deleted from disk
    if (!m_bKeepContents) {
        clear();
    }
}

int HistoryModel::pendingJobs() const
{
    return m_pendingJobs;
}

void HistoryModel::moveToTop(const QString &uuid)
{
    const int existingItemIndex = indexOf(uuid);
    if (existingItemIndex < 0) {
        return;
    }
    moveToTop(existingItemIndex);
}

KCoreConfigSkeleton *HistoryModel::settings()
{
    return KlipperSettings::self();
}

void HistoryModel::moveToTop(qsizetype row)
{
    if (row >= m_items.size()) [[unlikely]] {
        Q_ASSERT_X(false, Q_FUNC_INFO, std::to_string(row).c_str());
        return;
    }
    if (const HistoryItemPtr item = m_items[row]; item->isEncrypted()) {
        if (item->isLocked()) {
            Q_EMIT unlockRequested();
            return;
        }
        // Copied again while the vault was locked: keep the plain copy, drop the encrypted one
        auto plainCopy = std::ranges::find_if(m_items, [&item](const auto &other) {
            return !other->isEncrypted() && other->uuid() == item->contentUuid();
        });
        if (plainCopy != m_items.cend()) {
            const QString plainUuid = (*plainCopy)->uuid();
            removeRow(row);
            moveToTop(indexOf(plainUuid));
            return;
        }
        // Decrypt back into a normal entry so it can be put on the clipboard
        if (!applyVault({}, {row}) || m_items[row]->isEncrypted()) {
            qCWarning(KLIPPER_LOG) << "Failed to decrypt history entry";
            return;
        }
    }
    scheduleEnforceVault(); // the item it displaces may leave the plaintext window
    if (row == 0) {
        // The item is already at the top, but it still may be not be set as the actual clipboard
        // contents, normally this happens if the item is only in the X11 mouse selection but
        // not in the Ctrl+V clipboard, or if the current contents are not saved in the history.
        Q_EMIT changed(true);
        return;
    }
    beginMoveRows(QModelIndex(), row, row, QModelIndex(), 0);
    m_items.move(row, 0);
    endMoveRows();
}

void HistoryModel::saveToFile(QStringView dbFolder, const QByteArray &data, QStringView newUuid, QStringView dataUuid)
{
    const QString folderPath = dbFolder + u"/data/" + newUuid;
    QDir().mkpath(folderPath);
    QSaveFile file(folderPath + u'/' + dataUuid);
    if (!file.open(QIODevice::WriteOnly)) {
        qCWarning(KLIPPER_LOG) << file.errorString() << folderPath;
        return;
    }
    file.write(data);
    file.commit();
}

void HistoryModel::moveTopToBack()
{
    if (m_items.size() < 2 || m_items[1]->isLocked()) { // would put an unreadable item on the clipboard
        return;
    }
    beginMoveRows(QModelIndex(), 0, 0, QModelIndex(), m_items.size());
    auto item = m_items.takeFirst();
    m_items.append(item);
    endMoveRows();
    scheduleEnforceVault();
}

void HistoryModel::moveBackToTop()
{
    if (m_items.empty() || m_items.constLast()->isLocked()) {
        return;
    }
    moveToTop(m_items.size() - 1);
}

QHash<int, QByteArray> HistoryModel::roleNames() const
{
    QHash<int, QByteArray> hash;
    hash.insert(Qt::DisplayRole, QByteArrayLiteral("display"));
    hash.insert(ImageUrlRole, QByteArrayLiteral("decoration"));
    hash.insert(ImageSizeRole, QByteArrayLiteral("imageSize"));
    hash.insert(UuidRole, QByteArrayLiteral("uuid"));
    hash.insert(TypeIntRole, QByteArrayLiteral("type"));
    hash.insert(StarredRole, QByteArrayLiteral("starred"));
    hash.insert(EncryptedRole, QByteArrayLiteral("encrypted"));
    hash.insert(LockedRole, QByteArrayLiteral("locked"));
    return hash;
}

void HistoryModel::checkClipData(QClipboard::Mode mode, const QMimeData *data)
{
    Q_ASSERT(m_clip->isLocked(QClipboard::Selection) || m_clip->isLocked(QClipboard::Clipboard));
    bool changed = true; // ### FIXME (only relevant under polling, might be better to simply remove polling and rely on XFixes)

    // this must be below the "bNoNullClipboard" handling code!
    // XXX: I want a better handling of selection/clipboard in general.
    // XXX: Order sensitive code. Must die.
    const bool selectionMode = mode == QClipboard::Selection;
    if (selectionMode && m_bIgnoreSelection) {
        if (m_bSynchronize) {
            m_clip->setMimeData(data, SystemClipboard::Clipboard, SystemClipboard::ClipboardUpdateReason::SyncSelection);
        }
        return;
    }

    if (selectionMode && m_bSelectionTextOnly && !data->hasText()) {
        return;
    }

    if (data->data(QStringLiteral("x-kde-passwordManagerHint")) == QByteArrayView("secret")) {
        return;
    }

    if (!m_displayImages && data->hasImage() && !data->hasText() /*BUG 491488*/ && !data->hasFormat(QStringLiteral("x-kde-force-image-copy"))) {
        return;
    }

    if (changed && insert(data)) [[likely]] {
        qCDebug(KLIPPER_LOG) << "Synchronize?" << m_bSynchronize;
        if (m_bSynchronize) { // applyClipChanges can return nullptr
            m_clip->setMimeData(data, mode == QClipboard::Selection ? SystemClipboard::Clipboard : SystemClipboard::Selection);
        }
    }
}

void HistoryModel::slotIgnored(QClipboard::Mode mode)
{
    // internal to klipper, ignoring QSpinBox selections
    // keep our old clipboard, thanks
    // This won't quite work, but it's close enough for now.
    // The trouble is that the top selection =! top clipboard
    // but we don't track that yet. We will....
    if (auto top = first()) {
        m_clip->setMimeData(top, mode == QClipboard::Selection ? SystemClipboard::Selection : SystemClipboard::Clipboard);
    }
}

void HistoryModel::slotReceivedEmptyClipboard(QClipboard::Mode mode)
{
    Q_ASSERT(m_bNoNullClipboard);
    if (auto top = first()) {
        // keep old clipboard after someone set it to null
        qCDebug(KLIPPER_LOG) << "Resetting clipboard (Prevent empty clipboard)";
        m_clip->setMimeData(top,
                            mode == QClipboard::Selection ? SystemClipboard::Selection : SystemClipboard::Clipboard,
                            SystemClipboard::ClipboardUpdateReason::PreventEmptyClipboard);
    }
}

bool HistoryModel::isItemStarred(const QString &uuid) const
{
    QSqlQuery query(m_db);
    query.prepare(u"SELECT starred FROM main WHERE uuid = ?"_s);
    query.addBindValue(uuid);
    if (query.exec() && query.isSelect() && query.next()) {
        return query.value(0).toBool();
    }
    return false;
}

QString HistoryModel::dataFolder(const QString &uuid) const
{
    return m_dbFolder + u"/data/" + uuid;
}

void HistoryModel::removeFolders(const QStringList &paths)
{
    if (paths.empty()) {
        return;
    }

    // A payload directory is named after its content UUID. It may be recreated by a
    // promotion or a re-copy while background cleanup is still running. Never let a
    // delayed recursive delete target that live path: atomically rename each retired
    // directory first, then delete only the unique retired name on the worker thread.
    QStringList retiredPaths;
    retiredPaths.reserve(paths.size());
    for (const QString &path : paths) {
        const QFileInfo info(path);
        if (!info.isDir()) {
            continue;
        }
        QDir parent(info.absolutePath());
        const QString retiredName = u"vault-trash-"_s + randomUuid();
        if (parent.rename(info.fileName(), retiredName)) {
            retiredPaths.append(parent.filePath(retiredName));
        } else {
            // Do not schedule deletion against the reusable original name on failure.
            // This rare fallback is synchronous to preserve the same no-race guarantee.
            qCWarning(KLIPPER_LOG) << "Could not retire clipboard payload directory" << path;
            if (!QDir(info.absoluteFilePath()).removeRecursively()) {
                qCWarning(KLIPPER_LOG) << "Could not remove clipboard payload directory" << path;
            }
        }
    }
    if (retiredPaths.empty()) {
        return;
    }

    ++m_pendingJobs;
    auto watcher = new QFutureWatcher<void>(this);
    connect(watcher, &QFutureWatcher<void>::finished, this, [this, watcher] {
        --m_pendingJobs;
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([retiredPaths] {
        for (const QString &path : retiredPaths) {
            QDir(path).removeRecursively();
        }
    }));
}

bool HistoryModel::isVaultConfigured() const
{
    return m_vault.isConfigured();
}

bool HistoryModel::isVaultUnlocked() const
{
    return m_vault.isUnlocked();
}

bool HistoryModel::isVaultBusy() const
{
    return m_vaultBusy;
}

int HistoryModel::lockedCount() const
{
    return std::ranges::count_if(m_items, [](const auto &item) {
        return item->isLocked();
    });
}

qsizetype HistoryModel::plaintextItems() const
{
    return m_plaintextItems;
}

void HistoryModel::setVaultBusy(bool busy)
{
    if (m_vaultBusy != busy) {
        m_vaultBusy = busy;
        Q_EMIT vaultStateChanged();
    }
}

void HistoryModel::restartAutoLock()
{
    restartIfNeeded(m_autoLockTimer, m_vault.isUnlocked() ? KlipperSettings::vaultAutoLockMinutes() : 0);
}

void HistoryModel::slotScreenSaverActiveChanged(bool active)
{
    if (active) {
        lockVault();
    }
}

void HistoryModel::scheduleEnforceVault()
{
    if (m_enforceScheduled || !m_vault.isConfigured()) {
        return;
    }
    m_enforceScheduled = true;
    QMetaObject::invokeMethod(this, &HistoryModel::enforceVault, Qt::QueuedConnection);
}

void HistoryModel::enforceVault()
{
    m_enforceScheduled = false;
    if (!m_vault.isConfigured() || !m_db.isOpen() || m_vaultBusy) {
        return;
    }

    // Duplicates can appear when an entry is copied again while its older copy is locked
    if (m_vault.isUnlocked()) {
        QSet<QString> seen;
        for (qsizetype i = 0; i < m_items.size();) {
            const QString &contentUuid = m_items[i]->contentUuid();
            if (!contentUuid.isEmpty() && seen.contains(contentUuid)) {
                removeRow(i);
                continue;
            }
            seen.insert(contentUuid);
            ++i;
        }
    }

    // Policy: the newest m_plaintextItems entries are plain, everything older is encrypted.
    // Work in batches so a first-time setup over thousands of entries keeps the UI responsive.
    constexpr qsizetype batchSize = 200;
    QList<qsizetype> demoteRows;
    QList<qsizetype> promoteRows;
    for (qsizetype i = 0; i < m_items.size() && demoteRows.size() + promoteRows.size() < batchSize; ++i) {
        const auto &item = m_items[i];
        if (i < m_plaintextItems) {
            if (item->isEncrypted() && !item->isLocked()) {
                promoteRows.append(i);
            }
        } else if (!item->isEncrypted() && !m_pendingWrites.contains(item->uuid())) {
            demoteRows.append(i);
        }
    }
    const bool batchFull = demoteRows.size() + promoteRows.size() >= batchSize;
    const bool changedSomething = applyVault(demoteRows, promoteRows);
    if (batchFull && changedSomething) {
        scheduleEnforceVault();
        return;
    }
    if (m_vacuumPending) {
        // First setup over an existing history: rebuild the file so no page still holds old plaintext
        m_vacuumPending = false;
        QSqlQuery(u"VACUUM"_s, m_db).exec();
        QSqlQuery(u"PRAGMA wal_checkpoint(TRUNCATE)"_s, m_db).exec();
    }
}

bool HistoryModel::applyVault(const QList<qsizetype> &demoteRows, const QList<qsizetype> &promoteRows)
{
    if (!m_vault.isConfigured() || (demoteRows.empty() && promoteRows.empty())) {
        return false;
    }

    QList<std::pair<qsizetype, HistoryItemPtr>> replacements;
    QStringList obsoleteFolders; // plaintext data of demoted items, deleted after commit
    QStringList writtenFolders; // data of promoted items, deleted again on rollback
    bool ok = false;
    {
        TransactionGuard transaction(&m_db);

        for (const qsizetype row : demoteRows) {
            const HistoryItemPtr item = m_items[row];
            const QString oldUuid = item->uuid();
            QSqlQuery query(m_db);
            query.prepare(u"SELECT mimetypes, text FROM main WHERE uuid=?"_s);
            query.addBindValue(oldUuid);
            if (!query.exec() || !query.next()) {
                continue; // not (or no longer) in the database
            }
            VaultMeta meta{oldUuid, query.value(0).toString(), query.value(1).toString(), {}};

            AuxRows auxRows;
            Blobs blobs;
            query.prepare(u"SELECT mimetype, data_uuid FROM aux WHERE uuid=?"_s);
            query.addBindValue(oldUuid);
            query.exec();
            while (query.next()) {
                const QString mimeType = query.value(0).toString();
                const QString dataUuid = query.value(1).toString();
                auxRows.emplace_back(mimeType, dataUuid);
                if (!blobs.contains(dataUuid)) {
                    QFile file(dataFolder(oldUuid) + u'/' + dataUuid);
                    blobs.insert(dataUuid, file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray());
                }
                if (mimeType == s_imageFormat) {
                    QBuffer buffer(&blobs[dataUuid]);
                    buffer.open(QIODevice::ReadOnly);
                    meta.imageSize = QImageReader(&buffer).size();
                }
            }

            QByteArray payload;
            {
                QDataStream out(&payload, QIODevice::WriteOnly);
                out.setVersion(QDataStream::Qt_6_5);
                out << s_vaultFormat << auxRows << blobs;
            }
            for (QByteArray &blob : blobs) {
                HistoryVault::wipe(blob);
            }
            QByteArray metaData = serializeMeta(meta);

            const QString newUuid = randomUuid();
            const QByteArray sealedMeta = m_vault.seal(metaData, metaAd(newUuid));
            const QByteArray sealedPayload = m_vault.seal(payload, payloadAd(newUuid));
            HistoryVault::wipe(metaData);
            HistoryVault::wipe(payload);
            if (sealedMeta.isEmpty() || sealedPayload.isEmpty()) {
                break;
            }

            QSqlQuery insert(m_db);
            insert.prepare(u"INSERT INTO vault (uuid, meta, payload) VALUES (?, ?, ?)"_s);
            insert.addBindValue(newUuid);
            insert.addBindValue(sealedMeta);
            insert.addBindValue(sealedPayload);
            QSqlQuery update(m_db);
            update.prepare(u"UPDATE main SET uuid=?, mimetypes=?, text=NULL WHERE uuid=?"_s);
            update.addBindValue(newUuid);
            update.addBindValue(QString(HistoryItem::encryptedMimeType));
            update.addBindValue(oldUuid);
            QSqlQuery deleteAux(m_db);
            deleteAux.prepare(u"DELETE FROM aux WHERE uuid=?"_s);
            deleteAux.addBindValue(oldUuid);
            if (!transaction.exec(insert) || !transaction.exec(update) || !transaction.exec(deleteAux)) {
                break;
            }
            obsoleteFolders.append(dataFolder(oldUuid));
            replacements.emplace_back(row,
                                      m_vault.isUnlocked()
                                          ? HistoryItem::createEncrypted(newUuid, oldUuid, meta.mimeTypes.split(u','), meta.text, meta.imageSize)
                                          : HistoryItem::createLocked(newUuid));
        }

        for (const qsizetype row : promoteRows) {
            const HistoryItemPtr item = m_items[row];
            if (!item->isEncrypted() || item->isLocked() || !transaction.committed) {
                continue;
            }
            const QString oldUuid = item->uuid();
            QSqlQuery query(m_db);
            query.prepare(u"SELECT meta, payload FROM vault WHERE uuid=?"_s);
            query.addBindValue(oldUuid);
            if (!query.exec() || !query.next()) {
                continue;
            }
            std::optional<QByteArray> metaData = m_vault.open(query.value(0).toByteArray(), metaAd(oldUuid));
            std::optional<QByteArray> payload = m_vault.open(query.value(1).toByteArray(), payloadAd(oldUuid));
            const std::optional<VaultMeta> meta = metaData ? parseMeta(*metaData) : std::nullopt;
            if (!meta || !payload) {
                qCWarning(KLIPPER_LOG) << "Encrypted history entry failed authentication" << oldUuid;
                continue;
            }
            quint8 format = 0;
            AuxRows auxRows;
            Blobs blobs;
            {
                QDataStream in(*payload);
                in.setVersion(QDataStream::Qt_6_5);
                in >> format >> auxRows >> blobs;
                if (format != s_vaultFormat || in.status() != QDataStream::Ok) {
                    continue;
                }
            }
            HistoryVault::wipe(*payload);
            HistoryVault::wipe(*metaData);

            const QString &contentUuid = meta->contentUuid;
            for (auto it = blobs.begin(); it != blobs.end(); ++it) {
                saveToFile(m_dbFolder, it.value(), contentUuid, it.key());
                HistoryVault::wipe(it.value());
            }
            writtenFolders.append(dataFolder(contentUuid));

            for (const auto &[mimeType, dataUuid] : std::as_const(auxRows)) {
                QSqlQuery insert(m_db);
                insert.prepare(u"INSERT INTO aux (uuid, mimetype, data_uuid) VALUES (?, ?, ?)"_s);
                insert.addBindValue(contentUuid);
                insert.addBindValue(mimeType);
                insert.addBindValue(dataUuid);
                transaction.exec(insert);
            }
            QSqlQuery update(m_db);
            update.prepare(u"UPDATE main SET uuid=?, mimetypes=?, text=? WHERE uuid=?"_s);
            update.addBindValue(contentUuid);
            update.addBindValue(meta->mimeTypes);
            update.addBindValue(meta->text);
            update.addBindValue(oldUuid);
            QSqlQuery deleteVault(m_db);
            deleteVault.prepare(u"DELETE FROM vault WHERE uuid=?"_s);
            deleteVault.addBindValue(oldUuid);
            if (!transaction.exec(update) || !transaction.exec(deleteVault)) {
                break;
            }
            replacements.emplace_back(row, std::make_shared<HistoryItem>(contentUuid, meta->mimeTypes.split(u','), meta->text));
        }

        ok = transaction.inited && transaction.committed;
    }

    if (!ok) {
        removeFolders(writtenFolders);
        return false;
    }
    removeFolders(obsoleteFolders);
    for (auto &[row, item] : replacements) {
        m_items[row] = std::move(item);
        // UuidRole: lets the top entry be re-applied to the clipboard once it is decrypted
        Q_EMIT dataChanged(index(row),
                           index(row),
                           {Qt::DisplayRole, UuidRole, TypeRole, TypeIntRole, ImageUrlRole, ImageSizeRole, EncryptedRole, LockedRole});
    }
    if (!demoteRows.empty()) {
        // The WAL still holds the plaintext pages we just replaced; fold it into the (secure_delete) main file and truncate it
        QSqlQuery(u"PRAGMA wal_checkpoint(TRUNCATE)"_s, m_db).exec();
    }
    if (!replacements.empty()) {
        Q_EMIT vaultStateChanged();
    }
    return !replacements.empty();
}

void HistoryModel::setupVault(const QString &password)
{
    if (m_vault.isConfigured() || m_vaultBusy || password.isEmpty() || !m_db.isOpen()) {
        Q_EMIT vaultOperationFinished(VaultOperation::Setup, false);
        return;
    }
    setVaultBusy(true);
    auto watcher = new QFutureWatcher<std::optional<HistoryVault::NewKeys>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
        std::optional<HistoryVault::NewKeys> keys = watcher->result();
        watcher->deleteLater();
        setVaultBusy(false);
        const bool ok = keys && m_vault.install(m_db, std::move(*keys));
        if (ok) {
            m_vacuumPending = true;
            restartAutoLock();
            scheduleEnforceVault();
        }
        Q_EMIT vaultStateChanged();
        Q_EMIT vaultOperationFinished(VaultOperation::Setup, ok);
    });
    watcher->setFuture(QtConcurrent::run([password = password.toUtf8()]() mutable {
        return HistoryVault::createKeys(std::move(password));
    }));
}

void HistoryModel::unlockVault(const QString &password)
{
    if (!m_vault.isConfigured() || m_vault.isUnlocked() || m_vaultBusy) {
        Q_EMIT vaultOperationFinished(VaultOperation::Unlock, m_vault.isUnlocked());
        return;
    }
    QList<std::pair<QString, QByteArray>> sealedMetas;
    if (QSqlQuery query(m_db); query.exec(u"SELECT uuid, meta FROM vault"_s)) {
        while (query.next()) {
            sealedMetas.emplace_back(query.value(0).toString(), query.value(1).toByteArray());
        }
    }

    struct Result {
        HistoryVault::SecretKey secretKey;
        QHash<QString, VaultMeta> metas;
    };
    setVaultBusy(true);
    auto watcher = new QFutureWatcher<Result>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
        Result result = watcher->result();
        watcher->deleteLater();
        setVaultBusy(false);
        if (!result.secretKey) {
            Q_EMIT vaultOperationFinished(VaultOperation::Unlock, false);
            return;
        }
        m_vault.setSecretKey(std::move(result.secretKey));
        for (auto &item : m_items) {
            if (!item->isLocked()) {
                continue;
            }
            if (auto it = result.metas.constFind(item->uuid()); it != result.metas.cend()) {
                item = HistoryItem::createEncrypted(item->uuid(), it->contentUuid, it->mimeTypes.split(u','), it->text, it->imageSize);
            }
        }
        if (!m_items.empty()) {
            Q_EMIT dataChanged(index(0), index(m_items.size() - 1), {Qt::DisplayRole, TypeRole, TypeIntRole, ImageSizeRole, EncryptedRole, LockedRole});
        }
        restartAutoLock();
        Q_EMIT vaultStateChanged();
        Q_EMIT vaultOperationFinished(VaultOperation::Unlock, true);
        scheduleEnforceVault();
    });
    // Argon2id and the per-entry decryption run off the GUI thread
    watcher->setFuture(QtConcurrent::run([params = m_vault.params(), password = password.toUtf8(), sealedMetas = std::move(sealedMetas)]() mutable {
        Result result;
        result.secretKey = HistoryVault::unwrapSecretKey(params, std::move(password));
        if (!result.secretKey) {
            return result;
        }
        for (const auto &[uuid, sealed] : std::as_const(sealedMetas)) {
            std::optional<QByteArray> plain = HistoryVault::open(result.secretKey, params.publicKey, sealed, metaAd(uuid));
            if (!plain) {
                qCWarning(KLIPPER_LOG) << "Encrypted history entry failed authentication" << uuid;
                continue;
            }
            if (std::optional<VaultMeta> meta = parseMeta(*plain)) {
                result.metas.insert(uuid, std::move(*meta));
            }
            HistoryVault::wipe(*plain);
        }
        return result;
    }));
}

void HistoryModel::lockVault()
{
    m_autoLockTimer.stop();
    if (!m_vault.isUnlocked()) {
        return;
    }
    m_vault.lock(); // frees (and zeroes) the secret key
    for (auto &item : m_items) {
        if (item->isEncrypted()) {
            item = HistoryItem::createLocked(item->uuid());
        }
    }
    if (!m_items.empty()) {
        Q_EMIT dataChanged(index(0), index(m_items.size() - 1), {Qt::DisplayRole, TypeRole, TypeIntRole, ImageSizeRole, EncryptedRole, LockedRole});
    }
    Q_EMIT vaultStateChanged();
}

void HistoryModel::changeVaultPassword(const QString &oldPassword, const QString &newPassword)
{
    if (!m_vault.isConfigured() || m_vaultBusy || newPassword.isEmpty()) {
        Q_EMIT vaultOperationFinished(VaultOperation::ChangePassword, false);
        return;
    }
    setVaultBusy(true);
    auto watcher = new QFutureWatcher<HistoryVault::Params>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
        HistoryVault::Params params = watcher->result();
        watcher->deleteLater();
        setVaultBusy(false);
        const bool ok = !params.wrappedSecretKey.isEmpty() && m_vault.installParams(m_db, std::move(params));
        if (ok) {
            // The old wrapped key is still in free pages / the WAL; with it the old password would still open the vault
            QSqlQuery(u"PRAGMA wal_checkpoint(TRUNCATE)"_s, m_db).exec();
        }
        Q_EMIT vaultOperationFinished(VaultOperation::ChangePassword, ok);
    });
    // The old password is always verified, even when unlocked, so an unattended unlocked session can't change it
    watcher->setFuture(QtConcurrent::run([params = m_vault.params(), oldPassword = oldPassword.toUtf8(), newPassword = newPassword.toUtf8()]() mutable {
        HistoryVault::SecretKey secretKey = HistoryVault::unwrapSecretKey(params, std::move(oldPassword));
        if (!secretKey) {
            HistoryVault::wipe(newPassword);
            return HistoryVault::Params{};
        }
        return HistoryVault::rewrap(params, secretKey, std::move(newPassword));
    }));
}

void HistoryModel::resetVault()
{
    if (!m_vault.isConfigured() || m_vaultBusy) {
        return;
    }
    {
        TransactionGuard transaction(&m_db);
        QSqlQuery query(m_db);
        query.prepare(u"DELETE FROM main WHERE mimetypes=?"_s);
        query.addBindValue(QString(HistoryItem::encryptedMimeType));
        if (!transaction.exec(query) || !transaction.exec(u"DELETE FROM vault"_s)) {
            return;
        }
    }
    m_vault.reset(m_db);
    m_autoLockTimer.stop();
    int starredCount = m_starredCount;
    for (qsizetype i = m_items.size() - 1; i >= 0; --i) {
        if (m_items[i]->isEncrypted()) {
            beginRemoveRows(QModelIndex(), i, i);
            m_items.removeAt(i);
            endRemoveRows();
        }
    }
    // Starred encrypted rows are gone from the database now, so recount
    if (QSqlQuery query(u"SELECT COUNT(*) FROM main WHERE starred = 1"_s, m_db); query.next()) {
        starredCount = query.value(0).toInt();
    }
    m_starredCount = starredCount;
    QSqlQuery(u"VACUUM"_s, m_db).exec();
    QSqlQuery(u"PRAGMA wal_checkpoint(TRUNCATE)"_s, m_db).exec();
    Q_EMIT vaultStateChanged();
}

#include "moc_historymodel.cpp"
