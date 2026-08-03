import QtQuick

Item {
    required property bool confirming

    height: 20

    Text {
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        text: confirming ? "Enter / A  Confirm     Esc / B  Cancel"
                         : "↑↓ Navigate     Enter / A  Select     Esc / B  Back"
        color: "#8090a4"
        font.pixelSize: 11
    }

    Text {
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        visible: !confirming
        text: "← → / LB RB  Categories"
        color: "#8090a4"
        font.pixelSize: 11
    }
}
