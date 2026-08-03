#include "test_registry.h"

#include "perigee/actions/actionregistry.h"
#include "perigee/deck/actionlistmodel.h"
#include "perigee/deck/deckcontroller.h"
#include "perigee/deck/decksurfacerenderer.h"
#include "streaming/video/overlaymanager.h"

#include <QAbstractItemModel>
#include <QCoreApplication>
#include <QDir>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QtTest>

#include <SDL.h>

#include <memory>

namespace {

class DeckQmlHostAdapter final : public HostAdapter
{
public:
    HostSnapshot currentSnapshot;
    QStringList executedActionIds;

    HostSnapshot snapshot() override
    {
        return currentSnapshot;
    }

    void execute(const QString& actionId, const QVariantMap&, Completion) override
    {
        executedActionIds.push_back(actionId);
    }

    void cancel(const QString&) override
    {
    }
};

ActionDescriptor descriptor(const QString& id,
                            const QString& label,
                            ActionCategory category,
                            ConfirmationPolicy confirmation = ConfirmationPolicy::Never)
{
    return { id, label, category, {}, {}, {}, 0, confirmation, {} };
}

ActionState state(bool enabled,
                  const QString& value = {},
                  const QString& disabledReason = {},
                  ActionPhase phase = ActionPhase::Idle,
                  const QString& message = {})
{
    ActionState state;
    state.enabled = enabled;
    state.value = value;
    state.disabledReason = disabledReason;
    state.phase = phase;
    state.message = message;
    return state;
}

void sendKey(DeckSurfaceRenderer& renderer, int key)
{
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
    QVERIFY(renderer.sendKeyEvent(&press));
    QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
    QVERIFY(renderer.sendKeyEvent(&release));
    QCoreApplication::processEvents();
}

void sendTextKey(DeckSurfaceRenderer& renderer, int key, const QString& text)
{
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, text);
    QVERIFY(renderer.sendKeyEvent(&press));
    QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier, text);
    QVERIFY(renderer.sendKeyEvent(&release));
    QCoreApplication::processEvents();
}

QString focusedActionId(const QAbstractItemModel* model)
{
    for (int row = 0; row < model->rowCount(); ++row) {
        const QModelIndex index = model->index(row, 0);
        if (model->data(index, ActionListModel::FocusedRole).toBool()) {
            return model->data(index, ActionListModel::IdRole).toString();
        }
    }
    return {};
}

QQuickItem* findVisualItemWithProperty(QQuickItem* root,
                                       const char* propertyName,
                                       const QVariant& value)
{
    if (root->property(propertyName) == value) {
        return root;
    }
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* match = findVisualItemWithProperty(
                child, propertyName, value)) {
            return match;
        }
    }
    return nullptr;
}

QQuickItem* findListViewWithCount(QQuickItem* root, int count)
{
    if (root->property("count").isValid() &&
            root->property("contentY").isValid() &&
            root->property("count").toInt() == count) {
        return root;
    }
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* match = findListViewWithCount(child, count)) {
            return match;
        }
    }
    return nullptr;
}

}

class DeckQmlTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void exposesOnlyTheApprovedActionRoles();
    void loadsShellAndReachesCategoriesAndActionsFromKeyboard();
    void disabledRowCannotActivatePreviouslyFocusedAction();
    void realKeysKeepControllerAndQmlFocusInSync();
    void keyboardFocusKeepsFifthActionVisible();
    void controllerOpenRevealsInitiallyOffscreenFirstEnabledAction();
    void rendersAndPublishesOwnedArgbSurface();
    void realPointerEventSelectsCategoryThroughRenderer();
    void realTextInputCommitsThroughRenderer();
};

void DeckQmlTest::initTestCase()
{
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
}

void DeckQmlTest::exposesOnlyTheApprovedActionRoles()
{
    DeckQmlHostAdapter adapter;
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("display.select"), state(true, QStringLiteral("Desk monitor")));
    ActionRegistry registry({
        descriptor(QStringLiteral("display.select"), QStringLiteral("Choose display"),
                   ActionCategory::Display, ConfirmationPolicy::Always),
    }, adapter);
    DeckController controller(&registry);
    controller.openFromKeyboard();

    const QHash<int, QByteArray> roles = controller.actionModel()->roleNames();
    QSet<QByteArray> actualRoles;
    for (auto it = roles.cbegin(); it != roles.cend(); ++it) {
        actualRoles.insert(it.value());
    }
    const QSet<QByteArray> expectedRoles {
        "id", "label", "category", "valueText", "enabled",
        "disabledReason", "phase", "message", "focused",
        "requiresConfirmation"
    };

    QCOMPARE(actualRoles, expectedRoles);
    QCOMPARE(controller.actionModel()->data(
                 controller.actionModel()->index(0, 0),
                 ActionListModel::CategoryRole).toString(),
             QStringLiteral("Display"));
    QCOMPARE(controller.actionModel()->data(
                 controller.actionModel()->index(0, 0),
                 ActionListModel::ValueTextRole).toString(),
             QStringLiteral("Desk monitor"));
    QVERIFY(controller.actionModel()->data(
        controller.actionModel()->index(0, 0),
        ActionListModel::RequiresConfirmationRole).toBool());
}

void DeckQmlTest::loadsShellAndReachesCategoriesAndActionsFromKeyboard()
{
    DeckQmlHostAdapter adapter;
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("display.select"), state(true, QStringLiteral("Desk monitor")));
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("input.mouse-capture"), state(true, QStringLiteral("Captured")));
    ActionRegistry registry({
        descriptor(QStringLiteral("display.select"), QStringLiteral("Choose display"),
                   ActionCategory::Display),
        descriptor(QStringLiteral("input.mouse-capture"), QStringLiteral("Mouse capture"),
                   ActionCategory::Input),
    }, adapter);
    DeckController controller(&registry);
    controller.openFromKeyboard();
    QQmlEngine engine;
    DeckSurfaceRenderer renderer;
    QString error;

    QVERIFY2(renderer.initialize(
                 &engine,
                 QUrl(QStringLiteral("qrc:/gui/perigee/PerigeeDeck.qml")),
                 &controller,
                 &error),
             qPrintable(error));
    renderer.resize(QSize(960, 540), 1.0);
    QImage initialFrame;
    QVERIFY2(renderer.render(&initialFrame, &error), qPrintable(error));

    QObject* root = renderer.rootObject();
    QVERIFY(root != nullptr);
    QQuickItem* searchField = root->findChild<QQuickItem*>(
        QStringLiteral("searchField"));
    QQuickItem* categoryRail = root->findChild<QQuickItem*>(
        QStringLiteral("categoryRail"));
    QQuickItem* actionTray = root->findChild<QQuickItem*>(
        QStringLiteral("actionTray"));
    QVERIFY(searchField != nullptr);
    QVERIFY(categoryRail != nullptr);
    QVERIFY(actionTray != nullptr);
    QTRY_VERIFY(searchField->hasActiveFocus());

    sendKey(renderer, Qt::Key_Tab);
    QTRY_VERIFY(categoryRail->hasActiveFocus());

    sendKey(renderer, Qt::Key_Right);
    QCOMPARE(controller.activeCategory(), 1);
    QCOMPARE(controller.searchText(), QString());

    sendKey(renderer, Qt::Key_Down);
    QTRY_VERIFY(actionTray->hasActiveFocus());
    QCOMPARE(focusedActionId(controller.actionModel()),
             QStringLiteral("input.mouse-capture"));

    controller.close();
    controller.openFromKeyboard();
    QTRY_VERIFY(searchField->hasActiveFocus());

    controller.close();
    controller.openFromController();
    QTRY_VERIFY(actionTray->hasActiveFocus());
    QCOMPARE(focusedActionId(controller.actionModel()),
             QStringLiteral("display.select"));
}

void DeckQmlTest::disabledRowCannotActivatePreviouslyFocusedAction()
{
    DeckQmlHostAdapter adapter;
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("display.enabled"), state(true));
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("display.disabled"),
        state(false, {}, QStringLiteral("Display is unavailable.")));
    ActionRegistry registry({
        descriptor(QStringLiteral("display.enabled"), QStringLiteral("Enabled display"),
                   ActionCategory::Display),
        descriptor(QStringLiteral("display.disabled"), QStringLiteral("Disabled display"),
                   ActionCategory::Display),
    }, adapter);
    DeckController controller(&registry);
    controller.openFromController();
    QCOMPARE(focusedActionId(controller.actionModel()),
             QStringLiteral("display.enabled"));
    QQmlEngine engine;
    DeckSurfaceRenderer renderer;
    QString error;

    QVERIFY2(renderer.initialize(
                 &engine,
                 QUrl(QStringLiteral("qrc:/gui/perigee/PerigeeDeck.qml")),
                 &controller,
                 &error),
             qPrintable(error));
    renderer.resize(QSize(960, 540), 1.0);
    QImage frame;
    QVERIFY2(renderer.render(&frame, &error), qPrintable(error));

    QQuickItem* disabledRow = findVisualItemWithProperty(
        qobject_cast<QQuickItem*>(renderer.rootObject()),
        "actionId",
        QStringLiteral("display.disabled"));
    QVERIFY(disabledRow != nullptr);
    QVERIFY(QMetaObject::invokeMethod(disabledRow, "chosen"));
    QCoreApplication::processEvents();

    QVERIFY(adapter.executedActionIds.isEmpty());
    QCOMPARE(focusedActionId(controller.actionModel()),
             QStringLiteral("display.enabled"));
}

void DeckQmlTest::realKeysKeepControllerAndQmlFocusInSync()
{
    DeckQmlHostAdapter adapter;
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("display.select"), state(true));
    ActionRegistry registry({
        descriptor(QStringLiteral("display.select"), QStringLiteral("Choose display"),
                   ActionCategory::Display),
    }, adapter);
    DeckController controller(&registry);
    controller.openFromKeyboard();
    QQmlEngine engine;
    DeckSurfaceRenderer renderer;
    QString error;

    QVERIFY2(renderer.initialize(
                 &engine,
                 QUrl(QStringLiteral("qrc:/gui/perigee/PerigeeDeck.qml")),
                 &controller,
                 &error),
             qPrintable(error));
    renderer.resize(QSize(960, 540), 1.0);
    QImage frame;
    QVERIFY2(renderer.render(&frame, &error), qPrintable(error));

    QQuickItem* searchField = renderer.rootObject()->findChild<QQuickItem*>(
        QStringLiteral("searchField"));
    QQuickItem* categoryRail = renderer.rootObject()->findChild<QQuickItem*>(
        QStringLiteral("categoryRail"));
    QQuickItem* actionTray = renderer.rootObject()->findChild<QQuickItem*>(
        QStringLiteral("actionTray"));
    QVERIFY(searchField != nullptr);
    QVERIFY(categoryRail != nullptr);
    QVERIFY(actionTray != nullptr);
    QTRY_VERIFY(searchField->hasActiveFocus());
    QCOMPARE(controller.property("focusRegion").toInt(), 0);

    sendKey(renderer, Qt::Key_Tab);
    QTRY_VERIFY(categoryRail->hasActiveFocus());
    QCOMPARE(controller.property("focusRegion").toInt(), 1);

    sendKey(renderer, Qt::Key_Down);
    QTRY_VERIFY(actionTray->hasActiveFocus());
    QCOMPARE(controller.property("focusRegion").toInt(), 2);

    sendKey(renderer, Qt::Key_Escape);
    QVERIFY(controller.isOpen());
    QTRY_VERIFY(categoryRail->hasActiveFocus());
    QCOMPARE(controller.property("focusRegion").toInt(), 1);

    sendKey(renderer, Qt::Key_Up);
    QTRY_VERIFY(searchField->hasActiveFocus());
    QCOMPARE(controller.property("focusRegion").toInt(), 0);

    sendTextKey(renderer, Qt::Key_X, QStringLiteral("x"));
    QTRY_COMPARE(controller.searchText(), QStringLiteral("x"));
    sendKey(renderer, Qt::Key_Escape);
    QVERIFY(controller.isOpen());
    QCOMPARE(controller.searchText(), QString());
    QTRY_VERIFY(searchField->hasActiveFocus());

    sendKey(renderer, Qt::Key_Escape);
    QVERIFY(!controller.isOpen());
}

void DeckQmlTest::keyboardFocusKeepsFifthActionVisible()
{
    DeckQmlHostAdapter adapter;
    QVector<ActionDescriptor> descriptors;
    for (int actionIndex = 0; actionIndex < 6; ++actionIndex) {
        const QString actionId = QStringLiteral("display.%1").arg(actionIndex);
        adapter.currentSnapshot.actionStates.insert(actionId, state(true));
        descriptors.push_back(descriptor(
            actionId,
            QStringLiteral("Display action %1").arg(actionIndex),
            ActionCategory::Display));
    }
    ActionRegistry registry(descriptors, adapter);
    DeckController controller(&registry);
    controller.openFromController();
    QQmlEngine engine;
    DeckSurfaceRenderer renderer;
    QString error;

    QVERIFY2(renderer.initialize(
                 &engine,
                 QUrl(QStringLiteral("qrc:/gui/perigee/PerigeeDeck.qml")),
                 &controller,
                 &error),
             qPrintable(error));
    renderer.resize(QSize(960, 540), 1.0);
    QImage frame;
    QVERIFY2(renderer.render(&frame, &error), qPrintable(error));

    QQuickItem* actionList = findListViewWithCount(
        qobject_cast<QQuickItem*>(renderer.rootObject()), 6);
    QVERIFY(actionList != nullptr);
    for (int move = 0; move < 4; ++move) {
        sendKey(renderer, Qt::Key_Down);
    }

    QCOMPARE(focusedActionId(controller.actionModel()),
             QStringLiteral("display.4"));
    QTRY_COMPARE(actionList->property("currentIndex").toInt(), 4);
    QTRY_VERIFY(actionList->property("contentY").toReal() > 0.0);
}

void DeckQmlTest::controllerOpenRevealsInitiallyOffscreenFirstEnabledAction()
{
    DeckQmlHostAdapter adapter;
    QVector<ActionDescriptor> descriptors;
    for (int actionIndex = 0; actionIndex < 20; ++actionIndex) {
        const QString actionId = QStringLiteral("display.disabled.%1").arg(actionIndex);
        adapter.currentSnapshot.actionStates.insert(
            actionId,
            state(false, {}, QStringLiteral("Display is unavailable.")));
        descriptors.push_back(descriptor(
            actionId,
            QStringLiteral("Disabled display %1").arg(actionIndex),
            ActionCategory::Display));
    }
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("display.enabled"), state(true));
    descriptors.push_back(descriptor(
        QStringLiteral("display.enabled"),
        QStringLiteral("Enabled display"),
        ActionCategory::Display));
    ActionRegistry registry(descriptors, adapter);
    DeckController controller(&registry);
    controller.openFromController();
    QCOMPARE(focusedActionId(controller.actionModel()),
             QStringLiteral("display.enabled"));
    QQmlEngine engine;
    DeckSurfaceRenderer renderer;
    QString error;

    QVERIFY2(renderer.initialize(
                 &engine,
                 QUrl(QStringLiteral("qrc:/gui/perigee/PerigeeDeck.qml")),
                 &controller,
                 &error),
             qPrintable(error));
    renderer.resize(QSize(960, 540), 1.0);
    QImage frame;
    QVERIFY2(renderer.render(&frame, &error), qPrintable(error));

    QQuickItem* actionList = findListViewWithCount(
        qobject_cast<QQuickItem*>(renderer.rootObject()), 21);
    QVERIFY(actionList != nullptr);
    QTRY_COMPARE(actionList->property("currentIndex").toInt(), 20);
    QTRY_VERIFY(actionList->property("contentY").toReal() > 0.0);
}

void DeckQmlTest::rendersAndPublishesOwnedArgbSurface()
{
    DeckQmlHostAdapter adapter;
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("display.select"), state(true, QStringLiteral("Desk monitor")));
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("input.mouse-capture"),
        state(false, {}, QStringLiteral("Pointer lock is unavailable on this desktop.")));
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("stats.overlay"),
        state(false, {}, QStringLiteral("Updating stream statistics."),
              ActionPhase::Working, QStringLiteral("Working...")));
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("session.end"), state(true));
    ActionRegistry registry({
        descriptor(QStringLiteral("display.select"), QStringLiteral("Control display"),
                   ActionCategory::Display),
        descriptor(QStringLiteral("input.mouse-capture"), QStringLiteral("Control mouse capture"),
                   ActionCategory::Input),
        descriptor(QStringLiteral("stats.overlay"), QStringLiteral("Control stream statistics"),
                   ActionCategory::Stats),
        descriptor(QStringLiteral("session.end"), QStringLiteral("Control host session"),
                   ActionCategory::Session, ConfirmationPolicy::Always),
    }, adapter);
    DeckController controller(&registry);
    controller.openFromKeyboard();
    controller.setSearchText(QStringLiteral("control"));
    controller.focusActions();
    controller.focusAction(QStringLiteral("session.end"));
    controller.activateFocusedAction();
    QVERIFY(controller.confirmationVisible());
    QQmlEngine engine;
    DeckSurfaceRenderer renderer;
    Overlay::OverlayManager overlays;
    QString error;

    QVERIFY2(renderer.initialize(
                 &engine,
                 QUrl(QStringLiteral("qrc:/gui/perigee/PerigeeDeck.qml")),
                 &controller,
                 &error),
             qPrintable(error));
    renderer.resize(QSize(960, 640), 1.0);
    QVERIFY2(renderer.renderAndPublishDeck(&overlays, &error), qPrintable(error));
    QVERIFY(controller.confirmationVisible());

    SDL_Surface* ownedSurface = nullptr;
    Overlay::OverlayPresentation presentation;
    QVERIFY(overlays.getUpdatedOverlaySurface(
        Overlay::OverlayDeck, &ownedSurface, &presentation));
    std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)> surface(
        ownedSurface, SDL_FreeSurface);
    QVERIFY(surface != nullptr);
    QCOMPARE(surface->format->format, Uint32(SDL_PIXELFORMAT_ARGB8888));
    QVERIFY((surface->flags & SDL_PREALLOC) == 0);
    QCOMPARE(surface->w, 960);
    QCOMPARE(surface->h, 640);
    QVERIFY(surface->pitch >= surface->w * 4);
    QCOMPARE(presentation.anchor, Overlay::OverlayAnchor::TopCenter);

    const QImage view(static_cast<const uchar*>(surface->pixels),
                      surface->w,
                      surface->h,
                      surface->pitch,
                      QImage::Format_ARGB32_Premultiplied);
    const QImage ownedImage = view.copy();
    surface.reset();
    QCOMPARE(ownedImage.pixelColor(0, 0).alpha(), 0);
    int opaquePixelCount = 0;
    for (int y = 0; y < ownedImage.height(); y += 8) {
        for (int x = 0; x < ownedImage.width(); x += 8) {
            opaquePixelCount += ownedImage.pixelColor(x, y).alpha() > 0 ? 1 : 0;
        }
    }
    QVERIFY(opaquePixelCount > 100);
    QVERIFY(opaquePixelCount < 960 * 640 / 64 * 3 / 4);

    QDir artifactDirectory(
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("../artifacts")));
    QVERIFY(artifactDirectory.mkpath(QStringLiteral(".")));
    const QString artifactPath = artifactDirectory.filePath(
        QStringLiteral("perigee-search-rail.png"));
    QVERIFY2(ownedImage.save(artifactPath), qPrintable(artifactPath));
    qInfo().noquote() << "Deck visual artifact:" << artifactPath;
}

void DeckQmlTest::realPointerEventSelectsCategoryThroughRenderer()
{
    DeckQmlHostAdapter adapter;
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("display.select"), state(true));
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("input.capture"), state(true));
    ActionRegistry registry({
        descriptor(QStringLiteral("display.select"), QStringLiteral("Display"),
                   ActionCategory::Display),
        descriptor(QStringLiteral("input.capture"), QStringLiteral("Input"),
                   ActionCategory::Input),
    }, adapter);
    DeckController controller(&registry);
    controller.openFromKeyboard();
    QQmlEngine engine;
    DeckSurfaceRenderer renderer;
    QString error;
    QImage image;
    QVERIFY2(renderer.initialize(
                 &engine,
                 QUrl(QStringLiteral("qrc:/gui/perigee/PerigeeDeck.qml")),
                 &controller,
                 &error),
             qPrintable(error));
    renderer.resize(QSize(960, 540), 1.0);
    QVERIFY2(renderer.render(&image, &error), qPrintable(error));

    // The Input category is the second 120px item in the centered 745px row.
    const QPointF inputCategoryCenter(300.0, 105.0);
    QMouseEvent press(QEvent::MouseButtonPress, inputCategoryCenter,
                      inputCategoryCenter, inputCategoryCenter,
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QVERIFY(renderer.sendPointerEvent(&press));
    QMouseEvent release(QEvent::MouseButtonRelease, inputCategoryCenter,
                        inputCategoryCenter, inputCategoryCenter,
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QVERIFY(renderer.sendPointerEvent(&release));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);

    QTRY_COMPARE(controller.activeCategory(), 1);
    QCOMPARE(controller.focusRegion(), DeckController::CategoriesRegion);
}

void DeckQmlTest::realTextInputCommitsThroughRenderer()
{
    DeckController controller;
    controller.openFromKeyboard();
    QQmlEngine engine;
    DeckSurfaceRenderer renderer;
    QString error;
    QImage image;
    QVERIFY2(renderer.initialize(
                 &engine,
                 QUrl(QStringLiteral("qrc:/gui/perigee/PerigeeDeck.qml")),
                 &controller,
                 &error),
             qPrintable(error));
    renderer.resize(QSize(960, 540), 1.0);
    QVERIFY2(renderer.render(&image, &error), qPrintable(error));

    QVERIFY(renderer.sendTextInput(QString::fromUtf8("hé")));
    QTRY_COMPARE(controller.searchText(), QString::fromUtf8("hé"));
}

REGISTER_PERIGEE_TEST(DeckQmlTest);

#include "test_deckqml.moc"
