// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>

#pragma once

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

    explicit DomainXmlReader(const QString &path);

    XmlInfo readConfigFile(const QString &path);

    XmlInfo m_xmlInfo;

private:
    QString retrieveDiskPath(QXmlStreamReader &xmlReader);
};
