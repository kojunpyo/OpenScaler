# PresentMon 기록으로 확대 프로그램(OpenScaler, Lossless Scaling 등)이 화면에 낸 장면을 구간마다 요약한다.
# 다른 프로그램과 같은 기준으로 비교하려고 쓴다 (--fg-log는 OpenScaler 안에서만 기록된다).
#
# 사용법: python -I tools/test/pmcompare.py presentmon.csv markers.txt [시각 보정 시간]
#   presentmon.csv: PresentMon 2.x를 --date_time으로 기록한 CSV (관리자 권한 필요)
#                   예) PresentMon.exe --process_name LosslessScaling.exe --process_name OpenScaler.exe --date_time --output_file pm.csv
#   markers.txt:    "구간이름 start,2026-11-23 12:00:00.000" / "구간이름 end,..." 줄 (측정 스크립트가 남긴 시작/끝 시각)
#   시각 보정 시간: PresentMon이 적은 시각에서 뺄 시간. 이 PC에서는 9시간 앞선 시각이 적혀 기본값 9.
#
# 끊김은 "화면에 새 장면이 바뀐 간격(MsBetweenDisplayChange)이 1.5칸을 넘은 횟수"로, README 6.1 "평가 기준"과 같은 기준이다.
# 고정 배수(예: 144Hz에 60fps x2)에서 같은 장면이 두 번 나오는 것은 장면이 바뀐 것으로 세므로 이 숫자에 드러나지 않는다.
import csv
import datetime as dt
import re
import statistics
import sys


def parse_time(text):
    # "2026-11-24 2:27:58.1234567"처럼 시가 한 자리이거나 소수점 아래가 7자리여도 읽는다
    d, h, mi, sec, frac = re.match(r"\s*(\d+-\d+-\d+) (\d+):(\d+):(\d+)\.?(\d*)", text).groups()
    return dt.datetime.strptime(f"{d} {int(h):02d}:{mi}:{sec}.{(frac + '000000')[:6]}", "%Y-%m-%d %H:%M:%S.%f")

pm_path, marker_path = sys.argv[1], sys.argv[2]
shift = dt.timedelta(hours=float(sys.argv[3]) if len(sys.argv) > 3 else 9)
rows = list(csv.DictReader(open(pm_path, encoding="utf-8-sig", errors="replace")))
for r in rows:
    r["_t"] = parse_time(r["TimeInDateTime"]) - shift

marks = {}
for line in open(marker_path, encoding="utf-8"):
    name, t = line.strip().rsplit(",", 1)
    run, what = name.split()
    marks.setdefault(run, {})[what] = dt.datetime.strptime(t, "%Y-%m-%d %H:%M:%S.%f")

print(f"{'구간':<10} {'프로그램':<22} {'낸 장면':>8} {'표시':>8} {'끊김':>9} {'Present->표시':>14}")
for run, m in marks.items():
    a, b = m["start"] + dt.timedelta(seconds=2), m["end"]  # 켠 직후는 뺀다
    seg = [r for r in rows if a <= r["_t"] <= b]
    dur = (b - a).total_seconds()
    for app in sorted({r["Application"] for r in seg}):
        pr = [r for r in seg if r["Application"] == app]
        chain = max({r["SwapChainAddress"] for r in pr}, key=lambda x: sum(r["SwapChainAddress"] == x for r in pr))
        pr = [r for r in pr if r["SwapChainAddress"] == chain]
        shown = [r for r in pr if r["MsUntilDisplayed"] not in ("NA", "")]
        gaps = [float(r["MsBetweenDisplayChange"]) for r in shown if r["MsBetweenDisplayChange"] not in ("NA", "")]
        gaps = [g for g in gaps if g > 0]
        if not gaps:
            print(f"{run:<10} {app:<22} {len(pr) / dur:7.1f}/초 (표시 정보 없음)")
            continue
        slot = statistics.median(gaps)
        hitch = sum(g > 1.5 * slot for g in gaps) / dur * 60
        lat = statistics.median(float(r["MsUntilDisplayed"]) for r in shown)
        print(f"{run:<10} {app:<22} {len(pr) / dur:7.1f}/초 {len(shown) / dur:7.1f}/초 {hitch:6.1f}/분 {lat:11.1f}ms")
