// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Karton contributors

#include <QtTest>

#include "util/commandrunner.h"

class CommandRunnerTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void asyncSuccess();
    void asyncFailure();
    void asyncStartFailure();
    void blockingSuccessAndFailure();
    void asyncDoesNotInterpretArgumentsAsShell();
    void asyncReportsStderrOnFailure();
    void blockingDoesNotReturnStdoutOnFailure();
    void asyncEmptyOutput();
};

void CommandRunnerTest::asyncSuccess()
{
    CommandRunner runner;
    bool called = false;
    bool success = false;
    QString output;

    runner.runCommand(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), QStringLiteral("printf hello")}, [&](bool ok, const QString &text) {
        called = true;
        success = ok;
        output = text;
    });

    QTRY_VERIFY_WITH_TIMEOUT(called, 5000);
    QVERIFY(success);
    QCOMPARE(output, QStringLiteral("hello"));
}

void CommandRunnerTest::asyncFailure()
{
    CommandRunner runner;
    bool called = false;
    bool success = true;
    QString output;

    runner.runCommand(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), QStringLiteral("printf failure >&2; exit 3")}, [&](bool ok, const QString &text) {
        called = true;
        success = ok;
        output = text;
    });

    QTRY_VERIFY_WITH_TIMEOUT(called, 5000);
    QVERIFY(!success);
    QCOMPARE(output, QStringLiteral("failure"));
}

void CommandRunnerTest::asyncStartFailure()
{
    CommandRunner runner;
    bool called = false;
    bool success = true;

    runner.runCommand(QStringLiteral("/definitely/not/a/real/program"), {}, [&](bool ok, const QString &) {
        called = true;
        success = ok;
    });

    QTRY_VERIFY_WITH_TIMEOUT(called, 5000);
    QVERIFY(!success);
}

void CommandRunnerTest::blockingSuccessAndFailure()
{
    CommandRunner runner;
    QString output;

    QVERIFY(runner.runCommandAndWait(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), QStringLiteral("printf hi")}, &output));
    QCOMPARE(output, QStringLiteral("hi"));

    QVERIFY(!runner.runCommandAndWait(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), QStringLiteral("exit 1")}));
    QVERIFY(!runner.runCommandAndWait(QStringLiteral("/definitely/not/a/real/program"), {}));
}

void CommandRunnerTest::asyncDoesNotInterpretArgumentsAsShell()
{
    CommandRunner runner;
    bool called = false;
    bool success = false;
    QString output;

    runner.runCommand(QStringLiteral("/bin/echo"), {QStringLiteral("a b"), QStringLiteral("*")}, [&](bool ok, const QString &text) {
        called = true;
        success = ok;
        output = text;
    });

    QTRY_VERIFY_WITH_TIMEOUT(called, 5000);
    QVERIFY(success);
    QCOMPARE(output.trimmed(), QStringLiteral("a b *"));
}

void CommandRunnerTest::asyncReportsStderrOnFailure()
{
    CommandRunner runner;
    bool called = false;
    bool success = true;
    QString output;

    runner.runCommand(QStringLiteral("/bin/sh"),
                      {QStringLiteral("-c"), QStringLiteral("printf 'stdout'; printf 'stderr' >&2; exit 7")},
                      [&](bool ok, const QString &text) {
                          called = true;
                          success = ok;
                          output = text;
                      });

    QTRY_VERIFY_WITH_TIMEOUT(called, 5000);
    QVERIFY(!success);
    QCOMPARE(output, QStringLiteral("stderr"));
}

void CommandRunnerTest::blockingDoesNotReturnStdoutOnFailure()
{
    CommandRunner runner;
    QString output;
    QVERIFY(!runner.runCommandAndWait(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), QStringLiteral("printf 'stdout'; exit 9")}, &output));
    QCOMPARE(output, QStringLiteral("stdout"));
}

void CommandRunnerTest::asyncEmptyOutput()
{
    CommandRunner runner;
    bool called = false;
    bool success = false;
    QString output = QStringLiteral("not-empty");

    runner.runCommand(QStringLiteral("/bin/true"), {}, [&](bool ok, const QString &text) {
        called = true;
        success = ok;
        output = text;
    });

    QTRY_VERIFY_WITH_TIMEOUT(called, 5000);
    QVERIFY(success);
    QVERIFY(output.isEmpty());
}

QTEST_GUILESS_MAIN(CommandRunnerTest)

#include "tst_commandrunner.moc"
