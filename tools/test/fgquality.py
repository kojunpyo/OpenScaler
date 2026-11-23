# --fg-gaptest 결과(fg_gaptest.csv)를 README 6.1 "평가 기준"으로 매긴다.
# 사용법: python -I tools/test/fgquality.py fg_gaptest.csv
#
# 오차 감소율: 프레임 생성이 없을 때(앞 게임 프레임을 그대로 보여 줄 때) 정답과의 오차(평균 제곱 오차) 중 몇 %를 없앴는지.
# 픽셀 오차: PSNR을 0~255 밝기 단계의 평균 오차(RMSE)로 바꾼 값.
import csv
import sys


def rmse(psnr):
    return 255 / 10 ** (psnr / 20)


def reduction(psnr, hold):
    return 1 - 10 ** (-(psnr - hold) / 10)


def grade(r):
    return "좋음" if r >= 0.85 else "보통" if r >= 0.65 else "아쉬움" if r >= 0.40 else "나쁨"


rows = list(csv.DictReader(open(sys.argv[1], encoding="utf-8")))
columns = [("blend_db", "단순 섞기"), ("block_db", "블록 매칭"), ("block_longgap_db", "블록 (긴 간격)"), ("rife_db", "RIFE")]
print("오차 감소율 기준: 85% 이상 좋음, 65% 이상 보통, 40% 이상 아쉬움, 그 아래 나쁨 (단순 섞기가 보통 30~40%)")
for row in rows:
    hold = float(row["hold_db"])
    # 녹화의 이웃한 프레임 간격(frame_ms)이 있으면 그것으로, 없으면 60fps 녹화로 보고 게임 fps로 바꿔 보여 준다.
    frame_ms = float(row.get("frame_ms") or 1000 / 60)
    pair_ms = frame_ms * int(row["gap"])
    print(f"\n{row['gap']}칸, 시점 {float(row['t']):.2f}: 앞뒤 프레임 사이 {pair_ms:.0f}ms (게임 {1000 / pair_ms:.0f}fps를 채우는 조건)")
    print(f"  {'멈춤 (생성 없음)':<14}{hold:6.2f}dB  픽셀 오차 {rmse(hold):5.1f}단계")
    for key, name in columns:
        if key not in row or not row[key] or float(row[key]) == 0:
            continue
        v = float(row[key])
        r = reduction(v, hold)
        print(f"  {name:<14}{v:6.2f}dB  픽셀 오차 {rmse(v):5.1f}단계  오차 감소 {100 * r:4.0f}%  {grade(r)}")
