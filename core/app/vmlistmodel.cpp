// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#include "app/vmlistmodel.h"

#include "app/karton.h"
#include "karton_debug.h"

VMModel::VMModel(QObject *parent)
    : QAbstractListModel(parent)
    , m_karton(Karton::self())
{
    if (m_karton) {
        connect(m_karton, &Karton::domainsChanged, this, &VMModel::onDomainsChanged);
        // Deleting a domain must not re-enter refreshAllDomains() while the model
        // is already being reset.
        connect(
            m_karton,
            &Karton::domainAdded,
            this,
            [this] {
                refreshAllDomains();
            },
            Qt::QueuedConnection);
        connect(m_karton, &Karton::domainRemoved, this, &VMModel::onDomainRemoved);
        refreshAllDomains();
    } else {
        qCCritical(KARTON_DEBUG) << "vmmodellist: karton is null!";
    }
}
int VMModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }

    return m_datas.size();
}

VMModel::~VMModel()
{
    m_datas.clear();
}

QVariant VMModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_datas.size()) {
        return {};
    }

    if (role == DomainRole) {
        return QVariant::fromValue(m_datas[index.row()]);
    }
    return {};
}
QHash<int, QByteArray> VMModel::roleNames() const
{
    return {{DomainRole, "domain"}};
}

void VMModel::onDomainsChanged(const virDomainPtr domainPtr, int event, int detail)
{
    Q_UNUSED(detail);

    // An undefined domain may still return valid virDomainInfo() data, so do
    // not rely on updateDomains() to notice that it disappeared.
    if (event == VIR_DOMAIN_EVENT_UNDEFINED) {
        refreshAllDomains();
        return;
    }

    if (domainPtr) {
        updateDomains(domainPtr);
    }
}
void VMModel::onDomainRemoved(Domain *domain)
{
    // refreshAllDomains() will rebuild the list without this domain.
    if (m_resetting) {
        return;
    }

    const int index = m_datas.indexOf(domain);
    if (index < 0) {
        return;
    }

    beginRemoveRows(QModelIndex(), index, index);
    m_datas.removeAt(index);
    endRemoveRows();
}

void VMModel::moveItem(int from, int to)
{
    if (from == to || from < 0 || to < 0 || from >= m_datas.size() || to >= m_datas.size()) {
        return;
    }

    beginMoveRows(QModelIndex(), from, from, QModelIndex(), to > from ? to + 1 : to);
    m_datas.move(from, to);
    endMoveRows();

    // Keep Karton's own domain list in the same order so a later refresh does
    // not undo the move; this also persists the new order.
    m_karton->moveDomain(from, to);
}

int VMModel::rowOfUuid(const QString &uuid) const
{
    for (int i = 0; i < m_datas.size(); ++i) {
        if (m_datas[i]->config()->uuid() == uuid) {
            return i;
        }
    }
    return -1;
}

void VMModel::refreshAllDomains()
{
    qCInfo(KARTON_DEBUG) << "vmlistmodel: Refreshing all domains.";
    m_resetting = true;
    beginResetModel();
    m_datas.clear();
    m_karton->refreshDomainList();
    m_datas = m_karton->domains();
    endResetModel();
    m_resetting = false;
}

void VMModel::updateDomains(const virDomainPtr domainPtr)
{
    QString domainUuid = Domain::uuidString(domainPtr);
    qCDebug(KARTON_DEBUG) << "Domain UUID:" << domainUuid;

    virDomainInfo info = {};
    if (virDomainGetInfo(domainPtr, &info) < 0) {
        refreshAllDomains();
        return;
    }

    int modelIndex = -1;
    for (int i = 0; i < m_datas.size(); ++i) {
        if (m_datas[i]->config()->uuid() == domainUuid) {
            modelIndex = i;
            break;
        }
    }

    if (modelIndex >= 0) {
        qCDebug(KARTON_DEBUG) << "Found matching domain in model at index" << modelIndex;
        m_karton->refreshDomain(domainPtr);

        QModelIndex qModelIndex = createIndex(modelIndex, 0);
        Q_EMIT dataChanged(qModelIndex, qModelIndex);
    } else {
        refreshAllDomains();
    }
}