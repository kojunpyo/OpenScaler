# --fg-log 기록을 분석해 실시간 프레임 생성을 README의 "성능 평가 기준"으로 매긴다.
# 사용법: python -I tools/test/fgtiming.py fg.csv
#
# 기록의 열: 시각, 표시 시점, 게임 프레임 수, 걸러 낸 중복 수, RIFE 합성 수, RIFE 대신 블록 매칭 수, 신경망 ms, 화면 속 게임 시각, RIFE 입력 높이,
#           (1.3.1부터) 움직임 찾기 GPU ms, 합성 GPU ms, 작업 너비, 작업 높이 (프레임 생성 영역을 지정하면 영역 크기)
#           (1.3.3부터) 게임 프레임을 넘기는 데 CPU가 머문 ms, 표시 프레임을 만드는 데 머문 ms, 받아들이기 지연 ms, RIFE 결과 지연 ms(표시에 쓴 값),
#           마지막 RIFE 결과의 지연 ms, 캡처부터 넘길 때까지 ms (새 프레임이 없으면 -1)
# 주사율은 출력 간격의 중앙값으로 정한다 (수직 동기화로 화면마다 한 번 그리므로). 그래서 60Hz와 144Hz 모니터에 같은 기준을 쓴다.
import statistics
import sys


def grade(value, good, fair, higher_is_better=True):
    if higher_is_better:
        return "좋음" if value >= good else "보통" if value >= fair else "나쁨"
    return "좋음" if value <= good else "보통" if value <= fair else "나쁨"


rows = []
heights = []
gpu = []  # (움직임 찾기 ms, 합성 ms, 작업 너비, 작업 높이)
cpu = []  # (넘기기 ms, 만들기 ms, RIFE 결과 지연 ms)
for line in open(sys.argv[1]):
    a = line.strip().split(',')
    if len(a) >= 8 and a[0] != 'slowmo':
        rows.append((float(a[0]), float(a[1]), int(a[2]), int(a[3]), int(a[4]), int(a[5]), float(a[6]), float(a[7])))
        if len(a) >= 9:
            heights.append(a[8])
        if len(a) >= 13:
            gpu.append((float(a[9]), float(a[10]), int(a[11]), int(a[12])))
        if len(a) >= 18:
            cpu.append((float(a[13]), float(a[14]), float(a[17])))
rows = rows[len(rows) // 5:]  # 시작 직후(RIFE 준비, 표시 시계 안정)는 뺀다
heights = heights[len(heights) // 5:]
gpu = gpu[len(gpu) // 5:]
cpu = cpu[len(cpu) // 5:]

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
lat_values = [(r[0] - r[7]) * 1000 for r in rows if r[7] > 0]
latency = statistics.median(lat_values) if lat_values else 0  # 넣을 자리가 없으면 화면 속 시각이 기록되지 않는다
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
if lat_values:
    show("지연 (게임 프레임 간격 제외)", f"{latency:.1f}ms (+{added:.1f}ms)" if latency > 1 else f"{latency:.1f}ms", grade(added, 10, 25, False), "+10ms / +25ms 이하")
shown = rows[-1][4] - rows[0][4]
missed = rows[-1][5] - rows[0][5]
if shown + missed > 0:
    rate = shown / (shown + missed)
    show("RIFE를 제때 쓴 비율", f"{100 * rate:.0f}% ({rows[-1][6]:.1f}ms)", grade(rate, 0.95, 0.80), "95% / 80% 이상")
    if heights:
        print(f"RIFE 입력 높이: 처음 {heights[0]}, 마지막 {heights[-1]}")
if gpu:
    est = statistics.median(g[0] for g in gpu)
    syn = statistics.median(g[1] for g in gpu)
    w, h = gpu[-1][2], gpu[-1][3]
    print(f"GPU 시간 (중앙값): 움직임 찾기 {est:.2f}ms (게임 프레임마다), 합성 {syn:.2f}ms (화면마다), 작업 크기 {w}x{h}")
if cpu:
    # CPU가 GPU 결과를 기다리느라 멈추면 넘기기 시간이 길어지고 출력이 끊긴다 (1.3.2까지 RIFE에서 10~15ms).
    work = sorted(c[0] + c[1] for c in cpu)
    print(f"CPU 시간 (넘기기+만들기): 중앙값 {work[len(work) // 2]:.2f}ms, 최대 {work[-1]:.2f}ms")
    lags = sorted({c[2] for c in cpu if c[2] > 0})
    if lags:
        print(f"RIFE 결과 지연 (캡처부터): 중앙값 {lags[len(lags) // 2]:.1f}ms, 최대 {lags[-1]:.1f}ms")
if game_fps > refresh / 1.15:
    print("참고: 게임 fps가 주사율에 가까워 프레임을 넣을 자리가 없습니다. 화면 속 시간과 지연 항목은 의미가 없습니다.")
