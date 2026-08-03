QT += core gui testlib

TEMPLATE = app
TARGET = perigee-tests
CONFIG += c++17 testcase

include(../globaldefs.pri)

SOURCES += \
    test_main.cpp \
    test_smoke.cpp

HEADERS += \
    test_registry.h
