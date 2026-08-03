import QtQuick

Item {
    id: hint

    required property var controllerLayout
    required property bool confirming

    height: 24

    Row {
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        spacing: 7

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: hint.confirming ? "Enter" : "↑↓ Navigate    Enter"
            color: "#8fa0b5"
            font.pixelSize: 11
        }

        ControllerGlyph {
            objectName: "controllerConfirmGlyph"
            source: hint.controllerLayout.confirmGlyph
            label: hint.controllerLayout.confirmLabel
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: hint.confirming ? "Confirm    Esc" : "Select    Esc"
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
        visible: !hint.confirming

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: "← →"
            color: "#8fa0b5"
            font.pixelSize: 11
        }

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
