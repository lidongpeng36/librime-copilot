#!/usr/bin/env python3
"""
从 Rime 词典构建 copilot 预测库。

把每个词按所有切分点拆成「前缀 → 后缀」，交给 build_copilot 写成 copilot.db。
插件用同样的形状查询：光标前的最后 1..N 个字做键，取权重最高的后续串。

权重约定（务必与插件保持一致）：**数值越大越可能**。
src/db_provider.h 与 src/rerank.h 都按此排序，写反会让长尾专名排到最前面。

用法：
    make_copilot_db.py -c dict.json -o copilot.db

配置文件是一个数组，每项描述一个词库：

    [
      { "dict": "~/Library/Rime/private/custom.dict.yaml", "top": true },
      { "dict": "~/Library/Rime/cn_dicts/base.dict.yaml" },
      { "dict": "~/Library/Rime/cn_dicts/ext.dict.yaml" }
    ]

    top    该词库整体抬升到所有其它词库之上（叠加，不是替换，见下）
    scale  权重乘以一个系数
    range  线性缩放到 [min, max] —— 慎用，见 scale_priorities 的说明
"""
import argparse
import os
import json
import shutil
import subprocess
from tempfile import NamedTemporaryFile
from collections import defaultdict
from pypinyin import lazy_pinyin

# 没有权重列的词库（others 等）只提供「这个词存在」，给一个与 ext/sogou/tencent
# 同级的底分，让它们排在有真实词频的条目之后。
DEFAULT_PRIORITY = 100

# build_copilot 的历史名字：早期这个插件叫 predict。
BUILDER_NAMES = ('build_copilot', 'build_predict')


def iter_body_lines(fin, input_path):
    """
    只产出词典正文，跳过 `---` 到 `...` 之间的 YAML 元数据块。

    不跳会怎样：`name: tencent`、`columns:` / `  - text` 这些会被当成词条，拆成
    前缀/后缀喂给 build_copilot。它按空白切三列，于是 `- text` 拆出的
    `- t → ext 100` 报「格式错误」，而 `name: custom` 拆出的 `name: → custom`
    刚好凑够三列，**静默**写进库里。报错的那些只是噪音，没报错的才是脏数据。

    注释行由调用方按 `#` 跳过；这里只管元数据块。`---` 开了却没 `...` 收尾说明
    文件不对劲，这时宁可报错，也不要把整个词库当元数据吞掉。
    """
    in_header = False
    for line in fin:
        stripped = line.strip()
        if in_header:
            if stripped == '...':
                in_header = False
            continue
        if stripped == '---':
            in_header = True
            continue
        yield line
    if in_header:
        raise ValueError(f"{input_path}: YAML 头以 `---` 开始却没有 `...` 收尾")


def parse_file(input_path, convert_to_traditional=False):
    """
    读取词典文件，返回词条元组：(word, pinyin, priority)
    自动补全拼音，跳过非法或注释行
    """
    if convert_to_traditional:
        from opencc import OpenCC
        cc = OpenCC('s2t')

    entries = []
    unusable = []  # 含空白的词：build_copilot 按空白分列，写出去必然错位
    with open(input_path, 'r', encoding='utf-8') as fin:
        for line in iter_body_lines(fin, input_path):
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            parts = line.split('\t')
            if len(parts) == 3:
                word, pinyin, priority = parts
            elif len(parts) == 2:
                # 两列有两种形态：`词\t权重`（tencent）和 `词\t拼音`（others）。
                # 第二列能解析成整数才是权重，否则当拼音，权重取存在性默认值。
                word, second = parts
                if second.strip().isdigit():
                    priority = second
                    pinyin = ' '.join(lazy_pinyin(word))
                else:
                    pinyin = second
                    priority = DEFAULT_PRIORITY
            elif len(parts) == 1:
                word = parts[0]
                pinyin = ' '.join(lazy_pinyin(word))
                priority = DEFAULT_PRIORITY
            else:
                continue  # 格式错误跳过

            if convert_to_traditional:
                word = cc.convert(word)
            try:
                priority = int(priority)
            except ValueError:
                continue
            # 兜底：正文里不该有带空白的词。真出现了就是解析出了岔子，
            # 与其让它悄悄变成一条错位的库条目，不如丢掉并在最后喊一声。
            if any(c.isspace() for c in word):
                unusable.append(word)
                continue
            entries.append((word, pinyin, priority))

    if unusable:
        preview = ', '.join(repr(w) for w in unusable[:5])
        print(f"⚠️  {input_path}: 跳过 {len(unusable)} 个含空白的词: {preview}")
    return entries


def scale_priorities(entries, target_min=1, target_max=200):
    """
    线性 min-max 缩放。

    慎用：词频是长尾分布，线性缩放会把绝大多数词压成同一个值。实测某词库
    542,928 条里只有 1.07% 能超过区间下界，其余全部并列 —— 词频信息就没了。
    需要跨词库对齐量级时优先考虑 scale，或者干脆保留原始词频。
    """
    if not entries:
        return []
    all_priorities = [p for _, _, p in entries]
    min_p, max_p = min(all_priorities), max(all_priorities)
    if min_p == max_p:
        return [(w, p, 100) for w, p, _ in entries]
    scaled_entries = []
    for word, pinyin, priority in entries:
        scaled = int((priority - min_p) / (max_p - min_p) * (target_max - target_min)) + target_min
        scaled_entries.append((word, pinyin, scaled))
    return scaled_entries


def resolve_path(path):
    # 处理 ~ 和相对路径
    return os.path.abspath(os.path.expanduser(path))


def build_entry_list_from_config(config_path, traditional=False):
    """
    合并配置里的所有词库，返回 (word, pinyin, priority)，按 (首字, 权重降序) 排好。

    只有部分词库带真实词频（如 base、custom），其余是恒定底分 —— 它们提供的是
    「这个词存在」而不是「这个词多常见」，所以不要对它们做缩放，否则会盖过真实词频。

    标了 "top": true 的词库不是替换权重，而是在已有权重之上叠加一个偏移量：
    weight = ceiling + 原有权重 + 本词库权重。这样其中任何一条都排在其它词库之上，
    同时内部仍按真实词频排序 —— 直接覆盖会把词频抹掉（实测 建立 193 > 建设 45 >
    建议 4，与实际词频完全相反）。
    """
    with open(config_path, 'r', encoding='utf-8') as f:
        config = json.load(f)

    scaled_by_dict = []  # [(is_top, [(word, pinyin, priority), ...]), ...]

    for item in config:
        dict_path = resolve_path(item["dict"])
        if not os.path.isfile(dict_path):
            raise FileNotFoundError(f"词典文件不存在: {dict_path}")
        entries = parse_file(dict_path, convert_to_traditional=traditional)

        if "scale" in item and "range" in item:
            raise ValueError(f"{dict_path} 配置中不能同时包含 scale 和 range")
        elif "scale" in item:
            factor = item["scale"]
            entries = [(w, p, pr * factor) for (w, p, pr) in entries]
        elif "range" in item:
            min_v, max_v = item["range"]
            entries = scale_priorities(entries, min_v, max_v)

        scaled_by_dict.append((bool(item.get("top")), entries))

    # 先合并所有普通词库
    entry_map = {}
    for is_top, entries in scaled_by_dict:
        if is_top:
            continue
        for w, p, pr in entries:
            key = (w, p)
            if key not in entry_map or pr > entry_map[key]:
                entry_map[key] = pr

    # 再把 top 词库叠加上去：ceiling 保证越过所有普通词条，原有权重保住词频顺序
    ceiling = max(entry_map.values(), default=0)
    for is_top, entries in scaled_by_dict:
        if not is_top:
            continue
        for w, p, pr in entries:
            key = (w, p)
            entry_map[key] = ceiling + entry_map.get(key, 0) + pr

    deduplicated = [(w, p, pr) for (w, p), pr in entry_map.items()]
    deduplicated.sort(key=lambda x: (x[0][0], -x[2]))
    return deduplicated


def split_and_write(entries, output_path, max_per_key):
    """
    把每个词拆成「前缀 → 后缀」写出，第三列是真实权重。

    同一个词可能有多个读音（`空落落` 有 kong luo luo / kong lao lao），去重是按
    (词, 拼音) 做的，所以拆分后会产生完全相同的 (前缀, 后缀) 对。必须在这里再去
    重一次取最大权重，否则重复条目会白占插件的排名位次。

    entries 已按首字排序，同一首字的词是连续的，而一个前缀的首字就是该词的首字 ——
    所以按首字分块去重即可，无需把几百万条全存在内存里。
    """
    total = 0
    with open(output_path, 'w', encoding='utf-8') as fout:
        block_first_char = None
        block = {}  # (prefix, rest) -> weight，dict 保序，即按权重降序

        def flush():
            nonlocal total
            counts = defaultdict(int)
            for (prefix, rest), weight in block.items():
                counts[prefix] += 1
                if counts[prefix] > max_per_key:
                    continue
                fout.write(f"{prefix}\t{rest}\t{weight}\n")
                total += 1
            block.clear()

        for word, pinyin, pr in entries:
            if word[:1] != block_first_char:
                flush()
                block_first_char = word[:1]
            for i in range(1, len(word)):  # 拆分点：前缀 1 到 n-1
                key = (word[:i], word[i:])
                if pr > block.get(key, 0):
                    block[key] = pr
        flush()
    return total


def sort_and_write(entries, output_path):
    grouped = defaultdict(list)
    for word, pinyin, priority in entries:
        first = word[0]
        grouped[first].append((priority, f"{word}\t{pinyin}\t{priority}"))

    with open(output_path, 'w', encoding='utf-8') as fout:
        for first in sorted(grouped.keys()):
            for _, raw_line in sorted(grouped[first], key=lambda x: -x[0]):
                fout.write(raw_line + '\n')


def find_builder(explicit=None):
    """定位 build_copilot：命令行 > 脚本同目录 > PATH。"""
    if explicit:
        if not os.path.isfile(explicit):
            raise FileNotFoundError(f"builder 不存在: {explicit}")
        return explicit
    here = os.path.dirname(os.path.abspath(__file__))
    for name in BUILDER_NAMES:
        candidate = os.path.join(here, name)
        if os.path.isfile(candidate):
            return candidate
        found = shutil.which(name)
        if found:
            return found
    raise FileNotFoundError(
        f"找不到 {' / '.join(BUILDER_NAMES)}，用 --builder 指定，或把它放到 {here}")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('-c', '--config', required=True, help='Path to JSON config file')
    parser.add_argument('-o', '--output', default='copilot.db', help='Output db path')
    parser.add_argument('-s', '--sort', action='store_true', help='Only sort and output')
    parser.add_argument('-t', '--traditional', action='store_true',
                        help='Convert to Traditional Chinese')
    parser.add_argument('-m', '--max', type=int, default=-1,
                        help='Maximum continuations per key (<= 0 means unlimited)')
    parser.add_argument('--builder', help=f'Path to {BUILDER_NAMES[0]}')
    args = parser.parse_args()

    all_entries = build_entry_list_from_config(args.config, traditional=args.traditional)

    if args.sort:
        sort_and_write(all_entries, args.output)
        return

    builder = find_builder(args.builder)
    max_per_key = args.max if args.max > 0 else float('inf')
    with NamedTemporaryFile(mode='w+', encoding='utf-8', delete=False) as tmp_file:
        tmp_path = tmp_file.name
    try:
        lines = split_and_write(all_entries, tmp_path, max_per_key)
        print(f"{len(all_entries)} 个词 -> {lines} 条前缀/后缀，构建 {args.output}")
        subprocess.run([builder, args.output],
                       stdin=open(tmp_path, 'r', encoding='utf-8'),
                       check=True)
    finally:
        os.remove(tmp_path)


if __name__ == '__main__':
    main()
