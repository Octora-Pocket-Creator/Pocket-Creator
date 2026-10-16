#include "rtttl_player.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "bsp_audio.h"
#include "esp_log.h"
#include "rtttl_wave.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define RTTTL_CHUNK_SAMPLES 256
#define RTTTL_VOLUME        72
#define RTTTL_AMPLITUDE     4600
/* 背景音乐振幅:与音效同振幅,但走三角波 —— 三角波的 RMS 只有同振幅方波的
   约 0.58 倍,所以听感上仍然更柔、更靠后,同时不会"小到听不见"。
   实测反馈:2200/4200 都偏小,这一版直接给到与音效相同。 */
#define RTTTL_BGM_AMPLITUDE 5200
/* 加速道具的"冲刺"旋律(三角波 + 颤音)的振幅。
   为什么要单独给大一点:三角波的 RMS 只有同振幅方波的 ~0.58 倍,再叠上 25%~100%
   的颤音(平均增益 ~0.65),而且整条旋律比原来低一个八度 —— 三样加起来会比普通
   音效轻一半以上。实机反馈"加了闪电音效声音稍小",所以这里按 RMS 对齐到方波音效。 */
#define RTTTL_TREMOLO_AMPLITUDE 12000
/* 没有 BGM 时的空转周期,也决定音效的最坏响应延迟(毫秒)。 */
#define RTTTL_IDLE_MS       15

static const char *TAG = "rtttl";
static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static volatile uint32_t s_generation;
static volatile bool s_playing;
typedef struct {
    const char *song;
    uint32_t generation;
    rtttl_voice_t voice;
} rtttl_request_t;

typedef struct {
    uint16_t duration;
    uint8_t octave;
    uint16_t bpm;
} rtttl_defaults_t;

typedef struct {
    uint16_t frequency;    /* 0 = 休止符 */
    uint32_t duration_ms;
} rtttl_note_t;

/* 背景音乐状态:s_bgm 由调用方写,其余字段只由播放任务读写。 */
static const char *volatile s_bgm;
static const char *s_bgm_active;    /* 正在循环的那首;与 s_bgm 不一致即需要重新解析 */
static const char *s_bgm_cursor;    /* 下一个待播音符 */
static rtttl_defaults_t s_bgm_defaults;

static const char *skip_spaces(const char *text) {
    while (*text && isspace((unsigned char)*text)) text++;
    return text;
}

static const char *parse_number(const char *text, unsigned *value) {
    unsigned parsed = 0;
    bool found = false;
    while (isdigit((unsigned char)*text)) {
        parsed = parsed * 10U + (unsigned)(*text - '0');
        text++;
        found = true;
    }
    if (found) *value = parsed;
    return text;
}

static const char *parse_defaults(const char *song, rtttl_defaults_t *defaults) {
    defaults->duration = 4;
    defaults->octave = 6;
    defaults->bpm = 63;

    const char *text = song;
    while (*text && *text != ':') text++;
    if (*text != ':') return NULL;
    text++;

    while (*text && *text != ':') {
        text = skip_spaces(text);
        char key = (char)tolower((unsigned char)*text++);
        if (*text != '=') {
            while (*text && *text != ',' && *text != ':') text++;
        } else {
            text++;
            unsigned value = 0;
            text = parse_number(text, &value);
            if (key == 'd' && value > 0 && value <= 64) defaults->duration = (uint16_t)value;
            if (key == 'o' && value >= 2 && value <= 7) defaults->octave = (uint8_t)value;
            if (key == 'b' && value >= 25 && value <= 900) defaults->bpm = (uint16_t)value;
        }
        if (*text == ',') text++;
    }
    return *text == ':' ? text + 1 : NULL;
}

static uint16_t note_frequency(char note, bool sharp, uint8_t octave) {
    static const uint16_t OCTAVE_4[] = {
        262, 277, 294, 311, 330, 349,
        370, 392, 415, 440, 466, 494,
    };
    int semitone;
    switch (note) {
        case 'c': semitone = 0; break;
        case 'd': semitone = 2; break;
        case 'e': semitone = 4; break;
        case 'f': semitone = 5; break;
        case 'g': semitone = 7; break;
        case 'a': semitone = 9; break;
        case 'b': semitone = 11; break;
        default: return 0;
    }
    if (sharp) semitone++;
    if (semitone >= 12) {
        semitone -= 12;
        octave++;
    }
    uint32_t frequency = OCTAVE_4[semitone];
    while (octave > 4) {
        frequency *= 2U;
        octave--;
    }
    while (octave < 4) {
        frequency /= 2U;
        octave++;
    }
    return (uint16_t)frequency;
}

static void write_samples(int16_t *buffer, size_t count) {
    if (count > 0) bsp_audio_write(buffer, count * sizeof(buffer[0]));
}

/* 何时提前结束当前音符:换曲/被新音效打断;背景音乐还要给排队的音效让路。 */
static bool note_should_abort(uint32_t generation, bool background) {
    if (generation != s_generation) return true;
    return background && s_queue != NULL && uxQueueMessagesWaiting(s_queue) > 0;
}

static bool play_note(uint16_t frequency, uint32_t duration_ms, int16_t amplitude,
                      rtttl_wave_t wave, uint32_t generation, bool background) {
    int16_t buffer[RTTTL_CHUNK_SAMPLES];
    const uint32_t total_samples = rtttl_wave_total_samples(duration_ms);
    const uint32_t sound_samples = rtttl_wave_sound_samples(frequency, duration_ms);
    uint32_t phase = 0;
    uint32_t written = 0;

    while (written < total_samples) {
        if (note_should_abort(generation, background)) {
            // 被打断也要淡出:否则方波在中途满幅截断,听感同样是"啪"的一声。
            if (written > 0) {
                // released 的 index 接着本音符的位置走,颤音的调制相位不会跳。
                rtttl_wave_release(frequency, amplitude, wave, written, &phase, buffer);
                write_samples(buffer, RTTTL_WAVE_EDGE_SAMPLES);
            }
            return false;
        }
        size_t count = total_samples - written;
        if (count > RTTTL_CHUNK_SAMPLES) count = RTTTL_CHUNK_SAMPLES;
        for (size_t i = 0; i < count; i++) {
            buffer[i] = rtttl_wave_sample(frequency, amplitude, wave,
                                          written + (uint32_t)i, sound_samples, &phase);
        }
        write_samples(buffer, count);
        written += (uint32_t)count;
    }
    return true;
}

/* 解析一个音符并前移游标;返回 false 表示旋律结束或格式无法继续。 */
static bool parse_note(const char **cursor, const rtttl_defaults_t *defaults,
                       rtttl_note_t *note) {
    const char *text = *cursor;
    if (text == NULL) return false;
    text = skip_spaces(text);
    if (*text == '\0') return false;

    unsigned duration = defaults->duration;
    if (isdigit((unsigned char)*text)) text = parse_number(text, &duration);
    if (duration == 0) duration = defaults->duration;

    char name = (char)tolower((unsigned char)*text++);
    bool sharp = false;
    bool dotted = false;
    uint8_t octave = defaults->octave;
    if (*text == '#') {
        sharp = true;
        text++;
    }
    while (*text && *text != ',') {
        if (*text == '.') dotted = true;
        else if (isdigit((unsigned char)*text)) octave = (uint8_t)(*text - '0');
        text++;
    }

    uint32_t duration_ms = (240000U / defaults->bpm) / duration;
    if (dotted) duration_ms += duration_ms / 2U;

    note->frequency = name == 'p' ? 0 : note_frequency(name, sharp, octave);
    note->duration_ms = duration_ms;
    *cursor = (*text == ',') ? text + 1 : text;
    return true;
}

/* 正在播放的音效指针，用于"同一音效不自我重启"（连按不会嘟嘟嘟）。 */
static const char *volatile s_sfx_song;
/* 正在播的是高优先级音效（音乐道具的旋律）:普通音效在此期间直接丢弃。 */
static volatile bool s_sfx_high;

/* 音效:整首一次播完(除非被更新的音效替换)。voice 决定音色。 */
static void play_song(const char *song, uint32_t generation, rtttl_voice_t voice) {
    rtttl_defaults_t defaults;
    const char *text = parse_defaults(song, &defaults);
    if (!text) {
        ESP_LOGW(TAG, "invalid RTTTL string");
        return;
    }

    const bool tremolo = voice == RTTTL_VOICE_TREMOLO;
    const rtttl_wave_t wave = tremolo ? RTTTL_WAVE_TREMOLO : RTTTL_WAVE_SQUARE;
    // 不同载波的 RMS 差很多,振幅按音色分别给(见文件头的常量说明)。
    const int16_t amplitude = tremolo ? RTTTL_TREMOLO_AMPLITUDE : RTTTL_AMPLITUDE;
    s_sfx_song = song;
    rtttl_note_t note;
    while (parse_note(&text, &defaults, &note)) {
        if (!play_note(note.frequency, note.duration_ms, amplitude,
                       wave, generation, false)) {
            break;
        }
    }
    if (s_sfx_song == song) s_sfx_song = NULL;
}

/* 背景音乐:一次一个音符,音符之间回到任务主循环处理排队的音效。 */
static void play_bgm_step(void) {
    const char *song = s_bgm;
    if (song == NULL) {
        s_bgm_active = NULL;
        s_bgm_cursor = NULL;
        vTaskDelay(pdMS_TO_TICKS(RTTTL_IDLE_MS));
        return;
    }
    if (song != s_bgm_active) {
        const char *notes = parse_defaults(song, &s_bgm_defaults);
        s_bgm_active = song;
        s_bgm_cursor = notes;
        if (notes == NULL) {
            ESP_LOGW(TAG, "invalid BGM string");
            s_bgm = NULL;
            return;
        }
    }

    rtttl_note_t note;
    if (!parse_note(&s_bgm_cursor, &s_bgm_defaults, &note)) {
        s_bgm_active = NULL;               /* 一轮播完:下一轮从头循环 */
        return;
    }
    s_playing = true;
    (void)play_note(note.frequency, note.duration_ms, RTTTL_BGM_AMPLITUDE,
                    RTTTL_WAVE_TRIANGLE, s_generation, true);
    s_playing = false;
}

static void player_task(void *arg) {
    (void)arg;
    if (bsp_audio_set_format(RTTTL_WAVE_SAMPLE_RATE, 16, 1) != ESP_OK) {
        ESP_LOGE(TAG, "audio format setup failed");
    } else {
        bsp_audio_set_volume(RTTTL_VOLUME);
    }

    rtttl_request_t request;
    for (;;) {
        /* 音效优先:队列里有时先播音效(此时 BGM 的音符已被打断),
           队列空则推进 BGM 一个音符。 */
        if (xQueueReceive(s_queue, &request, 0) == pdTRUE) {
            if (request.song) {
                s_playing = true;
                play_song(request.song, request.generation, request.voice);
                s_playing = false;
                /* 只有"当前这一代"结束时才解除保护：期间若又来了新的高优先级曲目，
                   s_sfx_high 已经被那个请求接管了。 */
                if (request.generation == s_generation) s_sfx_high = false;
            }
            continue;
        }
        play_bgm_step();
    }
}

esp_err_t rtttl_player_start(void) {
    if (s_task) return ESP_OK;
    s_queue = xQueueCreate(4, sizeof(rtttl_request_t));
    if (!s_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreate(player_task, "rtttl", 3072, NULL, 4, &s_task) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void rtttl_player_stop(void) {
    s_generation++;
    if (s_queue) xQueueReset(s_queue);
    s_sfx_high = false;
    s_bgm = NULL;                          /* 静音也包含停掉背景音乐 */
    s_bgm_active = NULL;
    s_bgm_cursor = NULL;
}

void rtttl_player_set_bgm(const char *song) {
    if (s_bgm == song) return;             /* 同一首:不要重头开始 */
    s_generation++;                        /* 让正在播的音符立刻退出 */
    s_bgm_active = NULL;
    s_bgm_cursor = NULL;
    s_bgm = song;
}

/* 音效入队：普通音效与高优先级旋律（音乐道具/加速道具）共用一套。
   high 的那一路不会被普通音效替换：跑酷里跳跃音效每秒都在响，
   否则这两段旋律刚响一声就被切掉了。 */
static esp_err_t rtttl_player_play_ex(const char *song, bool high, rtttl_voice_t voice) {
    if (!s_queue || !song) return ESP_ERR_INVALID_STATE;
    if (s_sfx_high && !high) return ESP_OK;
    /* 同一个音效还在响就不要再起一遍:连续跳跃时那一串"嘟嘟嘟"就是这么来的。 */
    if (song == s_sfx_song) return ESP_OK;
    /* A newer game effect is more useful than a delayed one. Cancel the
       current melody and discard queued effects before starting this song. */
    uint32_t generation = ++s_generation;
    xQueueReset(s_queue);
    s_sfx_high = high;
    rtttl_request_t request = { .song = song, .generation = generation, .voice = voice };
    return xQueueSend(s_queue, &request, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t rtttl_player_play(const char *song) {
    return rtttl_player_play_ex(song, false, RTTTL_VOICE_SQUARE);
}

esp_err_t rtttl_player_play_priority(const char *song) {
    return rtttl_player_play_ex(song, true, RTTTL_VOICE_SQUARE);
}

esp_err_t rtttl_player_play_voice(const char *song, rtttl_voice_t voice) {
    return rtttl_player_play_ex(song, true, voice);
}

bool rtttl_player_is_playing(void) {
    return s_playing;
}
