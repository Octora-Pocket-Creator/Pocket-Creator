// main/rtttl_player.h —— 轻量 RTTTL 播放器(音效 + 循环背景音乐)。
//
// 复用自 demo/tetris-game 分支并扩展:同一份代码现在同时服务于
//   * 音效:整首立刻播放,新的音效替换旧的(跳跃、里程碑、结束音);
//   * 背景音乐:一次只播一个音符,音效到来时【立即让位】并在音效播完后
//     从下一个音符继续,因此频繁跳跃不会让 BGM 反复从头开始。
// 播放始终在播放器自己的任务里进行,调用方只写队列/指针,不会阻塞按键或 LVGL。
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* 启动播放任务(幂等)。之后才能调用下面两个接口。 */
esp_err_t rtttl_player_start(void);
/* 立刻静音:取消当前音效/背景音乐,任务保留以便下次播放。 */
void rtttl_player_stop(void);

/* 音色:短音效/奖励旋律用方波;加速道具的"冲刺"旋律加一层振幅调制(颤音)。 */
typedef enum {
    RTTTL_VOICE_SQUARE = 0,
    RTTTL_VOICE_TREMOLO,
} rtttl_voice_t;

/* 播放一次音效(整首,替换当前音效)。 */
esp_err_t rtttl_player_play(const char *song);
/* 播放一次高优先级音效(音乐道具的《欢乐颂》):普通音效不会把它打断。
   跑酷里跳跃音效每秒都在响,没有这条规则的话奖励旋律一拿到就被切掉。 */
esp_err_t rtttl_player_play_priority(const char *song);
/* 高优先级 + 指定音色(加速道具用 RTTTL_VOICE_TREMOLO)。 */
esp_err_t rtttl_player_play_voice(const char *song, rtttl_voice_t voice);
/* 设置循环播放的背景音乐;传同一指针不会重头开始,传 NULL 等于停止 BGM。 */
void rtttl_player_set_bgm(const char *song);
/* 参考实现(麦克风回声抑制)使用:当前是否有声音在播。 */
bool rtttl_player_is_playing(void);
