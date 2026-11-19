// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#include "app/vmlistmodel.h"

#include <QStringList>

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

    return m_filtered.size();
}

VMModel::~VMModel()
{
    m_datas.clear();
    m_filtered.clear();
}

QVariant VMModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_filtered.size()) {
        return {};
    }

    if (role == DomainRole) {
        return QVariant::fromValue(m_filtered[index.row()]);
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

    const int sourceIndex = m_datas.indexOf(domain);
    if (sourceIndex < 0) {
        return;
    }
    m_datas.removeAt(sourceIndex);

    const int visibleIndex = m_filtered.indexOf(domain);
    if (visibleIndex >= 0) {
        beginRemoveRows(QModelIndex(), visibleIndex, visibleIndex);
        m_filtered.removeAt(visibleIndex);
        endRemoveRows();
    }
}

void VMModel::moveItem(int from, int to)
{
    // The view shows a filtered subset; source and view rows would no longer
    // match, so reordering is disabled while a filter is active.
    if (hasFilter()) {
        return;
    }
    if (from == to || from < 0 || to < 0 || from >= m_filtered.size() || to >= m_filtered.size()) {
        return;
    }

    beginMoveRows(QModelIndex(), from, from, QModelIndex(), to > from ? to + 1 : to);
    m_datas.move(from, to);
    m_filtered.move(from, to);
    endMoveRows();

    // Keep Karton's own domain list in the same order so a later refresh does
    // not undo the move; this also persists the new order.
    m_karton->moveDomain(from, to);
}

int VMModel::rowOfUuid(const QString &uuid) const
{
    for (int i = 0; i < m_filtered.size(); ++i) {
        if (m_filtered[i]->config()->uuid() == uuid) {
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
    m_filtered.clear();
    m_karton->refreshDomainList();
    m_datas = m_karton->domains();
    rebuildFiltered();
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

        if (hasFilter()) {
            // Name, state and OS text may have changed, and with an
            // active/stopped filter the row may have to appear or disappear.
            beginResetModel();
            rebuildFiltered();
            endResetModel();
        } else {
            QModelIndex qModelIndex = createIndex(modelIndex, 0);
            Q_EMIT dataChanged(qModelIndex, qModelIndex);
        }
    } else {
        refreshAllDomains();
    }
}

void VMModel::setFilterText(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (m_filterText == trimmed) {
        return;
    }
    m_filterText = trimmed;
    applyFilter();
}

void VMModel::setFilterMode(int mode)
{
    const int bounded = qBound(int(AllVms), mode, int(StoppedVms));
    if (m_filterMode == bounded) {
        return;
    }
    m_filterMode = bounded;
    applyFilter();
}

void VMModel::applyFilter()
{
    beginResetModel();
    rebuildFiltered();
    endResetModel();
    Q_EMIT filterChanged();
}

void VMModel::rebuildFiltered()
{
    m_filtered.clear();
    m_filtered.reserve(m_datas.size());
    const QList<Domain *> &domains = m_datas;
    for (Domain *domain : domains) {
        if (matchesFilter(domain)) {
            m_filtered.append(domain);
        }
    }
}

bool VMModel::matchesFilter(const Domain *domain) const
{
    if (!domain || !domain->config()) {
        return false;
    }
    if (m_filterMode == RunningVms && !domain->isActive()) {
        return false;
    }
    if (m_filterMode == StoppedVms && domain->isActive()) {
        return false;
    }
    if (m_filterText.isEmpty()) {
        return true;
    }

    const DomainConfig *config = domain->config();
    const QStringList fields = {
        config->name(),
        config->uuid(),
        config->state(),
        m_karton ? m_karton->domainOsName(domain) : QString(),
    };
    for (const QString &field : fields) {
        if (field.contains(m_filterText, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}