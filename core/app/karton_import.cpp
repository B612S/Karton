// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#include "app/karton.h"
#include "app/karton_private.h"

#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>

#include <QCoreApplication>
#include <QDomDocument>
#include <QDomElement>
#include <QFile>
#include <QFileInfo>
#include <QString>

bool Karton::importDomainXml(const QString &path)
{
    if (!m_conn) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "not_connected_to_libvirt"));
        Q_EMIT domainImported(false, QString());
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_open_1_2").arg(path, file.errorString()));
        Q_EMIT domainImported(false, QString());
        return false;
    }
    const QByteArray xml = file.readAll();
    if (xml.trimmed().isEmpty()) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "the_file_is_not_a_libvirt_domain"));
        Q_EMIT domainImported(false, QString());
        return false;
    }

    QDomDocument document;
    const QDomDocument::ParseResult parseResult = document.setContent(xml);
    if (!parseResult) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_parse_the_domain_xml_1").arg(parseResult.errorMessage));
        Q_EMIT domainImported(false, QString());
        return false;
    }
    const QDomElement root = document.documentElement();
    if (root.tagName() != QStringLiteral("domain")) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "the_file_is_not_a_libvirt_domain"));
        Q_EMIT domainImported(false, QString());
        return false;
    }
    const QString name = root.firstChildElement(QStringLiteral("name")).text();

    // Validate the full document against libvirt's schema. Older libvirt
    // implementations do not know the flag, so fall back to a plain define.
    virDomainPtr domain = virDomainDefineXMLFlags(m_conn, xml.constData(), VIR_DOMAIN_DEFINE_VALIDATE);
    if (!domain) {
        domain = virDomainDefineXML(m_conn, xml.constData());
    }
    if (!domain) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_define_the_domain_1").arg(QString::fromUtf8(virGetLastErrorMessage())));
        Q_EMIT domainImported(false, name);
        return false;
    }

    virDomainFree(domain);
    refreshDomainList();
    Q_EMIT domainImported(true, name);
    return true;
}

bool Karton::exportDomainXml(const Domain *domain, const QString &path)
{
    if (!domain || !domain->domainPtr()) {
        return false;
    }

    char *xml = virDomainGetXMLDesc(domain->domainPtr(), VIR_DOMAIN_XML_INACTIVE);
    if (!xml) {
        xml = virDomainGetXMLDesc(domain->domainPtr(), 0);
    }
    if (!xml) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_read_the_virtual_machine_configuration"));
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        free(xml);
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_open_1_2").arg(path, file.errorString()));
        return false;
    }
    const bool written = file.write(xml) >= 0;
    free(xml);
    if (!written) {
        Q_EMIT errorOccurred(QCoreApplication::translate("Karton", "failed_to_write_1").arg(path));
        return false;
    }
    return true;
}
