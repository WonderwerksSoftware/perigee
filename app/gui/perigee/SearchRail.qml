import QtQuick

FocusScope {
    id: rail

    required property var deckController
    signal actionsRequested()

    width: 820
    height: 112

    function focusSearch() {
        deckController.focusSearch()
        searchField.forceActiveFocus()
    }

    function focusCategories() {
        deckController.focusCategories()
        categoryRail.forceActiveFocus()
    }

    Rectangle {
        anchors.fill: parent
        radius: 18
        color: "#ee111827"
        border.width: 1
        border.color: "#5b6b7f"
    }

    Rectangle {
        id: searchFrame
        x: 16
        y: 14
        width: parent.width - 32
        height: 44
        radius: 12
        color: searchField.activeFocus ? "#23334a" : "#192537"
        border.width: searchField.activeFocus ? 2 : 1
        border.color: searchField.activeFocus ? "#80d8ff" : "#46566c"

        Text {
            x: 15
            anchors.verticalCenter: parent.verticalCenter
            text: "⌕"
            color: "#80d8ff"
            font.pixelSize: 24
        }

        TextInput {
            id: searchField
            objectName: "searchField"
            x: 52
            width: parent.width - 68
            anchors.verticalCenter: parent.verticalCenter
            color: "#f7fbff"
            selectionColor: "#357aa5"
            selectedTextColor: "#ffffff"
            font.pixelSize: 18
            text: deckController.searchText
            activeFocusOnTab: false
            clip: true
            Accessible.role: Accessible.EditableText
            Accessible.name: "Search session controls"
            Accessible.description: "Filter controls by name, alias, or category"

            Text {
                anchors.fill: parent
                verticalAlignment: Text.AlignVCenter
                visible: searchField.text.length === 0
                text: "Search session controls"
                color: "#8e9db1"
                font: searchField.font
            }

            onTextChanged: {
                if (deckController.searchText !== text)
                    deckController.setSearchText(text)
            }

            onActiveFocusChanged: {
                if (activeFocus)
                    deckController.focusSearch()
            }

            Keys.onPressed: event => {
                if (event.key === Qt.Key_Tab) {
                    rail.focusCategories()
                    event.accepted = true
                } else if (event.key === Qt.Key_Down) {
                    rail.actionsRequested()
                    event.accepted = true
                } else if (event.key === Qt.Key_Escape) {
                    deckController.back()
                    event.accepted = true
                }
            }
        }
    }

    FocusScope {
        id: categoryRail
        objectName: "categoryRail"
        x: 16
        y: 66
        width: parent.width - 32
        height: 32
        activeFocusOnTab: false
        Accessible.role: Accessible.PageTabList
        Accessible.name: "Control categories"
        Accessible.description: "Choose a category of session controls"

        onActiveFocusChanged: {
            if (activeFocus)
                deckController.focusCategories()
        }

        Row {
            anchors.centerIn: parent
            spacing: 5

            Repeater {
                model: deckController.categories

                Rectangle {
                    required property int index
                    required property string modelData

                    width: 120
                    height: 30
                    radius: 9
                    color: deckController.activeCategory === index
                           ? (categoryRail.activeFocus ? "#28749b" : "#215b78")
                           : "transparent"
                    border.width: categoryRail.activeFocus &&
                                  deckController.activeCategory === index ? 1 : 0
                    border.color: "#a7e8ff"
                    Accessible.role: Accessible.PageTab
                    Accessible.name: modelData
                    Accessible.description: modelData + " session controls"
                    Accessible.selected: deckController.activeCategory === index
                    Accessible.onPressAction: {
                        deckController.selectCategory(index)
                        categoryRail.forceActiveFocus()
                    }

                    Text {
                        anchors.centerIn: parent
                        text: modelData
                        color: deckController.activeCategory === index
                               ? "#f5fbff" : "#aebbc9"
                        font.pixelSize: 14
                        font.weight: deckController.activeCategory === index
                                     ? Font.DemiBold : Font.Normal
                    }

                    MouseArea {
                        anchors.fill: parent
                        preventStealing: true
                        onPressed: categoryRail.forceActiveFocus()
                        onClicked: {
                            deckController.selectCategory(index)
                            categoryRail.forceActiveFocus()
                        }
                    }
                }
            }
        }

        Keys.onPressed: event => {
            if (event.key === Qt.Key_Left) {
                deckController.previousCategory()
                categoryRail.forceActiveFocus()
                event.accepted = true
            } else if (event.key === Qt.Key_Right) {
                deckController.nextCategory()
                categoryRail.forceActiveFocus()
                event.accepted = true
            } else if (event.key === Qt.Key_Down || event.key === Qt.Key_Tab) {
                rail.actionsRequested()
                event.accepted = true
            } else if (event.key === Qt.Key_Up || event.key === Qt.Key_Backtab) {
                rail.focusSearch()
                event.accepted = true
            } else if (event.key === Qt.Key_Escape) {
                deckController.back()
                event.accepted = true
            }
        }
    }
}
