/*
    SPDX-FileCopyrightText: 2026 giri256

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "vaultpreviewimageprovider.h"

#include <QFutureWatcher>
#include <QImage>
#include <QQuickTextureFactory>
#include <QtConcurrentRun>

#include "../historymodel.h"
#include "../historyvault.h"

class VaultPreviewImageResponse final : public QQuickImageResponse
{
    Q_OBJECT

public:
    VaultPreviewImageResponse(std::shared_ptr<HistoryModel> model, QString uuid, QSize requestedSize)
        : m_watcher(new QFutureWatcher<QImage>(this))
    {
        connect(m_watcher, &QFutureWatcher<QImage>::finished, this, [this] {
            m_image = m_watcher->result();
            m_watcher->deleteLater();
            Q_EMIT finished();
        });
        m_watcher->setFuture(QtConcurrent::run([model = std::move(model), uuid = std::move(uuid), requestedSize] {
            QByteArray encoded = model->vaultImagePreviewData(uuid);
            QImage image = QImage::fromData(encoded);
            HistoryVault::wipe(encoded);
            if (!image.isNull()) {
                if (requestedSize.width() > 0 && requestedSize.height() > 0) {
                    image = image.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                } else if (requestedSize.height() > 0) {
                    image = image.scaledToHeight(requestedSize.height(), Qt::SmoothTransformation);
                } else if (requestedSize.width() > 0) {
                    image = image.scaledToWidth(requestedSize.width(), Qt::SmoothTransformation);
                }
            }
            return image;
        }));
    }

    QQuickTextureFactory *textureFactory() const override
    {
        return QQuickTextureFactory::textureFactoryForImage(m_image);
    }

private:
    QFutureWatcher<QImage> *m_watcher;
    QImage m_image;
};

VaultPreviewImageProvider::VaultPreviewImageProvider(std::shared_ptr<HistoryModel> model)
    : m_model(std::move(model))
{
}

QQuickImageResponse *VaultPreviewImageProvider::requestImageResponse(const QString &id, const QSize &requestedSize)
{
    QString uuid = id;
    while (uuid.startsWith(u'/')) {
        uuid.remove(0, 1);
    }
    return new VaultPreviewImageResponse(m_model, std::move(uuid), requestedSize);
}

#include "vaultpreviewimageprovider.moc"
