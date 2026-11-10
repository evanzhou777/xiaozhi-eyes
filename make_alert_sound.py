"""合成「发现蚊子」警报提示音，打包成 mosquito_alert.h。

不需要任何录音素材：ffmpeg lavfi 直接生成 4 声短促 988Hz 提示音，
编码规格与官方提示音完全一致（16kHz 单声道 Ogg/Opus，60ms 帧，24kbps）。
以后想换成真人语音，把 ALERT_WAV/MP3 指向自己的音频重新跑一遍即可。

用法：python make_alert_sound.py [ffmpeg路径]
"""
import math
import os
import shutil
import struct
import subprocess
import sys
import wave

ROOT = os.path.dirname(os.path.abspath(__file__))
WAV = os.path.join(ROOT, "sounds", "ogg", "mosquito_alert.wav")
OGG = os.path.join(ROOT, "sounds", "ogg", "mosquito_alert.ogg")
HEADER = os.path.join(ROOT, "mosquito_alert.h")

SAMPLE_RATE = 16000

CANDIDATES = [
    r"C:/Users/zhou/AppData/Local/kzip_sogou/ffmpeg.exe",
    r"C:/Users/zhou/AppData/Local/JianyingPro/Apps/11.6.0.14552/ffmpeg.exe",
]


def find_ffmpeg():
    if len(sys.argv) > 1:
        return sys.argv[1]
    exe = shutil.which("ffmpeg")
    if exe:
        return exe
    for path in CANDIDATES:
        if os.path.exists(path):
            return path
    try:
        import imageio_ffmpeg
        return imageio_ffmpeg.get_ffmpeg_exe()
    except ImportError:
        pass
    sys.exit("找不到 ffmpeg，请把路径作为第一个参数传入")


def main():
    ff = find_ffmpeg()
    os.makedirs(os.path.dirname(OGG), exist_ok=True)

    # 1 秒内 4 声短促 beep：每 0.25 秒响 0.12 秒，988Hz。
    # 直接用 Python 写 PCM WAV（本机精简版 ffmpeg 不带 lavfi），加 8ms 淡入淡出防爆音。
    total = SAMPLE_RATE  # 1 秒
    pcm = bytearray()
    for i in range(total):
        t = i / SAMPLE_RATE
        phase = t % 0.25
        if phase < 0.12:
            edge = min(phase / 0.008, (0.12 - phase) / 0.008, 1.0)
            amp = 0.55 * edge
            value = int(32767 * amp * math.sin(2 * math.pi * 988 * t))
        else:
            value = 0
        pcm += struct.pack("<h", max(-32768, min(32767, value)))
    with wave.open(WAV, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(bytes(pcm))

    cmd = [
        ff, "-y", "-hide_banner", "-loglevel", "error",
        "-i", WAV,
        "-ar", "16000", "-ac", "1",
        "-c:a", "libopus", "-b:a", "24k",
        "-frame_duration", "60", "-application", "audio",
        OGG,
    ]
    subprocess.run(cmd, check=True)

    data = open(OGG, "rb").read()
    if data[:4] != b"OggS":
        raise RuntimeError("缺少 OggS 魔数")
    i = data.find(b"OpusHead")
    if i < 0:
        raise RuntimeError("缺少 OpusHead")
    ver, ch, preskip, sr, gain, mapping = struct.unpack_from("<BBHIhB", data, i + 8)
    if (ver, ch, preskip, sr, mapping) != (1, 1, 312, 16000, 0):
        raise RuntimeError(f"OpusHead 规格不符: {(ver, ch, preskip, sr, gain, mapping)}")

    lines = [
        "// 自动生成，请勿手改 —— make_alert_sound.py",
        "// 4 声短促提示音，16kHz 单声道 Ogg/Opus，与官方提示音规格一致",
        "#pragma once",
        "",
        "#include <cstdint>",
        "#include <string_view>",
        "",
        "namespace {",
        "",
        "static const uint8_t mosquito_alert_ogg[] = {",
    ]
    for off in range(0, len(data), 16):
        chunk = data[off:off + 16]
        lines.append("    " + ",".join(f"0x{b:02X}" for b in chunk) + ",")
    lines += [
        "};",
        "",
        "static const std::string_view MOSQUITO_ALERT_SOUND(",
        "    reinterpret_cast<const char*>(mosquito_alert_ogg), sizeof(mosquito_alert_ogg));",
        "",
        "}  // namespace",
        "",
    ]
    open(HEADER, "w", encoding="utf8", newline="\n").write("\n".join(lines))
    print(f"ffmpeg: {ff}")
    print(f"Ogg/Opus: {len(data)} B  OpusHead ver={ver} ch={ch} preskip={preskip} sr={sr}")
    print(f"头文件: {HEADER} ({os.path.getsize(HEADER)} B)")


if __name__ == "__main__":
    main()
