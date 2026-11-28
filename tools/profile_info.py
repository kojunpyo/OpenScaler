# 학습 데이터(profiles/*.json)를 요약하고 공유해도 되는 형식인지 확인한다.
# 사용법:
#   python -I tools/profile_info.py 파일.json [파일2.json ...]   요약 (두 파일을 주면 나란히 비교)
#   python -I tools/profile_info.py --check shared-profiles       폴더 안 파일을 모두 검사 (문제가 있으면 종료 코드 1)
#
# "기준보다 좋은 정도"는 설정 창과 같은 값이다: 최근 그래프 점 10개(채점 200회)에서 (점수 - 기준 점수)의 평균.
# 기준은 보간이면 두 장을 단순히 섞은 것, 외삽이면 앞 프레임을 그대로 둔 것이다.
import json
import os
import sys

EVALS_PER_POINT = 20
GAIN_POINTS = 10
SHARE_MIN_EVALS = 200
MAP_VALUES = 32 * 18 * 5
BOX_VALUES = 6  # 고정 UI 상자: x, y, 너비, 높이, 확신도, 화면 비율
BOX_MAX = 16
BOX_MAX_CONFIDENCE = 8
CHOICES = {
    "cellPx": [4, 8, 16],
    "coarseRadius": [8, 12, 16],
    "smoothness": [0.0005, 0.0015, 0.004],
    "blendTol": [0.04, 0.08, 0.14],
}


def recent_gain(history, baseline):
    n = min(len(history), len(baseline), GAIN_POINTS)
    if n == 0:
        return 0.0, 0
    return sum(h - b for h, b in zip(history[-n:], baseline[-n:])) / n, n * EVALS_PER_POINT


def safe_file_name(name):
    return "".join("_" if c in '\\/:*?"<>|' else c for c in name) + ".json"


def check(path, data):
    problems = []
    if data.get("version") != 1:
        problems.append("version이 1이 아님")
    name = data.get("name", "")
    if not name:
        problems.append("name(게임 이름)이 없음: 최신 OpenScaler의 '공유용으로 내보내기'로 다시 만들어 주세요")
    elif os.path.basename(path) != safe_file_name(name):
        problems.append(f"파일 이름이 게임 이름과 다름: {safe_file_name(name)} 이어야 함")
    for pre in ("", "x_"):
        for key, options in CHOICES.items():
            if pre + key in data and not any(abs(data[pre + key] - o) < 1e-6 for o in options):
                problems.append(f"{pre + key} 값이 후보({options})에 없음")
        if len(data.get(pre + "history", [])) != len(data.get(pre + "baseline", [])):
            problems.append(f"{pre}history와 {pre}baseline 길이가 다름")
    if len(data.get("map", [])) not in (0, MAP_VALUES):
        problems.append(f"map 값 개수가 {MAP_VALUES}이 아님")
    for key in ("regionX", "regionY", "regionW", "regionH"):
        if not 0 <= data.get(key, 0) <= 1:
            problems.append(f"{key}가 0~1 밖")
    boxes = data.get("stillBoxes", [])
    if len(boxes) % BOX_VALUES or len(boxes) > BOX_VALUES * BOX_MAX:
        problems.append(f"stillBoxes 값 개수가 {BOX_VALUES}의 배수(상자 {BOX_MAX}개 이하)가 아님")
    else:
        for i in range(0, len(boxes), BOX_VALUES):
            x, y, w, h, conf, _aspect = boxes[i:i + BOX_VALUES]
            if not (0 <= x <= 1 and 0 <= y <= 1 and 0 < w <= 1 - x + 1e-4 and 0 < h <= 1 - y + 1e-4 and 0 < conf <= BOX_MAX_CONFIDENCE):
                problems.append(f"고정 UI 상자 {i // BOX_VALUES + 1}의 값이 범위 밖")
    return problems


def summary(data):
    rows = []
    rows.append(("게임", data.get("name", "(없음)")))
    rows.append(("저장한 버전", data.get("app", "(1.2.0 이하)")))
    w, h = data.get("sourceW", 0), data.get("sourceH", 0)
    rows.append(("게임 화면 크기", f"{w}x{h}" if w else "(기록 없음)"))
    for pre, label in (("", "보간"), ("x_", "외삽")):
        evals = data.get(pre + "evalCount", 0)
        gain, recent = recent_gain(data.get(pre + "history", []), data.get(pre + "baseline", []))
        text = f"채점 {evals}회"
        if recent:
            text += f", 최근 {recent}회 평균 기준보다 {gain:+.2f} dB"
            if recent < SHARE_MIN_EVALS:
                text += " (채점이 더 쌓이면 공유 권장)"
        rows.append((label, text))
        rows.append((f"  {label} 학습값", f"칸 {data.get(pre + 'cellPx', '-')}, 범위 {data.get(pre + 'coarseRadius', '-')}, "
                     f"벌점 {data.get(pre + 'smoothness', '-')}, 허용 {data.get(pre + 'blendTol', '-')}, 지도 {'씀' if data.get(pre + 'useMap') else '안 씀'}"))
    rows.append(("화면 지도 표본", str(data.get("mapSamples", 0))))
    if data.get("regionW", 0) > 0:
        rows.append(("프레임 생성 영역", f"x {data['regionX']:.2f}, y {data['regionY']:.2f}, 너비 {data['regionW']:.2f}, 높이 {data['regionH']:.2f}"))
    else:
        rows.append(("프레임 생성 영역", "화면 전체"))
    # 상자 수가 파일마다 달라 맨 뒤에 둔다 (두 파일을 나란히 볼 때 위쪽 줄이 어긋나지 않게)
    boxes = data.get("stillBoxes", [])
    rows.append(("고정 UI 상자", f"{len(boxes) // BOX_VALUES}개" if boxes else "없음"))
    for i in range(0, len(boxes) - BOX_VALUES + 1, BOX_VALUES):
        x, y, w, h, conf, aspect = boxes[i:i + BOX_VALUES]
        rows.append((f"  상자 {i // BOX_VALUES + 1}", f"x {x:.3f}, y {y:.3f}, 너비 {w:.3f}, 높이 {h:.3f}, 확신도 {conf:.0f}, 화면 비율 {aspect:.2f}"))
    return rows


def load(path):
    with open(path, encoding="utf-8-sig") as f:
        return json.load(f)


def main(argv):
    sys.stdout.reconfigure(encoding="utf-8")  # 콘솔 코드 페이지와 상관없이 한글을 출력한다
    if len(argv) >= 2 and argv[0] == "--check":
        folder = argv[1]
        files = sorted(f for f in os.listdir(folder) if f.endswith(".json")) if os.path.isdir(folder) else []
        failed = 0
        for f in files:
            path = os.path.join(folder, f)
            try:
                problems = check(path, load(path))
            except (ValueError, OSError) as e:
                problems = [f"읽을 수 없음: {e}"]
            print(f"{f}: {'통과' if not problems else '문제 ' + str(len(problems)) + '개'}")
            for p in problems:
                print(f"  - {p}")
            failed += bool(problems)
        print(f"검사한 파일 {len(files)}개, 문제 있는 파일 {failed}개")
        return 1 if failed else 0

    if not argv:
        print(__doc__ or "사용법: python -I tools/profile_info.py 파일.json [파일2.json]")
        return 1
    tables = [summary(load(p)) for p in argv]
    width = max(len(r[0]) for t in tables for r in t) + 2
    for i in range(max(len(t) for t in tables)):
        label = next(t[i][0] for t in tables if i < len(t))
        values = [t[i][1] if i < len(t) else "" for t in tables]
        print(f"{label:<{width}}" + "  |  ".join(values))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
