import QtQuick

Rectangle {
    id: card

    required property string actionLabel
    signal cancelRequested()
    signal confirmRequested()

    height: 70
    radius: 12
    color: "#38242b"
    border.width: 1
    border.color: "#e5949f"

    Text {
        x: 16
        y: 12
        width: parent.width - 240
        text: "Confirm “" + actionLabel + "”?"
        color: "#fff4f5"
        font.pixelSize: 16
        font.weight: Font.DemiBold
        elide: Text.ElideRight
    }

    Text {
        x: 16
        y: 39
        text: "This action can interrupt the current session."
        color: "#d9b7bc"
        font.pixelSize: 12
    }

    Rectangle {
        anchors.right: confirmButton.left
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        width: 76
        height: 36
        radius: 10
        color: "#27364a"

        Text {
            anchors.centerIn: parent
            text: "Cancel"
            color: "#e2e8f0"
            font.pixelSize: 13
        }

        MouseArea {
            anchors.fill: parent
            onClicked: card.cancelRequested()
        }
    }

    Rectangle {
        id: confirmButton
        anchors.right: parent.right
        anchors.rightMargin: 14
        anchors.verticalCenter: parent.verticalCenter
        width: 120
        height: 36
        radius: 10
        color: "#a44555"

        Text {
            anchors.centerIn: parent
            text: "Confirm"
            color: "#ffffff"
            font.pixelSize: 13
            font.weight: Font.DemiBold
        }

        MouseArea {
            anchors.fill: parent
            onClicked: card.confirmRequested()
        }
    }
}
