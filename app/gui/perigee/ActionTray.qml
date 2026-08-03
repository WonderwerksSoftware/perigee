import QtQuick

FocusScope {
    id: tray
    objectName: "actionTray"

    required property var deckController
    signal searchRequested()
    signal categoriesRequested()

    width: 820
    height: Math.max(90, Math.min(4, actionList.count) * 68 + 28) +
            (confirmation.visible ? confirmation.height + 10 : 0) + 36
    activeFocusOnTab: false

    Rectangle {
        anchors.fill: parent
        radius: 18
        color: "#ee111827"
        border.width: tray.activeFocus ? 2 : 1
        border.color: tray.activeFocus ? "#80d8ff" : "#5b6b7f"
    }

    ListView {
        id: actionList
        x: 12
        y: 12
        width: parent.width - 24
        height: Math.max(62, Math.min(4, count) * 68)
        model: deckController.actionModel
        spacing: 6
        clip: true
        interactive: count > 4
        currentIndex: deckController.actionModel.focusedRow

        onCurrentIndexChanged: {
            if (currentIndex >= 0) {
                forceLayout()
                positionViewAtIndex(currentIndex, ListView.Contain)
            }
        }

        delegate: Item {
            width: actionList.width
            height: 62

            ActionRow {
                anchors.fill: parent
                actionId: model.id
                actionLabel: model.label
                actionCategory: model.category
                valueText: model.valueText
                actionEnabled: model.enabled
                disabledReason: model.disabledReason
                phase: model.phase
                message: model.message
                actionFocused: model.focused
                requiresConfirmation: model.requiresConfirmation

                onChosen: {
                    deckController.activateAction(actionId)
                    tray.forceActiveFocus()
                }
                onPointed: {
                    deckController.focusAction(actionId)
                    tray.forceActiveFocus()
                }
            }
        }

        Text {
            anchors.centerIn: parent
            visible: actionList.count === 0
            text: deckController.searchText.length > 0
                  ? "No matching controls"
                  : "No controls are available in this category"
            color: "#91a0b3"
            font.pixelSize: 15
        }
    }

    ConfirmationCard {
        id: confirmation
        x: 12
        anchors.top: actionList.bottom
        anchors.topMargin: 8
        width: parent.width - 24
        visible: deckController.confirmationVisible
        actionLabel: deckController.confirmationActionLabel
        onCancelRequested: {
            deckController.cancelConfirmation()
            tray.forceActiveFocus()
        }
        onConfirmRequested: {
            deckController.acceptConfirmation()
            tray.forceActiveFocus()
        }
    }

    ControllerHint {
        x: 18
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 9
        width: parent.width - 36
        confirming: confirmation.visible
    }

    onActiveFocusChanged: {
        if (activeFocus)
            deckController.focusActions()
    }

    Keys.onPressed: event => {
        if (deckController.confirmationVisible) {
            if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                deckController.acceptConfirmation()
                event.accepted = true
            } else if (event.key === Qt.Key_Escape || event.key === Qt.Key_Backspace) {
                deckController.cancelConfirmation()
                event.accepted = true
            }
            return
        }
        if (event.key === Qt.Key_Down) {
            deckController.moveActionFocus(1)
            event.accepted = true
        } else if (event.key === Qt.Key_Up) {
            deckController.moveActionFocus(-1)
            event.accepted = true
        } else if (event.key === Qt.Key_Left) {
            deckController.previousCategory()
            event.accepted = true
        } else if (event.key === Qt.Key_Right) {
            deckController.nextCategory()
            event.accepted = true
        } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter ||
                   event.key === Qt.Key_Space) {
            deckController.activateFocusedAction()
            event.accepted = true
        } else if (event.key === Qt.Key_Tab) {
            tray.categoriesRequested()
            event.accepted = true
        } else if (event.key === Qt.Key_Escape || event.key === Qt.Key_Backspace) {
            deckController.back()
            event.accepted = true
        }
    }
}
