import QtQuick 2.0
import QtQuick.Controls 2.2
import QtQuick.Window 2.2

import SdlGamepadKeyNavigation 1.0
import Session 1.0
import SystemProperties 1.0

Item {
    property Session session
    property string appName
    property string stageText : isResume ? qsTr("Resuming %1...").arg(appName) :
                                           qsTr("Starting %1...").arg(appName)
    property bool isResume : false
    property bool quitAfter : false
    property bool recoveryCarrier: false
    property bool recoveryActionsReady: recoveryCarrier && session !== null
    property bool sessionCleanupComplete: false

    onRecoveryActionsReadyChanged: {
        if (recoveryActionsReady && displayRecoverySurface.visible) {
            retryButton.forceActiveFocus()
        }
    }

    function stageStarting(stage)
    {
        // Update the spinner text
        stageText = qsTr("Starting %1...").arg(stage)
    }

    function stageFailed(stage, errorCode, failingPorts)
    {
        // Display the error dialog after Session::exec() returns
        streamSegueErrorDialog.text = qsTr("Starting %1 failed: Error %2").arg(stage).arg(errorCode)

        if (failingPorts) {
            streamSegueErrorDialog.text += "\n\n" + qsTr("Check your firewall and port forwarding rules for port(s): %1").arg(failingPorts)
        }
    }

    function connectionStarted()
    {
        // Hide the UI contents so the user doesn't
        // see them briefly when we pop off the StackView
        stageSpinner.visible = false
        stageLabel.visible = false
        hintText.visible = false

        // Hide the window now that streaming has begun
        window.visible = false
    }

    function displayLaunchError(text)
    {
        // Display the error dialog after Session::exec() returns
        streamSegueErrorDialog.text = text
        console.error(text)
    }

    function quitStarting()
    {
        // Avoid the push transition animation
        var component = Qt.createComponent("QuitSegue.qml")
        stackView.replace(stackView.currentItem, component.createObject(stackView, {"appName": appName}), StackView.Immediate)

        // Show the Qt window again to show quit segue
        window.visible = true
    }

    function sessionFinished(portTestResult)
    {
        if (portTestResult !== 0 && portTestResult !== -1 && streamSegueErrorDialog.text) {
            streamSegueErrorDialog.text += "\n\n" + qsTr("This PC's Internet connection is blocking Perigee. Streaming over the Internet might not work on this network.")
        }

        // Re-enable GUI gamepad usage now
        SdlGamepadKeyNavigation.enable()

        // Display transitions own their launcher lifetime. The old Session is
        // discarded only after deferred native cleanup has released its slot.
        if (session && session.displayTransitionHandoff) {
            stageText = DisplayTransitionCoordinator.statusText
            stageSpinner.visible = !DisplayTransitionCoordinator.recoveryVisible
            stageLabel.visible = !DisplayTransitionCoordinator.recoveryVisible
            hintText.visible = false
            window.visible = true
            return
        }

        // Pop the StreamSegue off the stack if this is a GUI-based app launch
        if (!quitAfter) {
            stackView.pop()
        }

        if (quitAfter && !streamSegueErrorDialog.text) {
            // If this was a CLI launch without errors, exit now
            Qt.quit()
        }
        else {
            // Show the Qt window again after streaming
            window.visible = true

            // Display any launch errors. We do this after
            // the Qt UI is visible again to prevent losing
            // focus on the dialog which would impact gamepad
            // users.
            if (streamSegueErrorDialog.text) {
                streamSegueErrorDialog.quitAfter = quitAfter
                streamSegueErrorDialog.open()
            }
        }
    }

    function sessionReadyForDeletion()
    {
        sessionCleanupComplete = true
        if (DisplayTransitionCoordinator.replacementPending) {
            replaceCleanedSession(true)
        }
        else if (DisplayTransitionCoordinator.recoveryVisible) {
            // Keep no retiring Session alive. A fresh, initialized but dormant
            // Session carries only the authenticated recovery control path.
            replaceCleanedSession(false)
        }
        else {
            // Garbage collect the Session object since it's pretty heavyweight
            // and keeps other libraries (like SDL_TTF) around until it is deleted.
            session = null
            gc()
        }
    }

    function hookSessionSignals()
    {
        if (!session) {
            return
        }
        session.stageStarting.connect(stageStarting)
        session.stageFailed.connect(stageFailed)
        session.connectionStarted.connect(connectionStarted)
        session.displayLaunchError.connect(displayLaunchError)
        session.quitStarting.connect(quitStarting)
        session.sessionFinished.connect(sessionFinished)
        session.readyForDeletion.connect(sessionReadyForDeletion)
    }

    function scheduleSessionStart()
    {
        // Don't wait unless we have toasts to display
        startSessionTimer.interval = 0

        // Display the toasts together in a vertical centered arrangement
        var yOffset = 0
        for (var i = 0; i < session.launchWarnings.length; i++) {
            var text = session.launchWarnings[i]
            console.warn(text)

            // Show the tooltip for 3 seconds
            var toast = Qt.createQmlObject('import QtQuick.Controls 2.2; ToolTip {}', parent, '')
            toast.timeout = 3000
            toast.text = text
            toast.y += yOffset
            toast.visible = true

            // Offset the next toast below the previous one
            yOffset = toast.y + toast.padding + toast.height

            // Allow an extra 500 ms for the tooltip's fade-out animation to finish
            startSessionTimer.interval = toast.timeout + 500
        }
        startSessionTimer.start()
    }

    function initializeActiveSession(startConnection)
    {
        if (!session.initialize(window)) {
            if (DisplayTransitionCoordinator.displaySelectionBusy ||
                    DisplayTransitionCoordinator.replacementPending) {
                recoveryCarrier = true
                session.displayTransitionInitializationFailed()
                SdlGamepadKeyNavigation.enable()
                window.visible = true
                if (DisplayTransitionCoordinator.replacementPending) {
                    Qt.callLater(replaceCleanedSession, true)
                }
            }
            else {
                sessionFinished(0)
                sessionReadyForDeletion()
            }
            return false
        }

        if (startConnection) {
            recoveryCarrier = false
            SdlGamepadKeyNavigation.disable()
            scheduleSessionStart()
        }
        else {
            recoveryCarrier = true
            SdlGamepadKeyNavigation.enable()
            recoveryControlPump.start()
            window.visible = true
        }
        return true
    }

    function replaceCleanedSession(startConnection)
    {
        var retiredSession = session
        if (!retiredSession) {
            return false
        }
        var replacement = retiredSession.createDisplayTransitionReplacement()
        if (recoveryCarrier) {
            retiredSession.disposeDormantDisplayTransitionCarrier()
        }

        // Never carry the retiring native/QML object across cleanup.
        session = null
        gc()
        if (!replacement) {
            recoveryCarrier = false
            return false
        }

        session = replacement
        sessionCleanupComplete = false
        hookSessionSignals()
        return initializeActiveSession(startConnection)
    }

    function startRecoveryCarrier()
    {
        if (!recoveryCarrier || !session) {
            return
        }
        recoveryCarrier = false
        recoveryControlPump.stop()
        SdlGamepadKeyNavigation.disable()
        scheduleSessionStart()
    }

    function finishTransitionWithoutSession()
    {
        if (recoveryCarrier && session) {
            session.disposeDormantDisplayTransitionCarrier()
        }
        recoveryCarrier = false
        recoveryControlPump.stop()
        session = null
        gc()
        SdlGamepadKeyNavigation.enable()
        window.visible = true
        if (quitAfter) {
            Qt.quit()
        }
        else {
            stackView.pop()
        }
    }

    StackView.onDeactivating: {
        if (recoveryCarrier && session) {
            session.disposeDormantDisplayTransitionCarrier()
            recoveryCarrier = false
            session = null
            gc()
        }

        // Show the toolbar again when popped off the stack
        toolBar.visible = true

        // Re-enable GUI gamepad usage now
        SdlGamepadKeyNavigation.enable()
    }

    Component.onDestruction: {
        if (recoveryCarrier && session) {
            session.disposeDormantDisplayTransitionCarrier()
        }
    }

    StackView.onActivated: {
        // Hide the toolbar before we start loading
        toolBar.visible = false

        // Hook up our signals
        hookSessionSignals()

        // Ensure the SystemProperties async thread is finished,
        // since it may currently be using the SDL video subsystem
        SystemProperties.waitForAsyncLoad()

        // Kick off the stream
        spinnerTimer.start()
        streamLoader.active = true
    }

    Timer {
        id: spinnerTimer

        // Display the spinner appearance a bit to allow us to reach
        // the code in Session.exec() that pumps the event loop.
        // If we display it immediately, it will briefly hang in the
        // middle of the animation on Windows, which looks very
        // obviously broken.
        interval: 100
        onTriggered: stageSpinner.visible = true
    }

    Timer {
        id: startSessionTimer
        onTriggered: {
            // Garbage collect QML stuff before we start streaming,
            // since we'll probably be streaming for a while and we
            // won't be able to GC during the stream.
            gc()

            // Run the streaming session to completion
            session.start()
        }
    }

    Timer {
        id: recoveryControlPump
        interval: 25
        repeat: true
        onTriggered: {
            if (recoveryCarrier && session) {
                session.pumpDisplayTransitionControl()
            }
        }
    }

    Connections {
        target: DisplayTransitionCoordinator

        function onReplacementRequested()
        {
            if (recoveryCarrier && session) {
                Qt.callLater(replaceCleanedSession, true)
            }
        }

        function onNormalDisconnectRequested()
        {
            finishTransitionWithoutSession()
        }

        function onStateChanged()
        {
            if (DisplayTransitionCoordinator.statusText) {
                stageText = DisplayTransitionCoordinator.statusText
            }
            if (recoveryCarrier &&
                    DisplayTransitionCoordinator.verificationPending) {
                Qt.callLater(startRecoveryCarrier)
            }
        }
    }

    Loader {
        id: streamLoader
        active: false
        asynchronous: true

        onLoaded: {
            // Set the hint text. We do this here rather than
            // in the hintText control itself to synchronize
            // with Session.exec() which requires no concurrent
            // gamepad usage.
            hintText.text = qsTr("Tip:") + " " + qsTr("Press %1 to disconnect your session").arg(SdlGamepadKeyNavigation.getConnectedGamepads() > 0 ?
                                                  qsTr("Start+Select+L1+R1") : qsTr("Ctrl+Alt+Shift+Q"))

            // Initialize the session and probe for host/client capabilities.
            initializeActiveSession(true)
        }

        sourceComponent: Item {}
    }

    Row {
        anchors.centerIn: parent
        spacing: 5

        BusyIndicator {
            id: stageSpinner
            running: visible
            visible: false
        }

        Label {
            id: stageLabel
            height: stageSpinner.height
            text: stageText
            font.pointSize: 20
            verticalAlignment: Text.AlignVCenter

            wrapMode: Text.Wrap
        }
    }

    Label {
        id: hintText
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 50
        anchors.horizontalCenter: parent.horizontalCenter
        font.pointSize: 18
        verticalAlignment: Text.AlignVCenter

        wrapMode: Text.Wrap
    }

    Pane {
        id: displayRecoverySurface
        objectName: "displayRecoverySurface"
        anchors.centerIn: parent
        width: Math.min(parent.width - 40, 620)
        visible: DisplayTransitionCoordinator.recoveryVisible
        focus: visible

        onVisibleChanged: {
            if (visible && recoveryActionsReady) {
                retryButton.forceActiveFocus()
            }
            else {
                focus = false
            }
        }

        Column {
            anchors.fill: parent
            spacing: 14

            Label {
                width: parent.width
                text: qsTr("Display recovery needed")
                font.pointSize: 20
                font.bold: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
            }

            Label {
                width: parent.width
                text: DisplayTransitionCoordinator.statusText
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
            }

            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 10

                Button {
                    id: retryButton
                    text: qsTr("Retry")
                    enabled: displayRecoverySurface.visible && recoveryActionsReady
                    activeFocusOnTab: displayRecoverySurface.visible && recoveryActionsReady
                    KeyNavigation.right: disconnectButton
                    KeyNavigation.down: disconnectButton
                    onClicked: DisplayTransitionCoordinator.retry()
                }

                Button {
                    id: disconnectButton
                    text: qsTr("Disconnect")
                    enabled: displayRecoverySurface.visible && recoveryActionsReady
                    activeFocusOnTab: displayRecoverySurface.visible && recoveryActionsReady
                    KeyNavigation.left: retryButton
                    KeyNavigation.right: returnToHostButton
                    KeyNavigation.up: retryButton
                    KeyNavigation.down: returnToHostButton
                    onClicked: DisplayTransitionCoordinator.disconnect()
                }

                Button {
                    id: returnToHostButton
                    text: qsTr("Return to host")
                    enabled: displayRecoverySurface.visible && recoveryActionsReady
                    activeFocusOnTab: displayRecoverySurface.visible && recoveryActionsReady
                    KeyNavigation.left: disconnectButton
                    KeyNavigation.up: disconnectButton
                    onClicked: DisplayTransitionCoordinator.returnToHost()
                }
            }
        }
    }
}
