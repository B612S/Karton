// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#include "app/karton.h"
#include "app/karton_private.h"

#include <QCoreApplication>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QVariantList>
#include <QVariantMap>

#include "util/portforwarder.h"

namespace
{
QString portForwardKey(const Domain *domain)
{
    return QStringLiteral("portForwards/") + domain->config()->uuid();
}

QVariantList storedRules(const Domain *domain)
{
    QVariantList result;
    if (!domain) {
        return result;
    }
    QSettings settings(KartonCommon::kartonConfigPath(), QSettings::IniFormat);
    const QByteArray json = settings.value(portForwardKey(domain)).toByteArray();
    const QJsonArray array = QJsonDocument::fromJson(json).array();
    for (const QJsonValue &value : array) {
        result.append(value.toObject().toVariantMap());
    }
    return result;
}

void storeRules(const Domain *domain, const QVariantList &rules)
{
    QJsonArray array;
    for (const QVariant &value : rules) {
        array.append(QJsonObject::fromVariantMap(value.toMap()));
    }
    QSettings settings(KartonCommon::kartonConfigPath(), QSettings::IniFormat);
    settings.setValue(portForwardKey(domain), QJsonDocument(array).toJson(QJsonDocument::Compact));
}
} // namespace

QVariantList Karton::domainPortForwards(const Domain *domain) const
{
    QVariantList result = storedRules(domain);
    for (QVariant &value : result) {
        QVariantMap rule = value.toMap();
        rule.insert(QStringLiteral("active"), m_portForwarder && m_portForwarder->isActive(quint16(rule.value(QStringLiteral("hostPort")).toInt())));
        value = rule;
    }
    return result;
}

bool Karton::addPortForward(const Domain *domain, int hostPort, const QString &guestIp, int guestPort, bool localOnly)
{
    if (!domain) {
        return false;
    }
    if (hostPort < 1 || hostPort > 65535 || guestPort < 1 || guestPort > 65535) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "port_forward_ports_must_be_between_1_and_65535"));
        return false;
    }
    QHostAddress address;
    if (!address.setAddress(guestIp.trimmed())) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "invalid_guest_address_1").arg(guestIp));
        return false;
    }

    QVariantList rules = storedRules(domain);
    for (const QVariant &value : rules) {
        if (value.toMap().value(QStringLiteral("hostPort")).toInt() == hostPort) {
            Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "a_port_forward_for_port_1_already_exists").arg(hostPort));
            return false;
        }
    }

    // Start first for a running VM. If the port is unavailable, keep the
    // previous configuration instead of saving a rule that cannot work.
    if (domain->isActive()) {
        QString error;
        if (!m_portForwarder->start(quint16(hostPort), address.toString(), quint16(guestPort), localOnly, &error)) {
            Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_start_port_forward_on_port_1_2").arg(hostPort).arg(error));
            return false;
        }
    }

    QVariantMap rule;
    rule.insert(QStringLiteral("hostPort"), hostPort);
    rule.insert(QStringLiteral("guestIp"), address.toString());
    rule.insert(QStringLiteral("guestPort"), guestPort);
    rule.insert(QStringLiteral("localOnly"), localOnly);
    rules.append(rule);
    storeRules(domain, rules);
    return true;
}

bool Karton::removePortForward(const Domain *domain, int hostPort)
{
    if (!domain) {
        return false;
    }
    m_portForwarder->stop(quint16(hostPort));
    QVariantList rules = storedRules(domain);
    bool removed = false;
    for (int i = rules.size() - 1; i >= 0; --i) {
        if (rules.at(i).toMap().value(QStringLiteral("hostPort")).toInt() == hostPort) {
            rules.removeAt(i);
            removed = true;
        }
    }
    if (removed) {
        storeRules(domain, rules);
    }
    return removed;
}

void Karton::startPortForwardsForDomain(const Domain *domain)
{
    if (!domain || !m_portForwarder) {
        return;
    }
    const QVariantList rules = storedRules(domain);
    for (const QVariant &value : rules) {
        const QVariantMap rule = value.toMap();
        const int hostPort = rule.value(QStringLiteral("hostPort")).toInt();
        QString error;
        if (!m_portForwarder->start(quint16(hostPort),
                                    rule.value(QStringLiteral("guestIp")).toString(),
                                    quint16(rule.value(QStringLiteral("guestPort")).toInt()),
                                    rule.value(QStringLiteral("localOnly"), true).toBool(),
                                    &error)) {
            Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_start_port_forward_on_port_1_2").arg(hostPort).arg(error));
        }
    }
}

void Karton::stopPortForwardsForDomain(const Domain *domain)
{
    if (!domain || !m_portForwarder) {
        return;
    }
    const QVariantList rules = storedRules(domain);
    for (const QVariant &value : rules) {
        m_portForwarder->stop(quint16(value.toMap().value(QStringLiteral("hostPort")).toInt()));
    }
}

void Karton::startPortForwardsForActiveDomains()
{
    for (const Domain *domain : std::as_const(m_domains)) {
        if (domain->isActive()) {
            startPortForwardsForDomain(domain);
        }
    }
}
