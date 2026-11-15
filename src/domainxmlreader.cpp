// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>

#include "domainxmlreader.h"

#include <cstdlib>

#include <libvirt/virterror.h>

#include "karton_debug.h"

DomainXmlReader::DomainXmlReader(virDomainPtr domainPtr)
{
    if (!domainPtr) {
        return;
    }

    // Ask libvirt for the persistent definition instead of reading
    // ~/.config/libvirt/qemu/<name>.xml directly. This also works with
    // qemu:///system and with non-default configuration directories.
    char *xmlDesc = virDomainGetXMLDesc(domainPtr, VIR_DOMAIN_XML_INACTIVE);
    if (!xmlDesc) {
        qCWarning(KARTON_DEBUG) << "Failed to read domain XML:" << virGetLastErrorMessage();
        return;
    }

    const QByteArray xml(xmlDesc);
    free(xmlDesc);
    parse(xml);
}

void DomainXmlReader::parse(const QByteArray &xml)
{
    QXmlStreamReader xmlReader(xml);

    while (!xmlReader.atEnd() && !xmlReader.hasError()) {
        const QXmlStreamReader::TokenType token = xmlReader.readNext();
        if (token != QXmlStreamReader::StartElement) {
            continue;
        }

        if (xmlReader.name() == QStringLiteral("domain")) {
            m_xmlInfo.indexId = xmlReader.attributes().value(QStringLiteral("id")).toInt();
            m_xmlInfo.hypervisorType = xmlReader.attributes().value(QStringLiteral("type")).toString();
        } else if (xmlReader.name() == QStringLiteral("os") && xmlReader.attributes().hasAttribute(QStringLiteral("id"))) {
            m_xmlInfo.osId = xmlReader.attributes().value(QStringLiteral("id")).toString();
            m_xmlInfo.shortOsId = xmlReader.attributes().value(QStringLiteral("short-id")).toString();
        } else if (xmlReader.name() == QStringLiteral("data")) {
            if (xmlReader.attributes().hasAttribute(QStringLiteral("maxDiskStorage"))) {
                m_xmlInfo.maxDiskStorage = xmlReader.attributes().value(QStringLiteral("maxDiskStorage")).toInt();
            }
            if (xmlReader.attributes().hasAttribute(QStringLiteral("screenshotPreviewPath"))) {
                m_xmlInfo.screenshotPreviewPath = xmlReader.attributes().value(QStringLiteral("screenshotPreviewPath")).toString();
            }
        } else if (xmlReader.name() == QStringLiteral("disk")) {
            const QString device = xmlReader.attributes().value(QStringLiteral("device")).toString();
            if (device == QStringLiteral("disk")) {
                m_xmlInfo.virtualDiskPath = retrieveDiskPath(xmlReader);
            } else if (device == QStringLiteral("cdrom")) {
                m_xmlInfo.isoDiskPath = retrieveDiskPath(xmlReader);
            }
        }
    }

    if (xmlReader.hasError()) {
        qCWarning(KARTON_DEBUG) << "XML parsing error:" << xmlReader.errorString();
    }
}

QString DomainXmlReader::retrieveDiskPath(QXmlStreamReader &xmlReader)
{
    while (!xmlReader.atEnd()) {
        const QXmlStreamReader::TokenType token = xmlReader.readNext();
        if (token == QXmlStreamReader::StartElement && xmlReader.name() == QStringLiteral("source")) {
            return xmlReader.attributes().value(QStringLiteral("file")).toString();
        }
        if (token == QXmlStreamReader::EndElement && xmlReader.name() == QStringLiteral("disk")) {
            break;
        }
    }
    return QString();
}
