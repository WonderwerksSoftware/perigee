#pragma once

class DrmRenderLifecycleState
{
public:
    void beginPrepare() noexcept
    {
        m_RestorationRequired = true;
    }

    void recordApplyResult(bool succeeded) noexcept
    {
        // A successful commit outside the expected prepare sequence must also
        // be restored. A failed commit never cancels an existing obligation.
        m_RestorationRequired = m_RestorationRequired || succeeded;
    }

    bool restorationRequired() const noexcept
    {
        return m_RestorationRequired;
    }

    void completeRestoration() noexcept
    {
        m_RestorationRequired = false;
    }

private:
    bool m_RestorationRequired = false;
};
