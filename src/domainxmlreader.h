// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>

#pragma once

#include <libvirt/libvirt.h>

#include <QByteArray>
#include <QString>
#include <QXmlStreamReader>

class DomainXmlReader
{
public:
    struct XmlInfo {
        QString hypervisorType;
        int indexId = 0;
        int maxDiskStorage = 0;
        QString osId;
        QString shortOsId;
        QString isoDiskPath;
        QString virtualDiskPath;
        QString screenshotPreviewPath;
    };

    explicit DomainXmlReader(virDomainPtr domainPtr);

    static XmlInfo parseXml(const QByteArray &xml);

    XmlInfo m_xmlInfo;

private:
    static QString retrieveDiskPath(QXmlStreamReader &xmlReader);
};
