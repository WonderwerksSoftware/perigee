QT += core gui testlib

TEMPLATE = app
TARGET = perigee-tests
CONFIG += c++17 testcase link_pkgconfig

include(../globaldefs.pri)

SOURCES += \
    ../app/perigee/actions/actionregistry.cpp \
    ../app/path.cpp \
    ../app/streaming/video/overlaymanager.cpp \
    test_main.cpp \
    test_actionregistry.cpp \
    test_overlaylayout.cpp \
    test_smoke.cpp

INCLUDEPATH += $$PWD/../app

PKGCONFIG += sdl2 SDL2_ttf

HEADERS += \
    ../app/perigee/actions/actionregistry.h \
    ../app/perigee/actions/actiontypes.h \
    ../app/perigee/actions/hostadapter.h \
    ../app/path.h \
    ../app/streaming/video/overlaymanager.h \
    test_registry.h
