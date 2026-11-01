# Progressive JPEG fixture

SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0

Original 48×32 RGB gradient, generated with Pillow as a progressive JPEG at quality95 and subsampling0. Pixel `(x,y)` is `(floor(x*255/47),floor(y*255/31),floor((x+y)*255/78))`. No external image assets.

原创48×32渐进式JPEG梯度，Pillow质量95、无色度抽样；用于实际lowzag流式裁切/缩放及失败清理回归，不包含外部图片。
