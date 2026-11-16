// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Karton contributors

#pragma once

#include <QImage>
#include <QQuickPaintedItem>
#include <QVariant>

// Small scene-graph item that paints the latest in-memory VM preview. Using a
// painted item avoids changing an Image source and decoding a PNG for every
// frame, which used to make the preview blink.
class PreviewItem : public QQuickPaintedItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QVariant image READ image WRITE setImage NOTIFY imageChanged)

public:
    explicit PreviewItem(QQuickItem *parent = nullptr);

    QVariant image() const;
    void setImage(const QVariant &image);

    void paint(QPainter *painter) override;

Q_SIGNALS:
    void imageChanged();

private:
    QImage m_image;
};
