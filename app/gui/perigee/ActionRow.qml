import QtQuick

Rectangle {
    id: row

    required property string actionId
    required property string actionLabel
    required property string actionCategory
    required property string valueText
    required property bool actionEnabled
    required property string disabledReason
    required property string phase
    required property string message
    required property bool actionFocused
    required property bool requiresConfirmation

    signal chosen()
    signal pointed()

    height: 62
    radius: 12
    color: actionFocused ? "#253f57" : "#182536"
    border.width: actionFocused ? 2 : 1
    border.color: actionFocused ? "#80d8ff" : "#34455a"
    opacity: actionEnabled || phase === "working" ? 1.0 : 0.76

    Rectangle {
        id: statusMark
        x: 12
        anchors.verticalCenter: parent.verticalCenter
        width: 5
        height: 36
        radius: 2
        color: phase === "failed" ? "#ff7c8c"
             : phase === "working" ? "#ffd166"
             : actionFocused ? "#80d8ff" : "#52677e"
    }

    Text {
        id: labelText
        x: 30
        y: 9
        width: parent.width * 0.56
        elide: Text.ElideRight
        text: actionLabel
        color: actionEnabled ? "#f5f9ff" : "#b3bfcc"
        font.pixelSize: 17
        font.weight: actionFocused ? Font.DemiBold : Font.Medium
    }

    Text {
        x: 30
        y: 35
        width: parent.width - 220
        elide: Text.ElideRight
        text: phase === "working" ? (message.length > 0 ? message : "Working…")
              : phase === "failed" ? message
              : !actionEnabled ? disabledReason
              : requiresConfirmation ? actionCategory + "  •  confirmation required"
              : actionCategory
        color: phase === "failed" ? "#ff9eaa"
             : phase === "working" ? "#ffd166"
             : !actionEnabled ? "#d2a7ad" : "#91a6ba"
        font.pixelSize: 12
    }

    Rectangle {
        visible: valueText.length > 0 || phase === "working"
        anchors.right: parent.right
        anchors.rightMargin: 14
        anchors.verticalCenter: parent.verticalCenter
        width: Math.min(220, valueLabel.implicitWidth + 24)
        height: 30
        radius: 9
        color: phase === "working" ? "#4a3d1e" : "#203149"

        Text {
            id: valueLabel
            anchors.centerIn: parent
            text: phase === "working" ? "WORKING" : valueText
            color: phase === "working" ? "#ffe39a" : "#c8e9f9"
            font.pixelSize: 12
            font.weight: Font.DemiBold
        }
    }

    MouseArea {
        anchors.fill: parent
        enabled: actionEnabled
        hoverEnabled: true
        onEntered: row.pointed()
        onClicked: row.chosen()
    }
}
