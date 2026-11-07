"""把 roast_*.ogg (Ogg/Opus 16kHz 单声道) 打包成 C++ 头文件。

用法：
    python pack_roast.py <ogg目录> <输出头文件>

生成的头文件提供：
    ROAST_SOUNDS[]        std::string_view 数组，可直接传给 Application::PlaySound()
    ROAST_SOUND_COUNT     音效条数
"""
import glob
import os
import sys


def c_array(name, data):
    lines = [f"static const uint8_t {name}[] = {{"]
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        lines.append("    " + "".join(f"0x{b:02X}," for b in chunk))
    lines.append("};")
    return "\n".join(lines)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    ogg_dir, out_path = sys.argv[1], sys.argv[2]
    files = sorted(glob.glob(os.path.join(ogg_dir, "roast_*.ogg")))
    if not files:
        print(f"错误：{ogg_dir} 里没有 roast_*.ogg")
        sys.exit(1)

    arrays = []
    views = []
    total = 0
    for path in files:
        stem = os.path.splitext(os.path.basename(path))[0]
        data = open(path, "rb").read()
        if data[:4] != b"OggS" or data.find(b"OpusHead") < 0:
            print(f"错误：{path} 不是 Ogg/Opus 文件")
            sys.exit(1)
        arrays.append(c_array(f"{stem}_data", data))
        views.append(
            f"    std::string_view(reinterpret_cast<const char*>({stem}_data), "
            f"sizeof({stem}_data)),"
        )
        total += len(data)
        print(f"  {stem:12s} {len(data):>7} B")

    header = f"""// 自动生成，请勿手改 —— pack_roast.py
// 数据源：本地录音转 16kHz 单声道 Ogg/Opus，与官方提示音规格一致
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace {{

{chr(10).join(arrays)}

static const std::string_view ROAST_SOUNDS[] = {{
{chr(10).join(views)}
}};

static constexpr size_t ROAST_SOUND_COUNT =
    sizeof(ROAST_SOUNDS) / sizeof(ROAST_SOUNDS[0]);

}}  // namespace
"""
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(header)
    print(f"共 {len(files)} 条，Ogg 合计 {total} B，头文件 {len(header)} B -> {out_path}")


if __name__ == "__main__":
    main()
