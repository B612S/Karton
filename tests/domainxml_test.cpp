// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Karton contributors

#include <QDomDocument>
#include <QDomElement>
#include <QtTest>

#include "domainxmlbuilder.h"
#include "domainxmlreader.h"

class DomainXmlTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void readerParsesMetadataAndDisks();
    void readerStopsAtDiskBoundary();
    void readerParsesRawDiskFormat();
    void builderUpdatesHardwareAndPreservesMetadata();
    void builderLeavesUnknownStorageUntouched();
    void builderAddsMetadataWhenMissing();
    void builderGeneratesEjectDisk();
};

static const QByteArray sampleDomainXml = R"(<domain type='kvm'>
  <name>test</name>
  <memory unit='KiB'>1048576</memory>
  <currentMemory unit='KiB'>1048576</currentMemory>
  <vcpu placement='static'>2</vcpu>
  <metadata>
    <karton:kde-karton xmlns:karton="https://invent.kde.org/sitter/karton">
      <karton:data maxDiskStorage="4096" screenshotPreviewPath="/tmp/preview.png"/>
    </karton:kde-karton>
    <libosinfo:libosinfo xmlns:libosinfo="http://libosinfo.org/xmlns/libvirt/domain/1.0">
      <libosinfo:os id="http://example.com/os/1" short-id="example1"/>
    </libosinfo:libosinfo>
  </metadata>
  <os><type arch='x86_64'>hvm</type></os>
  <devices>
    <disk type='file' device='cdrom'>
      <driver name='qemu' type='raw'/>
      <target dev='sda' bus='sata'/>
      <readonly/>
    </disk>
    <disk type='file' device='disk'>
      <driver name='qemu' type='qcow2'/>
      <source file='/tmp/disk.qcow2'/>
      <target dev='vda' bus='virtio'/>
    </disk>
  </devices>
</domain>)";

void DomainXmlTest::readerParsesMetadataAndDisks()
{
    const DomainXmlReader::XmlInfo info = DomainXmlReader::parseXml(sampleDomainXml);

    QCOMPARE(info.hypervisorType, QStringLiteral("kvm"));
    QCOMPARE(info.maxDiskStorage, 4096);
    QCOMPARE(info.screenshotPreviewPath, QStringLiteral("/tmp/preview.png"));
    QCOMPARE(info.shortOsId, QStringLiteral("example1"));
    QCOMPARE(info.osId, QStringLiteral("http://example.com/os/1"));
    QCOMPARE(info.virtualDiskPath, QStringLiteral("/tmp/disk.qcow2"));
    QCOMPARE(info.diskFormat, QStringLiteral("qcow2"));
    QVERIFY(info.isoDiskPath.isEmpty());
}

void DomainXmlTest::readerStopsAtDiskBoundary()
{
    // A CD-ROM without a source must not consume the source of the next disk.
    const DomainXmlReader::XmlInfo info = DomainXmlReader::parseXml(sampleDomainXml);

    QVERIFY(info.isoDiskPath.isEmpty());
    QCOMPARE(info.virtualDiskPath, QStringLiteral("/tmp/disk.qcow2"));
}

void DomainXmlTest::readerParsesRawDiskFormat()
{
    const QByteArray xml = R"(<domain type='kvm'>
  <name>raw</name>
  <devices>
    <disk type='file' device='disk'>
      <driver name='qemu' type='raw'/>
      <source file='/tmp/disk.raw'/>
      <target dev='vda' bus='virtio'/>
    </disk>
  </devices>
</domain>)";

    const DomainXmlReader::XmlInfo info = DomainXmlReader::parseXml(xml);
    QCOMPARE(info.virtualDiskPath, QStringLiteral("/tmp/disk.raw"));
    QCOMPARE(info.diskFormat, QStringLiteral("raw"));
}

void DomainXmlTest::builderUpdatesHardwareAndPreservesMetadata()
{
    DomainXmlBuilder builder;
    const QString updated = builder.updateXML(QString::fromUtf8(sampleDomainXml), {.maxRam = 2, .cpus = 4, .maxDiskStorage = 8});

    QDomDocument document;
    QVERIFY2(document.setContent(updated), qPrintable(updated));
    const QDomElement root = document.documentElement();

    QCOMPARE(root.firstChildElement(QStringLiteral("memory")).text(), QStringLiteral("2048"));
    QCOMPARE(root.firstChildElement(QStringLiteral("currentMemory")).text(), QStringLiteral("2048"));
    QCOMPARE(root.firstChildElement(QStringLiteral("vcpu")).text(), QStringLiteral("4"));

    const QDomNodeList dataNodes = root.elementsByTagName(QStringLiteral("karton:data"));
    QCOMPARE(dataNodes.count(), 1);
    const QDomElement data = dataNodes.at(0).toElement();
    QCOMPARE(data.attribute(QStringLiteral("maxDiskStorage")), QStringLiteral("8192"));
    QCOMPARE(data.attribute(QStringLiteral("screenshotPreviewPath")), QStringLiteral("/tmp/preview.png"));
}

void DomainXmlTest::builderLeavesUnknownStorageUntouched()
{
    DomainXmlBuilder builder;
    const QString updated = builder.updateXML(QString::fromUtf8(sampleDomainXml), {.maxRam = 1, .cpus = 1, .maxDiskStorage = 0});

    QDomDocument document;
    QVERIFY2(document.setContent(updated), qPrintable(updated));
    const QDomNodeList dataNodes = document.elementsByTagName(QStringLiteral("karton:data"));
    QCOMPARE(dataNodes.count(), 1);
    QCOMPARE(dataNodes.at(0).toElement().attribute(QStringLiteral("maxDiskStorage")), QStringLiteral("4096"));
}

void DomainXmlTest::builderAddsMetadataWhenMissing()
{
    const QByteArray importedXml = R"(<domain type='kvm'>
  <name>imported</name>
  <memory unit='KiB'>1048576</memory>
  <currentMemory unit='KiB'>1048576</currentMemory>
  <vcpu placement='static'>1</vcpu>
  <os><type arch='x86_64'>hvm</type></os>
</domain>)";

    DomainXmlBuilder builder;
    const QString updated = builder.updateXML(QString::fromUtf8(importedXml), {.maxRam = 1, .cpus = 2, .maxDiskStorage = 10});

    QDomDocument document;
    QVERIFY2(document.setContent(updated), qPrintable(updated));
    const QDomElement root = document.documentElement();
    QVERIFY(!root.firstChildElement(QStringLiteral("metadata")).isNull());

    const QDomNodeList dataNodes = root.elementsByTagName(QStringLiteral("karton:data"));
    QCOMPARE(dataNodes.count(), 1);
    QCOMPARE(dataNodes.at(0).toElement().attribute(QStringLiteral("maxDiskStorage")), QStringLiteral("10240"));
    QCOMPARE(root.firstChildElement(QStringLiteral("vcpu")).text(), QStringLiteral("2"));
}

void DomainXmlTest::builderGeneratesEjectDisk()
{
    DomainXmlBuilder builder;
    const QString xml = builder.generateDiskXML({.diskType = QStringLiteral("file"),
                                                 .device = QStringLiteral("cdrom"),
                                                 .driver = QStringLiteral("qemu"),
                                                 .format = QStringLiteral("raw"),
                                                 .sourcePath = QString(),
                                                 .target = QStringLiteral("sda"),
                                                 .bus = QStringLiteral("sata"),
                                                 .readonly = true});

    QDomDocument document;
    QVERIFY2(document.setContent(xml), qPrintable(xml));
    const QDomElement disk = document.documentElement();
    QCOMPARE(disk.tagName(), QStringLiteral("disk"));
    QCOMPARE(disk.attribute(QStringLiteral("type")), QStringLiteral("file"));
    QCOMPARE(disk.attribute(QStringLiteral("device")), QStringLiteral("cdrom"));
    QVERIFY(disk.firstChildElement(QStringLiteral("source")).isNull());

    const QDomElement driver = disk.firstChildElement(QStringLiteral("driver"));
    QCOMPARE(driver.attribute(QStringLiteral("type")), QStringLiteral("raw"));

    const QDomElement target = disk.firstChildElement(QStringLiteral("target"));
    QCOMPARE(target.attribute(QStringLiteral("dev")), QStringLiteral("sda"));
    QCOMPARE(target.attribute(QStringLiteral("bus")), QStringLiteral("sata"));
    QVERIFY(!disk.firstChildElement(QStringLiteral("readonly")).isNull());
}

QTEST_GUILESS_MAIN(DomainXmlTest)

#include "domainxml_test.moc"
