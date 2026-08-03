QT += core gui testlib

TEMPLATE = app
TARGET = perigee-tests
CONFIG += c++17 testcase

include(../globaldefs.pri)

SOURCES += \
    ../app/perigee/actions/actionregistry.cpp \
    test_main.cpp \
    test_actionregistry.cpp \
    test_smoke.cpp

INCLUDEPATH += $$PWD/../app

HEADERS += \
    ../app/perigee/actions/actionregistry.h \
    ../app/perigee/actions/actiontypes.h \
    ../app/perigee/actions/hostadapter.h \
    test_registry.h
