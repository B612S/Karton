// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#pragma once

#include <libvirt/libvirt.h>

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QModelIndex>
#include <QObject>
#include <QString>
#include <QVariant>

#include "app/karton.h"
#include "domain/domain.h"

class VMModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    enum Roles {
        DomainRole = Qt::UserRole
    };
    VMModel(QObject *parent = nullptr);
    ~VMModel() override;
    Q_DISABLE_COPY_MOVE(VMModel)

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QHash<int, QByteArray> roleNames() const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    void updateDomains(const virDomainPtr domainPtr = nullptr);
    void refreshAllDomains();
    Q_INVOKABLE void moveItem(int from, int to);
    Q_INVOKABLE int rowOfUuid(const QString &uuid) const;
private Q_SLOTS:
    void onDomainsChanged(const virDomainPtr domainPtr, int event, int detail);
    void onDomainRemoved(Domain *domain);

private:
    QList<Domain *> m_datas;
    Karton *m_karton;
    bool m_resetting = false;
};