import QtQuick

Item {
    id: glyph

    required property url source
    required property string label
    property bool shoulder: false
    readonly property string visualLabel:
        label === "Cross" ? "×" :
        label === "Circle" ? "○" :
        label === "Square" ? "□" :
        label === "Triangle" ? "△" : label

    width: shoulder ? 38 : 24
    height: 24
    Accessible.ignored: true

    Image {
        anchors.fill: parent
        source: glyph.source
        fillMode: Image.PreserveAspectFit
        smooth: true
    }

    Text {
        anchors.centerIn: parent
        anchors.verticalCenterOffset: glyph.shoulder ? 0 : -0.5
        text: glyph.visualLabel
        color: "#f7fbff"
        font.pixelSize: glyph.shoulder ? 9 :
                        glyph.visualLabel.length > 2 ? 7 : 12
        font.weight: Font.Bold
    }
}
