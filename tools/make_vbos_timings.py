"""Turn faster-whisper JSON transcripts of the Voiced Books of Skyrim audio into ReadItNow timing files.

    python make_vbos_timings.py <folder with .json transcripts> <output folder> [durations.csv]

durations.csv (optional): lines "<audio file name>,<length in seconds>" (e.g. from ffprobe). The length is written
into each timing file so the mod only uses it for that exact recording (another narrator's file differs).

Writes <output>/<audio name>.txt with one spoken word per line: "start end word" (seconds).
Words are normalized exactly like AutoTurn.cpp's Tokenize(): lowercase, apostrophes dropped,
any other non letter/digit character splits words.
"""
import json
import os
import re
import sys

APOS = "'’"
SPLIT = re.compile(r"[^0-9a-zÀ-ɏ]+")


def norm_tokens(s):
    s = s.lower()
    for a in APOS:
        s = s.replace(a, "")
    return [t for t in SPLIT.split(s) if t]


def convert(src, dst, duration=None):
    with open(src, encoding="utf-8") as f:
        d = json.load(f)
    lines = []
    for seg in d.get("segments", []):
        for w in seg.get("words", []) or []:
            for t in norm_tokens(w.get("word", "")):
                lines.append(f"{w['start']:.2f} {w['end']:.2f} {t}")
    with open(dst, "w", encoding="utf-8", newline="\n") as f:
        f.write("# ReadItNow VBoS timing v1: start end word\n")
        if duration:
            f.write(f"# duration {duration:.3f}\n")
        f.write("\n".join(lines))
        f.write("\n")
    return len(lines)


def main():
    src_dir, out_dir = sys.argv[1], sys.argv[2]
    durations = {}
    if len(sys.argv) > 3:
        with open(sys.argv[3], encoding="utf-8") as f:
            for line in f:
                name, _, dur = line.strip().rpartition(",")
                if name and dur:
                    durations[os.path.splitext(name)[0].lower()] = float(dur)
    os.makedirs(out_dir, exist_ok=True)
    done = empty = 0
    for name in sorted(os.listdir(src_dir)):
        if not name.lower().endswith(".json"):
            continue
        key = name[:-5].lower()
        n = convert(os.path.join(src_dir, name), os.path.join(out_dir, key + ".txt"), durations.get(key))
        done += 1
        if n == 0:
            empty += 1
            print("no words:", name)
    print(f"{done} timing files written to {out_dir} ({empty} without words)")


if __name__ == "__main__":
    main()
