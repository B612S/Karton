// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#include <QElapsedTimer>
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
    void asyncTimesOut();
    void blockingTimesOut();
    void cancelStopsCommand();
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

void CommandRunnerTest::asyncTimesOut()
{
    CommandRunner runner;
    int calls = 0;
    bool success = true;
    QString output;
    QElapsedTimer elapsed;
    elapsed.start();

    runner.runCommand(
        QStringLiteral("/bin/sh"),
        {QStringLiteral("-c"), QStringLiteral("sleep 10")},
        [&](bool ok, const QString &text) {
            ++calls;
            success = ok;
            output = text;
        },
        300);

    QTRY_COMPARE_WITH_TIMEOUT(calls, 1, 5000);
    QVERIFY(!success);
    QVERIFY(!output.isEmpty());
    QVERIFY(elapsed.elapsed() < 5000);

    // The killed process still reports `finished`; it must not invoke the
    // callback a second time.
    QTest::qWait(800);
    QCOMPARE(calls, 1);
}

void CommandRunnerTest::blockingTimesOut()
{
    CommandRunner runner;
    QElapsedTimer elapsed;
    elapsed.start();

    QVERIFY(!runner.runCommandAndWait(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), QStringLiteral("sleep 10")}, nullptr, 300));
    QVERIFY(elapsed.elapsed() < 5000);
}

void CommandRunnerTest::cancelStopsCommand()
{
    CommandRunner runner;
    bool called = false;
    bool success = true;
    QString output;
    QElapsedTimer elapsed;
    elapsed.start();

    const CommandRunner::CommandId id =
        runner.runCommand(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), QStringLiteral("sleep 10")}, [&](bool ok, const QString &text) {
            called = true;
            success = ok;
            output = text;
        });
    QVERIFY(id != 0);

    QTest::qWait(200);
    runner.cancelCommand(id);

    QTRY_VERIFY_WITH_TIMEOUT(called, 5000);
    QVERIFY(!success);
    QVERIFY(!output.isEmpty());
    QVERIFY(elapsed.elapsed() < 5000);
}

QTEST_GUILESS_MAIN(CommandRunnerTest)

#include "tst_commandrunner.moc"
