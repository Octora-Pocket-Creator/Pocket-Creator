#!/usr/bin/env python3
"""把固件里的 RTTTL 音效在本地合成为 WAV 试听(开发/调音用)。

为什么要这个工具:音效好不好听,只有在耳朵里才能判断 —— 每改一个音符就烧一次板、
烧到设备上听,迭代太慢。这里把 main/chick_run_ui.c 里的旋律字符串(和固件同一份,
不存在两处不一致)按【和固件完全相同的算法】合成出来:

    * 采样率 16000 Hz、单声道 16 bit(见 main/rtttl_wave.h);
    * 每个音符 = 发声段 + 尾部留白(发声占比 85%,见 rtttl_wave.c);
    * 首尾各 48 个采样线性淡入淡出(否则音符边界会"啪"一声);
    * 波形:方波(RTTTL_AMPLITUDE)/ 三角波 + 振幅调制(颤音,单独振幅);
    * 每个音符的相位从 0 开始(和 play_note() 一致)。

用法:
    python3 tools/rtttl_preview.py [输出目录]      # 默认 build/sfx-preview/

生成完成后直接播放:
    afplay build/sfx-preview/note.wav             # 音乐道具《欢乐颂》
    afplay build/sfx-preview/twinkle.wav          # 音乐道具《小星星》
    afplay build/sfx-preview/boost-boost.wav      # 加速道具的"冲刺"旋律(已按响度对齐)
    open  build/sfx-preview/                       # 或者双击试听全部
"""

from __future__ import annotations

import re
import struct
import sys
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
UI_SOURCE = ROOT / "main" / "chick_run_ui.c"

# --- 与固件一致的合成参数(main/rtttl_wave.*、main/rtttl_player.c)--------------
SAMPLE_RATE = 16000
EDGE_SAMPLES = 48
SOUND_PERCENT = 85
TREMOLO_HZ = 11
TREMOLO_MIN = 64
AMPLITUDE_SQUARE = 4600
AMPLITUDE_TRIANGLE = 5200        # 背景音乐(这里只作参考)
AMPLITUDE_TREMOLO = 12000        # 加速道具的"冲刺"旋律

WAVE_SQUARE, WAVE_TRIANGLE, WAVE_TREMOLO = 0, 1, 2

SFX_RE = re.compile(
    r'static const char \*const (SFX_\w+)\s*=\s*((?:"(?:[^"\\]|\\.)*"\s*)+);', re.S
)
STRING_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')
DEFAULTS_RE = re.compile(r"d=(\d+)\s*,\s*o=(\d+)\s*,\s*b=(\d+)")
NOTE_RE = re.compile(r"^(\d*)([a-gp]#?)(\d*)(\.?)$")

# 半音 -> OCTAVE_4 表(与 rtttl_player.c 的 note_frequency 相同,C4 = 262Hz)。
SEMITONE = {"c": 0, "d": 2, "e": 4, "f": 5, "g": 7, "a": 9, "b": 11}
OCTAVE_4 = (262, 277, 294, 311, 330, 349, 370, 392, 415, 440, 466, 494)

def c_div(numerator: int, denominator: int) -> int:
    """C 的整数除法(向 0 截断):负值上与 Python 的 // 差 1,合成要一致。"""
    quotient = abs(numerator) // denominator
    return quotient if numerator >= 0 else -quotient


def melodies() -> dict[str, str]:
    """main/chick_run_ui.c 里的 SFX_* 旋律(固件的唯一真源)。"""
    text = UI_SOURCE.read_text(encoding="utf-8")
    return {match.group(1): "".join(STRING_RE.findall(match.group(2)))
            for match in SFX_RE.finditer(text)}


def parse(song: str) -> tuple[list[tuple[str, int, bool]], dict[str, int]]:
    """RTTTL -> (音符列表, 头部默认值)。音符 = (音名, 八度, 附点)。"""
    parts = song.split(":", 2)
    if len(parts) != 3:
        raise SystemExit(f"{song}: 不是 `名称:默认值:音符` 三段式")
    _name, defaults_text, body = parts
    defaults = DEFAULTS_RE.search(defaults_text)
    if defaults is None:
        raise SystemExit(f"{song}: 默认值段里没有 d=/o=/b=")
    duration, octave, bpm = (int(value) for value in defaults.groups())
    notes: list[tuple[str, int, bool]] = []
    for raw in body.split(","):
        token = raw.strip()
        match = NOTE_RE.match(token)
        if match is None:
            raise SystemExit(f"{song}: 解析不了的音符 {token!r}")
        notes.append((match.group(2), int(match.group(3)) if match.group(3) else octave,
                      bool(match.group(4))))
    return notes, {"duration": duration, "octave": octave, "bpm": bpm,
                   "lengths": [int(NOTE_RE.match(t.strip()).group(1) or duration)
                               for t in body.split(",")]}


def frequency(name: str, octave: int) -> int:
    if name == "p":
        return 0
    semitone = SEMITONE[name[0]]
    if name.endswith("#"):
        semitone += 1
    if semitone >= 12:
        semitone -= 12
        octave += 1
    value = OCTAVE_4[semitone]
    while octave > 4:
        value *= 2
        octave -= 1
    while octave < 4:
        value //= 2
        octave += 1
    return value


def tremolo_gain(index: int) -> int:
    period = SAMPLE_RATE // TREMOLO_HZ
    half = period // 2
    t = index % period
    level = t * 256 // half if t < half else (period - t) * 256 // half
    return TREMOLO_MIN + (256 - TREMOLO_MIN) * level // 256


def wave_raw(phase: int, index: int, amplitude: int, wave: int) -> int:
    if wave in (WAVE_TRIANGLE, WAVE_TREMOLO):
        half = SAMPLE_RATE // 2
        raw = (phase * amplitude // half if phase < half
               else (SAMPLE_RATE - phase) * amplitude // half)
    else:
        raw = amplitude if phase < SAMPLE_RATE // 2 else -amplitude
    if wave == WAVE_TREMOLO:
        raw = c_div(raw * tremolo_gain(index), 256)
    return raw


def edge_gain(index: int, length: int) -> int:
    if length == 0:
        return 0
    gain = 256
    if index < EDGE_SAMPLES:
        gain = gain * index // EDGE_SAMPLES
    clamped = index if index < length else length - 1
    remaining = length - 1 - clamped
    if remaining < EDGE_SAMPLES:
        gain = gain * remaining // EDGE_SAMPLES
    return gain


def render(song: str, wave: int, amplitude: int) -> bytes:
    """按固件的算法把一条旋律合成成 16bit 单声道 PCM。"""
    notes, meta = parse(song)
    pcm = bytearray()
    for (name, octave, dotted), length in zip(notes, meta["lengths"]):
        duration_ms = (240000 // meta["bpm"]) // length
        if dotted:
            duration_ms += duration_ms // 2
        total = SAMPLE_RATE * duration_ms // 1000
        freq = frequency(name, octave)
        sound = 0 if freq == 0 else SAMPLE_RATE * duration_ms * SOUND_PERCENT // 100000
        phase = 0
        for index in range(total):
            if index < sound:
                raw = wave_raw(phase, index, amplitude, wave)
                value = c_div(raw * edge_gain(index, sound), 256)
                phase += freq
                if phase >= SAMPLE_RATE:
                    phase -= SAMPLE_RATE
            else:
                value = 0
            pcm += struct.pack("<h", max(-32768, min(32767, value)))
    return bytes(pcm)


def write_wav(path: Path, pcm: bytes) -> None:
    with wave.open(str(path), "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(SAMPLE_RATE)
        handle.writeframes(pcm)


def describe(song: str) -> str:
    notes, meta = parse(song)
    total_ms = 0
    spelled = []
    for (name, octave, dotted), length in zip(notes, meta["lengths"]):
        duration_ms = (240000 // meta["bpm"]) // length
        if dotted:
            duration_ms += duration_ms // 2
        total_ms += duration_ms
        spelled.append(f"{name}{octave}" if not dotted else f"{name}{octave}.")
    return f"{len(notes)} 个音 / {total_ms} ms: " + " ".join(spelled)


def main() -> int:
    out_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build" / "sfx-preview"
    out_dir.mkdir(parents=True, exist_ok=True)

    jobs: list[tuple[str, str, int, int]] = []
    for name, song in sorted(melodies().items()):
        label = name.removeprefix("SFX_").lower()
        if "BOOST" in name:
            jobs.append((f"boost-{label}", song, WAVE_TREMOLO, AMPLITUDE_TREMOLO))
        else:
            jobs.append((label, song, WAVE_SQUARE, AMPLITUDE_SQUARE))
    print()
    for name, song, wave, amplitude in jobs:
        pcm = render(song, wave, amplitude)
        path = out_dir / f"{name}.wav"
        write_wav(path, pcm)
        print(f"{path.relative_to(ROOT)}  {describe(song)}")

    print(f"\n共 {len(jobs)} 个文件,输出目录 {out_dir}")
    print(f"macOS 试听:afplay {out_dir}/twinkle.wav  或  open {out_dir}")
    print("注:WAV 的音量差就是固件里的相对响度(闪电旋律已按 RMS 对齐到普通音效)。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
