// vodsync - libmpv playback, rendered into a raylib framebuffer.
//
// Each pane owns an mpv instance with vo=libmpv and an OpenGL render context
// pointed at a raylib RenderTexture2D's FBO. Rendering happens before
// BeginDrawing so mpv never fights raylib's own draw batch; the resulting
// texture is then drawn like any other by the Clay CUSTOM handler.
#include "vs_player.h"
#include <mpv/client.h>
#include <mpv/render_gl.h>

#ifdef _WIN32
// raylib and windows.h collide on these three names. raylib's versions are the
// ones this file uses, so rename the Win32 ones out of the way; we still need
// wingdi.h for wglGetProcAddress.
#define WIN32_LEAN_AND_MEAN
#define Rectangle   Win32Rectangle
#define CloseWindow Win32CloseWindow
#define ShowCursor  Win32ShowCursor
#include <windows.h>
#undef Rectangle
#undef CloseWindow
#undef ShowCursor
#else
#include <dlfcn.h>
#endif
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "rlgl.h"

#define VS_SEEK_REPLY 0x5EE4

struct vs_pane_gpu {
    mpv_handle         *mpv;
    mpv_render_context *rctx;
    RenderTexture2D     tex;
    int                 w, h;
    bool                have_frame;
    bool                loaded;        // mpv has opened the manifest
    bool                have_pending;  // a seek arrived before that
    double              pending_seek;
    bool                paused;
    bool                render_warned;
    int                 probe_left;
    long long           render_count;
    double              volume;
    double              speed;
};

// mpv asks us to resolve GL symbols; raylib's context is already current.
#ifdef _WIN32
static void *gl_get_proc(void *ctx, const char *name) {
    (void)ctx;
    void *p = (void *)wglGetProcAddress(name);
    if (!p) {
        static HMODULE gl = NULL;
        if (!gl) gl = LoadLibraryA("opengl32.dll");
        if (gl) p = (void *)GetProcAddress(gl, name);
    }
    return p;
}
#else
// Asked for by name rather than linked against: whether the window system
// gave us GLX or EGL is GLFW's business, and both loaders are resolved out
// of the already-loaded GL stack rather than by opening a library by name.
typedef void *(*gl_getproc_fn)(const char *);

static void *gl_get_proc(void *ctx, const char *name) {
    (void)ctx;
    static gl_getproc_fn glx, egl;
    static bool looked;
    if (!looked) {
        looked = true;
        glx = (gl_getproc_fn)dlsym(RTLD_DEFAULT, "glXGetProcAddressARB");
        if (!glx) glx = (gl_getproc_fn)dlsym(RTLD_DEFAULT, "glXGetProcAddress");
        egl = (gl_getproc_fn)dlsym(RTLD_DEFAULT, "eglGetProcAddress");
    }
    void *p = NULL;
    if (glx) p = glx(name);
    if (!p && egl) p = egl(name);
    if (!p) p = dlsym(RTLD_DEFAULT, name);
    return p;
}
#endif

// Hardware decode, in priority order, constrained by what the OpenGL render
// API can actually map:
//   nvdec        - NVIDIA, decodes on the GPU and shares frames with GL via
//                  CUDA interop. The fast path on this machine.
//   vaapi/vulkan - other GPU paths that expose GL-importable surfaces.
//   auto-copy    - still decodes on the GPU but copies frames back through
//                  system memory; slower, but universally compatible.
// d3d11va is deliberately absent: its frames live in D3D11 textures that the
// GL interop cannot import, so it decodes fine and then renders nothing.
#ifdef _WIN32
#define VS_HWDEC "nvdec,vulkan,vaapi,auto-copy"
#else
// On Linux - and so on the Steam Deck, whose RDNA2 block decodes through
// VAAPI - the zero-copy path needs an EGL context; vaapi-copy works whatever
// GLFW gave us, at the cost of a trip through system memory.
#define VS_HWDEC "vaapi,vaapi-copy,auto-copy"
#endif

static void set_opt(mpv_handle *m, const char *k, const char *v) {
    mpv_set_option_string(m, k, v);
}

// The manifest is the ffconcat this app wrote for a VOD whose recording the
// feed dropped out of (see vs_sections_write), not a playlist URL.
static bool manifest_is_concat(const char *path) {
    size_t n = strlen(path);
    return n > 9 && strcmp(path + n - 9, ".ffconcat") == 0;
}

bool vs_player_open(vs_source *s) {
    if (s->gpu) return true;
    if (s->role != VS_ROLE_VIDEO || !s->manifest[0]) return false;

    struct vs_pane_gpu *g = (struct vs_pane_gpu *)calloc(1, sizeof *g);
    g->volume = 0.0;      // start silent; vs_apply_audio sets the real level
    g->speed  = 1.0;
    g->paused = true;
    g->probe_left = 3;

    g->mpv = mpv_create();
    if (!g->mpv) {
        free(g);
        snprintf(s->status, sizeof s->status, "mpv_create failed (libmpv-2.dll missing?)");
        s->state = VS_LOAD_FAILED;
        return false;
    }

    set_opt(g->mpv, "vo", "libmpv");
    set_opt(g->mpv, "terminal", "no");
    set_opt(g->mpv, "msg-level", "all=no");
    set_opt(g->mpv, "idle", "yes");
    set_opt(g->mpv, "keep-open", "yes");
    const char *hw_override = getenv("VODSYNC_HWDEC");
    set_opt(g->mpv, "hwdec", hw_override ? hw_override : VS_HWDEC);   // GL-compatible: frames
                                          // are copied back, unlike
                                          // d3d11va which GL cannot map
    set_opt(g->mpv, "hr-seek", "yes");            // exact seeks, not segment-rounded
    set_opt(g->mpv, "hr-seek-framedrop", "no");
    // The demuxer cache is a fixed ring buffer, so memory is bounded by these
    // numbers rather than by how long the VOD is. They are per pane, so the
    // defaults are sized for four panes rather than one. Measured: ~1.0 GB
    // working set for two panes at 192/64, ~0.6 GB at 96/32.
    // Override with VODSYNC_CACHE_MB (forward size; back gets a third).
    int cache_mb = 96;
    const char *cache_env = getenv("VODSYNC_CACHE_MB");
    if (cache_env) {
        int v = atoi(cache_env);
        if (v >= 16 && v <= 2048) cache_mb = v;
    }
    char fwd[32], back[32];
    snprintf(fwd,  sizeof fwd,  "%dMiB", cache_mb);
    snprintf(back, sizeof back, "%dMiB", cache_mb / 3 > 8 ? cache_mb / 3 : 8);
    set_opt(g->mpv, "cache", "yes");
    set_opt(g->mpv, "cache-on-disk", "no");   // nothing is ever written to disk
    set_opt(g->mpv, "demuxer-max-bytes", fwd);
    set_opt(g->mpv, "demuxer-max-back-bytes", back);
    set_opt(g->mpv, "audio-client-name", "vodsync");
    set_opt(g->mpv, "volume", "0");
    set_opt(g->mpv, "pause", "yes");
    // An in-progress VOD has no EXT-X-ENDLIST, so mpv would treat it as a live
    // stream and jump to the live edge. We always position explicitly.
    set_opt(g->mpv, "start", "0");
    if (s->live) {
        // The playlist the app writes for a live stream has no ENDLIST either,
        // so ffmpeg's HLS reader would start three segments from its end
        // instead of at segment 0 - the one segment whose PROGRAM-DATE-TIME we
        // know, and the whole basis for aligning the panes.
        // Two options, and both are load-bearing:
        //
        //   live_start_index=0   without it ffmpeg starts three segments from
        //                        the end of our playlist instead of at segment
        //                        0, the one whose PDT we know.
        //   protocol_whitelist   a playlist opened from a *file* may only pull
        //                        nested URLs over file, crypto and data, so
        //                        every https segment in it is refused:
        //                        "Protocol 'https' not on whitelist". Windows'
        //                        older libmpv did not enforce this; 2.5 does,
        //                        and the panes simply never load.
        //
        // The value contains commas, so it uses mpv's length-prefixed form
        // (%35% is the byte count) to keep them out of the key/value split.
        set_opt(g->mpv, "demuxer-lavf-o",
                "live_start_index=0,"
                "protocol_whitelist=%35%file,crypto,data,http,https,tcp,tls");
        // Live panes have no manifest to re-download from, so hold more of
        // what has already been played in memory for scrubbing back.
        set_opt(g->mpv, "demuxer-max-back-bytes", "128MiB");
    }
    if (manifest_is_concat(s->manifest)) {
        // The ffconcat is a local file naming local playlists which in turn
        // name https segments, and a playlist opened from a file may only
        // pull nested URLs over file, crypto and data unless told otherwise -
        // the same wall the live path hits. safe=0 is needed because concat
        // treats any path in its input list as unsafe by default; the file in
        // question is one this app wrote itself, in its own temp directory.
        set_opt(g->mpv, "demuxer-lavf-o",
                "safe=0,protocol_whitelist="
                "%42%file,crypto,data,http,https,tcp,tls,concat");
    }
    if (getenv("VODSYNC_MUTE")) set_opt(g->mpv, "mute", "yes");
    if (getenv("VODSYNC_BG")) {
        set_opt(g->mpv, "background", "color");
        set_opt(g->mpv, "background-color", getenv("VODSYNC_BG"));
    }
    set_opt(g->mpv, "user-agent",
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64)");
    if (s->platform == VS_PLAT_KICK)
        set_opt(g->mpv, "referrer", "https://kick.com/");
    else
        set_opt(g->mpv, "referrer", "https://www.twitch.tv/");

    if (mpv_initialize(g->mpv) < 0) {
        mpv_destroy(g->mpv);
        free(g);
        snprintf(s->status, sizeof s->status, "mpv_initialize failed");
        s->state = VS_LOAD_FAILED;
        return false;
    }

    mpv_opengl_init_params gl_init = { gl_get_proc, NULL };
    // Advanced control lets mpv manage its own timing but makes it far more
    // aggressive with GL state, which corrupts raylib's cached bindings.
    int advanced = 0;
    mpv_render_param params[] = {
        { MPV_RENDER_PARAM_API_TYPE, (void *)MPV_RENDER_API_TYPE_OPENGL },
        { MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init },
        { MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced },
        { 0, NULL }
    };
    if (mpv_render_context_create(&g->rctx, g->mpv, params) < 0) {
        mpv_terminate_destroy(g->mpv);
        free(g);
        snprintf(s->status, sizeof s->status, "mpv GL render context failed");
        s->state = VS_LOAD_FAILED;
        return false;
    }

    if (getenv("VODSYNC_MPV_LOG"))
        mpv_request_log_messages(g->mpv, getenv("VODSYNC_MPV_LOG"));

    mpv_observe_property(g->mpv, 0, "time-pos",      MPV_FORMAT_DOUBLE);
    mpv_observe_property(g->mpv, 0, "paused-for-cache", MPV_FORMAT_FLAG);
    mpv_observe_property(g->mpv, 0, "eof-reached",   MPV_FORMAT_FLAG);

    const char *cmd[] = { "loadfile", s->manifest, "replace", NULL };
    mpv_command_async(g->mpv, 0, cmd);

    s->gpu = g;
    s->tex_ready = false;
    return true;
}

void vs_player_close(vs_source *s) {
    struct vs_pane_gpu *g = s->gpu;
    if (!g) return;
    if (g->rctx) mpv_render_context_free(g->rctx);
    if (g->mpv)  mpv_terminate_destroy(g->mpv);
    if (g->tex.id) UnloadRenderTexture(g->tex);
    free(g);
    s->gpu = NULL;
    s->tex_ready = false;
}

void vs_player_poll(vs_source *s) {
    struct vs_pane_gpu *g = s->gpu;
    if (!g) return;
    for (;;) {
        mpv_event *ev = mpv_wait_event(g->mpv, 0);
        if (!ev || ev->event_id == MPV_EVENT_NONE) break;
        switch (ev->event_id) {
            case MPV_EVENT_PROPERTY_CHANGE: {
                mpv_event_property *p = (mpv_event_property *)ev->data;
                if (!p || !p->data) break;
                if (strcmp(p->name, "time-pos") == 0 && p->format == MPV_FORMAT_DOUBLE) {
                    s->pos = *(double *)p->data;
                } else if (strcmp(p->name, "eof-reached") == 0 && p->format == MPV_FORMAT_FLAG) {
                    s->mpv_eof = *(int *)p->data != 0;
                }
                break;
            }
            case MPV_EVENT_LOG_MESSAGE: {
                mpv_event_log_message *lm = (mpv_event_log_message *)ev->data;
                if (lm) fprintf(stderr, "[mpv/%s/%s] %s",
                                lm->prefix, lm->level, lm->text);
                break;
            }
            case MPV_EVENT_FILE_LOADED:
                g->loaded = true;
                if (g->have_pending) {
                    g->have_pending = false;
                    vs_player_seek(s, g->pending_seek);
                }
                break;
            case MPV_EVENT_COMMAND_REPLY:
                if (ev->reply_userdata == VS_SEEK_REPLY && ev->error < 0) {
                    s->seek_refused = true;
                    fprintf(stderr, "[mpv] seek refused: %s\n",
                            mpv_error_string(ev->error));
                }
                break;
            case MPV_EVENT_END_FILE:
                s->mpv_eof = true;
                break;
            default:
                break;
        }
    }
}


// mpv rebinds textures, shaders, the FBO and assorted toggles. rlgl caches the
// texture and shader it thinks are bound and skips redundant binds, so unless
// that cache is invalidated every later raylib draw samples whatever mpv left
// behind - which shows up as text rendering with solid blocks instead of
// glyphs, and video never appearing at all.

// mpv assumes it gets a clean fixed-function state and does not defensively
// disable what it does not use. raylib leaves alpha blending enabled by
// default, and that alone is enough to make video vanish: glClear ignores
// GL_BLEND, so mpv's background still paints, but its video quad gets
// composited with whatever alpha the shader emits and disappears against the
// black it just cleared to. That is exactly the symptom this had.
static void prepare_gl_for_mpv(void) {
    rlDrawRenderBatchActive();      // flush raylib's queued geometry first

    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_TRUE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilMask(0xFF);

    rlDisableVertexArray();         // GL 3.0 entry point, so go through rlgl
}

static void restore_raylib_gl_state(void) {
    rlDisableFramebuffer();
    rlDisableScissorTest();
    rlDisableDepthTest();
    rlDisableDepthMask();

    // Force rlgl to forget its cached texture and shader bindings.
    rlSetTexture(0);
    rlDisableVertexArray();
    rlDisableVertexBuffer();
    rlDisableVertexBufferElement();
    rlDisableShader();
    rlEnableShader(rlGetShaderIdDefault());

    rlEnableColorBlend();
    rlSetBlendMode(RL_BLEND_ALPHA);
    rlViewport(0, 0, GetScreenWidth(), GetScreenHeight());
}

void vs_player_render(vs_source *s, int w, int h) {
    struct vs_pane_gpu *g = s->gpu;
    if (!g || w <= 1 || h <= 1) return;

    if (g->tex.id == 0 || g->w != w || g->h != h) {
        if (g->tex.id) UnloadRenderTexture(g->tex);
        g->tex = LoadRenderTexture(w, h);
        g->w = w; g->h = h;
        SetTextureFilter(g->tex.texture, TEXTURE_FILTER_BILINEAR);
    }

    mpv_render_context_update(g->rctx);

    prepare_gl_for_mpv();

    mpv_opengl_fbo fbo;
    memset(&fbo, 0, sizeof fbo);
    fbo.fbo = (int)g->tex.id;
    fbo.w   = w;
    fbo.h   = h;
    int flip = 1;
    mpv_render_param rp[] = {
        { MPV_RENDER_PARAM_OPENGL_FBO, &fbo },
        { MPV_RENDER_PARAM_FLIP_Y,     &flip },
        { 0, NULL }
    };
    int rc = mpv_render_context_render(g->rctx, rp);
    if (rc < 0 && !g->render_warned) {
        g->render_warned = true;
        fprintf(stderr, "[mpv] render failed: %s\n", mpv_error_string(rc));
    }

    // One-shot probe: read straight out of mpv's target FBO, bypassing
    // raylib's texture helpers, to see whether mpv wrote anything at all.
    g->render_count++;
    if (getenv("VODSYNC_PROBE_FBO") && g->render_count % 400 == 0) {
        rlEnableFramebuffer(g->tex.id);
        unsigned char *px = rlReadScreenPixels(w, h);
        rlDisableFramebuffer();
        if (px) {
            long long sum = 0; int nz = 0, n = w * h * 4;
            for (int i = 0; i < n; i += 4) {
                int v = px[i] + px[i+1] + px[i+2];
                sum += v;
                if (v > 12) nz++;
                }
            fprintf(stderr, "[probe] fbo=%u %dx%d direct sum=%lld nonzero=%d"
                    " alphaCorner=%d alphaCentre=%d", (unsigned)g->tex.id, w, h, sum, nz, px[3], px[((h/2)*w + w/2)*4 + 3]);
            fprintf(stderr, "\n");
            RL_FREE(px);
        } else {
            fprintf(stderr, "[probe] rlReadScreenPixels returned NULL\n");
        }
    }

    restore_raylib_gl_state();

    g->have_frame = true;
    s->tex_ready  = true;
}


// Reports what mpv actually decoded, for the self test.
void vs_player_debug(vs_source *s, char *out, int cap) {
    struct vs_pane_gpu *g = s->gpu;
    if (!g) { snprintf(out, cap, "no mpv"); return; }
    int64_t w = 0, h = 0;
    char *codec = NULL, *hw = NULL;
    mpv_get_property(g->mpv, "width",  MPV_FORMAT_INT64, &w);
    mpv_get_property(g->mpv, "height", MPV_FORMAT_INT64, &h);
    mpv_get_property(g->mpv, "video-codec",  MPV_FORMAT_STRING, &codec);
    mpv_get_property(g->mpv, "hwdec-current", MPV_FORMAT_STRING, &hw);
    // Read the FBO back so we can tell "mpv wrote nothing" apart from
    // "we failed to draw what mpv wrote".
    long long sum = 0; int nonzero = 0;
    if (g->tex.id) {
        Image img = LoadImageFromTexture(g->tex.texture);
        if (img.data) {
            Color *px = LoadImageColors(img);
            if (px) {
                int n = img.width * img.height;
                int step = n > 4096 ? n / 4096 : 1;
                for (int i = 0; i < n; i += step) {
                    int v = px[i].r + px[i].g + px[i].b;
                    sum += v;
                    if (v > 12) nonzero++;
                }
                UnloadImageColors(px);
            }
            UnloadImage(img);
        }
    }
    snprintf(out, cap, "loaded=%d %lldx%lld codec=%s hwdec=%s fbo=%u %dx%d "
             "texsum=%lld texnonzero=%d renders=%lld",
             g->loaded ? 1 : 0, (long long)w, (long long)h,
             codec ? codec : "-", hw ? hw : "-",
             (unsigned)g->tex.id, g->w, g->h, sum, nonzero, g->render_count);
    if (codec) mpv_free(codec);
    if (hw) mpv_free(hw);
}

Texture2D *vs_player_texture(vs_source *s) {
    struct vs_pane_gpu *g = s->gpu;
    if (!g || !g->have_frame || g->tex.id == 0) return NULL;
    return &g->tex.texture;
}

bool vs_player_has_frame(const vs_source *s) {
    return s->gpu && s->gpu->have_frame;
}

void vs_player_seek(vs_source *s, double pos) {
    struct vs_pane_gpu *g = s->gpu;
    if (!g) return;
    if (pos < 0) pos = 0;
    if (!g->loaded) {
        // loadfile is async; a seek now would be dropped. Remember it.
        g->have_pending = true;
        g->pending_seek = pos;
        s->pos = pos;
        return;
    }
    char buf[64];
    snprintf(buf, sizeof buf, "%.3f", pos < 0 ? 0 : pos);
    const char *cmd[] = { "seek", buf, "absolute+exact", NULL };
    // Tagged so the reply can be matched in the event loop: a seek mpv
    // refuses is otherwise invisible, and the position simply carries on
    // from where it was.
    mpv_command_async(g->mpv, VS_SEEK_REPLY, cmd);
    s->pos = pos;
}

void vs_player_reload(vs_source *s, double at) {
    struct vs_pane_gpu *g = s->gpu;
    if (!g || !s->manifest[0]) return;
    // Same shape as the first load: mark it unloaded and let the pending-seek
    // path put the playhead back once mpv reports FILE_LOADED. Seeking now
    // would be dropped.
    g->loaded = false;
    g->have_pending = true;
    g->pending_seek = at > 0 ? at : 0;
    s->mpv_eof = false;
    const char *cmd[] = { "loadfile", s->manifest, "replace", NULL };
    mpv_command_async(g->mpv, 0, cmd);
}

void vs_player_set_speed(vs_source *s, double speed) {
    struct vs_pane_gpu *g = s->gpu;
    if (!g) return;
    if (speed < 0.25) speed = 0.25;
    if (speed > 4.0)  speed = 4.0;
    g->speed = speed;
    mpv_set_property_async(g->mpv, 0, "speed", MPV_FORMAT_DOUBLE, &speed);
}

void vs_player_set_volume(vs_source *s, float vol01) {
    struct vs_pane_gpu *g = s->gpu;
    if (!g) return;
    double v = vol01 * 100.0;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    if (fabs(v - g->volume) < 0.5) return;
    g->volume = v;
    mpv_set_property_async(g->mpv, 0, "volume", MPV_FORMAT_DOUBLE, &v);
}

void vs_player_set_paused(vs_source *s, bool paused) {
    struct vs_pane_gpu *g = s->gpu;
    if (!g || g->paused == paused) return;
    g->paused = paused;
    int flag = paused ? 1 : 0;
    mpv_set_property_async(g->mpv, 0, "pause", MPV_FORMAT_FLAG, &flag);
}
