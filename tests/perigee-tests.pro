QT += core gui opengl qml quick testlib

TEMPLATE = app
TARGET = perigee-tests
CONFIG += c++17 testcase link_pkgconfig

include(../globaldefs.pri)

SOURCES += \
    ../app/perigee/actions/actionregistry.cpp \
    ../app/perigee/deck/actionlistmodel.cpp \
    ../app/perigee/deck/deckcontroller.cpp \
    ../app/perigee/deck/decksurfacerenderer.cpp \
    ../app/perigee/input/deckinputrouter.cpp \
    ../app/streaming/input/remoteinputstate.cpp \
    ../app/path.cpp \
    ../app/streaming/video/overlaymanager.cpp \
    test_main.cpp \
    test_actionregistry.cpp \
    test_deckcontroller.cpp \
    test_deckqml.cpp \
    test_decksurfacerenderer.cpp \
    test_deckinputrouter.cpp \
    test_inputneutralization.cpp \
    test_overlaylayout.cpp \
    test_smoke.cpp

INCLUDEPATH += $$PWD/../app

PKGCONFIG += sdl2 SDL2_ttf

HEADERS += \
    ../app/perigee/actions/actionregistry.h \
    ../app/perigee/actions/actiontypes.h \
    ../app/perigee/actions/hostadapter.h \
    ../app/perigee/deck/actionlistmodel.h \
    ../app/perigee/deck/deckcontroller.h \
    ../app/perigee/deck/decksurfacerenderer.h \
    ../app/perigee/input/deckinputrouter.h \
    ../app/streaming/input/remoteinputstate.h \
    ../app/path.h \
    ../app/streaming/video/overlaymanager.h \
    test_registry.h

RESOURCES += ../app/qml.qrc
