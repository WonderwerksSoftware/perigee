import QtQuick

FocusScope {
    id: card

    required property string actionLabel
    required property string message
    signal cancelRequested()
    signal confirmRequested()
    property int focusedChoice: 0

    height: 70
    activeFocusOnTab: false
    Accessible.role: Accessible.Pane
    Accessible.name: "Confirm " + actionLabel
    Accessible.description: message.length > 0
                            ? message
                            : "This action can interrupt the current session."

    function focusChoice(index) {
        focusedChoice = ((index % 2) + 2) % 2
        if (!visible)
            return
        if (focusedChoice === 0)
            cancelButton.forceActiveFocus()
        else
            confirmButton.forceActiveFocus()
    }

    function moveChoice(delta) {
        focusChoice(focusedChoice + delta)
    }

    function activateFocused() {
        if (focusedChoice === 0)
            cancelRequested()
        else
            confirmRequested()
    }

    Rectangle {
        anchors.fill: parent
        radius: 12
        color: "#38242b"
        border.width: 1
        border.color: "#e5949f"
    }

    Text {
        x: 16
        y: 12
        width: parent.width - 240
        text: "Confirm “" + card.actionLabel + "”?"
        color: "#fff4f5"
        font.pixelSize: 16
        font.weight: Font.DemiBold
        elide: Text.ElideRight
    }

    Text {
        x: 16
        y: 39
        text: card.message.length > 0
              ? card.message
              : "This action can interrupt the current session."
        color: "#d9b7bc"
        font.pixelSize: 12
    }

    Rectangle {
        id: cancelButton
        objectName: "confirmationCancel"
        anchors.right: confirmButton.left
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        width: 76
        height: 36
        radius: 10
        color: activeFocus ? "#355274" : "#27364a"
        border.width: activeFocus ? 2 : 1
        border.color: activeFocus ? "#b9edff" : "#60738b"
        focus: card.visible && card.focusedChoice === 0
        activeFocusOnTab: false
        Accessible.role: Accessible.Button
        Accessible.name: "Cancel " + card.actionLabel
        Accessible.description: "Keep the current session unchanged"
        Accessible.onPressAction: card.cancelRequested()

        Text {
            anchors.centerIn: parent
            text: "Cancel"
            color: "#e2e8f0"
            font.pixelSize: 13
        }

        MouseArea {
            anchors.fill: parent
            onPressed: card.focusChoice(0)
            onClicked: card.cancelRequested()
        }
    }

    Rectangle {
        id: confirmButton
        objectName: "confirmationConfirm"
        anchors.right: parent.right
        anchors.rightMargin: 14
        anchors.verticalCenter: parent.verticalCenter
        width: 120
        height: 36
        radius: 10
        color: activeFocus ? "#c15368" : "#a44555"
        border.width: activeFocus ? 2 : 1
        border.color: activeFocus ? "#fff0f3" : "#dc8c99"
        focus: card.visible && card.focusedChoice === 1
        activeFocusOnTab: false
        Accessible.role: Accessible.Button
        Accessible.name: "Confirm " + card.actionLabel
        Accessible.description: card.message.length > 0
                                ? card.message
                                : "Perform this session action"
        Accessible.onPressAction: card.confirmRequested()

        Text {
            anchors.centerIn: parent
            text: "Confirm"
            color: "#ffffff"
            font.pixelSize: 13
            font.weight: Font.DemiBold
        }

        MouseArea {
            anchors.fill: parent
            onPressed: card.focusChoice(1)
            onClicked: card.confirmRequested()
        }
    }

    Keys.onPressed: event => {
        if (event.key === Qt.Key_Left || event.key === Qt.Key_Backtab) {
            card.moveChoice(-1)
            event.accepted = true
        } else if (event.key === Qt.Key_Right || event.key === Qt.Key_Tab) {
            card.moveChoice(1)
            event.accepted = true
        } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter ||
                   event.key === Qt.Key_Space) {
            card.activateFocused()
            event.accepted = true
        } else if (event.key === Qt.Key_Escape || event.key === Qt.Key_Backspace) {
            card.cancelRequested()
            event.accepted = true
        }
    }

    onVisibleChanged: {
        if (visible)
            focusChoice(0)
    }

    Component.onCompleted: {
        if (visible)
            focusChoice(0)
    }
}
