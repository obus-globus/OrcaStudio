#include "wxMediaCtrl3.h"
#include "AVVideoDecoder.hpp"
#include "I18N.hpp"
#include "Printer/StaticBambuLib.hpp"
#include "libslic3r/Utils.hpp"
#include <boost/log/trivial.hpp>
#include <wx/dcclient.h>
#include <wx/dcgraph.h>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/log.h>
}
#ifdef __WIN32__
#include <versionhelpers.h>
#include <wx/msw/registry.h>
#include <shellapi.h>
#endif

wxDEFINE_EVENT(EVT_MEDIA_CTRL_STAT, wxCommandEvent);

BEGIN_EVENT_TABLE(wxMediaCtrl3, wxWindow)

// catch paint events
EVT_PAINT(wxMediaCtrl3::paintEvent)

END_EVENT_TABLE()


static std::string wxmedia_safe_log_text(const std::string& value)
{
    if (value.find("bambu:///") != std::string::npos ||
        value.find("authkey=") != std::string::npos ||
        value.find("passwd=") != std::string::npos ||
        value.find("token=") != std::string::npos ||
        value.find("license=") != std::string::npos)
        return "<redacted len=" + std::to_string(value.size()) + ">";
    return value;
}

wxMediaCtrl3::wxMediaCtrl3(wxWindow *parent)
    : wxWindow(parent, wxID_ANY)
    , BambuLib(StaticBambuLib::get(this))
    , m_thread([this] { PlayThread(); })
    , m_frame_buffer(9)
{
    SetBackgroundColour("#000001ff");
    m_render_timer.SetOwner(this);
    Bind(wxEVT_TIMER, &wxMediaCtrl3::OnRenderTimer, this);
}

wxMediaCtrl3::~wxMediaCtrl3()
{
    m_render_timer.Stop();
    {
        std::unique_lock<std::mutex> lk(m_ui_mutex);
        m_frame = wxImage(m_idle_image);
    }
    {
        std::unique_lock<std::mutex> lk(m_mutex);
#if defined(__WXMAC__) || defined(__APPLE__)
        m_url = std::make_shared<MediaUrl>();
#else
        m_url.reset(new wxURI);
#endif
        m_cond.notify_all();
    }
    m_thread.join();
    StaticBambuLib::remove(this);
}

void wxMediaCtrl3::Load(wxURI url)
{
    Load(url, {});
}

void wxMediaCtrl3::Load(wxURI url, std::chrono::system_clock::time_point play_start_time)
{
    std::unique_lock<std::mutex> lk(m_mutex);
    if (m_external)
        return;
    m_video_size = wxDefaultSize;
    m_error = 0;
    m_play_start_time = play_start_time;
#if defined(__WXMAC__) || defined(__APPLE__)
    const wxCharBuffer utf8 = url.BuildURI().ToUTF8();
    auto media_url = std::make_shared<MediaUrl>();
    media_url->value = utf8.data() ? utf8.data() : "";
    media_url->has_scheme = url.HasScheme();
    m_url = media_url;
#else
    m_url.reset(new wxURI(url));
#endif
    m_cond.notify_all();
}

#if defined(__WXMAC__) || defined(__APPLE__)
void wxMediaCtrl3::LoadRaw(wxString const &url, std::chrono::system_clock::time_point play_start_time)
{
    const wxCharBuffer utf8 = url.ToUTF8();
    std::unique_lock<std::mutex> lk(m_mutex);
    m_video_size = wxDefaultSize;
    m_error = 0;
    m_play_start_time = play_start_time;
    auto media_url = std::make_shared<MediaUrl>();
    media_url->value = utf8.data() ? utf8.data() : "";
    media_url->has_scheme = !media_url->value.empty();
    m_url = media_url;
    m_cond.notify_all();
}
#endif

void wxMediaCtrl3::Play()
{
    std::unique_lock<std::mutex> lk(m_mutex);
    if (m_external)
        return;
    if (m_state != wxMEDIASTATE_PLAYING) {
        m_state = wxMEDIASTATE_PLAYING;
        wxMediaEvent event(wxEVT_MEDIA_STATECHANGED);
        event.SetId(GetId());
        event.SetEventObject(this);
        wxPostEvent(this, event);
    }
}

void wxMediaCtrl3::Stop()
{
    {
        std::unique_lock<std::mutex> lk(m_ui_mutex);
        m_frame = wxImage(m_idle_image);
    }
    std::unique_lock<std::mutex> lk(m_mutex);
    m_url.reset();
    NotifyStopped();
    m_cond.notify_all();
    CallAfter([this] { Refresh(); });
}

// adjust_frame_size is defined below with the paint code.
static void adjust_frame_size(wxSize & frame, wxSize const & video, wxSize const & window);

void wxMediaCtrl3::SetExternalFrame(const wxImage& frame, wxSize videoSize)
{
    if (!frame.IsOk())
        return;
    {
        std::unique_lock<std::mutex> lk(m_mutex);
        if (!m_external)
            return;
        m_video_size = videoSize.IsFullySpecified() ? videoSize : frame.GetSize();
        adjust_frame_size(m_frame_size, m_video_size, GetSize());
    }
    {
        std::unique_lock<std::mutex> lk(m_ui_mutex);
#ifdef _WIN32
        m_frame = wxBitmap(frame);
#else
        m_frame = frame;
#endif
    }
    CallAfter([this] { Refresh(); });
}

#ifdef _WIN32
void wxMediaCtrl3::SetExternalFrame(const wxBitmap& frame, wxSize videoSize)
{
    if (!frame.IsOk())
        return;
    {
        std::unique_lock<std::mutex> lk(m_mutex);
        if (!m_external)
            return;
        m_video_size = videoSize.IsFullySpecified() ? videoSize : frame.GetSize();
        adjust_frame_size(m_frame_size, m_video_size, GetSize());
    }
    {
        std::unique_lock<std::mutex> lk(m_ui_mutex);
        m_frame = frame;
    }
    CallAfter([this] { Refresh(); });
}
#endif

void wxMediaCtrl3::BeginExternalStream()
{
    {
        std::unique_lock<std::mutex> lk(m_mutex);
        m_external = true;
        m_url.reset();
#if !defined(__WXMAC__) && !defined(__APPLE__)
        m_active_url.reset();
#endif
        m_video_size = wxDefaultSize;
        m_cond.notify_all();
    }
    {
        std::unique_lock<std::mutex> lk(m_ui_mutex);
        m_frame = wxImage(m_idle_image);
    }
    Refresh();
}

void wxMediaCtrl3::EndExternalStream()
{
    {
        std::unique_lock<std::mutex> lk(m_mutex);
        m_external = false;
        m_url.reset();
#if !defined(__WXMAC__) && !defined(__APPLE__)
        m_active_url.reset();
#endif
        m_video_size = wxDefaultSize;
        m_cond.notify_all();
    }
    {
        std::unique_lock<std::mutex> lk(m_ui_mutex);
        m_frame = wxImage(m_idle_image);
    }
    Refresh();
}

void wxMediaCtrl3::SetIdleImage(wxString const &image, wxString const &watermark_text)
{
    if (m_idle_image == image && m_watermark_text == watermark_text)
        return;
    m_idle_image = image;
    m_watermark_text = watermark_text;
    if (m_url == nullptr) {
        std::unique_lock<std::mutex> lk(m_ui_mutex);
        m_frame = wxImage(m_idle_image);
        assert(m_frame.IsOk());
        Refresh();
    }
}

void wxMediaCtrl3::SetIdleImage(const wxImage &image, wxString const &watermark_text)
{
    if (!image.IsOk())
        return;
    m_idle_image.clear();
    m_watermark_text = watermark_text;
    if (m_url == nullptr) {
        std::unique_lock<std::mutex> lk(m_ui_mutex);
        m_frame = image;
        assert(m_frame.IsOk());
        Refresh();
    }
}

wxMediaState wxMediaCtrl3::GetState()
{
    std::unique_lock<std::mutex> lk(m_mutex);
    return m_state;
}

int wxMediaCtrl3::GetLastError() const
{
    std::unique_lock<std::mutex> lk(m_mutex);
    return m_error;
}

wxSize wxMediaCtrl3::GetVideoSize() const
{
    std::unique_lock<std::mutex> lk(m_mutex);
    return m_video_size;
}

wxSize wxMediaCtrl3::DoGetBestSize() const
{
    return {-1, -1};
}

static void adjust_frame_size(wxSize & frame, wxSize const & video, wxSize const & window)
{
    if (video.x * window.y < video.y * window.x)
        frame = { video.x * window.y / video.y, window.y };
    else
        frame = { window.x, video.y * window.x / video.x };
}

void wxMediaCtrl3::paintEvent(wxPaintEvent &evt)
{
    wxPaintDC dc(this);
    auto      size = GetSize();
    if (size.x <= 0 || size.y <= 0)
        return;
    PlayFrame current_frame;
    {
        std::unique_lock<std::mutex> lk(m_ui_mutex);
        if (!m_frame.IsOk()) {
            return;
        }
        current_frame = m_frame;
    }
    wxSize frame_size;
    {
        std::unique_lock<std::mutex> lk(m_mutex);
        frame_size = m_frame_size;
    }
    auto size2 = current_frame.GetSize();
    if (size2.x != frame_size.x && size2.y == frame_size.y)
        size2.x = frame_size.x;
    auto size3 = (size - size2) / 2;
    if (size2.x != size.x && size2.y != size.y) {
        double scale = 1.;
        if (size.x * size2.y > size.y * size2.x) {
            size3 = {size.x * size2.y / size.y, size2.y};
            scale = double(size.y) / size2.y;
        } else {
            size3 = {size2.x, size.y * size2.x / size.x};
            scale = double(size.x) / size2.x;
        }
        dc.SetUserScale(scale, scale);
        size3 = (size3 - size2) / 2;
    }
#ifdef _WIN32
    dc.DrawBitmap(current_frame, size3.x, size3.y);
#else
    dc.DrawBitmap(wxBitmap(current_frame), size3.x, size3.y);
#endif

    // Draw watermark overlay when showing device preview image
    if (!m_watermark_text.empty() && m_url == nullptr) {
        // Reset user scale for watermark (draw at 1:1)
        dc.SetUserScale(1.0, 1.0);

        wxString watermark_text = m_watermark_text;

        // Setup font
        wxFont font = dc.GetFont();
        font.SetPointSize(10);
        font.SetWeight(wxFONTWEIGHT_BOLD);
        dc.SetFont(font);
        wxSize text_size = dc.GetTextExtent(watermark_text);

        // Calculate watermark rectangle with padding
        int pad_h = FromDIP(12);
        int pad_v = FromDIP(8);
        int wm_w = text_size.GetWidth() + 2 * pad_h;
        int wm_h = text_size.GetHeight() + 2 * pad_v;
        int wm_x = (size.GetWidth() - wm_w) / 2;
        int wm_y = size.GetHeight() - wm_h - FromDIP(10);
        int radius = 8;

        // Use wxGCDC for alpha-blended rounded rectangle
        wxGCDC gcdc(dc);
        gcdc.SetBrush(wxBrush(wxColour(51, 51, 51, 160)));
        gcdc.SetPen(*wxTRANSPARENT_PEN);
        gcdc.DrawRoundedRectangle(wm_x, wm_y, wm_w, wm_h, radius);

        // Draw text centered in the rectangle
        gcdc.SetTextForeground(wxColour(220, 220, 220));
        gcdc.SetFont(font);
        int tx = wm_x + (wm_w - text_size.GetWidth()) / 2;
        int ty = wm_y + (wm_h - text_size.GetHeight()) / 2;
        gcdc.DrawText(watermark_text, tx, ty);
    }
}

void wxMediaCtrl3::DoSetSize(int x, int y, int width, int height, int sizeFlags)
{
    wxWindow::DoSetSize(x, y, width, height, sizeFlags);
    if (sizeFlags == wxSIZE_USE_EXISTING) return;
    wxMediaCtrl_OnSize(this, m_video_size, width, height);
    std::unique_lock<std::mutex> lk(m_mutex);
    adjust_frame_size(m_frame_size, m_video_size, GetSize());
    Refresh();
}

void wxMediaCtrl3::bambu_log(void *ctx, int level, tchar const *msg2)
{
#ifdef _WIN32
    wxString msg(msg2);
#else
    wxString msg = wxString::FromUTF8(msg2);
#endif
    if (level == 1) {
        if (msg.EndsWith("]")) {
            int n = msg.find_last_of('[');
            if (n != wxString::npos) {
                long val = 0;
                wxMediaCtrl3 *ctrl = (wxMediaCtrl3 *) ctx;
                if (msg.SubString(n + 1, msg.Length() - 2).ToLong(&val)) {
                    std::unique_lock<std::mutex> lk(ctrl->m_mutex);
                    ctrl->m_error = (int) val;
                }
            }
        } else if (msg.Contains("stat_log")) {
            wxCommandEvent evt(EVT_MEDIA_CTRL_STAT);
            wxMediaCtrl3 *ctrl = (wxMediaCtrl3 *) ctx;
            evt.SetEventObject(ctrl);
            evt.SetString(msg.Mid(msg.Find(' ') + 1));
            wxPostEvent(ctrl, evt);
        }
    }
    BOOST_LOG_TRIVIAL(info) << msg.ToUTF8().data();
}

static std::string wxmedia_bambu_last_error(BambuLib& lib)
{
    if (!lib.Bambu_GetLastErrorMsg)
        return {};
    const char* raw = lib.Bambu_GetLastErrorMsg();
    std::string out = raw ? std::string(raw) : std::string();
#if defined(__WXMAC__) || defined(__APPLE__)
    if (raw && lib.Bambu_FreeLogMsg)
        lib.Bambu_FreeLogMsg(raw);
#endif
    return out;
}

#if !defined(__WXMAC__) && !defined(__APPLE__)
// FFmpeg's own diagnostics (HTTP status, "Invalid data found", demuxer choice,
// missing stream dimensions, ...) are otherwise swallowed: a failed camera open
// only surfaces as wxMediaCtrl3's generic error code, which MediaPlayCtrl maps to
// the misleading "Player is malfunctioning" string. Forward them to the Orca log
// instead. Verbosity defaults to AV_LOG_VERBOSE and can be raised at runtime with
// ORCA_FFMPEG_LOG_LEVEL=debug|trace|... (or lowered to warning/error/quiet).
static int ffmpeg_log_level_from_env()
{
    const char *env = std::getenv("ORCA_FFMPEG_LOG_LEVEL");
    if (env == nullptr || *env == '\0')
        return AV_LOG_VERBOSE;
    const wxString v = wxString(env).Lower();
    if (v == "quiet")   return AV_LOG_QUIET;
    if (v == "panic")   return AV_LOG_PANIC;
    if (v == "fatal")   return AV_LOG_FATAL;
    if (v == "error")   return AV_LOG_ERROR;
    if (v == "warning") return AV_LOG_WARNING;
    if (v == "info")    return AV_LOG_INFO;
    if (v == "verbose") return AV_LOG_VERBOSE;
    if (v == "debug")   return AV_LOG_DEBUG;
    if (v == "trace")   return AV_LOG_TRACE;
    return AV_LOG_VERBOSE;
}

static void ffmpeg_log_callback(void *avcl, int level, const char *fmt, va_list vl)
{
    if (level > av_log_get_level())
        return;
    thread_local int print_prefix = 1;
    char line[1024];
    av_log_format_line2(avcl, level, fmt, vl, line, (int) sizeof(line), &print_prefix);
    size_t len = std::strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' '))
        line[--len] = '\0';
    if (len == 0)
        return;
    if (level <= AV_LOG_ERROR)
        BOOST_LOG_TRIVIAL(error) << "ffmpeg: " << line;
    else if (level <= AV_LOG_WARNING)
        BOOST_LOG_TRIVIAL(warning) << "ffmpeg: " << line;
    else if (level <= AV_LOG_INFO)
        BOOST_LOG_TRIVIAL(info) << "ffmpeg: " << line;
    else
        BOOST_LOG_TRIVIAL(debug) << "ffmpeg: " << line;
}

static void install_ffmpeg_logger()
{
    av_log_set_level(ffmpeg_log_level_from_env());
    av_log_set_callback(&ffmpeg_log_callback);
}

int wxMediaCtrl3::ffmpeg_interrupt_callback(void *opaque)
{
    auto *ctrl = static_cast<wxMediaCtrl3 *>(opaque);
    std::lock_guard<std::mutex> lock(ctrl->m_mutex);
    return ctrl->m_url != ctrl->m_active_url;
}

int wxMediaCtrl3::PlayFfmpeg(std::shared_ptr<wxURI> const &url, std::unique_lock<std::mutex> &lock)
{
    static std::once_flag logger_once;
    std::call_once(logger_once, install_ffmpeg_logger);

    if (avformat_network_init() < 0)
        return 2;

    AVFormatContext *format_context = avformat_alloc_context();
    if (!format_context) {
        avformat_network_deinit();
        return 2;
    }

    format_context->interrupt_callback = {&wxMediaCtrl3::ffmpeg_interrupt_callback, this};
    format_context->flags |= AVFMT_FLAG_NOBUFFER;
    format_context->max_delay = 0;
    m_active_url = url;

    auto finish = [&](int error) {
        lock.unlock();
        avformat_close_input(&format_context);
        avformat_network_deinit();
        lock.lock();
        m_active_url.reset();
        return error;
    };

    const std::string uri = url->BuildURI().ToUTF8().data();
    const wxString scheme = url->GetScheme();
    const bool http_stream = scheme.CmpNoCase("http") == 0 || scheme.CmpNoCase("https") == 0;
    AVDictionary *options = nullptr;
    if (http_stream) {
        // Live multipart MJPEG. fflags=nobuffer / AVFMT_FLAG_NOBUFFER / max_delay=0
        // (set above) are the low-latency levers - they disable the demuxer
        // read-ahead queue. probesize / analyzeduration only bound the one-off
        // avformat_find_stream_info() at open; a 32-byte budget returned before a
        // whole JPEG frame was seen, so width/height came back unset and the open
        // was rejected. Give it room to identify one frame (a startup cost only).
        // rw_timeout / timeout bound a wedged connect or read so a stale stream
        // fails fast and is retried, instead of the reader thread hanging.
        // avioflags=direct is deliberately NOT set: unbuffered reads make the
        // mpjpeg demuxer emit "Packet corrupt" and bail on any short read across
        // a multipart boundary.
        av_dict_set(&options, "fflags", "nobuffer", 0);
        av_dict_set(&options, "probesize", "5000000", 0);
        av_dict_set(&options, "analyzeduration", "1000000", 0);
        av_dict_set(&options, "rw_timeout", "5000000", 0);
        av_dict_set(&options, "timeout", "5000000", 0);
    } else {
        av_dict_set(&options, "rtsp_transport", "tcp", 0);
    }
    lock.unlock();
    int error = avformat_open_input(&format_context, uri.c_str(), nullptr, &options);
    av_dict_free(&options);
    lock.lock();
    if (error < 0)
        return finish(2);

    lock.unlock();
    error = avformat_find_stream_info(format_context, nullptr);
    lock.lock();
    if (error < 0)
        return finish(2);

    const int video_stream = av_find_best_stream(format_context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream < 0)
        return finish(2);

    AVVideoDecoder decoder;
    if (decoder.open(*format_context->streams[video_stream]->codecpar) < 0)
        return finish(2);

    // Prefer the dimensions the container reported. A small probe budget, or a
    // camera that doesn't announce a size up front, can leave these unset - in
    // that case fill them in from the first frame that decodes (below) rather
    // than failing the open outright.
    auto apply_video_size = [&](wxSize size) {
        if (!size.IsFullySpecified() || size.x <= 0 || size.y <= 0)
            return false;
        m_video_size = size;
        adjust_frame_size(m_frame_size, m_video_size, GetSize());
        NotifyStopped();
        return true;
    };
    bool have_size = apply_video_size({format_context->streams[video_stream]->codecpar->width,
                                      format_context->streams[video_stream]->codecpar->height});
    int size_probe_frames = 0; // frames spent still waiting for a usable size

    AVPacket *packet = av_packet_alloc();
    if (!packet)
        return finish(2);

    while (m_url == url) {
        lock.unlock();
        error = av_read_frame(format_context, packet);
        lock.lock();
        if (m_url != url)
            break;
        if (error < 0)
            break;
        if (packet->stream_index == video_stream) {
            const int decode_error = decoder.decode(*packet);
            if (decode_error == 0) {
                if (!have_size) {
                    have_size = apply_video_size(decoder.decoded_frame_size());
                    if (!have_size) {
                        av_packet_unref(packet);
                        // MJPEG yields a sized frame on the first full packet; if
                        // several seconds of frames never do, treat it as a bad
                        // stream instead of sitting in "Loading..." forever.
                        if (++size_probe_frames > 120)
                            break;
                        continue;
                    }
                }
                auto frame_size = m_frame_size;
                lock.unlock();
#ifdef _WIN32
                wxBitmap frame;
                decoder.toWxBitmap(frame, frame_size);
#else
                wxImage frame;
                decoder.toWxImage(frame, frame_size);
#endif
                lock.lock();
                if (m_url != url)
                    break;
                if (frame.IsOk()) {
                    std::unique_lock<std::mutex> ui_lock(m_ui_mutex);
                    m_frame = frame;
                }
                if (!m_refresh_pending.exchange(true)) {
                    CallAfter([this] {
                        m_refresh_pending.store(false);
                        Refresh();
                    });
                }
            }
        }
        av_packet_unref(packet);
    }

    av_packet_free(&packet);
    return finish(m_url == url ? 2 : 1);
}
#endif

void wxMediaCtrl3::PlayThread()
{
#if !defined(_WIN32) && !defined(__WXMAC__) && !defined(__APPLE__)
    using namespace std::chrono_literals;
    std::shared_ptr<wxURI> url;
    const int decode_warn_thres = 33;
    std::unique_lock<std::mutex> lk(m_mutex);

    //frame count
    int                                                frameCount = 0;
    std::chrono::time_point<std::chrono::system_clock> lastSecondTime;

    while (true) {
        m_cond.wait(lk, [this, &url] { return m_url != url; });
        url = m_url;
        if (url == nullptr)
            continue;
        if (!url->HasScheme())
            break;

        {
            // Generic camera streams of non-Bambu printer agents go through FFmpeg directly.
            const wxString scheme = url->GetScheme();
            if (scheme.CmpNoCase("http") == 0 || scheme.CmpNoCase("https") == 0 ||
                scheme.CmpNoCase("rtsp") == 0 || scheme.CmpNoCase("rtsps") == 0) {
                const int ffmpeg_error = PlayFfmpeg(url, lk);
                if (m_url == url)
                    m_error = ffmpeg_error;
                m_frame_size = wxDefaultSize;
                m_video_size = wxDefaultSize;
                NotifyStopped();
                continue;
            }
        }


        //reset frame
        frameCount     = 0;
        lastSecondTime = std::chrono::system_clock::now();

        lk.unlock();
        Bambu_Tunnel tunnel = nullptr;
        auto t0 = std::chrono::steady_clock::now();
        int error = Bambu_Create(&tunnel, m_url->BuildURI().ToUTF8());
        if (error == 0) {
            Bambu_SetLogger(tunnel, &wxMediaCtrl3::bambu_log, this);
            error = Bambu_Open(tunnel);
            auto t1 = std::chrono::steady_clock::now();
            BOOST_LOG_TRIVIAL(info) << "wxMediaCtrl3: Bambu_Open took "
                                    << std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count() << "ms, error=" << error;
            if (error == 0)
                error = Bambu_would_block;

            else if (error == -2)
            {
                m_error = error;
                BOOST_LOG_TRIVIAL(info) << "MediaPlayCtrl::DLL load error ";
                if (tunnel) {
                    Bambu_Close(tunnel);
                    Bambu_Destroy(tunnel);
                    tunnel = nullptr;
                }
                lk.lock();
                NotifyStopped();
                continue;
            }
        }
        lk.lock();
        auto t_stream_start = std::chrono::steady_clock::now();
        while (error == int(Bambu_would_block)) {
            m_cond.wait_for(lk, 100ms);
            if (m_url != url) {
                error = 1;
                break;
            }
            lk.unlock();
            error = Bambu_StartStream(tunnel, true);
            lk.lock();
        }
        {
            auto t_stream_end = std::chrono::steady_clock::now();
            BOOST_LOG_TRIVIAL(info) << "wxMediaCtrl3: Bambu_StartStream loop took "
                                    << std::chrono::duration_cast<std::chrono::milliseconds>(t_stream_end - t_stream_start).count()
                                    << "ms, error=" << error;
        }
        Bambu_StreamInfo info;
        if (error == 0)
            error = Bambu_GetStreamInfo(tunnel, 0, &info);
        AVVideoDecoder decoder;
        if (error == 0) {
            decoder.open(info);
            m_video_size = { info.format.video.width, info.format.video.height };
            adjust_frame_size(m_frame_size, m_video_size, GetSize());
            NotifyStopped();
            size_t buffer_cap = (size_t) (m_buffer_time * info.format.video.frame_rate / 1000);
            if (buffer_cap == 0) {
                buffer_cap = 1;
            }
            m_frame_buffer.set_capacity(buffer_cap);
            m_get_frame_exit.store(false);
            m_get_frame_thread = std::thread(&wxMediaCtrl3::GetFrameThread, this, info.format.video.frame_rate);
            m_need_refresh.store(false);
            m_render_timer.Start(1000 / (info.format.video.frame_rate + 5));
        }
        Bambu_Sample sample;
        while (error == 0) {
            lk.unlock();
            error = Bambu_ReadSample(tunnel, &sample);
            lk.lock();
            while (error == int(Bambu_would_block)) {
                m_cond.wait_for(lk, 10ms);
                if (m_url != url) {
                    error = 1;
                    break;
                }
                lk.unlock();
                error = Bambu_ReadSample(tunnel, &sample);
                lk.lock();
            }
            if (error == 0) {
                auto frame_size = m_frame_size;
                lk.unlock();
                PlayFrame bm;
                auto start_decode = std::chrono::steady_clock::now();
                decoder.decode(sample);
                auto end_decode = std::chrono::steady_clock::now();
#ifdef _WIN32
                decoder.toWxBitmap(bm, frame_size);
#else
                decoder.toWxImage(bm, frame_size);
#endif
                auto end_convert = std::chrono::steady_clock::now();
                int elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_convert - start_decode).count();
                if (elapsed_ms > decode_warn_thres) {
                    BOOST_LOG_TRIVIAL(warning) << "wxMediaCtrl3: decode + convert too long, decode: "
                                               << std::chrono::duration_cast<std::chrono::milliseconds>(end_decode - start_decode).count()
                                               << " convert: " << std::chrono::duration_cast<std::chrono::milliseconds>(end_convert - end_decode).count();
                }
                lk.lock();
                if (m_url != url) {
                    error = 1;
                    break;
                }
                if (bm.IsOk()) {
                    auto now = std::chrono::system_clock::now();
                    frameCount++;
                    auto elapsedTime = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSecondTime).count();
                    if (elapsedTime >= 10000) {
                        int fps = static_cast<int>(frameCount * 1000 / elapsedTime); // 100 is from frameCount * 1000 / elapsedTime * 10 , becasue  calculate the average rate over 10s
                        BOOST_LOG_TRIVIAL(info) << "wxMediaCtrl3:Decode Real Rate: " << fps << " FPS";
                        frameCount     = 0;
                        lastSecondTime = now;
                    }

                    m_frame_buffer.enqueue(bm);
                }
            }
        }
        if (m_get_frame_thread.joinable()) {
            m_get_frame_exit.store(true);
            m_get_frame_thread.join();
        }
        m_frame_buffer.reset();
        if (tunnel) {
            lk.unlock();
            auto t_close_start = std::chrono::steady_clock::now();
            Bambu_Close(tunnel);
            Bambu_Destroy(tunnel);
            auto t_close_end = std::chrono::steady_clock::now();
            auto close_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_close_end - t_close_start).count();
            if (close_ms > 3000) {
                BOOST_LOG_TRIVIAL(warning) << "wxMediaCtrl3: Bambu_Close+Destroy took " << close_ms << "ms (>3s, potential hang source)";
            } else {
                BOOST_LOG_TRIVIAL(info) << "wxMediaCtrl3: Bambu_Close+Destroy took " << close_ms << "ms";
            }
            tunnel = nullptr;
            lk.lock();
        }
        m_render_timer.Stop();
        if (m_url == url)
            m_error = error;
        m_frame_size = wxDefaultSize;
        m_video_size = wxDefaultSize;
        NotifyStopped();
    }
#else
    using namespace std::chrono_literals;
#if defined(__WXMAC__) || defined(__APPLE__)
    std::shared_ptr<MediaUrl> url;
#else
    std::shared_ptr<wxURI> url;
#endif
    const int decode_warn_thres = 33;
    std::unique_lock<std::mutex> lk(m_mutex);

    //frame count
    int                                                frameCount = 0;
    std::chrono::time_point<std::chrono::system_clock> lastSecondTime;

    while (true) {
        m_cond.wait(lk, [this, &url] { return m_url != url; });
        url = m_url;
        if (url == nullptr)
            continue;
#if defined(__WXMAC__) || defined(__APPLE__)
        if (!url->has_scheme)
            break;
#else
        if (!url->HasScheme())
            break;

        {
            // Generic camera streams of non-Bambu printer agents go through FFmpeg directly.
            const wxString scheme = url->GetScheme();
            if (scheme.CmpNoCase("http") == 0 || scheme.CmpNoCase("https") == 0 ||
                scheme.CmpNoCase("rtsp") == 0 || scheme.CmpNoCase("rtsps") == 0) {
                const int ffmpeg_error = PlayFfmpeg(url, lk);
                if (m_url == url)
                    m_error = ffmpeg_error;
                m_frame_size = wxDefaultSize;
                m_video_size = wxDefaultSize;
                NotifyStopped();
                continue;
            }
        }
#endif


        //reset frame
        frameCount     = 0;
        lastSecondTime = std::chrono::system_clock::now();

#if defined(__WXMAC__) || defined(__APPLE__)
        const std::string uri_utf8 = url->value;
#else
        const auto uri_utf8 = url->BuildURI().ToUTF8();
#endif
        static_cast<BambuLib&>(*this) = StaticBambuLib::get(this);
        lk.unlock();
        Bambu_Tunnel tunnel = nullptr;
        auto t0 = std::chrono::steady_clock::now();
        int error = -2;
#if defined(__WXMAC__) || defined(__APPLE__)
        const bool is_bambu_url = uri_utf8.rfind("bambu:///", 0) == 0;
        const bool is_tutk_url = uri_utf8.rfind("bambu:///tutk", 0) == 0;
        BOOST_LOG_TRIVIAL(info) << "wxMediaCtrl3: PlayThread url_len=" << uri_utf8.size()
                                << ", is_bambu=" << (is_bambu_url ? 1 : 0)
                                << ", is_tutk=" << (is_tutk_url ? 1 : 0)
                                << ", create_loaded=" << (Bambu_Create ? 1 : 0)
                                << ", open_loaded=" << (Bambu_Open ? 1 : 0)
                                << ", start_loaded=" << (Bambu_StartStream ? 1 : 0);
        const int create_attempts = is_tutk_url ? 31 : (is_bambu_url ? 3 : 1);
        const auto create_retry_delay = is_tutk_url ? 500ms : 100ms;
        for (int attempt = 0; attempt < create_attempts; ++attempt) {
            if (!Bambu_Create) {
                BOOST_LOG_TRIVIAL(info) << "wxMediaCtrl3: Bambu_Create function is not available";
                break;
            }
            if (Bambu_Init) {
                int init_error = Bambu_Init();
                BOOST_LOG_TRIVIAL(info) << "wxMediaCtrl3: Bambu_Init attempt=" << attempt << ", error=" << init_error;
            }
            tunnel = nullptr;
            error = Bambu_Create(&tunnel, uri_utf8.c_str());
            BOOST_LOG_TRIVIAL(info) << "wxMediaCtrl3: Bambu_Create attempt=" << attempt << ", error=" << error << ", tunnel=" << (tunnel ? 1 : 0);
            if (error == 0)
                break;
            std::string last_error = wxmedia_bambu_last_error(*this);
            if (!last_error.empty())
                BOOST_LOG_TRIVIAL(info) << "wxMediaCtrl3: Bambu_Create attempt=" << attempt << ", last_error=" << wxmedia_safe_log_text(last_error);
            if (tunnel) {
                if (Bambu_Close) Bambu_Close(tunnel);
                if (Bambu_Destroy) Bambu_Destroy(tunnel);
                tunnel = nullptr;
            }
            if (attempt + 1 >= create_attempts)
                break;
            lk.lock();
            if (m_url != url) {
                error = 1;
                lk.unlock();
                break;
            }
            m_cond.wait_for(lk, create_retry_delay);
            if (m_url != url) {
                error = 1;
                lk.unlock();
                break;
            }
            lk.unlock();
        }
#elif defined(_WIN32)
        if (!Bambu_Create) {
            BOOST_LOG_TRIVIAL(info) << "wxMediaCtrl3: Bambu_Create function is not available";
        } else {
            if (Bambu_Init) {
                int init_error = Bambu_Init();
                BOOST_LOG_TRIVIAL(info) << "wxMediaCtrl3: Bambu_Init error=" << init_error;
            }
            error = Bambu_Create(&tunnel, uri_utf8.data());
            BOOST_LOG_TRIVIAL(info) << "wxMediaCtrl3: Bambu_Create error=" << error << ", tunnel=" << (tunnel ? 1 : 0);
            if (error != 0) {
                std::string last_error = wxmedia_bambu_last_error(*this);
                if (!last_error.empty())
                    BOOST_LOG_TRIVIAL(info) << "wxMediaCtrl3: Bambu_Create last_error=" << wxmedia_safe_log_text(last_error);
            }
        }
#else
        if (!Bambu_Create) {
            BOOST_LOG_TRIVIAL(info) << "wxMediaCtrl3: Bambu_Create function is not available";
        } else {
            error = Bambu_Create(&tunnel, uri_utf8.data());
            BOOST_LOG_TRIVIAL(info) << "wxMediaCtrl3: Bambu_Create error=" << error << ", tunnel=" << (tunnel ? 1 : 0);
            if (error != 0) {
                std::string last_error = wxmedia_bambu_last_error(*this);
                if (!last_error.empty())
                    BOOST_LOG_TRIVIAL(info) << "wxMediaCtrl3: Bambu_Create last_error=" << wxmedia_safe_log_text(last_error);
            }
        }
#endif
        if (error == 0) {
            if (Bambu_SetLogger)
                Bambu_SetLogger(tunnel, &wxMediaCtrl3::bambu_log, this);
            error = Bambu_Open ? Bambu_Open(tunnel) : -2;
            auto t1 = std::chrono::steady_clock::now();
            BOOST_LOG_TRIVIAL(info) << "wxMediaCtrl3: Bambu_Open took "
                                    << std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count() << "ms, error=" << error;
            if (error != 0) {
                std::string last_error = wxmedia_bambu_last_error(*this);
                if (!last_error.empty())
                    BOOST_LOG_TRIVIAL(info) << "wxMediaCtrl3: Bambu_Open last_error=" << wxmedia_safe_log_text(last_error);
            }
            if (error == 0)
                error = Bambu_would_block;

            else if (error == -2)
            {
                m_error = error;
                BOOST_LOG_TRIVIAL(info) << "MediaPlayCtrl::DLL load error ";
                if (tunnel) {
                    Bambu_Close(tunnel);
                    Bambu_Destroy(tunnel);
                    tunnel = nullptr;
                }
                lk.lock();
                NotifyStopped();
                continue;
            }
        }
        lk.lock();
        auto t_stream_start = std::chrono::steady_clock::now();
        while (error == int(Bambu_would_block)) {
            m_cond.wait_for(lk, 100ms);
            if (m_url != url) {
                error = 1;
                break;
            }
            lk.unlock();
            error = Bambu_StartStream(tunnel, true);
            lk.lock();
        }
        {
            auto t_stream_end = std::chrono::steady_clock::now();
            BOOST_LOG_TRIVIAL(info) << "wxMediaCtrl3: Bambu_StartStream loop took "
                                    << std::chrono::duration_cast<std::chrono::milliseconds>(t_stream_end - t_stream_start).count()
                                    << "ms, error=" << error;
            if (error != 0) {
                std::string last_error = wxmedia_bambu_last_error(*this);
                if (!last_error.empty())
                    BOOST_LOG_TRIVIAL(info) << "wxMediaCtrl3: Bambu_StartStream last_error=" << wxmedia_safe_log_text(last_error);
            }
        }
        Bambu_StreamInfo info{};
        int video_stream_index = 0;
        int stream_count = 0;
#if !defined(_WIN32) && !defined(__WXMAC__) && !defined(__APPLE__)
        if (error == 0)
            error = Bambu_GetStreamInfo ? Bambu_GetStreamInfo(tunnel, 0, &info) : -2;
#else
        if (error == 0) {
            stream_count = Bambu_GetStreamCount ? Bambu_GetStreamCount(tunnel) : 0;
            BOOST_LOG_TRIVIAL(info) << "wxMediaCtrl3: Bambu_GetStreamCount returned " << stream_count;
            if (stream_count > 0) {
                error = -1;
                for (int i = 0; i < stream_count; ++i) {
                    Bambu_StreamInfo candidate{};
                    int info_error = Bambu_GetStreamInfo(tunnel, i, &candidate);
                    BOOST_LOG_TRIVIAL(info) << "wxMediaCtrl3: stream " << i
                                            << " info_error=" << info_error
                                            << " type=" << candidate.type
                                            << " sub_type=" << candidate.sub_type
                                            << " width=" << (candidate.type == VIDE ? candidate.format.video.width : 0)
                                            << " height=" << (candidate.type == VIDE ? candidate.format.video.height : 0)
                                            << " frame_rate=" << (candidate.type == VIDE ? candidate.format.video.frame_rate : 0);
                    if (info_error == 0 && candidate.type == VIDE) {
                        info = candidate;
                        video_stream_index = i;
                        error = 0;
                        break;
                    }
                }
            } else {
                error = Bambu_GetStreamInfo(tunnel, 0, &info);
            }
        }
#endif
        if (error == 0 && info.type != VIDE) {
            BOOST_LOG_TRIVIAL(warning) << "wxMediaCtrl3: selected stream is not video, type=" << info.type;
            error = -1;
        }
        if (error == 0 && (info.format.video.width <= 0 || info.format.video.height <= 0)) {
            BOOST_LOG_TRIVIAL(warning) << "wxMediaCtrl3: invalid video size " << info.format.video.width << "x" << info.format.video.height;
            error = -1;
        }
        int frame_rate = 30;
        if (error == 0) {
            frame_rate = info.format.video.frame_rate > 0 ? info.format.video.frame_rate : 30;
            if (info.format.video.frame_rate <= 0)
                BOOST_LOG_TRIVIAL(warning) << "wxMediaCtrl3: invalid frame_rate=" << info.format.video.frame_rate << ", using 30";
        }
        AVVideoDecoder decoder;
        if (error == 0) {
            error = decoder.open(info);
        }
        if (error == 0) {
            BOOST_LOG_TRIVIAL(info) << "wxMediaCtrl3: selected video stream " << video_stream_index
                                    << " " << info.format.video.width << "x" << info.format.video.height
                                    << " fps=" << frame_rate
                                    << " sub_type=" << info.sub_type
                                    << " format_type=" << info.format_type;
            m_video_size = { info.format.video.width, info.format.video.height };
            adjust_frame_size(m_frame_size, m_video_size, GetSize());
            NotifyStopped();
            size_t buffer_cap = (size_t) (m_buffer_time * frame_rate / 1000);
            if (buffer_cap == 0) {
                buffer_cap = 1;
            }
            m_frame_buffer.set_capacity(buffer_cap);
            m_get_frame_exit.store(false);
            m_get_frame_thread = std::thread(&wxMediaCtrl3::GetFrameThread, this, frame_rate);
            m_need_refresh.store(false);
            int render_interval = 1000 / (frame_rate + 5);
            if (render_interval < 1)
                render_interval = 1;
            CallAfter([this, render_interval] { m_render_timer.Start(render_interval); });
        }
        Bambu_Sample sample;
        while (error == 0) {
            lk.unlock();
            error = Bambu_ReadSample(tunnel, &sample);
            lk.lock();
            while (error == int(Bambu_would_block)) {
                m_cond.wait_for(lk, 10ms);
                if (m_url != url) {
                    error = 1;
                    break;
                }
                lk.unlock();
                error = Bambu_ReadSample(tunnel, &sample);
                lk.lock();
            }
            if (error == 0) {
                if (stream_count > 1 && sample.itrack != video_stream_index)
                    continue;
                auto frame_size = m_frame_size;
                lk.unlock();
                PlayFrame bm;
                auto start_decode = std::chrono::steady_clock::now();
                decoder.decode(sample);
                auto end_decode = std::chrono::steady_clock::now();
#ifdef _WIN32
                decoder.toWxBitmap(bm, frame_size);
#elif defined(__WXMAC__)
                decoder.toWxImageOwned(bm, frame_size);
#else
                decoder.toWxImage(bm, frame_size);
#endif
                auto end_convert = std::chrono::steady_clock::now();
                int elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_convert - start_decode).count();
                if (elapsed_ms > decode_warn_thres) {
                    BOOST_LOG_TRIVIAL(warning) << "wxMediaCtrl3: decode + convert too long, decode: "
                                               << std::chrono::duration_cast<std::chrono::milliseconds>(end_decode - start_decode).count()
                                               << " convert: " << std::chrono::duration_cast<std::chrono::milliseconds>(end_convert - end_decode).count();
                }
                lk.lock();
                if (m_url != url) {
                    error = 1;
                    break;
                }
                if (bm.IsOk()) {
                    auto now = std::chrono::system_clock::now();
                    frameCount++;
                    auto elapsedTime = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSecondTime).count();
                    if (elapsedTime >= 10000) {
                        int fps = static_cast<int>(frameCount * 1000 / elapsedTime); // 100 is from frameCount * 1000 / elapsedTime * 10 , becasue  calculate the average rate over 10s
                        BOOST_LOG_TRIVIAL(info) << "wxMediaCtrl3:Decode Real Rate: " << fps << " FPS";
                        frameCount     = 0;
                        lastSecondTime = now;
                    }

                    m_frame_buffer.enqueue(bm);
                }
            }
        }
        if (error != 0 && error != 1) {
            std::string last_error = wxmedia_bambu_last_error(*this);
            BOOST_LOG_TRIVIAL(info) << "wxMediaCtrl3: stream finished with error=" << error << (last_error.empty() ? "" : (", last_error=" + wxmedia_safe_log_text(last_error)));
        }
        if (m_get_frame_thread.joinable()) {
            m_get_frame_exit.store(true);
            m_get_frame_thread.join();
        }
        m_frame_buffer.reset();
        if (tunnel) {
            lk.unlock();
            auto t_close_start = std::chrono::steady_clock::now();
            Bambu_Close(tunnel);
            Bambu_Destroy(tunnel);
            auto t_close_end = std::chrono::steady_clock::now();
            auto close_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_close_end - t_close_start).count();
            if (close_ms > 3000) {
                BOOST_LOG_TRIVIAL(warning) << "wxMediaCtrl3: Bambu_Close+Destroy took " << close_ms << "ms (>3s, potential hang source)";
            } else {
                BOOST_LOG_TRIVIAL(info) << "wxMediaCtrl3: Bambu_Close+Destroy took " << close_ms << "ms";
            }
            tunnel = nullptr;
            lk.lock();
        }
        CallAfter([this] { m_render_timer.Stop(); });
        if (m_url == url)
            m_error = error;
        m_frame_size = wxDefaultSize;
        m_video_size = wxDefaultSize;
        NotifyStopped();
    }
#endif
}

void wxMediaCtrl3::NotifyStopped()
{
    m_state = wxMEDIASTATE_STOPPED;
    wxMediaEvent event(wxEVT_MEDIA_STATECHANGED);
    event.SetId(GetId());
    event.SetEventObject(this);
    wxPostEvent(this, event);
}

void wxMediaCtrl3::GetFrameThread(int frame_rate)
{
    PlayFrame temp_frame;
    long long frame_count = 0;
    bool pop_success = false;
    std::chrono::system_clock::time_point first_frame_time;
    while (m_get_frame_exit.load() == false) {
        if (m_frame_buffer.try_dequeue(temp_frame) == true) {
            if (!temp_frame.IsOk()) {
                continue;
            }
            {
                std::unique_lock<std::mutex> lk(m_ui_mutex);
                m_frame = temp_frame;
                m_need_refresh.store(true);
            }
            if (pop_success == false) {
                first_frame_time = std::chrono::system_clock::now();
                pop_success = true;
                frame_count = 0;
                auto play_start = m_play_start_time;
                if (play_start != std::chrono::system_clock::time_point{}) {
                    int ms = (int) std::chrono::duration_cast<std::chrono::milliseconds>(first_frame_time - play_start).count();
                    CallAfter([this, ms] {
                        wxCommandEvent evt(EVT_MEDIA_CTRL_FIRST_FRAME);
                        evt.SetEventObject(this);
                        evt.SetInt(ms);
                        wxPostEvent(this, evt);
                    });
                }
            }
            ++frame_count;
            long long  pts_gap = (frame_count * 1000) / frame_rate;
            auto wake_up_time = first_frame_time + std::chrono::milliseconds(pts_gap);
            std::this_thread::sleep_until(wake_up_time);
        } else {
            if (pop_success == true) {
                BOOST_LOG_TRIVIAL(info) << "wxMediaCtrl3:decode too slow or unsteady network, bitmap buffer running out...";
                pop_success = false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    {
        std::unique_lock<std::mutex> lk(m_ui_mutex);
        m_frame = wxImage(m_idle_image);
        m_need_refresh.store(true);
        CallAfter([this] { Refresh(false); });
    }
}

void wxMediaCtrl3::OnRenderTimer(wxTimerEvent &evt)
{
    if (m_need_refresh.load() == true) {
        Refresh(false);
        Update();
        m_need_refresh.store(false);
    }
}
