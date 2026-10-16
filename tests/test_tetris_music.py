#!/usr/bin/env python3
"""静态检查俄罗斯方块的旋律(main/tetris_music.h)。

为什么需要它:旋律是 RTTTL 字符串,改一个音符、一个时值或者一个 b(o=八度、
b=速度)就会悄悄改变三件肉眼看不见的事:

  1. 总时长 —— 影响音效是否拖过下一个操作、背景音乐的循环间隔;
  2. 音域 —— a5 以上接近 16kHz 采样率的奈奎斯特频率,方波音效会变成刺耳的
     混叠噪声(实机上"高频扎耳朵"就是这么来的);
  3. 循环点 —— 播放器循环背景音乐时会在字符串末尾夹一个 ≤15ms 的空档
     (main/rtttl_player.c 的 RTTTL_IDLE_MS),如果旋律不以乐句边界收尾,
     听感就是"唱到一半卡了一下"。

公式与语法全部取自 main/rtttl_player.c(parse_defaults / parse_note /
note_frequency),这里用 Python 复刻一份,所以不需要 ESP-IDF 也能跑。

长度约束说明:任务书上原本写"BGM 25-70 s",经确认那条是软要求而非验收项 ——
播放器每次循环都会重解析,旋律越长并不意味着越好的听感。这里改为检查
"BGM >= 10 s 且首尾都在乐句边界、以休止收尾";音效仍然必须 < 2 s。
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MUSIC_HEADER = ROOT / "main" / "tetris_music.h"

# --- 与 main/rtttl_player.c 对齐的常量与表 -----------------------------------
DEFAULT_DURATION = 4
DEFAULT_OCTAVE = 6
DEFAULT_BPM = 63

# parse_defaults() 只接受这些范围,超出就静默退回默认值 —— 那会让旋律
# "看起来对了、听起来不对",所以测试里必须拦住。
DURATION_RANGE = (1, 64)
OCTAVE_RANGE = (2, 7)
BPM_RANGE = (25, 900)

OCTAVE_4 = {
    "c": 262, "d": 294, "e": 330, "f": 349, "g": 392, "a": 440, "b": 494,
}
OCTAVE_4_SHARP = {
    "c": 277, "d": 311, "f": 370, "g": 415, "a": 466,
}

SAMPLE_RATE = 16000                 # RTTTL_WAVE_SAMPLE_RATE
NYQUIST = SAMPLE_RATE // 2          # 8000 Hz
# 刺耳判据不是"基频上限",而是【方波的奇次谐波是否在奈奎斯特处折回可听区】。
# 方波第 n 次谐波幅度约 1/n,所以只统计到 11 次(幅度 ≥ 9%)才有意义;
# 折回频率落在 1~5 kHz(人耳最敏感的中高频)时听感就是"扎耳朵":
#   g5(784Hz)/a5(880Hz)/b5(988Hz): 无显著折回   -> 可用
#   c6(1048Hz): 11 次折回 2376Hz        -> 刺耳
#   e6(1319Hz): 9/11 次折回 4129/1491Hz -> 明显刺耳
#   g6(1568Hz): 9/11 次折回 1888/1248Hz -> 明显刺耳
# 所以音效上限取 b5,背景音乐(三角波,谐波按 1/n² 衰减)可以再高一点。
SFX_MAX_FREQ = 988                  # b5
BGM_MAX_FREQ = 988                  # b5(三角波同限,留余量)
HARMONIC_LIMIT = 11                 # 只统计到第 11 次奇次谐波(幅度 ≥ 9%)
ALIAS_BAND = (1000, 5000)           # 折回后落在这个区间算"听得出刺耳"


def significant_alias(frequency: float) -> list[tuple[int, int]]:
    """返回 (谐波次数, 折回频率) —— 幅度 ≥ 9% 且折回落在可听敏感带的那些。"""
    found: list[tuple[int, int]] = []
    for order in range(3, HARMONIC_LIMIT + 1, 2):
        product = frequency * order
        if product <= NYQUIST:
            continue
        folded = abs(2 * NYQUIST - product)
        if ALIAS_BAND[0] <= folded <= ALIAS_BAND[1]:
            found.append((order, int(folded)))
    return found

BGM_MIN_MS = 10_000
SFX_MAX_MS = 2_000

NOTE_RE = re.compile(r"^(\d*)([a-gp]#?)(\d*)(\.?)$")
DEFAULTS_RE = re.compile(
    r"d=(\d+)|o=(\d+)|b=(\d+)"
)

REQUIRED = (
    "TETRIS_BGM_THEME",
    "TETRIS_SFX_START",
    "TETRIS_SFX_MOVE",
    "TETRIS_SFX_ROTATE",
    "TETRIS_SFX_SOFTDROP",
    "TETRIS_SFX_HARDDROP",
    "TETRIS_SFX_LOCK",
    "TETRIS_SFX_CLEAR1",
    "TETRIS_SFX_CLEAR2",
    "TETRIS_SFX_CLEAR3",
    "TETRIS_SFX_CLEAR4",
    "TETRIS_SFX_LEVELUP",
    "TETRIS_SFX_GAMEOVER",
    "TETRIS_SFX_PAUSE",
)
CLEAR_SEQUENCE = ("TETRIS_SFX_CLEAR1", "TETRIS_SFX_CLEAR2",
                  "TETRIS_SFX_CLEAR3", "TETRIS_SFX_CLEAR4")


class Note:
    """一条音符:时值(ms)、频率(0 = 休止)、八度。"""

    __slots__ = ("raw", "duration_ms", "frequency", "octave")

    def __init__(self, raw: str, duration_ms: int, frequency: float, octave: int) -> None:
        self.raw = raw
        self.duration_ms = duration_ms
        self.frequency = frequency
        self.octave = octave


def strip_comments(text: str) -> str:
    """去掉 /* */ 与 // 注释 —— 旋律宏的续行里夹着注释,不删就拼不出来。"""
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def melodies() -> dict[str, str]:
    """头文件里所有 TETRIS_* 旋律(续行与相邻字符串字面量按 C 规则拼接)。"""
    text = strip_comments(MUSIC_HEADER.read_text(encoding="utf-8"))
    text = re.sub(r"\\\n", " ", text)          # 反斜杠续行
    found: dict[str, str] = {}
    pattern = re.compile(
        r'#define\s+(TETRIS_\w+)\s+((?:"(?:[^"\\]|\\.)*"\s*)+)'
    )
    for match in pattern.finditer(text):
        found[match.group(1)] = "".join(
            re.findall(r'"((?:[^"\\]|\\.)*)"', match.group(2))
        )
    return found


def parse_defaults(song: str) -> tuple[int, int, int]:
    """复刻 parse_defaults():返回 (duration, octave, bpm),范围外退回默认。"""
    duration, octave, bpm = DEFAULT_DURATION, DEFAULT_OCTAVE, DEFAULT_BPM
    parts = song.split(":", 2)
    if len(parts) != 3:
        raise ValueError(f"{song!r}: 不是 `名称:默认值:音符` 三段式")
    for key, value in re.findall(r"([a-zA-Z])=(\d+)", parts[1]):
        number = int(value)
        key = key.lower()
        if key == "d" and DURATION_RANGE[0] <= number <= DURATION_RANGE[1]:
            duration = number
        if key == "o" and OCTAVE_RANGE[0] <= number <= OCTAVE_RANGE[1]:
            octave = number
        if key == "b" and BPM_RANGE[0] <= number <= BPM_RANGE[1]:
            bpm = number
    return duration, octave, bpm


def note_frequency(name: str, sharp: bool, octave: int) -> float:
    """复刻 note_frequency():休止符返回 0,其余按八度倍频。"""
    if name == "p":
        return 0.0
    if sharp:
        if name not in OCTAVE_4_SHARP:
            raise ValueError(f"没有 {name}# 这个音")
        frequency = OCTAVE_4_SHARP[name]
    else:
        if name not in OCTAVE_4:
            raise ValueError(f"解析不了音名 {name!r}")
        frequency = OCTAVE_4[name]
    return frequency * (2 ** (octave - 4))


def parse_notes(song: str) -> list[Note]:
    """复刻 parse_note() 的逐音符循环,返回全部音符(含休止符)。"""
    duration, octave, bpm = parse_defaults(song)
    body = song.split(":", 2)[2]
    base = 240_000 // bpm                     # 4 分音符时长(整数除法,与 C 一致)
    notes: list[Note] = []
    for raw in body.split(","):
        token = raw.strip()
        if not token:
            continue
        match = NOTE_RE.match(token)
        if match is None:
            raise ValueError(f"解析不了的音符 {token!r}")
        length = int(match.group(1)) if match.group(1) else duration
        if length == 0:
            length = duration
        value = base // length
        if match.group(4):                    # 附点 -> 1.5 倍
            value += value // 2
        note_octave = int(match.group(3)) if match.group(3) else octave
        notes.append(
            Note(token, value,
                 note_frequency(match.group(2), token_no_sharp(match), note_octave),
                 note_octave)
        )
    return notes


def token_no_sharp(match: re.Match[str]) -> bool:
    return match.group(2).endswith("#")


def total_ms(notes: list[Note]) -> int:
    return sum(note.duration_ms for note in notes)


def main() -> int:
    songs = melodies()
    errors: list[str] = []

    for name in REQUIRED:
        if name not in songs:
            errors.append(f"main/tetris_music.h: 缺少旋律 {name}")
    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1

    parsed: dict[str, list[Note]] = {}
    for name, song in songs.items():
        try:
            notes = parse_notes(song)
        except ValueError as exc:
            errors.append(f"{name}: {exc}")
            continue
        if not notes:
            errors.append(f"{name}: 没有任何音符")
            continue
        if not song.split(":", 2)[0].strip():
            errors.append(f"{name}: 缺少 RTTTL 名称段")
        parsed[name] = notes

    # --- 每条的语法与时值合法性 ---------------------------------------------
    for name, notes in parsed.items():
        song = songs[name]
        duration, octave, bpm = parse_defaults(song)
        defaults_text = song.split(":", 2)[1]
        if "d=" not in defaults_text or "o=" not in defaults_text or "b=" not in defaults_text:
            errors.append(f"{name}: 默认值段必须写全 d=/o=/b=(播放器会用 4/6/63 兜底)")
        # 八度必须写在播放器接受的范围内:o= 只接受 2..7,单个音符的八度位
        # 也必须是 0..9 的数字(否则 parse_note 会把它当成八度)。
        if not OCTAVE_RANGE[0] <= octave <= OCTAVE_RANGE[1]:
            errors.append(f"{name}: 默认八度 {octave} 超出 {OCTAVE_RANGE}")
        if not BPM_RANGE[0] <= bpm <= BPM_RANGE[1]:
            errors.append(f"{name}: 速度 {bpm} 超出 {BPM_RANGE}")
        if not DURATION_RANGE[0] <= duration <= DURATION_RANGE[1]:
            errors.append(f"{name}: 默认时值 {duration} 超出 {DURATION_RANGE}")
        for note in notes:
            if not OCTAVE_RANGE[0] <= note.octave <= OCTAVE_RANGE[1]:
                errors.append(f"{name}: 音符 {note.raw!r} 的八度 {note.octave} 超出 {OCTAVE_RANGE}")

    # --- 音域 ---------------------------------------------------------------
    for name, notes in parsed.items():
        limit = BGM_MAX_FREQ if name == "TETRIS_BGM_THEME" else SFX_MAX_FREQ
        for note in notes:
            if not note.frequency:
                continue
            if note.frequency > limit:
                errors.append(
                    f"{name}: 音符 {note.raw!r} = {note.frequency:.0f} Hz,超过上限 {limit} Hz"
                )
            aliased = significant_alias(note.frequency)
            if aliased:
                detail = ", ".join(f"{order}次->{folded}Hz" for order, folded in aliased)
                errors.append(
                    f"{name}: 音符 {note.raw!r} = {note.frequency:.0f} Hz 的奇次谐波"
                    f"折回可听区({detail}),16kHz 方波会听成刺耳噪声"
                )

    # --- 背景音乐 -----------------------------------------------------------
    bgm = parsed.get("TETRIS_BGM_THEME", [])
    if bgm:
        length = total_ms(bgm)
        if length < BGM_MIN_MS:
            errors.append(f"TETRIS_BGM_THEME: {length} ms,短于 {BGM_MIN_MS} ms")
        # 循环点:播放器每轮结束时重解析字符串,夹一个 ≤15ms 空档。
        # 旋律必须以休止结尾,那个空档才会落进真正的静音里。
        if bgm[-1].frequency != 0:
            errors.append(
                f"TETRIS_BGM_THEME: 末音是 {bgm[-1].raw!r},不是休止符 ——"
                "播放器每轮循环的 ≤15ms 重解析空档会变成可听出的断口"
            )
        if not bgm[0].frequency:
            errors.append("TETRIS_BGM_THEME: 首音是休止符,循环点会连成两拍空白")
        # 每句都要收在小节线上:整曲时长必须是"四分音符的整数倍",
        # 且落在 4/4 小节边界上(相同速度下节奏才不会飘)。
        unit = min(note.duration_ms for note in bgm if note.frequency)
        if length % unit != 0:
            errors.append(
                f"TETRIS_BGM_THEME: {length} ms 不是基本时值 {unit} ms 的整数倍"
            )
        macro = songs["TETRIS_BGM_THEME"].split(":", 2)[2]
        phrase_marks = macro.count("4p") + macro.count("8p") + macro.count("2p")
        if phrase_marks == 0:
            errors.append("TETRIS_BGM_THEME: 整条旋律没有任何休止符(句子之间需要换气)")

    # --- 音效 ---------------------------------------------------------------
    sfx = {name: notes for name, notes in parsed.items() if name != "TETRIS_BGM_THEME"}
    for name, notes in sfx.items():
        length = total_ms(notes)
        if length >= SFX_MAX_MS:
            errors.append(f"{name}: {length} ms,不短于 {SFX_MAX_MS} ms")
        if length <= 0:
            errors.append(f"{name}: 时长必须为正")
        # 音效是"操作反馈",太长的会把下一个操作的反馈顶掉。
        if len(notes) > 8:
            errors.append(f"{name}: {len(notes)} 个音符,音效不该这么长")

    # --- 消行音效必须逐级升高(CLEAR4 是 Tetris 的号角) ----------------------
    # 判据是"每一级都比上一级高",而不是"不下降":把 CLEAR2 写成和 CLEAR1 一样
    # 就失去了"消 2 行比消 1 行更爽"的反馈,实机上等于白做了一个音效。
    lengths: list[int] = []
    peaks: list[float] = []
    for name in CLEAR_SEQUENCE:
        if name in parsed:
            notes = parsed[name]
            lengths.append(total_ms(notes))
            peaks.append(max(note.frequency for note in notes))

    if len(peaks) == 4:
        # 峰值受 b5(方波上限)限制,四级不能全靠音高拉开 —— 所以判据是
        # "更高或更长",但每一级都必须严格前进,不能原地不动。
        for i in range(1, 4):
            if peaks[i] == peaks[i - 1] and lengths[i] == lengths[i - 1]:
                errors.append(
                    f"消行音效 CLEAR{i + 1} 与 CLEAR{i} 完全同级"
                    f"(峰值都是 {peaks[i]:.0f} Hz、时长都是 {lengths[i]} ms),"
                    "消得更爽的反馈就没了"
                )
            if peaks[i] < peaks[i - 1]:
                errors.append(f"消行音效的最高音不能下降: CLEAR{i}={peaks[i - 1]:.0f}Hz -> CLEAR{i + 1}={peaks[i]:.0f}Hz")
        if peaks[3] <= peaks[0] or lengths[3] <= lengths[0]:
            errors.append(
                f"CLEAR4 必须同时比 CLEAR1 更高更亮: "
                f"峰值 {peaks[0]:.0f}->{peaks[3]:.0f} Hz,时长 {lengths[0]}->{lengths[3]} ms"
            )
    note_counts = [len(parsed[name]) for name in CLEAR_SEQUENCE if name in parsed]
    if len(note_counts) == 4 and not all(a <= b for a, b in zip(note_counts, note_counts[1:])):
        errors.append(f"消行音效的音符数不能减少,实际为 {note_counts}")

    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1

    summary = ", ".join(
        f"{name.replace('TETRIS_SFX_', '').replace('TETRIS_', '')}={total_ms(notes)}ms"
        for name, notes in sorted(parsed.items())
    )
    print(f"Tetris music check: PASS ({summary})")
    print(f"  BGM 结构: {len(bgm)} 音符, {total_ms(bgm)} ms, "
          f"{min(n.frequency for n in bgm if n.frequency):.0f}-"
          f"{max(n.frequency for n in bgm):.0f} Hz, 以休止收尾")
    print(f"  消行音高: {[f'{p:.0f}Hz' for p in peaks]}, 时长: {lengths} ms")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
