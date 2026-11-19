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

    Q_PROPERTY(QString filterText READ filterText WRITE setFilterText NOTIFY filterChanged)
    Q_PROPERTY(int filterMode READ filterMode WRITE setFilterMode NOTIFY filterChanged)
    Q_PROPERTY(bool hasFilter READ hasFilter NOTIFY filterChanged)

public:
    enum Roles {
        DomainRole = Qt::UserRole
    };
    // Keep the values in sync with the VM list page's filter combo box.
    enum FilterMode {
        AllVms = 0,
        RunningVms = 1,
        StoppedVms = 2,
    };
    Q_ENUM(FilterMode)

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

    QString filterText() const
    {
        return m_filterText;
    }
    void setFilterText(const QString &text);
    int filterMode() const
    {
        return m_filterMode;
    }
    void setFilterMode(int mode);
    bool hasFilter() const
    {
        return !m_filterText.isEmpty() || m_filterMode != AllVms;
    }

Q_SIGNALS:
    void filterChanged();

private Q_SLOTS:
    void onDomainsChanged(const virDomainPtr domainPtr, int event, int detail);
    void onDomainRemoved(Domain *domain);

private:
    void applyFilter();
    void rebuildFiltered();
    bool matchesFilter(const Domain *domain) const;

    QList<Domain *> m_datas;
    // The rows the view currently shows: either the full list or the subset
    // matching filterText/filterMode.
    QList<Domain *> m_filtered;
    QString m_filterText;
    int m_filterMode = AllVms;
    Karton *m_karton;
    bool m_resetting = false;
};