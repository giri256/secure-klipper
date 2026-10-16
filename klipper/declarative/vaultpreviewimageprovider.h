/*
    SPDX-FileCopyrightText: 2026 giri256

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include <memory>

#include <QQuickAsyncImageProvider>

class HistoryModel;

class VaultPreviewImageProvider final : public QQuickAsyncImageProvider
{
public:
    explicit VaultPreviewImageProvider(std::shared_ptr<HistoryModel> model);
    QQuickImageResponse *requestImageResponse(const QString &id, const QSize &requestedSize) override;

private:
    std::shared_ptr<HistoryModel> m_model;
};
