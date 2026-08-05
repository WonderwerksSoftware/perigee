#include "test_registry.h"

#include "perigee/actions/actionregistry.h"
#include "perigee/deck/actionlistmodel.h"
#include "perigee/deck/deckcontroller.h"
#include "perigee/deck/decksurfacerenderer.h"
#include "streaming/video/overlaymanager.h"

#include <QAbstractItemModel>
#include <QAccessible>
#include <QCoreApplication>
#include <QDir>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QtTest>

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <memory>

namespace {

QStringList capturedQtWarnings;
QtMessageHandler previousQtMessageHandler = nullptr;

void captureQtWarnings(QtMsgType type,
                       const QMessageLogContext& context,
                       const QString& message)
{
    if (type == QtWarningMsg) {
        capturedQtWarnings.push_back(message);
    }
    if (previousQtMessageHandler != nullptr) {
        previousQtMessageHandler(type, context, message);
    }
}

class DeckQmlHostAdapter final : public HostAdapter
{
public:
    HostSnapshot currentSnapshot;
    QStringList executedActionIds;

    HostSnapshot snapshot() override
    {
        return currentSnapshot;
    }

    void execute(const QString& actionId, QVariantMap, Completion) override
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

QQuickItem* findMouseArea(QQuickItem* root)
{
    if (root->property("preventStealing").isValid()) {
        return root;
    }
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* match = findMouseArea(child)) {
            return match;
        }
    }
    return nullptr;
}

qreal relativeLuminance(const QColor& color)
{
    const auto linearChannel = [](qreal channel) {
        channel /= 255.0;
        return channel <= 0.04045
            ? channel / 12.92
            : std::pow((channel + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linearChannel(color.red()) +
           0.7152 * linearChannel(color.green()) +
           0.0722 * linearChannel(color.blue());
}

qreal contrastRatio(const QColor& first, const QColor& second)
{
    const qreal firstLuminance = relativeLuminance(first);
    const qreal secondLuminance = relativeLuminance(second);
    const qreal lighter = qMax(firstLuminance, secondLuminance);
    const qreal darker = qMin(firstLuminance, secondLuminance);
    return (lighter + 0.05) / (darker + 0.05);
}

QImage compositeOver(const QImage& foreground, const QColor& background)
{
    QImage composite(foreground.size(), QImage::Format_ARGB32_Premultiplied);
    composite.fill(background);
    QPainter painter(&composite);
    painter.drawImage(QPoint(), foreground);
    return composite;
}

}

class DeckQmlTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void exposesOnlyTheApprovedActionRoles();
    void actionRowRendersSucceededEvidence();
    void loadsShellAndReachesCategoriesAndActionsFromKeyboard();
    void disabledRowCannotActivatePreviouslyFocusedAction();
    void realKeysKeepControllerAndQmlFocusInSync();
    void keyboardFocusKeepsFifthActionVisible();
    void controllerOpenRevealsInitiallyOffscreenFirstEnabledAction();
    void controllerGlyphsFollowEffectiveLayout();
    void focusedActionBorderContrastsAgainstLightAndDarkVideo();
    void categoryResetDoesNotPositionAnOutOfRangeDelegate();
    void actionRowRetainsPointerPressForClick();
    void exposesAccessibleNamesAndDisabledReasons();
    void confirmationTrapsFocusUntilAcceptedOrCancelled();
    void rendersControllerLayoutReferencePngs();
    void rendersAndPublishesOwnedArgbSurface();
    void realPointerEventSelectsCategoryThroughRenderer();
    void pointerClickActivatesActionAfterFocus();
    void pointerClickOnSearchFrameFocusesSearchField();
    void realTextInputCommitsThroughRenderer();
    void realKeyInputEditsSearchTextThroughRenderer();
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

void DeckQmlTest::actionRowRendersSucceededEvidence()
{
    QQmlEngine engine;
    QQmlComponent component(
        &engine, QUrl(QStringLiteral("qrc:/gui/perigee/ActionRow.qml")));
    std::unique_ptr<QObject> object(component.createWithInitialProperties({
        {QStringLiteral("actionId"), QStringLiteral("display.select")},
        {QStringLiteral("actionLabel"), QStringLiteral("Choose display")},
        {QStringLiteral("actionCategory"), QStringLiteral("Display")},
        {QStringLiteral("valueText"), QStringLiteral("Desk monitor")},
        {QStringLiteral("actionEnabled"), true},
        {QStringLiteral("disabledReason"), QString()},
        {QStringLiteral("phase"), QStringLiteral("succeeded")},
        {QStringLiteral("message"), QStringLiteral("Display confirmed")},
        {QStringLiteral("actionFocused"), false},
        {QStringLiteral("requiresConfirmation"), false},
    }));
    QVERIFY2(object != nullptr, qPrintable(component.errorString()));
    auto* root = qobject_cast<QQuickItem*>(object.get());
    QVERIFY(root != nullptr);

    QQuickItem* evidence = findVisualItemWithProperty(
        root, "text", QStringLiteral("Display  •  Display confirmed"));

    QVERIFY(evidence != nullptr);
    QCOMPARE(evidence->property("color").value<QColor>(),
             QColor(QStringLiteral("#8de6a7")));
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

void DeckQmlTest::controllerGlyphsFollowEffectiveLayout()
{
    ControllerLayout layout;
    layout.configure(ControllerLayout::Family::PlayStation, false);
    QQmlEngine engine;
    QQmlComponent component(
        &engine, QUrl(QStringLiteral("qrc:/gui/perigee/ControllerHint.qml")));
    std::unique_ptr<QObject> object(component.createWithInitialProperties({
        {QStringLiteral("confirming"), false},
        {QStringLiteral("controllerLayout"), QVariant::fromValue(&layout)},
    }));
    QVERIFY2(object != nullptr, qPrintable(component.errorString()));
    auto* root = qobject_cast<QQuickItem*>(object.get());
    QVERIFY(root != nullptr);

    QQuickItem* confirmGlyph = root->findChild<QQuickItem*>(
        QStringLiteral("controllerConfirmGlyph"));
    QQuickItem* backGlyph = root->findChild<QQuickItem*>(
        QStringLiteral("controllerBackGlyph"));
    QVERIFY(confirmGlyph != nullptr);
    QVERIFY(backGlyph != nullptr);
    QCOMPARE(confirmGlyph->property("label").toString(),
             QStringLiteral("Cross"));
    QCOMPARE(confirmGlyph->property("visualLabel").toString(),
             QString::fromUtf8("×"));
    QCOMPARE(backGlyph->property("label").toString(),
             QStringLiteral("Circle"));
    QCOMPARE(backGlyph->property("visualLabel").toString(),
             QString::fromUtf8("○"));
    QCOMPARE(confirmGlyph->property("source").toUrl(),
             QUrl(QStringLiteral(
                 "qrc:/gui/perigee/glyphs/playstation-cross.svg")));

    layout.configure(ControllerLayout::Family::Nintendo, true);
    QCoreApplication::processEvents();
    QCOMPARE(confirmGlyph->property("label").toString(), QStringLiteral("A"));
    QCOMPARE(confirmGlyph->property("visualLabel").toString(),
             QStringLiteral("A"));
    QCOMPARE(backGlyph->property("label").toString(), QStringLiteral("B"));
    QCOMPARE(confirmGlyph->property("source").toUrl(),
             QUrl(QStringLiteral("qrc:/gui/perigee/glyphs/nintendo-a.svg")));
}

void DeckQmlTest::focusedActionBorderContrastsAgainstLightAndDarkVideo()
{
    DeckQmlHostAdapter adapter;
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("display.select"), state(true));
    ActionRegistry registry({
        descriptor(QStringLiteral("display.select"),
                   QStringLiteral("Choose display"),
                   ActionCategory::Display),
    }, adapter);
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

    auto* root = qobject_cast<QQuickItem*>(renderer.rootObject());
    QVERIFY(root != nullptr);
    QQuickItem* focusedRow = findVisualItemWithProperty(
        root, "actionId", QStringLiteral("display.select"));
    QVERIFY(focusedRow != nullptr);
    QVERIFY(focusedRow->property("actionFocused").toBool());
    const QPointF rowTopLeft = focusedRow->mapToItem(root, QPointF());
    const QPoint borderSample(
        qRound(rowTopLeft.x() + focusedRow->width() / 2.0),
        qRound(rowTopLeft.y() + 1.0));
    const QPoint interiorSample(
        qRound(rowTopLeft.x() + focusedRow->width() - 24.0),
        qRound(rowTopLeft.y() + focusedRow->height() / 2.0));
    QVERIFY(frame.rect().contains(borderSample));
    QVERIFY(frame.rect().contains(interiorSample));

    const QColor videoBackgrounds[] { Qt::black, Qt::white };
    for (const QColor& videoBackground : videoBackgrounds) {
        const QImage composite = compositeOver(frame, videoBackground);
        const QColor border = composite.pixelColor(borderSample);
        const QColor interior = composite.pixelColor(interiorSample);
        const qreal ratio = contrastRatio(border, interior);
        qInfo().noquote()
            << QStringLiteral("Focused action contrast over %1 video: %2:1")
                   .arg(videoBackground == QColor(Qt::black)
                            ? QStringLiteral("dark")
                            : QStringLiteral("light"))
                   .arg(ratio, 0, 'f', 2);
        QVERIFY2(ratio >= 3.0,
                 qPrintable(QStringLiteral(
                     "Focused action contrast over %1 video was %2:1 (%3 vs %4)")
                     .arg(videoBackground == QColor(Qt::black)
                              ? QStringLiteral("dark")
                              : QStringLiteral("light"))
                     .arg(ratio, 0, 'f', 2)
                     .arg(border.name(QColor::HexArgb),
                          interior.name(QColor::HexArgb))));
    }
}

void DeckQmlTest::categoryResetDoesNotPositionAnOutOfRangeDelegate()
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
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("input.capture"), state(true));
    descriptors.push_back(descriptor(
        QStringLiteral("input.capture"), QStringLiteral("Input capture"),
        ActionCategory::Input));
    ActionRegistry registry(descriptors, adapter);
    DeckController controller(&registry);
    controller.openFromKeyboard();
    controller.focusActions();

    QQmlEngine engine;
    QQmlComponent component(
        &engine, QUrl(QStringLiteral("qrc:/gui/perigee/ActionTray.qml")));
    std::unique_ptr<QObject> object(component.createWithInitialProperties({
        {QStringLiteral("deckController"), QVariant::fromValue(&controller)},
    }));
    QVERIFY2(object != nullptr, qPrintable(component.errorString()));
    auto* tray = qobject_cast<QQuickItem*>(object.get());
    QVERIFY(tray != nullptr);
    tray->setSize(QSizeF(820, 520));
    QQuickWindow window;
    window.resize(820, 520);
    tray->setParentItem(window.contentItem());
    window.show();
    QCoreApplication::processEvents();

    QQuickItem* actionList = findListViewWithCount(tray, 6);
    QVERIFY(actionList != nullptr);
    controller.focusAction(QStringLiteral("display.5"));
    QCoreApplication::processEvents();
    QCOMPARE(actionList->property("currentIndex").toInt(), 5);

    capturedQtWarnings.clear();
    previousQtMessageHandler = qInstallMessageHandler(captureQtWarnings);
    controller.setSearchText(QStringLiteral("Display action 5"));
    QCoreApplication::processEvents();
    qInstallMessageHandler(previousQtMessageHandler);
    previousQtMessageHandler = nullptr;

    QVERIFY2(std::none_of(
                  capturedQtWarnings.cbegin(), capturedQtWarnings.cend(),
                  [](const QString& warning) {
                      return warning.contains(
                          QStringLiteral("DelegateModel::cancel: index out of range"));
                  }),
              qPrintable(capturedQtWarnings.join(QStringLiteral("\n"))));
    QCOMPARE(actionList->property("count").toInt(), 1);
    QVERIFY(actionList->property("currentIndex").toInt() <
            actionList->property("count").toInt());
}

void DeckQmlTest::actionRowRetainsPointerPressForClick()
{
    QQmlEngine engine;
    QQmlComponent component(
        &engine, QUrl(QStringLiteral("qrc:/gui/perigee/ActionRow.qml")));
    std::unique_ptr<QObject> object(component.createWithInitialProperties({
        {QStringLiteral("actionId"), QStringLiteral("display.select")},
        {QStringLiteral("actionLabel"), QStringLiteral("Choose display")},
        {QStringLiteral("actionCategory"), QStringLiteral("Display")},
        {QStringLiteral("valueText"), QString()},
        {QStringLiteral("actionEnabled"), true},
        {QStringLiteral("disabledReason"), QString()},
        {QStringLiteral("phase"), QStringLiteral("idle")},
        {QStringLiteral("message"), QString()},
        {QStringLiteral("actionFocused"), false},
        {QStringLiteral("requiresConfirmation"), false},
    }));
    QVERIFY2(object != nullptr, qPrintable(component.errorString()));
    auto* root = qobject_cast<QQuickItem*>(object.get());
    QVERIFY(root != nullptr);
    QQuickItem* mouseArea = findMouseArea(root);
    QVERIFY(mouseArea != nullptr);
    QVERIFY(mouseArea->property("preventStealing").toBool());
}

void DeckQmlTest::exposesAccessibleNamesAndDisabledReasons()
{
    DeckQmlHostAdapter adapter;
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("display.select"), state(true));
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("display.disabled"),
        state(false, {}, QStringLiteral("No alternate display is available.")));
    ActionRegistry registry({
        descriptor(QStringLiteral("display.select"),
                   QStringLiteral("Choose display"),
                   ActionCategory::Display),
        descriptor(QStringLiteral("display.disabled"),
                   QStringLiteral("Unavailable display"),
                   ActionCategory::Display),
    }, adapter);
    DeckController controller(&registry);
    controller.openFromController();
    QQmlEngine engine;
    QQmlComponent component(
        &engine, QUrl(QStringLiteral("qrc:/gui/perigee/PerigeeDeck.qml")));
    std::unique_ptr<QObject> object(component.createWithInitialProperties({
        {QStringLiteral("deckController"), QVariant::fromValue(&controller)},
    }));
    QVERIFY2(object != nullptr, qPrintable(component.errorString()));
    auto* root = qobject_cast<QQuickItem*>(object.get());
    QVERIFY(root != nullptr);

    const auto verifyAccessible = [](QQuickItem* item,
                                     const QString& expectedName) {
        QVERIFY(item != nullptr);
        QAccessibleInterface* interface =
            QAccessible::queryAccessibleInterface(item);
        QVERIFY(interface != nullptr);
        QCOMPARE(interface->text(QAccessible::Name), expectedName);
        QVERIFY(!interface->text(QAccessible::Description).trimmed().isEmpty());
    };

    verifyAccessible(root->findChild<QQuickItem*>(
                         QStringLiteral("searchField")),
                     QStringLiteral("Search session controls"));
    verifyAccessible(root->findChild<QQuickItem*>(
                         QStringLiteral("categoryRail")),
                     QStringLiteral("Control categories"));
    verifyAccessible(root->findChild<QQuickItem*>(
                         QStringLiteral("actionTray")),
                     QStringLiteral("Session controls"));

    QQmlComponent rowComponent(
        &engine, QUrl(QStringLiteral("qrc:/gui/perigee/ActionRow.qml")));
    std::unique_ptr<QObject> rowObject(rowComponent.createWithInitialProperties({
        {QStringLiteral("actionId"), QStringLiteral("display.disabled")},
        {QStringLiteral("actionLabel"), QStringLiteral("Unavailable display")},
        {QStringLiteral("actionCategory"), QStringLiteral("Display")},
        {QStringLiteral("valueText"), QString()},
        {QStringLiteral("actionEnabled"), false},
        {QStringLiteral("disabledReason"),
         QStringLiteral("No alternate display is available.")},
        {QStringLiteral("phase"), QStringLiteral("idle")},
        {QStringLiteral("message"), QString()},
        {QStringLiteral("actionFocused"), false},
        {QStringLiteral("requiresConfirmation"), false},
    }));
    QVERIFY2(rowObject != nullptr, qPrintable(rowComponent.errorString()));
    auto* disabledRow = qobject_cast<QQuickItem*>(rowObject.get());
    QVERIFY(disabledRow != nullptr);
    QAccessibleInterface* disabledInterface =
        QAccessible::queryAccessibleInterface(disabledRow);
    QVERIFY(disabledInterface != nullptr);
    QCOMPARE(disabledInterface->text(QAccessible::Name),
             QStringLiteral("Unavailable display"));
    QCOMPARE(disabledInterface->text(QAccessible::Description),
             QStringLiteral("No alternate display is available."));
    QVERIFY(disabledInterface->state().disabled);
}

void DeckQmlTest::confirmationTrapsFocusUntilAcceptedOrCancelled()
{
    QQmlEngine engine;
    QQmlComponent component(
        &engine, QUrl(QStringLiteral("qrc:/gui/perigee/ConfirmationCard.qml")));
    std::unique_ptr<QObject> object(component.createWithInitialProperties({
        {QStringLiteral("actionLabel"), QStringLiteral("Disconnect client")},
        {QStringLiteral("message"),
         QStringLiteral("This disconnects only this client.")},
    }));
    QVERIFY2(object != nullptr, qPrintable(component.errorString()));
    auto* root = qobject_cast<QQuickItem*>(object.get());
    QVERIFY(root != nullptr);
    QQuickItem* cancelButton = root->findChild<QQuickItem*>(
        QStringLiteral("confirmationCancel"));
    QQuickItem* confirmButton = root->findChild<QQuickItem*>(
        QStringLiteral("confirmationConfirm"));
    QVERIFY(cancelButton != nullptr);
    QVERIFY(confirmButton != nullptr);
    QCOMPARE(root->property("focusedChoice").toInt(), 0);
    QVERIFY(QMetaObject::invokeMethod(
        root, "moveChoice", Q_ARG(QVariant, 1)));
    QCOMPARE(root->property("focusedChoice").toInt(), 1);
    QVERIFY(QMetaObject::invokeMethod(
        root, "moveChoice", Q_ARG(QVariant, 1)));
    QCOMPARE(root->property("focusedChoice").toInt(), 0);

    QSignalSpy cancelSpy(root, SIGNAL(cancelRequested()));
    QSignalSpy confirmSpy(root, SIGNAL(confirmRequested()));
    root->setProperty("focusedChoice", 1);
    QVERIFY(QMetaObject::invokeMethod(root, "activateFocused"));
    QCOMPARE(confirmSpy.count(), 1);
    QCOMPARE(cancelSpy.count(), 0);
    root->setProperty("focusedChoice", 0);
    QVERIFY(QMetaObject::invokeMethod(root, "activateFocused"));
    QCOMPARE(confirmSpy.count(), 1);
    QCOMPARE(cancelSpy.count(), 1);
}

void DeckQmlTest::rendersControllerLayoutReferencePngs()
{
    DeckQmlHostAdapter adapter;
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("display.select"),
        state(true, QStringLiteral("Desk monitor")));
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("display.unavailable"),
        state(false, {}, QStringLiteral("No alternate display is available.")));
    ActionRegistry registry({
        descriptor(QStringLiteral("display.select"),
                   QStringLiteral("Choose display"),
                   ActionCategory::Display),
        descriptor(QStringLiteral("display.unavailable"),
                   QStringLiteral("Unavailable display"),
                   ActionCategory::Display),
    }, adapter);
    DeckController controller(&registry);
    const QDir artifactDirectory(
        QDir(QCoreApplication::applicationDirPath()).filePath(
            QStringLiteral("../artifacts")));
    QVERIFY(QDir().mkpath(artifactDirectory.path()));

    struct ReferenceLayout {
        ControllerLayout::Family family;
        const char* name;
    };
    const ReferenceLayout layouts[] {
        {ControllerLayout::Family::Xbox, "xbox"},
        {ControllerLayout::Family::PlayStation, "playstation"},
        {ControllerLayout::Family::Nintendo, "nintendo"},
    };
    controller.setControllerLayout(ControllerLayout::Family::Xbox, false);
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

    QVector<QImage> referenceImages;
    for (const ReferenceLayout& layout : layouts) {
        controller.setControllerLayout(layout.family, false);
        renderer.markDirty();
        QCoreApplication::processEvents();
        QImage warmup;
        QVERIFY2(renderer.render(&warmup, &error), qPrintable(error));
        renderer.markDirty();
        QCoreApplication::processEvents();
        QImage image;
        QVERIFY2(renderer.render(&image, &error), qPrintable(error));
        QCOMPARE(image.size(), QSize(960, 540));
        QCOMPARE(image.format(), QImage::Format_ARGB32_Premultiplied);
        const auto visibleSamples = [&image](const QRect& region) {
            int count = 0;
            for (int y = region.top(); y <= region.bottom(); y += 8) {
                for (int x = region.left(); x <= region.right(); x += 8) {
                    const QColor color = image.pixelColor(x, y);
                    count += color.alpha() > 0 &&
                             color.red() + color.green() + color.blue() > 20
                        ? 1 : 0;
                }
            }
            return count;
        };
        const int searchRailCoverage = visibleSamples(QRect(70, 24, 820, 112));
        const int actionTrayCoverage = visibleSamples(QRect(70, 146, 820, 200));
        QVERIFY2(searchRailCoverage > 800,
                 qPrintable(QStringLiteral("%1 search rail is incomplete (%2 samples)")
                                .arg(QString::fromLatin1(layout.name))
                                .arg(searchRailCoverage)));
        QVERIFY2(actionTrayCoverage > 1200,
                 qPrintable(QStringLiteral("%1 action tray is incomplete (%2 samples)")
                                .arg(QString::fromLatin1(layout.name))
                                .arg(actionTrayCoverage)));
        const QString artifactPath = artifactDirectory.filePath(
            QStringLiteral("perigee-controller-%1.png")
                .arg(QString::fromLatin1(layout.name)));
        QVERIFY2(image.save(artifactPath), qPrintable(artifactPath));
        qInfo().noquote() << "Deck controller artifact:" << artifactPath;
        referenceImages.push_back(std::move(image));
    }

    QCOMPARE(referenceImages.size(), 3);
    QVERIFY(referenceImages.at(0) != referenceImages.at(1));
    QVERIFY(referenceImages.at(0) != referenceImages.at(2));
    QVERIFY(referenceImages.at(1) != referenceImages.at(2));
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

void DeckQmlTest::pointerClickActivatesActionAfterFocus()
{
    DeckQmlHostAdapter adapter;
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("display.first"), state(true));
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("display.second"), state(true));
    ActionRegistry registry({
        descriptor(QStringLiteral("display.first"), QStringLiteral("First"),
                   ActionCategory::Display),
        descriptor(QStringLiteral("display.second"), QStringLiteral("Second"),
                   ActionCategory::Display),
    }, adapter);
    DeckController controller(&registry);
    controller.openFromController();
    QCOMPARE(focusedActionId(controller.actionModel()),
             QStringLiteral("display.first"));

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

    // The second action row begins below the first 62px row and 6px gap.
    const QPointF secondActionCenter(480.0, 257.0);
    QMouseEvent press(QEvent::MouseButtonPress, secondActionCenter,
                      secondActionCenter, secondActionCenter,
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    capturedQtWarnings.clear();
    previousQtMessageHandler = qInstallMessageHandler(captureQtWarnings);
    QVERIFY(renderer.sendPointerEvent(&press));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);

    QCOMPARE(focusedActionId(controller.actionModel()),
             QStringLiteral("display.second"));

    QMouseEvent release(QEvent::MouseButtonRelease, secondActionCenter,
                        secondActionCenter, secondActionCenter,
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QVERIFY(renderer.sendPointerEvent(&release));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    qInstallMessageHandler(previousQtMessageHandler);
    previousQtMessageHandler = nullptr;

    QVERIFY2(std::none_of(
                  capturedQtWarnings.cbegin(), capturedQtWarnings.cend(),
                  [](const QString& warning) {
                      return warning.contains(
                          QStringLiteral("ReferenceError: tray is not defined"));
                  }),
              qPrintable(capturedQtWarnings.join(QStringLiteral("\n"))));
    QCOMPARE(adapter.executedActionIds,
             QStringList({QStringLiteral("display.second")}));
}

void DeckQmlTest::pointerClickOnSearchFrameFocusesSearchField()
{
    DeckQmlHostAdapter adapter;
    adapter.currentSnapshot.actionStates.insert(
        QStringLiteral("display.select"), state(true));
    ActionRegistry registry({
        descriptor(QStringLiteral("display.select"), QStringLiteral("Choose display"),
                   ActionCategory::Display),
    }, adapter);
    DeckController controller(&registry);
    controller.openFromController();

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

    QQuickItem* searchField = renderer.rootObject()->findChild<QQuickItem*>(
        QStringLiteral("searchField"));
    QVERIFY(searchField != nullptr);
    QVERIFY(!searchField->hasActiveFocus());

    // Click the search frame's icon/padding rather than the TextInput text area.
    const QPointF searchFrameIcon(110.0, 60.0);
    QMouseEvent press(QEvent::MouseButtonPress, searchFrameIcon,
                      searchFrameIcon, searchFrameIcon,
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QVERIFY(renderer.sendPointerEvent(&press));
    QMouseEvent release(QEvent::MouseButtonRelease, searchFrameIcon,
                        searchFrameIcon, searchFrameIcon,
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QVERIFY(renderer.sendPointerEvent(&release));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);

    QVERIFY(searchField->hasActiveFocus());
    QCOMPARE(controller.focusRegion(), DeckController::SearchRegion);
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

void DeckQmlTest::realKeyInputEditsSearchTextThroughRenderer()
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

    QVERIFY(renderer.sendTextInput(QStringLiteral("abc")));
    QTRY_COMPARE(controller.searchText(), QStringLiteral("abc"));

    QKeyEvent press(QEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier);
    QVERIFY(renderer.sendKeyEvent(&press));
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Backspace, Qt::NoModifier);
    QVERIFY(renderer.sendKeyEvent(&release));
    QTRY_COMPARE(controller.searchText(), QStringLiteral("ab"));

    QKeyEvent deletePress(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    QVERIFY(renderer.sendKeyEvent(&deletePress));
    QKeyEvent deleteRelease(QEvent::KeyRelease, Qt::Key_Delete, Qt::NoModifier);
    QVERIFY(renderer.sendKeyEvent(&deleteRelease));
    QTRY_COMPARE(controller.searchText(), QStringLiteral("ab"));
}

REGISTER_PERIGEE_TEST(DeckQmlTest);

#include "test_deckqml.moc"
