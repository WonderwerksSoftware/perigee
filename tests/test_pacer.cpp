#include "test_registry.h"

#include "streaming/streamutils.h"
#include "streaming/video/ffmpeg-renderers/pacer/pacer.h"

#include <QMutex>
#include <QMutexLocker>
#include <QWaitCondition>
#include <QtTest>

#include <atomic>

extern "C" uint64_t LiGetMicroseconds(void)
{
    static std::atomic<uint64_t> now {0};
    return now.fetch_add(1) + 1;
}

int StreamUtils::getDisplayRefreshRate(SDL_Window*)
{
    return 60;
}

class RecordingPacerRenderer final : public IFFmpegRenderer
{
public:
    explicit RecordingPacerRenderer(bool useRenderThread = false) :
        IFFmpegRenderer(RendererType::Unknown),
        m_UseRenderThread(useRenderThread)
    {
    }

    bool initialize(PDECODER_PARAMETERS) override
    {
        return true;
    }

    bool prepareDecoderContext(AVCodecContext*, AVDictionary**) override
    {
        return true;
    }

    void renderFrame(AVFrame* frame) override
    {
        QMutexLocker locker(&m_Mutex);
        m_RenderedFrames.push_back(frame);
        m_FrameRendered.wakeAll();
    }

    bool isRenderThreadSupported() override
    {
        return m_UseRenderThread;
    }

    bool waitForFrameCount(int expected, int timeoutMs = 1000)
    {
        QMutexLocker locker(&m_Mutex);
        while (m_RenderedFrames.size() < expected) {
            if (!m_FrameRendered.wait(&m_Mutex, timeoutMs)) {
                return false;
            }
        }
        return true;
    }

    QVector<AVFrame*> renderedFrames() const
    {
        QMutexLocker locker(&m_Mutex);
        return m_RenderedFrames;
    }

private:
    bool m_UseRenderThread;
    mutable QMutex m_Mutex;
    QWaitCondition m_FrameRendered;
    QVector<AVFrame*> m_RenderedFrames;
};

class PacerTest : public QObject
{
    Q_OBJECT

private slots:
    void redrawsRetainedFrameWithoutCountingNetworkFrame();
    void redrawsRetainedFrameOnRendererThread();
};

void PacerTest::redrawsRetainedFrameWithoutCountingNetworkFrame()
{
    RecordingPacerRenderer renderer;
    VIDEO_STATS stats {};
    Pacer pacer(&renderer, &stats);
    QVERIFY(pacer.initialize(nullptr, 60, false));

    AVFrame* frame = av_frame_alloc();
    QVERIFY(frame != nullptr);
    frame->pkt_dts = LiGetMicroseconds();

    pacer.submitFrame(frame);
    pacer.renderOnMainThread();

    QCOMPARE(renderer.renderedFrames().size(), 1);
    QCOMPARE(stats.renderedFrames, quint32(1));

    pacer.requestRedraw();
    pacer.renderOnMainThread();

    QCOMPARE(renderer.renderedFrames().size(), 2);
    QCOMPARE(renderer.renderedFrames().at(1), renderer.renderedFrames().at(0));
    QCOMPARE(stats.renderedFrames, quint32(1));

    pacer.requestRedraw();
    pacer.requestRedraw();
    pacer.renderOnMainThread();
    pacer.renderOnMainThread();

    QCOMPARE(renderer.renderedFrames().size(), 3);
    QCOMPARE(stats.renderedFrames, quint32(1));

    SDL_FlushEvents(SDL_USEREVENT, SDL_USEREVENT);
}

void PacerTest::redrawsRetainedFrameOnRendererThread()
{
    RecordingPacerRenderer renderer(true);
    VIDEO_STATS stats {};
    Pacer pacer(&renderer, &stats);
    QVERIFY(pacer.initialize(nullptr, 60, false));

    AVFrame* frame = av_frame_alloc();
    QVERIFY(frame != nullptr);
    frame->pkt_dts = LiGetMicroseconds();

    pacer.submitFrame(frame);
    QVERIFY(renderer.waitForFrameCount(1));

    pacer.requestRedraw();
    QVERIFY(renderer.waitForFrameCount(2));

    const QVector<AVFrame*> renderedFrames = renderer.renderedFrames();
    QCOMPARE(renderedFrames.at(1), renderedFrames.at(0));
    QCOMPARE(stats.renderedFrames, quint32(1));
}

REGISTER_PERIGEE_TEST(PacerTest);

#include "test_pacer.moc"
