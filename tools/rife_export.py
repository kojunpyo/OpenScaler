# RIFE 학습 모델을 프레임 생성에 쓸 ONNX 파일로 바꾼다.
#
# 사용법: python -I tools/rife_export.py flownet.pkl rife.onnx
# 필요: torch, onnx, onnxscript (변환할 때만 쓰고 배포물에는 들어가지 않는다)
#
# 아래 신경망 구조는 Practical-RIFE의 IFNet_HDv3(v4.25/4.26)를 옮긴 것이다.
# 원본: https://github.com/hzwer/Practical-RIFE (MIT, Copyright (c) 2021 hzwer)
# 바꾼 점: 학습용 코드를 빼고, 합성한 영상 대신 시점 0.5의 움직임과 마스크를 내보낸다.
# 합성은 원본 해상도에서 셰이더로 하므로 신경망을 낮은 해상도로 돌려도 화면이 흐려지지 않는다.
#
# 모델 파일은 파이썬 피클이라 그냥 열면 코드가 실행될 수 있다. weights_only=True로 가중치만 읽는다.

import sys

import torch
import torch.nn as nn
import torch.nn.functional as F


def conv(in_planes, out_planes, kernel_size=3, stride=1, padding=1):
    return nn.Sequential(nn.Conv2d(in_planes, out_planes, kernel_size, stride, padding, bias=True), nn.LeakyReLU(0.2, True))


def warp(image, flow):
    # 입력 크기가 바뀌어도 동작하도록 linspace와 expand 대신 arange와 브로드캐스트로 계산한다.
    # (linspace는 내보낼 때 상수로 굳고, DirectML은 크기가 바뀌는 expand를 실행하지 못한다.)
    _, _, h, w = image.shape
    ys = torch.arange(h, device=image.device, dtype=image.dtype).unsqueeze(1).unsqueeze(0)
    xs = torch.arange(w, device=image.device, dtype=image.dtype).unsqueeze(0).unsqueeze(0)
    half_w = (w - 1.0) / 2.0
    half_h = (h - 1.0) / 2.0
    gx = (xs + flow[:, 0]) / half_w - 1.0
    gy = (ys + flow[:, 1]) / half_h - 1.0
    return F.grid_sample(image, torch.stack((gx, gy), 3), mode="bilinear", padding_mode="border", align_corners=True)


class Head(nn.Module):
    def __init__(self):
        super().__init__()
        self.cnn0 = nn.Conv2d(3, 16, 3, 2, 1)
        self.cnn1 = nn.Conv2d(16, 16, 3, 1, 1)
        self.cnn2 = nn.Conv2d(16, 16, 3, 1, 1)
        self.cnn3 = nn.ConvTranspose2d(16, 4, 4, 2, 1)
        self.relu = nn.LeakyReLU(0.2, True)

    def forward(self, x):
        x = self.relu(self.cnn0(x))
        x = self.relu(self.cnn1(x))
        x = self.relu(self.cnn2(x))
        return self.cnn3(x)


class ResConv(nn.Module):
    def __init__(self, c):
        super().__init__()
        self.conv = nn.Conv2d(c, c, 3, 1, 1)
        self.beta = nn.Parameter(torch.ones((1, c, 1, 1)))
        self.relu = nn.LeakyReLU(0.2, True)

    def forward(self, x):
        return self.relu(self.conv(x) * self.beta + x)


class IFBlock(nn.Module):
    def __init__(self, in_planes, c):
        super().__init__()
        self.conv0 = nn.Sequential(conv(in_planes, c // 2, 3, 2, 1), conv(c // 2, c, 3, 2, 1))
        self.convblock = nn.Sequential(*[ResConv(c) for _ in range(8)])
        self.lastconv = nn.Sequential(nn.ConvTranspose2d(c, 4 * 13, 4, 2, 1), nn.PixelShuffle(2))

    def forward(self, x, flow, scale):
        size = x.shape[2:]
        small = (size[0] // scale, size[1] // scale)
        x = F.interpolate(x, size=small, mode="bilinear", align_corners=False)
        if flow is not None:
            flow = F.interpolate(flow, size=small, mode="bilinear", align_corners=False) / scale
            x = torch.cat((x, flow), 1)
        feat = self.convblock(self.conv0(x))
        tmp = F.interpolate(self.lastconv(feat), size=size, mode="bilinear", align_corners=False)
        return tmp[:, :4] * scale, tmp[:, 4:5], tmp[:, 5:]


class RifeFlow(nn.Module):
    """입력: 0~1 RGB 두 장 (가로세로 64의 배수). 출력: 시점 0.5에서 두 프레임으로 가는 움직임(4채널), 마스크(시그모이드 후)."""

    def __init__(self):
        super().__init__()
        self.block0 = IFBlock(7 + 8, 192)
        self.block1 = IFBlock(8 + 4 + 8 + 8, 128)
        self.block2 = IFBlock(8 + 4 + 8 + 8, 96)
        self.block3 = IFBlock(8 + 4 + 8 + 8, 64)
        self.block4 = IFBlock(8 + 4 + 8 + 8, 32)
        self.encode = Head()

    def forward(self, img0, img1):
        timestep = torch.ones_like(img0[:, :1]) * 0.5
        f0 = self.encode(img0)
        f1 = self.encode(img1)
        flow, mask, feat = self.block0(torch.cat((img0, img1, f0, f1, timestep), 1), None, 16)
        blocks = [(self.block1, 8), (self.block2, 4), (self.block3, 2), (self.block4, 1)]
        for block, scale in blocks:
            wf0 = warp(f0, flow[:, :2])
            wf1 = warp(f1, flow[:, 2:4])
            w0 = warp(img0, flow[:, :2])
            w1 = warp(img1, flow[:, 2:4])
            fd, mask, feat = block(torch.cat((w0, w1, wf0, wf1, timestep, mask, feat), 1), flow, scale)
            flow = flow + fd
        return flow, torch.sigmoid(mask)


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    source, target = sys.argv[1], sys.argv[2]
    state = torch.load(source, map_location="cpu", weights_only=True)
    state = {k.replace("module.", ""): v for k, v in state.items()}
    model = RifeFlow().eval()
    result = model.load_state_dict(state, strict=False)
    if result.missing_keys:
        raise SystemExit(f"가중치가 모자랍니다: {result.missing_keys[:5]}")
    print("학습 전용 가중치 제외:", sorted({k.split(".")[0] for k in result.unexpected_keys}))

    img0 = torch.rand(1, 3, 384, 640)
    img1 = torch.roll(img0, 8, 3)
    with torch.no_grad():
        flow, mask = model(img0, img1)
    print("출력 크기:", flow.shape, mask.shape, "움직임 평균:", flow.abs().mean().item())

    h = torch.export.Dim("h", min=1, max=64)
    w = torch.export.Dim("w", min=1, max=64)
    shapes = {"img0": {2: h * 64, 3: w * 64}, "img1": {2: h * 64, 3: w * 64}}
    program = torch.onnx.export(model, (img0, img1), input_names=["img0", "img1"], output_names=["flow", "mask"],
                                dynamo=True, opset_version=17, dynamic_shapes=shapes, external_data=False)
    program.save(target, external_data=False)
    print("저장:", target)


if __name__ == "__main__":
    main()
