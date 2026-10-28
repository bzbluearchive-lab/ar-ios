import json
import sys
import re

P = r"C:\Users\92072\.dsh\storages\session_projcache\sessions\session-92b29f03-085e-45ca-a564-610eed769756.json"

KEYS = ["kfd", "physpuppet", "landa", "smith", "GWorld", "UWorld", "GNames",
        "FNamePool", "三角洲", "偏移", "task_for_pid", "骨骼", "ESP", "透视",
        "ArenaESP", "ipa", "越狱", "TrollStore", "巨魔", "投递", "匿名页", "AMFI"]

with open(P, encoding="utf-8") as f:
    data = json.load(f)

strings = []


def walk(node, path=""):
    if isinstance(node, dict):
        for k, v in node.items():
            walk(v, path + "/" + str(k))
    elif isinstance(node, list):
        for i, v in enumerate(node):
            walk(v, path + "/%d" % i)
    elif isinstance(node, str):
        if len(node) >= 40:
            strings.append((path, node))


walk(data)
print("长字符串条目: %d" % len(strings), file=sys.stderr)

hits = []
for path, s in strings:
    score = sum(1 for k in KEYS if k in s)
    if score:
        hits.append((score, path, s))

hits.sort(key=lambda x: -x[0])
print("命中关键词的段落: %d" % len(hits), file=sys.stderr)

# 优先打印得分最高的若干段（这些最可能是分析结论）
for score, path, s in hits[:6]:
    print("=" * 70)
    print("score=%d  %s" % (score, path[-90:]))
    print("-" * 70)
    body = s if len(s) <= 4000 else s[:4000] + "\n…(截断)"
    print(body)
    print()
