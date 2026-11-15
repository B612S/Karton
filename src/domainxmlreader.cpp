// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>

#include "domainxmlreader.h"

#include <QFile>
#include <QXmlStreamReader>

#include "karton_debug.h"

DomainXmlReader::DomainXmlReader(const QString &path)
    : m_xmlInfo(readConfigFile(path))
{
}

DomainXmlReader::XmlInfo DomainXmlReader::readConfigFile(const QString &path)
{
    XmlInfo info;

    QFile xmlFile(path);
    if (!xmlFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qCInfo(KARTON_DEBUG) << "Could not open domain config file:" << path;
        return info;
    }
    QXmlStreamReader xmlReader(&xmlFile);

    while (!xmlReader.atEnd() && !xmlReader.hasError()) {
        const QXmlStreamReader::TokenType token = xmlReader.readNext();
        if (token != QXmlStreamReader::StartElement) {
            continue;
        }

        if (xmlReader.name() == QStringLiteral("domain")) {
            info.indexId = xmlReader.attributes().value(QStringLiteral("id")).toInt();
            info.hypervisorType = xmlReader.attributes().value(QStringLiteral("type")).toString();
        } else if (xmlReader.name() == QStringLiteral("os") && xmlReader.attributes().hasAttribute(QStringLiteral("id"))) {
            info.osId = xmlReader.attributes().value(QStringLiteral("id")).toString();
            info.shortOsId = xmlReader.attributes().value(QStringLiteral("short-id")).toString();
        } else if (xmlReader.name() == QStringLiteral("data")) {
            if (xmlReader.attributes().hasAttribute(QStringLiteral("maxDiskStorage"))) {
                info.maxDiskStorage = xmlReader.attributes().value(QStringLiteral("maxDiskStorage")).toInt();
            }
            if (xmlReader.attributes().hasAttribute(QStringLiteral("screenshotPreviewPath"))) {
                info.screenshotPreviewPath = xmlReader.attributes().value(QStringLiteral("screenshotPreviewPath")).toString();
            }
        } else if (xmlReader.name() == QStringLiteral("disk")) {
            const QString device = xmlReader.attributes().value(QStringLiteral("device")).toString();
            if (device == QStringLiteral("disk")) {
                info.virtualDiskPath = retrieveDiskPath(xmlReader);
            } else if (device == QStringLiteral("cdrom")) {
                info.isoDiskPath = retrieveDiskPath(xmlReader);
            }
        }
    }

    if (xmlReader.hasError()) {
        qCWarning(KARTON_DEBUG) << "XML parsing error:" << xmlReader.errorString();
    }

    return info;
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
