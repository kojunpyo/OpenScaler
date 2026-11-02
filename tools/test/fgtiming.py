# --fg-log 기록을 분석한다: 출력 간격, 진짜 프레임 수, 화면에 보이는 게임 시간의 진행이 고른지 본다.
import statistics
import sys

rows = []
for line in open(sys.argv[1]):
    a = line.strip().split(',')
    if len(a) >= 8:
        rows.append((float(a[0]), float(a[1]), int(a[2]), int(a[3]), int(a[4]), int(a[5]), float(a[6]), float(a[7])))
rows = rows[len(rows) // 5:]  # 시작 직후는 뺀다

times = [r[0] for r in rows]
gaps = [(b - a) * 1000 for a, b in zip(times, times[1:])]
duration = times[-1] - times[0]
print(f"측정 {duration:.1f}초")
print(f"출력 {len(gaps) / duration:.1f} fps, 간격 중앙값 {statistics.median(gaps):.2f} ms, 표준편차 {statistics.pstdev(gaps):.2f} ms, "
      f"최대 {max(gaps):.1f} ms, 25ms 초과 {sum(g > 25 for g in gaps)}회")
print(f"진짜 프레임 {(rows[-1][2] - rows[0][2]) / duration:.1f} fps, 걸러 낸 중복 {(rows[-1][3] - rows[0][3]) / duration:.1f}개/초")

# 화면에 보이는 게임 시간. 칸마다 16.7ms씩 늘어나는 것이 이상적이다.
steps = [(b[7] - a[7]) * 1000 for a, b in zip(rows, rows[1:]) if b[7] > 0 and a[7] > 0]
print(f"화면 속 시간 진행(칸당): 중앙값 {statistics.median(steps):.1f} ms, 표준편차 {statistics.pstdev(steps):.1f} ms, "
      f"멈춤 {sum(abs(s) < 0.5 for s in steps)}번, 되돌아감 {sum(s < -0.5 for s in steps)}번, 33ms 넘게 건너뜀 {sum(s > 33 for s in steps)}번 (전체 {len(steps)}칸)")

latency = [(r[0] - r[7]) * 1000 for r in rows if r[7] > 0]
print(f"지연(캡처 시각 기준, 계산과 표시 대기는 제외): 중앙값 {statistics.median(latency):.1f} ms")

shown = rows[-1][4] - rows[0][4]
missed = rows[-1][5] - rows[0][5]
if shown + missed > 0:
    print(f"RIFE: 합성 {shown}번, 블록 매칭으로 대신 {missed}번 ({100 * shown / (shown + missed):.0f}%가 RIFE), 신경망 평균 {rows[-1][6]:.1f} ms")
