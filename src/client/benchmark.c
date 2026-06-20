/*
Copyright (C) 2026 PenguinBurner contributors.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// PenguinBurner benchmark controller.

#include "client.h"

#include <errno.h>
#include <float.h>
#include <limits.h>

#ifdef _WIN32
#include <io.h>
#define PB_WRITE _write
#else
#include <unistd.h>
#define PB_WRITE write
#endif

#define PB_BENCHMARK_MAX_RUNS 1000000

static cvar_t *bench;
static cvar_t *bench_seconds;
static cvar_t *bench_demo;
static cvar_t *bench_headless;
static cvar_t *bench_constant_load;
static cvar_t *bench_min_loops;
static cvar_t *bench_width;
static cvar_t *bench_height;
static cvar_t *bench_event_fd;
static cvar_t *bench_event_stdout;

typedef struct {
    bool        active;
    bool        launched;
    bool        finished;
    bool        loop_open;
    bool        hold_open;
    int         fd;
    unsigned    start_ms;
    unsigned    measure_start_ms;
    unsigned    loop_start_ms;
    unsigned    target_ms;
    unsigned    total_loop_ms;
    unsigned    total_frames;
    unsigned    render_start_ms;
    unsigned    last_render_ms;
    unsigned    render_elapsed_ms;
    unsigned    render_frames;
    unsigned    frame_samples;
    int         completed_loops;
    float       min_fps;
    float       max_fps;
    float       frame_ms_min;
    float       frame_ms_max;
    double      frame_ms_sum;
    float       sample_fps_min;
    float       sample_fps_max;
    double      sample_fps_sum;
    char        demo[MAX_QPATH];
} pb_benchmark_state_t;

static pb_benchmark_state_t pb_benchmark;

static unsigned elapsed_since(unsigned start, unsigned now)
{
    if (!start || now < start) {
        return 0;
    }
    return now - start;
}

static void write_all(int fd, const char *data, size_t len)
{
    while (len) {
        int ret = PB_WRITE(fd, data, len);
        if (ret < 0 && errno == EINTR) {
            continue;
        }
        if (ret <= 0) {
            pb_benchmark.fd = -1;
            Com_EPrintf("bench_event_fd write failed: %s\n", strerror(errno));
            return;
        }
        data += ret;
        len -= ret;
    }
}

static size_t json_escape(char *dst, size_t size, const char *src)
{
    static const char hex[] = "0123456789abcdef";
    size_t len = 0;

    if (!size) {
        return 0;
    }

    for (; *src; src++) {
        unsigned char c = (unsigned char)*src;
        char encoded[6];
        size_t encoded_len = 0;

        switch (c) {
        case '\\':
            encoded[0] = '\\';
            encoded[1] = '\\';
            encoded_len = 2;
            break;
        case '"':
            encoded[0] = '\\';
            encoded[1] = '"';
            encoded_len = 2;
            break;
        case '\b':
            encoded[0] = '\\';
            encoded[1] = 'b';
            encoded_len = 2;
            break;
        case '\f':
            encoded[0] = '\\';
            encoded[1] = 'f';
            encoded_len = 2;
            break;
        case '\n':
            encoded[0] = '\\';
            encoded[1] = 'n';
            encoded_len = 2;
            break;
        case '\r':
            encoded[0] = '\\';
            encoded[1] = 'r';
            encoded_len = 2;
            break;
        case '\t':
            encoded[0] = '\\';
            encoded[1] = 't';
            encoded_len = 2;
            break;
        default:
            if (c < 0x20) {
                encoded[0] = '\\';
                encoded[1] = 'u';
                encoded[2] = '0';
                encoded[3] = '0';
                encoded[4] = hex[c >> 4];
                encoded[5] = hex[c & 15];
                encoded_len = 6;
            } else {
                encoded[0] = c;
                encoded_len = 1;
            }
            break;
        }

        if (len + encoded_len >= size) {
            break;
        }

        memcpy(dst + len, encoded, encoded_len);
        len += encoded_len;
    }

    dst[len] = 0;
    return len;
}

static void emit_event(const char *event, const char *fmt, ...)
{
    char fields[2048];
    char line[4096];
    unsigned now;
    size_t len;
    va_list argptr;

    if (!pb_benchmark.active && strcmp(event, "fatal")) {
        return;
    }

    fields[0] = 0;
    if (fmt && *fmt) {
        va_start(argptr, fmt);
        Q_vscnprintf(fields, sizeof(fields), fmt, argptr);
        va_end(argptr);
    }

    now = Sys_Milliseconds();
    len = Q_scnprintf(line, sizeof(line),
                      "{\"event\":\"%s\",\"time_ms\":%u,\"elapsed_ms\":%u%s%s}\n",
                      event, now, elapsed_since(pb_benchmark.start_ms, now),
                      fields[0] ? "," : "", fields);

    if (len >= sizeof(line)) {
        Com_EPrintf("Benchmark event too large, dropping %s\n", event);
        return;
    }

    if (pb_benchmark.fd >= 0) {
        write_all(pb_benchmark.fd, line, len);
    }

    if (bench_event_stdout && bench_event_stdout->integer) {
        fputs(line, stdout);
        fflush(stdout);
    }
}

static void emit_simple_string_event(const char *event, const char *key,
                                     const char *value)
{
    char escaped[MAX_QPATH * 2];

    json_escape(escaped, sizeof(escaped), value);
    emit_event(event, "\"%s\":\"%s\"", key, escaped);
}

static bool valid_demo_name(const char *name)
{
    if (!name || !*name) {
        return false;
    }

    return !strpbrk(name, "\";\r\n");
}

static int loops_started(void)
{
    return pb_benchmark.completed_loops + (pb_benchmark.loop_open ? 1 : 0);
}

static void finish_benchmark(const char *reason)
{
    float loop_mean_fps;
    float render_avg_fps;
    float frame_ms_mean;
    float sample_fps_mean;
    float fps_min;
    float fps_max;
    unsigned measured_ms;
    unsigned drain_ms = 0;

    if (pb_benchmark.finished) {
        return;
    }

    if (R_BenchmarkWaitIdle) {
        unsigned before = Sys_Milliseconds();
        R_BenchmarkWaitIdle();
        drain_ms = elapsed_since(before, Sys_Milliseconds());
        if (pb_benchmark.render_start_ms) {
            pb_benchmark.render_elapsed_ms =
                elapsed_since(pb_benchmark.render_start_ms, Sys_Milliseconds());
        }
    }

    measured_ms = pb_benchmark.render_elapsed_ms ?
                  pb_benchmark.render_elapsed_ms : pb_benchmark.total_loop_ms;
    loop_mean_fps = (float)pb_benchmark.total_frames * 1000.0f /
                    max(pb_benchmark.total_loop_ms, 1u);
    render_avg_fps = (float)pb_benchmark.render_frames * 1000.0f /
                     max(measured_ms, 1u);
    frame_ms_mean = pb_benchmark.frame_samples ?
                    (float)(pb_benchmark.frame_ms_sum / pb_benchmark.frame_samples) : 0.0f;
    sample_fps_mean = pb_benchmark.frame_samples ?
                      (float)(pb_benchmark.sample_fps_sum / pb_benchmark.frame_samples) :
                      render_avg_fps;
    fps_min = pb_benchmark.frame_samples ? pb_benchmark.sample_fps_min : render_avg_fps;
    fps_max = pb_benchmark.frame_samples ? pb_benchmark.sample_fps_max : render_avg_fps;

    pb_benchmark.finished = true;
    emit_event("done",
               "\"reason\":\"%s\",\"loops\":%d,\"loops_started\":%d,"
               "\"demo_frames\":%u,\"render_frames\":%u,"
               "\"target_ms\":%u,\"measured_ms\":%u,\"render_ms\":%u,\"drain_ms\":%u,"
               "\"loop_fps_mean\":%.3f,\"fps_avg\":%.3f,"
               "\"fps_min\":%.3f,\"fps_max\":%.3f,\"fps_mean\":%.3f,"
               "\"frame_ms_min\":%.3f,\"frame_ms_max\":%.3f,\"frame_ms_mean\":%.3f",
               reason,
               pb_benchmark.completed_loops,
               loops_started(),
               pb_benchmark.total_frames,
               pb_benchmark.render_frames,
               pb_benchmark.target_ms,
               measured_ms,
               pb_benchmark.render_elapsed_ms,
               drain_ms,
               loop_mean_fps,
               render_avg_fps,
               fps_min,
               fps_max,
               sample_fps_mean,
               pb_benchmark.frame_samples ? pb_benchmark.frame_ms_min : 0.0f,
               pb_benchmark.frame_ms_max,
               frame_ms_mean);
    Cvar_Set("timedemo", "0");
    Cbuf_InsertText(&cmd_buffer, "quit\n");
}

void CL_BenchmarkInit(void)
{
    bench = Cvar_Get("bench", "0", CVAR_NOARCHIVE);
    bench_seconds = Cvar_Get("bench_seconds", "60", CVAR_NOARCHIVE);
    bench_demo = Cvar_Get("bench_demo", "q2demo1", CVAR_NOARCHIVE);
    bench_headless = Cvar_Get("bench_headless", "1", CVAR_NOARCHIVE);
    bench_constant_load = Cvar_Get("bench_constant_load", "1", CVAR_NOARCHIVE);
    bench_min_loops = Cvar_Get("bench_min_loops", "1", CVAR_NOARCHIVE);
    bench_width = Cvar_Get("bench_width", "3840", CVAR_NOARCHIVE);
    bench_height = Cvar_Get("bench_height", "2160", CVAR_NOARCHIVE);
    bench_event_fd = Cvar_Get("bench_event_fd", "-1", CVAR_NOARCHIVE);
    bench_event_stdout = Cvar_Get("bench_event_stdout", "0", CVAR_NOARCHIVE);
}

bool CL_BenchmarkActive(void)
{
    return pb_benchmark.active;
}

bool CL_BenchmarkFinished(void)
{
    return pb_benchmark.finished;
}

bool CL_BenchmarkHeadless(void)
{
    return bench && bench->integer && bench_headless && bench_headless->integer;
}

bool CL_BenchmarkHoldAfterDemo(void)
{
    return pb_benchmark.active && !pb_benchmark.finished &&
           bench_constant_load && bench_constant_load->integer;
}

int CL_BenchmarkWidth(void)
{
    return bench_width ? Cvar_ClampInteger(bench_width, 320, 8192) : 3840;
}

int CL_BenchmarkHeight(void)
{
    return bench_height ? Cvar_ClampInteger(bench_height, 240, 8192) : 2160;
}

void CL_BenchmarkMaybeStart(void)
{
    float seconds;
    int min_loops;

    if (!bench || !bench->integer || pb_benchmark.launched) {
        return;
    }

    memset(&pb_benchmark, 0, sizeof(pb_benchmark));
    pb_benchmark.active = true;
    pb_benchmark.launched = true;
    pb_benchmark.fd = bench_event_fd->integer;
    pb_benchmark.start_ms = Sys_Milliseconds();
    pb_benchmark.min_fps = FLT_MAX;
    pb_benchmark.frame_ms_min = FLT_MAX;
    pb_benchmark.sample_fps_min = FLT_MAX;
    Q_strlcpy(pb_benchmark.demo, bench_demo->string, sizeof(pb_benchmark.demo));

    if (!valid_demo_name(pb_benchmark.demo)) {
        emit_simple_string_event("fatal", "message", "invalid bench_demo");
        pb_benchmark.finished = true;
        Cbuf_InsertText(&cmd_buffer, "quit\n");
        return;
    }

    seconds = Cvar_ClampValue(bench_seconds, 1.0f, 24.0f * 60.0f * 60.0f);
    pb_benchmark.target_ms = (unsigned)(seconds * 1000.0f);
    min_loops = Cvar_ClampInteger(bench_min_loops, 1, PB_BENCHMARK_MAX_RUNS);

    char escaped_demo[MAX_QPATH * 2];
    json_escape(escaped_demo, sizeof(escaped_demo), pb_benchmark.demo);

    emit_event("start",
               "\"demo\":\"%s\",\"target_ms\":%u,\"min_loops\":%d,"
               "\"headless\":%d,\"constant_load\":%d,"
               "\"width\":%d,\"height\":%d,\"quality\":\"rt_max\"",
               escaped_demo,
               pb_benchmark.target_ms,
               min_loops,
               bench_headless->integer ? 1 : 0,
               bench_constant_load->integer ? 1 : 0,
               CL_BenchmarkWidth(),
               CL_BenchmarkHeight());

    if (pb_benchmark.fd < 0 && !bench_event_stdout->integer) {
        Com_WPrintf("bench is enabled without bench_event_fd or bench_event_stdout\n");
    }

    Cbuf_AddText(&cmd_buffer, va("timedemo %d\n", PB_BENCHMARK_MAX_RUNS));
    Cbuf_AddText(&cmd_buffer, va("demo \"%s\"\n", pb_benchmark.demo));
}

void CL_BenchmarkDemoFirstFrame(void)
{
    char escaped_demo[MAX_QPATH * 2];
    unsigned now;

    if (!pb_benchmark.active) {
        return;
    }

    now = Sys_Milliseconds();
    pb_benchmark.loop_start_ms = now;
    pb_benchmark.loop_open = true;

    if (!cls.timedemo.runs_total) {
        cls.timedemo.runs_total = PB_BENCHMARK_MAX_RUNS;
        cls.timedemo.run_current = 0;
    }

    json_escape(escaped_demo, sizeof(escaped_demo), pb_benchmark.demo);
    emit_event("loop_start",
               "\"loop\":%d,\"demo\":\"%s\",\"measured_ms\":%u",
               pb_benchmark.completed_loops + 1,
               escaped_demo,
               pb_benchmark.total_loop_ms);
}

void CL_BenchmarkDemoDone(unsigned frames, unsigned loop_ms)
{
    float fps;
    bool done;

    if (!pb_benchmark.active || pb_benchmark.finished) {
        return;
    }

    if (!loop_ms) {
        loop_ms = 1;
    }

    fps = (float)frames * 1000.0f / loop_ms;
    pb_benchmark.completed_loops++;
    pb_benchmark.total_frames += frames;
    pb_benchmark.total_loop_ms += loop_ms;
    pb_benchmark.loop_open = false;
    pb_benchmark.min_fps = min(pb_benchmark.min_fps, fps);
    pb_benchmark.max_fps = max(pb_benchmark.max_fps, fps);

    emit_event("loop_done",
               "\"loop\":%d,\"frames\":%u,\"duration_ms\":%u,\"fps\":%.3f,"
               "\"measured_ms\":%u",
               pb_benchmark.completed_loops,
               frames,
               loop_ms,
               fps,
               pb_benchmark.total_loop_ms);

    cls.timedemo.run_current++;

    done = pb_benchmark.completed_loops >= bench_min_loops->integer &&
           pb_benchmark.render_elapsed_ms >= pb_benchmark.target_ms;
    if (done) {
        finish_benchmark("target");
        return;
    }

    if (!bench_constant_load->integer) {
        Cbuf_InsertText(&cmd_buffer, va("demo \"%s\"\n", pb_benchmark.demo));
    }
}

void CL_BenchmarkDemoHoldStart(void)
{
    if (!pb_benchmark.active || pb_benchmark.finished || pb_benchmark.hold_open) {
        return;
    }

    pb_benchmark.hold_open = true;
    emit_event("phase",
               "\"name\":\"hold_last_scene\",\"measured_ms\":%u,"
               "\"render_ms\":%u,\"render_frames\":%u",
               pb_benchmark.total_loop_ms,
               pb_benchmark.render_elapsed_ms,
               pb_benchmark.render_frames);
}

void CL_BenchmarkFrameRendered(void)
{
    unsigned now;
    unsigned delta;
    float frame_ms;
    float fps;

    if (!pb_benchmark.active || pb_benchmark.finished ||
        (!pb_benchmark.loop_open && !pb_benchmark.hold_open)) {
        return;
    }

    now = Sys_Milliseconds();
    if (!pb_benchmark.measure_start_ms) {
        pb_benchmark.measure_start_ms = now;
        pb_benchmark.render_start_ms = now;
        pb_benchmark.last_render_ms = now;
        emit_event("phase",
                   "\"name\":\"measure_start\",\"trigger\":\"demo_first_render\"");
    }

    pb_benchmark.render_frames++;
    pb_benchmark.render_elapsed_ms = elapsed_since(pb_benchmark.render_start_ms, now);

    delta = elapsed_since(pb_benchmark.last_render_ms, now);
    if (delta) {
        frame_ms = (float)delta;
        fps = 1000.0f / frame_ms;
        pb_benchmark.frame_samples++;
        pb_benchmark.frame_ms_min = min(pb_benchmark.frame_ms_min, frame_ms);
        pb_benchmark.frame_ms_max = max(pb_benchmark.frame_ms_max, frame_ms);
        pb_benchmark.frame_ms_sum += frame_ms;
        pb_benchmark.sample_fps_min = min(pb_benchmark.sample_fps_min, fps);
        pb_benchmark.sample_fps_max = max(pb_benchmark.sample_fps_max, fps);
        pb_benchmark.sample_fps_sum += fps;
    }
    pb_benchmark.last_render_ms = now;

    if (loops_started() >= bench_min_loops->integer &&
        pb_benchmark.render_elapsed_ms >= pb_benchmark.target_ms) {
        finish_benchmark("target");
    }
}

void CL_BenchmarkFatal(error_type_t code, const char *message)
{
    char escaped[MAXERRORMSG * 2];

    if (!pb_benchmark.active || pb_benchmark.finished) {
        return;
    }

    json_escape(escaped, sizeof(escaped), message ? message : "");
    emit_event("fatal", "\"code\":%d,\"message\":\"%s\"", code, escaped);
    pb_benchmark.finished = true;
}
