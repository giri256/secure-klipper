/*
    SPDX-FileCopyrightText: 2024 Fushan Wen <qydwhotmail@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QPropertyNotifier>
#include <QSortFilterProxyModel>
#include <qqmlregistration.h>

class HistoryModel;

/**
 * This class provides a view for history clip items in QML
 **/
class DeclarativeHistoryModel : public QSortFilterProxyModel
{
    Q_OBJECT
    QML_NAMED_ELEMENT(HistoryModel)

    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(int sourceCount READ sourceCount NOTIFY sourceCountChanged)
    Q_PROPERTY(int starredCount READ starredCount NOTIFY starredCountChanged)
    Q_PROPERTY(QString currentText READ currentText NOTIFY currentTextChanged)

    Q_PROPERTY(bool starredOnly READ starredOnly WRITE setStarredOnly NOTIFY starredOnlyChanged)

    // History vault (entries older than the newest plaintextItems are encrypted)
    Q_PROPERTY(bool vaultConfigured READ vaultConfigured NOTIFY vaultStateChanged)
    Q_PROPERTY(bool vaultUnlocked READ vaultUnlocked NOTIFY vaultStateChanged)
    Q_PROPERTY(bool vaultBusy READ vaultBusy NOTIFY vaultStateChanged)
    Q_PROPERTY(int lockedCount READ lockedCount NOTIFY vaultStateChanged)
    Q_PROPERTY(int plaintextItems READ plaintextItems NOTIFY vaultStateChanged)

public:
    explicit DeclarativeHistoryModel(QObject *parent = nullptr);
    ~DeclarativeHistoryModel() override;

    QString currentText() const;
    int sourceCount() const;
    int starredCount() const;

    bool starredOnly() const;
    void setStarredOnly(bool value);

    Q_INVOKABLE void moveToTop(const QString &uuid);

    Q_INVOKABLE void remove(const QString &uuid);
    Q_INVOKABLE void clearHistory();

    Q_INVOKABLE void invokeAction(const QString &uuid);

    bool vaultConfigured() const;
    bool vaultUnlocked() const;
    bool vaultBusy() const;
    int lockedCount() const;
    int plaintextItems() const;
    Q_INVOKABLE void setupVault(const QString &password);
    Q_INVOKABLE void unlockVault(const QString &password);
    Q_INVOKABLE void lockVault();
    Q_INVOKABLE void changeVaultPassword(const QString &oldPassword, const QString &newPassword);
    Q_INVOKABLE void resetVault();

Q_SIGNALS:
    void countChanged();
    void sourceCountChanged();
    void starredCountChanged();
    void currentTextChanged();
    void starredOnlyChanged();
    void vaultStateChanged();
    /// @p operation: 0 setup, 1 unlock, 2 change password (HistoryModel::VaultOperation)
    void vaultOperationFinished(int operation, bool success);
    /// A locked entry was activated (e.g. via the history cycler): the popup should ask for the password
    void unlockRequested();

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;

private:
    std::shared_ptr<HistoryModel> m_model;
    bool m_starredOnly = false;
    QPropertyNotifier m_starredCountNotifier;
};
