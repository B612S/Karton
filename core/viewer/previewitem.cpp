// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#include "viewer/previewitem.h"

#include <QPainter>

PreviewItem::PreviewItem(QQuickItem *parent)
    : QQuickPaintedItem(parent)
{
    setAntialiasing(true);
}

QVariant PreviewItem::image() const
{
    return QVariant::fromValue(m_image);
}

void PreviewItem::setImage(const QVariant &image)
{
    const QImage next = image.value<QImage>();
    if (next.cacheKey() == m_image.cacheKey()) {
        return;
    }
    m_image = next;
    update();
    Q_EMIT imageChanged();
}

void PreviewItem::paint(QPainter *painter)
{
    if (m_image.isNull() || width() <= 0 || height() <= 0) {
        return;
    }

    const QSize scaledSize = m_image.size().scaled(size().toSize(), Qt::KeepAspectRatio);
    const QRectF target((width() - scaledSize.width()) / 2.0, (height() - scaledSize.height()) / 2.0, scaledSize.width(), scaledSize.height());
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter->drawImage(target, m_image);
}
