# third_party — 空目录，有意保留

KTile 的硬指标之一是「单 exe、零外部依赖」，因此**没有引入任何第三方
源码库**：

- JSON 解析/写入：`src/infrastructure/json/` 自研（够用即可，不追求
  RFC 全覆盖）
- UI / 渲染：只用系统自带的 Direct2D、DirectWrite、WIC
- 图标：Lucide 的 SVG 形状在**构建前**用脚本转成 C++ 字符串常量
  （见 `tools/`），运行时不依赖 Lucide 源码

本目录保留下来，是为了将来若确有必要引入第三方代码时，有一个固定的
位置放源码与许可证说明。当前为空。
