QT += core gui opengl qml quick testlib

TEMPLATE = app
TARGET = perigee-tests
CONFIG += c++17 testcase link_pkgconfig

include(../globaldefs.pri)

SOURCES += \
    ../app/perigee/actions/actionregistry.cpp \
    ../app/perigee/deck/decksurfacerenderer.cpp \
    ../app/path.cpp \
    ../app/streaming/video/overlaymanager.cpp \
    test_main.cpp \
    test_actionregistry.cpp \
    test_decksurfacerenderer.cpp \
    test_overlaylayout.cpp \
    test_smoke.cpp

INCLUDEPATH += $$PWD/../app

PKGCONFIG += sdl2 SDL2_ttf

HEADERS += \
    ../app/perigee/actions/actionregistry.h \
    ../app/perigee/actions/actiontypes.h \
    ../app/perigee/actions/hostadapter.h \
    ../app/perigee/deck/decksurfacerenderer.h \
    ../app/path.h \
    ../app/streaming/video/overlaymanager.h \
    test_registry.h

RESOURCES += ../app/qml.qrc
