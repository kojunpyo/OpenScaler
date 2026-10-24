#pragma once

#include <cstdint>

// src/fsr.hlsl의 cbuffer FsrParams와 같은 레이아웃
struct FsrParams {
    uint32_t easuCon0[4];
    uint32_t easuCon1[4];
    uint32_t easuCon2[4];
    uint32_t easuCon3[4];
    uint32_t rcasCon[4];
    int32_t outputOffset[2];
    int32_t pad[2];
};

// AMD 공식 헤더의 CPU용 함수로 EASU/RCAS 상수를 계산한다.
// sharpness: 0(가장 선명) ~ N (값이 1 커질 때마다 샤프닝이 절반)
void ComputeFsrParams(FsrParams& p, float srcW, float srcH, float dstW, float dstH, float rcasSharpnessStops);
