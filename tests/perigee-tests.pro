QT += core gui network opengl qml quick testlib

TEMPLATE = app
TARGET = perigee-tests
CONFIG += c++17 testcase link_pkgconfig
DEFINES += PERIGEE_TESTING
QMAKE_LFLAGS += -Wl,--wrap=SDL_GetNumTouchFingers \
    -Wl,--wrap=SDL_SetRelativeMouseMode \
    -Wl,--wrap=SDL_PushEvent \
    -Wl,--wrap=SDL_AddTimer

include(../globaldefs.pri)

SOURCES += \
    ../app/perigee/actions/actionregistry.cpp \
    ../app/perigee/actions/actioncategories.cpp \
    ../app/perigee/display/displaytransaction.cpp \
    ../app/perigee/display/sessiontransitioncoordinator.cpp \
    ../app/perigee/actions/gamestreamadapter.cpp \
    ../app/perigee/deck/actionlistmodel.cpp \
    ../app/perigee/deck/deckcontroller.cpp \
    ../app/perigee/deck/deckuipump.cpp \
    ../app/perigee/deck/decksurfacerenderer.cpp \
    ../app/perigee/input/deckbindings.cpp \
    ../app/perigee/input/controllerlayout.cpp \
    ../app/perigee/input/deckinputrouter.cpp \
    ../app/perigee/input/deckinputdelivery.cpp \
    ../app/perigee/polaris/polarismodels.cpp \
    ../app/perigee/polaris/polarisadapter.cpp \
    ../app/perigee/polaris/polarisapiclient.cpp \
    ../app/perigee/support/redaction.cpp \
    ../app/perigee/branding/productidentity.cpp \
    ../app/settings/moonlightsettingsimport.cpp \
    ../app/backend/nvaddress.cpp \
    ../app/backend/identitymanager.cpp \
    ../app/streaming/input/abstouch.cpp \
    ../app/streaming/input/gamepad.cpp \
    ../app/streaming/input/input.cpp \
    ../app/streaming/input/keyboard.cpp \
    ../app/streaming/input/mouse.cpp \
    ../app/streaming/input/reltouch.cpp \
    ../app/streaming/input/remoteinputstate.cpp \
    ../app/settings/streamingpreferences.cpp \
    ../app/gui/sdlgamepadkeynavigation.cpp \
    ../app/path.cpp \
    ../app/streaming/video/overlaymanager.cpp \
    support/fakepolarisserver.cpp \
    test_main.cpp \
    test_actionregistry.cpp \
    test_gamestreamadapter.cpp \
    test_deckcontroller.cpp \
    test_deckuipump.cpp \
    test_deckqml.cpp \
    test_decksurfacerenderer.cpp \
    test_deckbindings.cpp \
    test_deckbindingsqml.cpp \
    test_deckinputrouter.cpp \
    test_deckinputdelivery.cpp \
    test_controllerlayout.cpp \
    test_displaytransaction.cpp \
    test_inputneutralization.cpp \
    test_inputintegration.cpp \
    test_sessionexitintent.cpp \
    test_sessiondisplaytransition.cpp \
    test_sessiontransitioncoordinator.cpp \
    test_sdlgamepadkeynavigation.cpp \
    test_streamingpreferences.cpp \
    test_streamsegueqml.cpp \
    input_integration_stubs.cpp \
    test_branding.cpp \
    test_overlaylayout.cpp \
    test_polarismodels.cpp \
    test_polarisadapter.cpp \
    test_polarisactions.cpp \
    test_polarisapiclient.cpp \
    test_polarisintegration.cpp \
    test_redaction.cpp \
    test_smoke.cpp

INCLUDEPATH += \
    $$PWD/../app \
    $$PWD/../moonlight-common-c/moonlight-common-c/src \
    $$PWD/../qmdnsengine/qmdnsengine/src/include \
    $$PWD/../qmdnsengine

PKGCONFIG += sdl2 SDL2_ttf opus openssl

HEADERS += \
    ../app/perigee/actions/actionregistry.h \
    ../app/perigee/actions/actioncategories.h \
    ../app/perigee/actions/actiontypes.h \
    ../app/perigee/actions/gamestreamadapter.h \
    ../app/perigee/actions/hostadapter.h \
    ../app/perigee/actions/sessionfacade.h \
    ../app/perigee/deck/actionlistmodel.h \
    ../app/perigee/deck/deckcontroller.h \
    ../app/perigee/deck/deckuipump.h \
    ../app/perigee/deck/decksurfacerenderer.h \
    ../app/perigee/display/displaytransaction.h \
    ../app/perigee/display/sessiontransitioncoordinator.h \
    ../app/perigee/input/deckbindings.h \
    ../app/perigee/input/controllerlayout.h \
    ../app/perigee/input/deckinputrouter.h \
    ../app/perigee/input/deckinputdelivery.h \
    ../app/perigee/polaris/polarisapiclient.h \
    ../app/perigee/polaris/polarisresponse.h \
    ../app/perigee/support/redaction.h \
    ../app/perigee/branding/productidentity.h \
    ../app/settings/moonlightsettingsimport.h \
    ../app/streaming/input/remoteinputstate.h \
    ../app/streaming/sessionexitintent.h \
    ../app/settings/streamingpreferences.h \
    ../app/gui/sdlgamepadkeynavigation.h \
    input_integration_stubs.h \
    ../app/path.h \
    ../app/streaming/video/overlaymanager.h \
    ../app/perigee/polaris/polarismodels.h \
    ../app/perigee/polaris/polarisadapter.h \
    support/fakepolarisserver.h \
    test_registry.h

RESOURCES += ../app/qml.qrc \
    ../app/resources.qrc
