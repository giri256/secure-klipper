/*
    SPDX-FileCopyrightText: 2004 Esben Mose Hansen <kde@mosehansen.dk>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include <memory>

#include <QFlags>
#include <QSize>
#include <QString>

#include "klipper_export.h"

class QString;
class QMimeData;
class QDataStream;
class QSqlQuery;

class HistoryItem;
typedef std::shared_ptr<HistoryItem> HistoryItemPtr;
typedef std::shared_ptr<const HistoryItem> HistoryItemConstPtr;

enum class HistoryItemType {
    Unknown = 1 << 0,
    Text = 1 << 1,
    Image = 1 << 2,
    Url = 1 << 3,
};
Q_DECLARE_FLAGS(HistoryItemTypes, HistoryItemType);

/**
 * An entry in the clipboard history.
 */
class KLIPPER_EXPORT HistoryItem
{
public:
    explicit HistoryItem(const QString &uuid, const QStringList &mimeTypes, const QString &text);
    virtual ~HistoryItem();

    /**
     * Returns the primary item type.
     */
    HistoryItemType type() const;

    /**
     * Returns all the item types the item owns.
     */
    HistoryItemTypes allTypes() const
    {
        return m_types;
    }

    /**
     * Return the current item as text
     * An image would be returned as a descriptive
     * text, such as 32x43 image.
     */
    QString text() const;

    /**
     * @return uuid of current item.
     */
    const QString &uuid() const
    {
        return m_uuid;
    }

    /**
     * Equality.
     */
    bool operator==(const HistoryItem &rhs) const
    {
        return m_uuid == rhs.m_uuid;
    }

    /**
     * Create an HistoryItem from data stream (i.e., disk file)
     * returns null if creation fails. In this case, the datastream
     * is left in an undefined state.
     */
    static HistoryItemPtr create(const QSqlQuery &query);

    /**
     * An item whose data lives encrypted in the history vault (see HistoryVault).
     * @p uuid is the random storage id of the row; @p contentUuid the SHA1 of the
     * content (used for de-duplication) and only known while unlocked.
     */
    static HistoryItemPtr createEncrypted(const QString &uuid,
                                          const QString &contentUuid,
                                          const QStringList &mimeTypes,
                                          const QString &text,
                                          const QSize &imageSize);
    /// An encrypted item whose content is unknown because the vault is locked.
    static HistoryItemPtr createLocked(const QString &uuid);

    /// The mimetypes column value marking a row whose data is in the vault table.
    static constexpr QLatin1StringView encryptedMimeType{"application/x-kde-klipper-encrypted"};

    bool isEncrypted() const
    {
        return m_encrypted;
    }
    bool isLocked() const
    {
        return m_locked;
    }
    /// SHA1 of the content; equals uuid() for plain items, empty for locked items.
    const QString &contentUuid() const
    {
        return m_contentUuid;
    }
    QSize imageSize() const
    {
        return m_imageSize;
    }

private:
    QString m_uuid;
    HistoryItemTypes m_types = HistoryItemType::Unknown;
    QString m_text;
    QString m_contentUuid;
    QSize m_imageSize;
    bool m_encrypted = false;
    bool m_locked = false;
};
