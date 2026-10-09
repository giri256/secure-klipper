/*
    SPDX-FileCopyrightText: 2014 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include <memory>

#include <QAbstractListModel>
#include <QBindable>
#include <QClipboard>
#include <QDateTime>
#include <QSet>
#include <QSqlDatabase>
#include <QTimer>

#include "historyvault.h"
#include "klipper_export.h"

class KCoreConfigSkeleton;
class HistoryItem;
class SystemClipboard;
class UpdateDatabaseJob;

class KLIPPER_EXPORT HistoryModel : public QAbstractListModel
{
    Q_OBJECT
public:
    enum RoleType {
        HistoryItemConstPtrRole = Qt::UserRole,
        UuidRole,
        TypeRole,
        TypeIntRole,
        ImageUrlRole,
        ImageSizeRole,
        StarredRole,
        EncryptedRole, ///< data lives in the history vault
        LockedRole, ///< encrypted and the vault is locked: content unknown, hidden in the UI
    };
    Q_ENUM(RoleType)

    enum class VaultOperation {
        Setup,
        Unlock,
        ChangePassword,
    };
    Q_ENUM(VaultOperation)

    [[nodiscard]] static std::shared_ptr<HistoryModel> self();
    ~HistoryModel() override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QBindable<int> bindableStarredCount() const;
    bool removeRows(int row, int count, const QModelIndex &parent = QModelIndex()) override;
    QHash<int, QByteArray> roleNames() const override;

    /**
     * Remove (first) history item equal to item from history
     */
    bool remove(const QString &uuid);

    qsizetype maxSize() const;
    void setMaxSize(qsizetype size);

    /**
     * Clear history
     */
    void clear();
    void clearHistory();

    /**
     * Move the history in position pos to top
     */
    void moveToTop(const QString &uuid);
    void moveTopToBack();
    void moveBackToTop();

    int indexOf(const QString &uuid) const;
    int indexOf(const HistoryItem *item) const;

    /**
     * Traversal: Get first item
     */
    std::shared_ptr<const HistoryItem> first() const;

    /**
     * Inserts item into clipboard history top
     * if duplicate entry exist, the older duplicate is deleted.
     * The duplicate concept is "deep", so that two text string
     * are considered duplicate if identical.
     */
    bool insert(const QMimeData *mimeData, qreal timestamp = 0);
    bool insert(const QString &text);

    /**
     * @short Loads history from disk.
     * Inserts items into clipboard without any checks
     * Used when restoring a saved history and internally.
     * Don't use this unless you're reasonable the list
     * should be reset.
     */
    bool loadHistory();
    bool saveClipboardHistory();

    void loadSettings();

    int pendingJobs() const;

    /**
     * History vault: when configured, only the newest plaintextItems() entries are kept
     * in plaintext on disk, all older ones are encrypted (see HistoryVault).
     */
    bool isVaultConfigured() const;
    bool isVaultUnlocked() const;
    /// True while a slow (Argon2id) vault operation runs on a worker thread.
    bool isVaultBusy() const;
    /// Number of encrypted entries whose content is unavailable because the vault is locked.
    int lockedCount() const;
    qsizetype plaintextItems() const;
    /// Asynchronous; emits vaultOperationFinished(Setup, ok).
    void setupVault(const QString &password);
    /// Asynchronous; emits vaultOperationFinished(Unlock, ok).
    void unlockVault(const QString &password);
    void lockVault();
    /// Asynchronous; emits vaultOperationFinished(ChangePassword, ok).
    void changeVaultPassword(const QString &oldPassword, const QString &newPassword);
    /// Forgotten password: irrecoverably deletes all encrypted entries and the vault keys.
    void resetVault();
    /// Brings storage in line with the policy now (normally deferred to the event loop).
    void enforceVault();

Q_SIGNALS:
    void changed(bool isTop = false);

    void vaultStateChanged();
    void vaultOperationFinished(HistoryModel::VaultOperation operation, bool success);
    /// The user tried to use an entry that needs the vault unlocked.
    void unlockRequested();

    void actionInvoked(const std::shared_ptr<const HistoryItem> &item);

private Q_SLOTS:
    /**
     * Check data in clipboard, and if it passes these checks,
     * store the data in the clipboard history.
     */
    void checkClipData(QClipboard::Mode mode, const QMimeData *data);

    void slotIgnored(QClipboard::Mode mode);
    void slotReceivedEmptyClipboard(QClipboard::Mode mode);
    void slotScreenSaverActiveChanged(bool active);

private:
    explicit HistoryModel();

    static KCoreConfigSkeleton *settings();

    void moveToTop(qsizetype row);

    static void saveToFile(QStringView dbFolder, const QByteArray &data, QStringView newUuid, QStringView dataUuid);

    void clearNonStarredHistory();

    /**
     * Check if the item is starred before removing
     * @param uuid The UUID of the item to check
     * @return true if the item is starred, false otherwise
     */
    bool isItemStarred(const QString &uuid) const;

    void scheduleEnforceVault();
    /// Encrypts the plain items at @p demoteRows and decrypts the unlocked encrypted items at
    /// @p promoteRows, in one transaction. Returns false if nothing was changed.
    bool applyVault(const QList<qsizetype> &demoteRows, const QList<qsizetype> &promoteRows);
    /// Deletes folders (absolute paths) on a worker thread; counted in pendingJobs().
    void removeFolders(const QStringList &paths);
    QString dataFolder(const QString &uuid) const;
    void setVaultBusy(bool busy);
    void restartAutoLock();

    std::shared_ptr<SystemClipboard> m_clip;
    QList<std::shared_ptr<HistoryItem>> m_items;
    Q_OBJECT_BINDABLE_PROPERTY_WITH_ARGS(HistoryModel, int, m_starredCount, 0)
    int m_pendingJobs = 0;
    QString m_dbFolder;
    QSqlDatabase m_db;
    qsizetype m_maxSize = 0;
    bool m_displayImages = false;
    bool m_bNoNullClipboard = true;
    bool m_bIgnoreSelection = true;
    bool m_bKeepContents = true;
    bool m_bSynchronize = false;
    bool m_bSelectionTextOnly = true;

    HistoryVault m_vault;
    QSet<QString> m_pendingWrites; ///< uuids whose UpdateDatabaseJob is still running
    qsizetype m_plaintextItems = 5;
    bool m_enforceScheduled = false;
    bool m_vaultBusy = false;
    bool m_vacuumPending = false;
    QTimer m_autoLockTimer;

    friend class DeclarativeHistoryModel;
    friend class HistoryModelTest;
};
