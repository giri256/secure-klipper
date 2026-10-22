/*
    SPDX-FileCopyrightText: 2026 giri256

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringView>

// Clipboard images are encoded as PNG by UpdateDatabaseJob. Keep MIME data_uuid
// values stable in SQLite, but give image payload files a useful suffix so
// file-path paste/drag consumers can identify the format without sniffing bytes.
namespace ClipboardDataFile
{
inline QString extensionForMimeType(QStringView mimeType)
{
    if (mimeType == u"image/png") {
        return QStringLiteral(".png");
    }
    if (mimeType == u"image/jpeg") {
        return QStringLiteral(".jpg");
    }
    if (mimeType == u"image/webp") {
        return QStringLiteral(".webp");
    }
    if (mimeType == u"image/gif") {
        return QStringLiteral(".gif");
    }
    if (mimeType == u"image/bmp") {
        return QStringLiteral(".bmp");
    }
    if (mimeType == u"image/tiff") {
        return QStringLiteral(".tiff");
    }
    return {};
}

inline QString fileName(QStringView dataUuid, QStringView mimeType)
{
    return QString(dataUuid) + extensionForMimeType(mimeType);
}

// Read the new suffixed path first, then fall back to the pre-migration
// extensionless layout. The fallback keeps a failed/partial rename readable.
inline QString path(const QString &folder, QStringView dataUuid, QStringView mimeType)
{
    const QString basePath = QString(folder) + u'/' + QString(dataUuid);
    const QString extension = extensionForMimeType(mimeType);
    if (!extension.isEmpty()) {
        const QString suffixedPath = basePath + extension;
        if (QFileInfo::exists(suffixedPath)) {
            return suffixedPath;
        }
    }
    return basePath;
}

inline bool migrateLegacyFile(const QString &folder, QStringView dataUuid, QStringView mimeType)
{
    const QString extension = extensionForMimeType(mimeType);
    if (extension.isEmpty()) {
        return false;
    }

    const QString basePath = QString(folder) + u'/' + QString(dataUuid);
    const QString suffixedPath = basePath + extension;
    if (QFileInfo::exists(suffixedPath)) {
        return true;
    }
    if (!QFileInfo::exists(basePath)) {
        return false;
    }
    return QFile::rename(basePath, suffixedPath);
}
}
