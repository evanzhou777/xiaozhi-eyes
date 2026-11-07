"""把 sounds/roast_*.mp3 转成 16kHz 单声道 Ogg/Opus，再打包成 roast_sounds.h。

规格与官方提示音 (main/assets/common/success.ogg) 完全一致：
    OpusHead ver=1 ch=1 preskip=312 input_sr=16000 gain=0 map=0
    每帧 60ms，码率 24kbps，APPLICATION_AUDIO

用法：
    python convert_roast.py [ffmpeg路径]
不传路径时自动在常见位置查找已安装的 ffmpeg。
"""
import glob
import os
import shutil
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
SOUNDS = os.path.join(ROOT, "sounds")
OGG_DIR = os.path.join(SOUNDS, "ogg")
HEADER = os.path.join(ROOT, "roast_sounds.h")

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


def check_ogg(path):
    data = open(path, "rb").read()
    if data[:4] != b"OggS":
        raise RuntimeError(f"{path}: 缺少 OggS 魔数")
    i = data.find(b"OpusHead")
    if i < 0:
        raise RuntimeError(f"{path}: 缺少 OpusHead")
    ver, ch, preskip, sr, gain, mapping = struct.unpack_from("<BBHIhB", data, i + 8)
    if (ver, ch, preskip, sr, mapping) != (1, 1, 312, 16000, 0):
        raise RuntimeError(f"{path}: OpusHead 规格不符 {ver, ch, preskip, sr, mapping}")
    return len(data)


def main():
    ff = find_ffmpeg()
    print(f"ffmpeg: {ff}")
    os.makedirs(OGG_DIR, exist_ok=True)
    files = sorted(glob.glob(os.path.join(SOUNDS, "roast_*.mp3")))
    if not files:
        sys.exit(f"错误：{SOUNDS} 里没有 roast_*.mp3")

    total = 0
    for src in files:
        stem = os.path.splitext(os.path.basename(src))[0]
        dst = os.path.join(OGG_DIR, stem + ".ogg")
        cmd = [
            ff, "-y", "-hide_banner", "-loglevel", "error",
            "-i", src,
            "-ar", "16000", "-ac", "1",
            "-c:a", "libopus", "-b:a", "24k",
            "-frame_duration", "60", "-application", "audio",
            dst,
        ]
        subprocess.run(cmd, check=True)
        size = check_ogg(dst)
        total += size
        print(f"  {stem:12s} {os.path.getsize(src):>7} B mp3 -> {size:>6} B ogg")

    print(f"Ogg 合计 {total} B")
    subprocess.run([sys.executable, os.path.join(ROOT, "pack_roast.py"), OGG_DIR, HEADER], check=True)


if __name__ == "__main__":
    main()
