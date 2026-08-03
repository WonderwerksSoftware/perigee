#pragma once

#include <QObject>
#include <QVector>

#include <functional>

using TestFactory = std::function<QObject*()>;

QVector<TestFactory>& perigeeTestFactories();

#define REGISTER_PERIGEE_TEST(TestClass) \
    static const bool TestClass##_registered = [] { \
        perigeeTestFactories().push_back([] { return new TestClass; }); \
        return true; \
    }()
