import QtQuick

FocusScope {
    id: deckRoot

    required property var deckController

    width: parent ? parent.width : 960
    height: parent ? parent.height : 540
    visible: deckController && deckController.isOpen
    focus: visible

    function focusSearch() {
        searchRail.focusSearch()
    }

    function focusCategories() {
        searchRail.focusCategories()
    }

    function focusActions() {
        deckController.focusActions()
        actionTray.forceActiveFocus()
    }

    function syncControllerFocus() {
        if (!visible)
            return
        if (deckController.searchFocused)
            focusSearch()
        else
            focusActions()
    }

    SearchRail {
        id: searchRail
        anchors.top: parent.top
        anchors.topMargin: 24
        anchors.horizontalCenter: parent.horizontalCenter
        deckController: deckRoot.deckController
        onActionsRequested: deckRoot.focusActions()
    }

    ActionTray {
        id: actionTray
        anchors.top: searchRail.bottom
        anchors.topMargin: 10
        anchors.horizontalCenter: parent.horizontalCenter
        deckController: deckRoot.deckController
        onSearchRequested: deckRoot.focusSearch()
        onCategoriesRequested: deckRoot.focusCategories()
    }

    Connections {
        target: deckController

        function onOpenChanged() {
            deckRoot.syncControllerFocus()
        }

        function onFocusModeChanged() {
            deckRoot.syncControllerFocus()
        }
    }

    onVisibleChanged: syncControllerFocus()

    Component.onCompleted: syncControllerFocus()
}
