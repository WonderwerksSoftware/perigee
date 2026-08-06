import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.2

GroupBox {
    id: root
    objectName: "perigeeDeckSettings"
    title: "<font color=\"skyblue\">" + qsTr("Perigee Deck") + "</font>"
    font.pointSize: 12

    property var preferences
    property var gamepadNavigation
    property string captureMode: ""
    property string conflictMessage: ""

    function isModifierKey(key) {
        return key === Qt.Key_Shift || key === Qt.Key_Control ||
               key === Qt.Key_Alt || key === Qt.Key_Meta
    }

    function beginKeyboardCapture() {
        cancelCapture()
        captureMode = "keyboard"
        conflictMessage = ""
        forceActiveFocus(Qt.ShortcutFocusReason)
    }

    function beginControllerCapture() {
        cancelCapture()
        captureMode = "controller"
        conflictMessage = ""
        if (gamepadNavigation) {
            gamepadNavigation.beginControllerBindingCapture()
        }
    }

    function cancelCapture() {
        if (gamepadNavigation) {
            gamepadNavigation.cancelControllerBindingCapture()
        }
        captureMode = ""
        conflictMessage = ""
    }

    function applyControllerCapture(buttons) {
        if (captureMode !== "controller" || !preferences) {
            return
        }
        var reason = preferences.deckControllerConflictReason(buttons)
        if (reason.length !== 0 || !preferences.setDeckControllerBinding(buttons)) {
            conflictMessage = reason.length !== 0 ? reason :
                qsTr("Choose at least two supported controller buttons.")
            if (gamepadNavigation) {
                gamepadNavigation.beginControllerBindingCapture()
            }
            return
        }
        captureMode = ""
        conflictMessage = ""
        controllerCaptureButton.forceActiveFocus(Qt.TabFocusReason)
    }

    function handleKeyboardCapture(modifiers, logicalKey, nativeScanCode,
                                   autoRepeat) {
        if (captureMode !== "keyboard" || autoRepeat ||
                isModifierKey(logicalKey)) {
            return
        }
        var shortcutModifiers = modifiers &
            (Qt.ShiftModifier | Qt.ControlModifier |
             Qt.AltModifier | Qt.MetaModifier)
        if (shortcutModifiers === Qt.NoModifier) {
            conflictMessage = qsTr("Hold one or more modifiers, then press a physical key.")
            return
        }
        if (!preferences ||
                !preferences.setDeckKeyboardBindingFromNative(shortcutModifiers,
                                                               nativeScanCode)) {
            conflictMessage = qsTr("Perigee cannot map this physical key on the current platform. The existing shortcut was not changed.")
            return
        }
        captureMode = ""
        conflictMessage = ""
        keyboardCaptureButton.forceActiveFocus(Qt.TabFocusReason)
    }

    Keys.onPressed: function(event) {
        if (captureMode.length === 0) {
            return
        }
        event.accepted = true
        if (event.key === Qt.Key_Escape) {
            cancelCapture()
            return
        }
        handleKeyboardCapture(event.modifiers, event.key,
                              event.nativeScanCode, event.isAutoRepeat)
    }

    onVisibleChanged: {
        if (!visible) {
            cancelCapture()
        }
    }

    onEnabledChanged: {
        if (!enabled) {
            cancelCapture()
        }
    }

    Component.onDestruction: {
        if (gamepadNavigation) {
            gamepadNavigation.cancelControllerBindingCapture()
        }
    }

    Connections {
        target: root.gamepadNavigation

        function onControllerBindingCaptured(buttons) {
            root.applyControllerCapture(buttons)
        }

        function onControllerBindingCaptureCancelled() {
            root.cancelCapture()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 5

        Label {
            Layout.fillWidth: true
            text: qsTr("Open the in-stream menu with one keyboard shortcut or controller chord.")
            wrapMode: Text.Wrap
            font.pointSize: 9
        }

        RowLayout {
            Layout.fillWidth: true

            Label {
                Layout.fillWidth: true
                text: qsTr("Keyboard: %1").arg(root.preferences ?
                    root.preferences.formatDeckKeyboardBinding(
                        root.preferences.deckKeyModifiers,
                        root.preferences.deckKeyScancode) : "")
                wrapMode: Text.Wrap
            }

            Button {
                id: keyboardCaptureButton
                objectName: "deckKeyboardCaptureButton"
                activeFocusOnTab: true
                text: root.captureMode === "keyboard" ?
                    qsTr("Press shortcut...") : qsTr("Capture")
                Accessible.name: qsTr("Capture keyboard shortcut")
                Accessible.description: qsTr("Record the keyboard shortcut that opens Perigee Deck")
                onClicked: root.beginKeyboardCapture()
            }
        }

        RowLayout {
            Layout.fillWidth: true

            Label {
                Layout.fillWidth: true
                text: qsTr("Controller: %1").arg(root.preferences ?
                    root.preferences.formatDeckControllerBinding(
                        root.preferences.deckControllerButtons) : "")
                wrapMode: Text.Wrap
            }

            Button {
                id: controllerCaptureButton
                objectName: "deckControllerCaptureButton"
                activeFocusOnTab: true
                text: root.captureMode === "controller" ?
                    qsTr("Press chord (B cancels)...") : qsTr("Capture")
                Accessible.name: qsTr("Capture controller chord")
                Accessible.description: qsTr("Record the controller chord that opens Perigee Deck")
                onClicked: root.beginControllerCapture()
            }
        }

        Label {
            id: conflictLabel
            objectName: "deckBindingConflict"
            Layout.fillWidth: true
            visible: root.conflictMessage.length !== 0
            text: root.conflictMessage
            color: "#ffb4a8"
            wrapMode: Text.Wrap
        }

        RowLayout {
            Layout.fillWidth: true

            Button {
                objectName: "deckCaptureCancelButton"
                activeFocusOnTab: true
                visible: root.captureMode.length !== 0
                text: qsTr("Cancel capture")
                Accessible.name: qsTr("Cancel binding capture")
                Accessible.description: qsTr("Stop recording a new Perigee Deck binding")
                onClicked: root.cancelCapture()
            }

            Button {
                objectName: "deckBindingsResetButton"
                activeFocusOnTab: true
                text: qsTr("Reset to defaults")
                Accessible.name: qsTr("Reset Perigee Deck bindings")
                Accessible.description: qsTr("Restore the default keyboard shortcut and controller chord")
                onClicked: {
                    root.cancelCapture()
                    if (root.preferences) {
                        root.preferences.resetDeckBindings()
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true

            Label {
                Layout.fillWidth: true
                text: qsTr("Physical displays in Deck")
            }

            SpinBox {
                objectName: "deckPhysicalDisplayCount"
                activeFocusOnTab: true
                from: 1
                to: 13
                value: root.preferences ?
                    root.preferences.deckPhysicalDisplayCount : 3
                editable: true
                Accessible.name: qsTr("Physical displays in Perigee Deck")
                Accessible.description: qsTr("Set the number of physical host displays shown in Perigee Deck")
                onValueModified: {
                    if (root.preferences) {
                        root.preferences.setDeckPhysicalDisplayCount(value)
                    }
                }
            }
        }

        CheckBox {
            objectName: "legacyDisconnectCheck"
            Layout.fillWidth: true
            activeFocusOnTab: true
            text: qsTr("Legacy direct disconnect")
            Accessible.name: qsTr("Legacy direct disconnect")
            Accessible.description: qsTr("Use the legacy controller chord to disconnect immediately")
            checked: root.preferences ?
                root.preferences.legacyGamepadDisconnect : false
            onToggled: {
                if (root.preferences) {
                    root.preferences.legacyGamepadDisconnect = checked
                }
            }

            ToolTip.delay: 1000
            ToolTip.timeout: 10000
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Use LB+RB+Back+Start to disconnect immediately. The keyboard shortcut still opens Perigee Deck.")
        }
    }
}
