import QtQuick

Item {
    id: hint

    required property var controllerLayout
    required property bool confirming
    required property bool controllerConnected

    height: 24

    Row {
        objectName: "keyboardNavigationHint"
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        spacing: 7
        visible: !hint.controllerConnected

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: hint.confirming
                  ? "Enter  Confirm    Esc  Cancel"
                  : "↑↓  Navigate    Enter  Select    Esc  Back"
            color: "#8fa0b5"
            font.pixelSize: 11
        }
    }

    Row {
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        spacing: 7
        visible: hint.controllerConnected

        ControllerGlyph {
            objectName: "controllerConfirmGlyph"
            source: hint.controllerLayout.confirmGlyph
            label: hint.controllerLayout.confirmLabel
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: hint.confirming ? "Confirm" : "Select"
            color: "#8fa0b5"
            font.pixelSize: 11
        }

        ControllerGlyph {
            objectName: "controllerBackGlyph"
            source: hint.controllerLayout.backGlyph
            label: hint.controllerLayout.backLabel
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: hint.confirming ? "Cancel" : "Back"
            color: "#8fa0b5"
            font.pixelSize: 11
        }
    }

    Row {
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        spacing: 5
        visible: !hint.confirming && !hint.controllerConnected

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: "← →  Categories"
            color: "#8fa0b5"
            font.pixelSize: 11
        }
    }

    Row {
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        spacing: 5
        visible: !hint.confirming && hint.controllerConnected

        ControllerGlyph {
            source: hint.controllerLayout.previousCategoryGlyph
            label: hint.controllerLayout.previousCategoryLabel
            shoulder: true
        }

        ControllerGlyph {
            source: hint.controllerLayout.nextCategoryGlyph
            label: hint.controllerLayout.nextCategoryLabel
            shoulder: true
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: "Categories"
            color: "#8fa0b5"
            font.pixelSize: 11
        }
    }
}
