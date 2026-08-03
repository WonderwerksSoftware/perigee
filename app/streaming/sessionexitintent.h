#pragma once

class SessionExitIntent
{
public:
    void requestClientDisconnect()
    {
        m_KeepHostSessionRunning = true;
    }

    void requestPerigeeQuit()
    {
        m_KeepHostSessionRunning = true;
        m_ShouldExitPerigee = true;
    }

    void requestPerigeeExit(bool quitHostApp)
    {
        m_ShouldExitPerigee = true;
        if (quitHostApp) {
            m_KeepHostSessionRunning = false;
            m_ForceQuitHost = true;
        }
    }

    bool shouldExitPerigee() const
    {
        return m_ShouldExitPerigee;
    }

    bool shouldQuitHost(bool unexpectedTermination,
                        bool quitHostAfterSession) const
    {
        return !unexpectedTermination &&
                (m_ForceQuitHost || quitHostAfterSession) &&
                !m_KeepHostSessionRunning;
    }

private:
    bool m_ShouldExitPerigee = false;
    bool m_KeepHostSessionRunning = false;
    bool m_ForceQuitHost = false;
};
