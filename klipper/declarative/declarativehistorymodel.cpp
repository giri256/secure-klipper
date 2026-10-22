/*
    SPDX-FileCopyrightText: 2024 Fushan Wen <qydwhotmail@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "declarativehistorymodel.h"
#include "historyitem.h"
#include "historymodel.h"

DeclarativeHistoryModel::DeclarativeHistoryModel(QObject *parent)
    : QSortFilterProxyModel(parent)
    , m_model(HistoryModel::self())
{
    setSourceModel(m_model.get());

    connect(this, &QSortFilterProxyModel::rowsInserted, this, &DeclarativeHistoryModel::countChanged);
    connect(this, &QSortFilterProxyModel::rowsRemoved, this, &DeclarativeHistoryModel::countChanged);
    connect(this, &QSortFilterProxyModel::modelReset, this, &DeclarativeHistoryModel::countChanged);

    // Connect source model changes to sourceCountChanged signal
    connect(m_model.get(), &HistoryModel::rowsInserted, this, &DeclarativeHistoryModel::sourceCountChanged);
    connect(m_model.get(), &HistoryModel::rowsRemoved, this, &DeclarativeHistoryModel::sourceCountChanged);
    connect(m_model.get(), &HistoryModel::modelReset, this, &DeclarativeHistoryModel::sourceCountChanged);

    m_starredCountNotifier = m_model->bindableStarredCount().addNotifier([this] {
        if (m_starredOnly && m_model->bindableStarredCount().value() == 0) {
            // If there are no starred items and we're in starred-only mode, switch to all history
            setStarredOnly(false);
        }
        Q_EMIT starredCountChanged();
    });

    connect(m_model.get(), &HistoryModel::changed, this, &DeclarativeHistoryModel::currentTextChanged);

    connect(m_model.get(), &HistoryModel::vaultStateChanged, this, [this] {
        // Locked rows are hidden; lock/unlock flips that for many rows at once
        invalidateRowsFilter();
        Q_EMIT vaultStateChanged();
    });
    connect(m_model.get(), &HistoryModel::vaultOperationFinished, this, [this](HistoryModel::VaultOperation operation, bool success) {
        Q_EMIT vaultOperationFinished(static_cast<int>(operation), success);
    });
    connect(m_model.get(), &HistoryModel::unlockRequested, this, &DeclarativeHistoryModel::unlockRequested);
}

DeclarativeHistoryModel::~DeclarativeHistoryModel() = default;

QString DeclarativeHistoryModel::currentText() const
{
    return m_model->rowCount() == 0 ? QString() : m_model->index(0).data(Qt::DisplayRole).toString();
}

int DeclarativeHistoryModel::sourceCount() const
{
    return m_model->rowCount();
}

int DeclarativeHistoryModel::starredCount() const
{
    return m_model->bindableStarredCount().value();
}

bool DeclarativeHistoryModel::starredOnly() const
{
    return m_starredOnly;
}

void DeclarativeHistoryModel::setStarredOnly(bool value)
{
    if (m_starredOnly == value) {
        return;
    }
    m_starredOnly = value;
    invalidateRowsFilter();
    Q_EMIT starredOnlyChanged();
}

void DeclarativeHistoryModel::moveToTop(const QString &uuid)
{
    m_model->moveToTop(uuid);
}

void DeclarativeHistoryModel::remove(const QString &uuid)
{
    m_model->remove(uuid);
}

void DeclarativeHistoryModel::clearHistory()
{
    m_model->clearHistory();
}

void DeclarativeHistoryModel::invokeAction(const QString &uuid)
{
    if (const qsizetype row = m_model->indexOf(uuid); row >= 0) {
        Q_EMIT m_model->actionInvoked(m_model->m_items[row]);
    }
}

QUrl DeclarativeHistoryModel::imageFileUrlForDrag(const QString &uuid)
{
    return m_model->imageFileUrlForDrag(uuid);
}

void DeclarativeHistoryModel::releaseImageFileUrlForDrag(const QUrl &url)
{
    m_model->releaseImageFileUrlForDrag(url);
}

bool DeclarativeHistoryModel::vaultConfigured() const
{
    return m_model->isVaultConfigured();
}

bool DeclarativeHistoryModel::vaultUnlocked() const
{
    return m_model->isVaultUnlocked();
}

bool DeclarativeHistoryModel::vaultBusy() const
{
    return m_model->isVaultBusy();
}

int DeclarativeHistoryModel::lockedCount() const
{
    return m_model->lockedCount();
}

int DeclarativeHistoryModel::plaintextItems() const
{
    return int(m_model->plaintextItems());
}

void DeclarativeHistoryModel::setupVault(const QString &password)
{
    m_model->setupVault(password);
}

void DeclarativeHistoryModel::unlockVault(const QString &password)
{
    m_model->unlockVault(password);
}

void DeclarativeHistoryModel::lockVault()
{
    m_model->lockVault();
}

void DeclarativeHistoryModel::changeVaultPassword(const QString &oldPassword, const QString &newPassword)
{
    m_model->changeVaultPassword(oldPassword, newPassword);
}

void DeclarativeHistoryModel::resetVault()
{
    m_model->resetVault();
}

bool DeclarativeHistoryModel::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    const QModelIndex index = m_model->index(sourceRow, 0, sourceParent);
    if (index.data(HistoryModel::LockedRole).toBool()) {
        return false; // content unknown until the vault is unlocked; the vault bar shows how many
    }
    if (m_starredOnly) {
        return index.data(HistoryModel::StarredRole).toBool();
    }

    return true;
}

#include "moc_declarativehistorymodel.cpp"
