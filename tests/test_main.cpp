#include "test_registry.h"

#include <QGuiApplication>
#include <QString>
#include <QtTest>

#include <memory>
#include <vector>

QVector<TestFactory>& perigeeTestFactories()
{
    static QVector<TestFactory> factories;
    return factories;
}

int main(int argc, char* argv[])
{
    QGuiApplication application(argc, argv);

    std::vector<std::unique_ptr<QObject>> tests;
    tests.reserve(static_cast<std::size_t>(perigeeTestFactories().size()));
    for (const TestFactory& factory : perigeeTestFactories()) {
        tests.push_back(std::unique_ptr<QObject>(factory()));
    }

    QString selectedClass;
    if (argc > 1) {
        const QString candidate = QString::fromLocal8Bit(argv[1]);
        for (const auto& test : tests) {
            if (candidate == QString::fromLatin1(test->metaObject()->className())) {
                selectedClass = candidate;
                break;
            }
        }
    }

    std::vector<char*> testArguments;
    testArguments.reserve(static_cast<std::size_t>(argc));
    testArguments.push_back(argv[0]);
    const int firstForwardedArgument = selectedClass.isEmpty() ? 1 : 2;
    for (int i = firstForwardedArgument; i < argc; ++i) {
        testArguments.push_back(argv[i]);
    }

    int status = 0;
    const int testArgumentCount = static_cast<int>(testArguments.size());
    for (const auto& test : tests) {
        if (!selectedClass.isEmpty()
                && selectedClass != QString::fromLatin1(test->metaObject()->className())) {
            continue;
        }

        status |= QTest::qExec(test.get(), testArgumentCount, testArguments.data());
    }

    return status;
}
