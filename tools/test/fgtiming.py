# --fg-log 기록을 분석해 실시간 프레임 생성을 README의 "성능 평가 기준"으로 매긴다.
# 사용법: python -I tools/test/fgtiming.py fg.csv
#
# 기록의 열: 시각, 표시 시점, 게임 프레임 수, 걸러 낸 중복 수, RIFE 합성 수, RIFE 대신 블록 매칭 수, 신경망 ms, 화면 속 게임 시각, RIFE 입력 높이
# 주사율은 출력 간격의 중앙값으로 정한다 (수직 동기화로 화면마다 한 번 그리므로). 그래서 60Hz와 144Hz 모니터에 같은 기준을 쓴다.
import statistics
import sys


def grade(value, good, fair, higher_is_better=True):
    if higher_is_better:
        return "좋음" if value >= good else "보통" if value >= fair else "나쁨"
    return "좋음" if value <= good else "보통" if value <= fair else "나쁨"


rows = []
heights = []
for line in open(sys.argv[1]):
    a = line.strip().split(',')
    if len(a) >= 8 and a[0] != 'slowmo':
        rows.append((float(a[0]), float(a[1]), int(a[2]), int(a[3]), int(a[4]), int(a[5]), float(a[6]), float(a[7])))
        if len(a) >= 9:
            heights.append(a[8])
rows = rows[len(rows) // 5:]  # 시작 직후(RIFE 준비, 표시 시계 안정)는 뺀다
heights = heights[len(heights) // 5:]

times = [r[0] for r in rows]
gaps = [(b - a) * 1000 for a, b in zip(times, times[1:])]
duration = times[-1] - times[0]
minutes = duration / 60
frame_ms = statistics.median(gaps)          # 주사율 한 칸
refresh = 1000 / frame_ms
output_fps = len(gaps) / duration
game_fps = (rows[-1][2] - rows[0][2]) / duration

steps = [(b[7] - a[7]) * 1000 for a, b in zip(rows, rows[1:]) if b[7] > 0 and a[7] > 0]
stalls = sum(abs(s) < 0.5 for s in steps)
backs = sum(s < -0.5 for s in steps)
skips = sum(s > 2 * frame_ms for s in steps)
hitches = sum(g > 1.5 * frame_ms for g in gaps)
latency = statistics.median([(r[0] - r[7]) * 1000 for r in rows if r[7] > 0])
game_ms = 1000 / game_fps if game_fps > 0 else 0

print(f"측정 {duration:.1f}초, 주사율 약 {refresh:.0f}Hz (한 칸 {frame_ms:.2f}ms), 게임 {game_fps:.1f}fps")
print(f"{'항목':<22}{'값':>22}  등급  (좋음 / 보통 기준)")


def show(name, text, g, rule):
    print(f"{name:<22}{text:>22}  {g}  ({rule})")


show("출력 fps / 주사율", f"{output_fps:.1f}fps ({100 * output_fps / refresh:.0f}%)", grade(output_fps / refresh, 0.98, 0.90), "98% / 90% 이상")
show("출력 끊김 (1.5칸 넘는 간격)", f"분당 {hitches / minutes:.1f}번", grade(hitches / minutes, 1, 6, False), "분당 1번 / 6번 이하")
if steps:
    spread = statistics.pstdev(steps) / frame_ms
    show("화면 속 시간 고르기", f"표준편차 {statistics.pstdev(steps):.1f}ms ({spread:.2f}칸)", grade(spread, 0.25, 0.5, False), "0.25칸 / 0.5칸 이하")
    show("멈춤 (같은 장면 반복)", f"{100 * stalls / len(steps):.1f}%", grade(stalls / len(steps), 0.01, 0.05, False), "1% / 5% 이하")
    show("건너뜀 (2칸 넘게)", f"분당 {skips / minutes:.1f}번", grade(skips / minutes, 1, 6, False), "분당 1번 / 6번 이하")
    show("되돌아감", f"{backs}번", "좋음" if backs == 0 else "나쁨", "0번")
added = latency - game_ms
show("지연 (게임 프레임 간격 제외)", f"{latency:.1f}ms (+{added:.1f}ms)" if latency > 1 else f"{latency:.1f}ms", grade(added, 10, 25, False), "+10ms / +25ms 이하")
shown = rows[-1][4] - rows[0][4]
missed = rows[-1][5] - rows[0][5]
if shown + missed > 0:
    rate = shown / (shown + missed)
    show("RIFE를 제때 쓴 비율", f"{100 * rate:.0f}% ({rows[-1][6]:.1f}ms)", grade(rate, 0.95, 0.80), "95% / 80% 이상")
    if heights:
        print(f"RIFE 입력 높이: 처음 {heights[0]}, 마지막 {heights[-1]}")
if game_fps > refresh / 1.15:
    print("참고: 게임 fps가 주사율에 가까워 프레임을 넣을 자리가 없습니다. 화면 속 시간과 지연 항목은 의미가 없습니다.")
