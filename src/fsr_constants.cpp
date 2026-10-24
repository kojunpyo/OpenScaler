// AMD FSR 헤더의 CPU 쪽 코드는 매크로가 많아서 이 파일 안에만 포함한다.
#include "fsr_constants.h"

#include <cmath>

#define A_CPU 1
#include "ffx_a.h"
#include "ffx_fsr1.h"

void ComputeFsrParams(FsrParams& p, float srcW, float srcH, float dstW, float dstH, float rcasSharpnessStops)
{
    FsrEasuCon(p.easuCon0, p.easuCon1, p.easuCon2, p.easuCon3, srcW, srcH, srcW, srcH, dstW, dstH);
    FsrRcasCon(p.rcasCon, rcasSharpnessStops);
}
