#include "Utility/CppMacros.h"

#include <map>
#include <stdio.h>

typedef bool Bool;
typedef int Int;
typedef unsigned int UnsignedInt;
typedef unsigned int u32;
typedef char Char;
typedef float Real;
#define TRUE true
#define FALSE false

static int s_failures = 0;
#define CHECK(expression) check((expression), #expression, __LINE__)
static void check(bool result, const char *expression, int line)
{
    if (!result) {
        printf("FAIL line %d: %s\n", line, expression);
        ++s_failures;
    }
}
#define DEBUG_CRASH(message) CHECK(false)

class RectClass;
#include "VideoInterface.inc"

VideoBuffer::VideoBuffer(Type format)
    : m_xPos(0), m_yPos(0), m_width(1), m_height(1),
      m_textureWidth(1), m_textureHeight(1), m_pitch(sizeof(int)), m_format(format)
{
}

class TestBuffer : public VideoBuffer
{
public:
    TestBuffer() : VideoBuffer(TYPE_X8R8G8B8), pixel(0), failLock(false) {}
    virtual Bool allocate(UnsignedInt, UnsignedInt) { return TRUE; }
    virtual void free() {}
    virtual void *lock() { return failLock ? 0 : &pixel; }
    virtual void unlock() {}
    virtual Bool valid() { return TRUE; }
    void setFormat(Type format) { m_format = format; }
    int pixel;
    bool failLock;
};

class VideoStream : public VideoStreamInterface
{
public:
    virtual VideoStreamInterface *next() { return 0; }
    virtual void update() {}
    virtual void close() {}
};

// Model only the SDK transport: Bink selects one-based frames, waits before
// decode, and wraps at the end. Completion decisions come from production.
struct TestBink
{
    explicit TestBink(unsigned frames)
        : FrameNum(1), Frames(frames), Width(1), Height(1), ready(false),
          decodedFrame(0), copies(0), advances(0), seeks(0) {}
    unsigned FrameNum, Frames, Width, Height;
    bool ready;
    unsigned decodedFrame;
    int copies, advances, seeks;
};
typedef TestBink *HBINK;
enum { BINKSURFACE32, BINKSURFACE24, BINKSURFACE565, BINKSURFACE555 };
static void BinkClose(HBINK) {}
static int BinkWait(HBINK handle) { return !handle->ready; }
static void BinkDoFrame(HBINK handle)
{
    CHECK(handle->ready);
    handle->decodedFrame = handle->FrameNum;
}
static void BinkCopyToBuffer(HBINK handle, void *memory, unsigned, unsigned,
    unsigned, unsigned, u32)
{
    CHECK(handle->ready);
    CHECK(handle->decodedFrame == handle->FrameNum);
    *static_cast<int *>(memory) = static_cast<int>(handle->FrameNum);
    ++handle->copies;
}
static void BinkNextFrame(HBINK handle)
{
    handle->FrameNum = handle->FrameNum == handle->Frames ? 1 : handle->FrameNum + 1;
    handle->ready = false;
    ++handle->advances;
}
static void BinkGoto(HBINK handle, Int frame, Int)
{
    // Preserve the existing Bink adapter's SDK-index convention in this test.
    CHECK(frame >= 1 && static_cast<unsigned>(frame) <= handle->Frames);
    handle->FrameNum = frame;
    handle->ready = false;
    ++handle->seeks;
}

#include "BinkStream.inc"

class TestBinkStream : public BinkVideoStream
{
public:
    explicit TestBinkStream(TestBink &handle) { m_handle = &handle; }
    virtual ~TestBinkStream() {}
};

// Fixture hosts expose state and record the stop side effect. Their update
// methods and window completion actions are the production definitions below.
class Display
{
public:
    Display() : m_videoStream(0), m_videoBuffer(0), stops(0), pixelAtStop(0) {}
    void update();
    void stopMovie()
    {
        ++stops;
        pixelAtStop = static_cast<TestBuffer *>(m_videoBuffer)->pixel;
        m_videoStream = 0;
    }
    VideoStreamInterface *m_videoStream;
    VideoBuffer *m_videoBuffer;
    int stops, pixelAtStop;
};
class GeneralsInGameUI : public Display { public: void update(); };
class GeneralsMDInGameUI : public Display { public: void update(); };

#include "DisplayMovieUpdate.inc"
#include "GeneralsMovieUpdate.inc"
#include "GeneralsMDMovieUpdate.inc"

enum WindowVideoPlayType
{
    WINDOW_PLAY_MOVIE_ONCE, WINDOW_PLAY_MOVIE_SHOW_LAST_FRAME, WINDOW_PLAY_MOVIE_LOOP
};
enum WindowVideoStates
{
    WINDOW_VIDEO_STATE_STOP, WINDOW_VIDEO_STATE_PAUSE, WINDOW_VIDEO_STATE_PLAY,
    WINDOW_VIDEO_STATE_HIDDEN
};
class GameWindow { public: Bool winIsHidden() { return FALSE; } };
class WindowVideo
{
public:
    WindowVideo(GameWindow &window, VideoStreamInterface &stream, VideoBuffer &buffer,
        WindowVideoPlayType playType)
        : m_win(&window), m_stream(&stream), m_buffer(&buffer), m_playType(playType),
          m_state(WINDOW_VIDEO_STATE_PLAY) {}
    GameWindow *getWin() { return m_win; }
    VideoStreamInterface *getVideoStream() { return m_stream; }
    VideoBuffer *getVideoBuffer() { return m_buffer; }
    WindowVideoPlayType getPlayType() { return m_playType; }
    WindowVideoStates getState() { return m_state; }
    void setWindowState(WindowVideoStates state) { m_state = state; }
private:
    GameWindow *m_win;
    VideoStreamInterface *m_stream;
    VideoBuffer *m_buffer;
    WindowVideoPlayType m_playType;
    WindowVideoStates m_state;
};
class WindowVideoManager
{
public:
    explicit WindowVideoManager(WindowVideo &movie) : m_stopAllMovies(FALSE), m_pauseAllMovies(FALSE)
    {
        m_playingVideos[movie.getWin()] = &movie;
    }
    void update();
    void handleFinishedMovie(WindowVideo *, GameWindow *, VideoStreamInterface *);
    void pauseMovie(GameWindow *);
    void hideMovie(GameWindow *);
    void resumeMovie(GameWindow *);
    void stopMovie(GameWindow *);
private:
    typedef std::map<const GameWindow *, WindowVideo *> WindowVideoMap;
    WindowVideoMap m_playingVideos;
    Bool m_stopAllMovies, m_pauseAllMovies;
};

#include "WindowMovieMethods.inc"

static void testSelectedRenderedAndSeek()
{
    TestBink sdk(2);
    TestBinkStream stream(sdk);
    TestBuffer buffer;
    CHECK(!stream.isFinished());
    sdk.ready = true;
    stream.frameDecompress();
    stream.frameRender(&buffer);
    CHECK(buffer.pixel == 1 && !stream.isFinished());
    stream.frameNext();
    CHECK(stream.frameIndex() == 1 && !stream.isFrameReady() && !stream.isFinished());
    stream.update();
    CHECK(!stream.isFinished());
    sdk.ready = true;
    stream.frameDecompress();
    stream.frameRender(0);
    CHECK(!stream.isFinished());
    buffer.failLock = true;
    stream.frameRender(&buffer);
    CHECK(!stream.isFinished() && sdk.copies == 1);
    buffer.failLock = false;
    buffer.setFormat(VideoBuffer::TYPE_UNKNOWN);
    stream.frameRender(&buffer);
    CHECK(!stream.isFinished() && sdk.copies == 1);
    buffer.setFormat(VideoBuffer::TYPE_X8R8G8B8);
    stream.frameRender(&buffer);
    CHECK(stream.isFinished() && buffer.pixel == 2 && sdk.copies == 2);
    CHECK(stream.finishPlayback()); // Legacy loading hosts retain their no-op drain.
    CHECK(stream.frameGoto(2));
    CHECK(!stream.isFrameReady() && !stream.isFinished());
    sdk.ready = true;
    stream.frameDecompress();
    stream.frameRender(&buffer);
    CHECK(stream.isFinished());
    stream.frameNext();
    CHECK(stream.frameIndex() == 0 && !stream.isFinished());
    CHECK(stream.frameGoto(1));
    CHECK(!stream.isFinished());
}

template<class Host> static void testClosingHost(unsigned frames)
{
    TestBink sdk(frames);
    TestBinkStream stream(sdk);
    TestBuffer buffer;
    Host host;
    host.m_videoStream = &stream;
    host.m_videoBuffer = &buffer;
    if (frames == 2) {
        sdk.ready = true;
        host.update();
        CHECK(host.stops == 0 && buffer.pixel == 1 && sdk.copies == 1);
        CHECK(stream.frameIndex() == 1);
    }
    CHECK(!stream.isFinished() && !stream.isFrameReady());
    host.update();
    CHECK(host.stops == 0 && sdk.copies == static_cast<int>(frames - 1));
    sdk.ready = true;
    host.update();
    CHECK(host.stops == 1 && host.pixelAtStop == static_cast<int>(frames));
    CHECK(sdk.copies == static_cast<int>(frames));
}

static void testWindowHost(unsigned frames, WindowVideoPlayType playType)
{
    TestBink sdk(frames);
    TestBinkStream stream(sdk);
    TestBuffer buffer;
    GameWindow window;
    WindowVideo movie(window, stream, buffer, playType);
    WindowVideoManager manager(movie);
    if (frames == 2) {
        sdk.ready = true;
        manager.update();
        CHECK(movie.getState() == WINDOW_VIDEO_STATE_PLAY && buffer.pixel == 1);
        CHECK(sdk.copies == 1 && sdk.seeks == 0);
    }
    manager.update(); // The selected final frame is still waiting for BinkWait.
    CHECK(movie.getState() == WINDOW_VIDEO_STATE_PLAY);
    CHECK(sdk.copies == static_cast<int>(frames - 1));
    sdk.ready = true;
    manager.update();
    CHECK(buffer.pixel == static_cast<int>(frames) && sdk.copies == static_cast<int>(frames));
    CHECK(stream.frameIndex() == 0 && !stream.isFinished());
    CHECK(sdk.seeks == 0); // Bink loops by wrapping, without a native-style seek.
    const WindowVideoStates expected = playType == WINDOW_PLAY_MOVIE_ONCE ? WINDOW_VIDEO_STATE_STOP
        : playType == WINDOW_PLAY_MOVIE_SHOW_LAST_FRAME ? WINDOW_VIDEO_STATE_PAUSE : WINDOW_VIDEO_STATE_PLAY;
    CHECK(movie.getState() == expected);
    sdk.ready = true;
    manager.update();
    if (playType == WINDOW_PLAY_MOVIE_LOOP) {
        CHECK(buffer.pixel == 1 && sdk.copies == static_cast<int>(frames + 1));
    } else {
        CHECK(buffer.pixel == static_cast<int>(frames) && sdk.copies == static_cast<int>(frames));
    }
}

// A native backend can be terminal without a ready frame. Keep that host exit
// path independent of Bink's per-frame render flag.
class TerminalStream : public VideoStream
{
public:
    TerminalStream() : finished(false) {}
    virtual Bool isFrameReady() { return FALSE; }
    virtual Bool isFinished() const { return finished; }
    virtual void frameDecompress() { CHECK(false); }
    virtual void frameRender(VideoBuffer *) { CHECK(false); }
    virtual void frameNext() { CHECK(false); }
    virtual Int frameIndex() { return 1; }
    virtual Int frameCount() { return 2; }
    virtual Bool frameGoto(Int) { return FALSE; }
    virtual Int height() { return 1; }
    virtual Int width() { return 1; }
    bool finished;
};
template<class Host> static void testTerminalHost()
{
    TerminalStream stream;
    TestBuffer buffer;
    Host host;
    host.m_videoStream = &stream;
    host.m_videoBuffer = &buffer;
    host.update();
    CHECK(host.stops == 0); // A native audio tail can still be draining.
    stream.finished = true;
    host.update();
    CHECK(host.stops == 1);
}

static void testTerminalWindowHost()
{
    TerminalStream stream;
    TestBuffer buffer;
    GameWindow window;
    WindowVideo movie(window, stream, buffer, WINDOW_PLAY_MOVIE_SHOW_LAST_FRAME);
    WindowVideoManager manager(movie);
    manager.update();
    CHECK(movie.getState() == WINDOW_VIDEO_STATE_PLAY);
    stream.finished = true;
    manager.update();
    CHECK(movie.getState() == WINDOW_VIDEO_STATE_PAUSE);
}

int main()
{
    testSelectedRenderedAndSeek();
    for (unsigned frames = 1; frames <= 2; ++frames) {
        testClosingHost<Display>(frames);
        testClosingHost<GeneralsInGameUI>(frames);
        testClosingHost<GeneralsMDInGameUI>(frames);
        testWindowHost(frames, WINDOW_PLAY_MOVIE_ONCE);
        testWindowHost(frames, WINDOW_PLAY_MOVIE_SHOW_LAST_FRAME);
        testWindowHost(frames, WINDOW_PLAY_MOVIE_LOOP);
    }
    testTerminalHost<Display>();
    testTerminalHost<GeneralsInGameUI>();
    testTerminalHost<GeneralsMDInGameUI>();
    testTerminalWindowHost();
    return s_failures == 0 ? 0 : 1;
}
