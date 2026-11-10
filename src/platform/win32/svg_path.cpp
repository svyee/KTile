#include "platform/win32/svg_path.h"

#include <d2d1helper.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <string>
#include <utility>
#include <vector>

namespace ktile {
namespace platform {
namespace win32 {
namespace {

// ---------------------------------------------------------------- 仿射变换
// D2D 用行向量约定：p' = p * M，因此 x' = x*m11 + y*m21 + dx。
struct Affine {
    float m11 = 1.0F;
    float m12 = 0.0F;
    float m21 = 0.0F;
    float m22 = 1.0F;
    float dx = 0.0F;
    float dy = 0.0F;
};

D2D1_POINT_2F mapPoint(const Affine& a, const float x, const float y) {
    return D2D1::Point2F(x * a.m11 + y * a.m21 + a.dx,
                         x * a.m12 + y * a.m22 + a.dy);
}

float uniformScaleOf(const Affine& a) {
    return std::sqrt(a.m11 * a.m11 + a.m12 * a.m12);
}

// result = outer ∘ inner（先把点交给 inner，再交给 outer）
Affine compose(const Affine& outer, const Affine& inner) {
    Affine result;
    result.m11 = inner.m11 * outer.m11 + inner.m12 * outer.m21;
    result.m12 = inner.m11 * outer.m12 + inner.m12 * outer.m22;
    result.m21 = inner.m21 * outer.m11 + inner.m22 * outer.m21;
    result.m22 = inner.m21 * outer.m12 + inner.m22 * outer.m22;
    result.dx = inner.dx * outer.m11 + inner.dy * outer.m21 + outer.dx;
    result.dy = inner.dx * outer.m12 + inner.dy * outer.m22 + outer.dy;
    return result;
}

// ---------------------------------------------------------------- 词法
bool isSeparator(const wchar_t c) {
    return c == L' ' || c == L',' || c == L'\t' || c == L'\n' || c == L'\r';
}

void skipSeparators(const wchar_t** cursor) {
    const wchar_t* p = *cursor;
    while (*p != L'\0' && isSeparator(*p)) {
        ++p;
    }
    *cursor = p;
}

// 读一个 SVG 数字。SVG 允许「5.4.8」这种省略分隔符的写法，
// wcstod 正好也是同样的贪婪语义（读 5.4，下一个从 .8 开始）。
bool readNumber(const wchar_t** cursor, float* out) {
    skipSeparators(cursor);
    if (**cursor == L'\0') {
        return false;
    }
    wchar_t* end = nullptr;
    const double value = std::wcstod(*cursor, &end);
    if (end == *cursor) {
        return false;
    }
    *out = static_cast<float>(value);
    *cursor = end;
    return true;
}

// ---------------------------------------------------------------- 写入器
// 把一个 SVG 图元的命令序列写进 D2D 几何接收器。
// 坐标一律以「SVG 用户单位」传入，由内部按仿射矩阵映射到目标画布 ——
// 这样解析器可以完全不关心缩放。
class PathWriter {
public:
    PathWriter(ID2D1GeometrySink* sink, const Affine& transform,
               const D2D1_FIGURE_BEGIN figureBegin)
        : sink_(sink),
          transform_(transform),
          figureBegin_(figureBegin),
          scale_(uniformScaleOf(transform)) {}

    void moveTo(const float x, const float y) {
        endFigure(D2D1_FIGURE_END_OPEN);
        current_ = mapPoint(transform_, x, y);
        subpathStart_ = current_;
        expandBounds(current_);
        sink_->BeginFigure(current_, figureBegin_);
        open_ = true;
    }

    void lineTo(const float x, const float y) {
        ensureOpen();
        const D2D1_POINT_2F start = current_;
        current_ = mapPoint(transform_, x, y);
        expandBounds(current_);
        addSegment(start, current_);
        sink_->AddLine(current_);
    }

    void cubicTo(const float x1, const float y1, const float x2, const float y2,
                 const float x, const float y) {
        ensureOpen();
        D2D1_BEZIER_SEGMENT segment;
        segment.point1 = mapPoint(transform_, x1, y1);
        segment.point2 = mapPoint(transform_, x2, y2);
        segment.point3 = mapPoint(transform_, x, y);
        expandBounds(segment.point1);
        expandBounds(segment.point2);
        expandBounds(segment.point3);
        // 用 de Casteljau 细分采样贝塞尔，累加弧长加权质心。
        sampleBezier(current_, segment.point1, segment.point2, segment.point3, 4);
        sink_->AddBezier(&segment);
        current_ = segment.point3;
    }

    void quadTo(const float x1, const float y1, const float x, const float y) {
        ensureOpen();
        D2D1_QUADRATIC_BEZIER_SEGMENT segment;
        segment.point1 = mapPoint(transform_, x1, y1);
        segment.point2 = mapPoint(transform_, x, y);
        expandBounds(segment.point1);
        expandBounds(segment.point2);
        // 二次贝塞尔转三次（p1,p1,p2）后用同一采样器
        sampleBezier(current_, segment.point1, segment.point1, segment.point2, 3);
        sink_->AddQuadraticBezier(&segment);
        current_ = segment.point2;
    }

    // SVG 的 arc 与 D2D 的 D2D1_ARC_SEGMENT 语义几乎一一对应：
    // sweep-flag=1（顺时针，y 轴向下）对应 D2D1_SWEEP_DIRECTION_CLOCKWISE；
    // large-arc-flag 对应 D2D1_ARC_SIZE_LARGE。半径按缩放系数换算。
    void arcTo(const float rx, const float ry, const float rotation,
               const bool largeArc, const bool sweep, const float x,
               const float y) {
        ensureOpen();
        const D2D1_POINT_2F start = current_;
        const D2D1_POINT_2F end = mapPoint(transform_, x, y);
        // 半径为 0 或起终点重合时，SVG 规定退化成直线
        if (rx <= 0.0F || ry <= 0.0F || (end.x == start.x && end.y == start.y)) {
            sink_->AddLine(end);
            expandBounds(end);
            addSegment(start, end);
            current_ = end;
            return;
        }
        D2D1_ARC_SEGMENT segment;
        segment.point = end;
        segment.size = D2D1::SizeF(rx * scale_, ry * scale_);
        segment.rotationAngle = rotation;
        segment.sweepDirection = sweep ? D2D1_SWEEP_DIRECTION_CLOCKWISE
                                       : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE;
        segment.arcSize = largeArc ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL;
        sink_->AddArc(&segment);
        // 弧的包围盒粗略用起终点 + 半径估算。
        const float rxPx = rx * scale_;
        const float ryPx = ry * scale_;
        expandBounds(D2D1::Point2F(end.x - rxPx, end.y - ryPx));
        expandBounds(D2D1::Point2F(end.x + rxPx, end.y + ryPx));
        expandBounds(end);
        // 弧长质心：用弦细分近似（圆/椭圆弧，8 段足够居中用）。
        sampleArc(start, end, rx * scale_, ry * scale_, 8);
        current_ = end;
    }

    void closePath() {
        if (!open_) {
            return;
        }
        sink_->EndFigure(D2D1_FIGURE_END_CLOSED);
        open_ = false;
        // Z 之后未指定 M 就继续画时，新子路径从原子路径起点开始
        current_ = subpathStart_;
    }

    void finish() { endFigure(D2D1_FIGURE_END_OPEN); }

    bool hasBounds() const { return hasBounds_; }
    D2D1_RECT_F bounds() const { return bounds_; }

    // 弧长加权质心（描边图元的视觉中心近似）。
    // 剪刀左重右轻、撤销箭头尾重头轻这类不对称形状，用包围盒中心会
    // 把视觉重心甩到一边；用沿路径长度加权的质心更接近人眼看到的中心。
    bool hasCentroid() const { return totalLength_ > 0.0F; }
    D2D1_POINT_2F centroid() const {
        return D2D1::Point2F(weightedCx_ / totalLength_,
                             weightedCy_ / totalLength_);
    }
    float totalLength() const { return totalLength_; }

private:
    void ensureOpen() {
        if (!open_) {
            subpathStart_ = current_;
            sink_->BeginFigure(current_, figureBegin_);
            open_ = true;
        }
    }

    void endFigure(const D2D1_FIGURE_END end) {
        if (!open_) {
            return;
        }
        sink_->EndFigure(end);
        open_ = false;
    }

    void expandBounds(const D2D1_POINT_2F& p) {
        if (!hasBounds_) {
            bounds_ = D2D1::RectF(p.x, p.y, p.x, p.y);
            hasBounds_ = true;
        } else {
            if (p.x < bounds_.left) bounds_.left = p.x;
            if (p.y < bounds_.top) bounds_.top = p.y;
            if (p.x > bounds_.right) bounds_.right = p.x;
            if (p.y > bounds_.bottom) bounds_.bottom = p.y;
        }
    }

    void addSegment(const D2D1_POINT_2F& a, const D2D1_POINT_2F& b) {
        const float dx = b.x - a.x;
        const float dy = b.y - a.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len <= 0.0F) return;
        totalLength_ += len;
        weightedCx_ += (a.x + b.x) * 0.5F * len;
        weightedCy_ += (a.y + b.y) * 0.5F * len;
    }

    void sampleBezier(const D2D1_POINT_2F& p0, const D2D1_POINT_2F& p1,
                      const D2D1_POINT_2F& p2, const D2D1_POINT_2F& p3,
                      int subdivisions) {
        D2D1_POINT_2F prev = p0;
        for (int i = 1; i <= subdivisions; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(subdivisions);
            const float u = 1.0F - t;
            const D2D1_POINT_2F pt = D2D1::Point2F(
                u * u * u * p0.x + 3.0F * u * u * t * p1.x +
                    3.0F * u * t * t * p2.x + t * t * t * p3.x,
                u * u * u * p0.y + 3.0F * u * u * t * p1.y +
                    3.0F * u * t * t * p2.y + t * t * t * p3.y);
            addSegment(prev, pt);
            prev = pt;
        }
    }

    void sampleArc(const D2D1_POINT_2F& start, const D2D1_POINT_2F& end,
                   float rx, float ry, int segments) {
        // 不精确反解椭圆参数，直接用起终点间的圆弧细分：把弦等分后
        // 沿半径方向外推，足以近似弧长质心。
        if (rx <= 0.0F) rx = ry;
        if (ry <= 0.0F) ry = rx;
        const float r = (rx + ry) * 0.5F;
        const float dx = end.x - start.x;
        const float dy = end.y - start.y;
        const float chord = std::sqrt(dx * dx + dy * dy);
        if (chord <= 0.0F || r <= 0.0F) {
            addSegment(start, end);
            return;
        }
        // 弓形高度（近似）：h = r - sqrt(r^2 - (chord/2)^2)
        const float halfChord = chord * 0.5F;
        float h = r - std::sqrt((std::max)(0.0F, r * r - halfChord * halfChord));
        if (h > r) h = r;
        const float nx = -dy / chord;
        const float ny = dx / chord;
        D2D1_POINT_2F prev = start;
        for (int i = 1; i <= segments; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(segments);
            const float bx = start.x + dx * t;
            const float by = start.y + dy * t;
            const float sagitta = 4.0F * h * t * (1.0F - t);
            const D2D1_POINT_2F pt =
                D2D1::Point2F(bx + nx * sagitta, by + ny * sagitta);
            addSegment(prev, pt);
            prev = pt;
        }
    }

    ID2D1GeometrySink* sink_;
    Affine transform_;
    D2D1_FIGURE_BEGIN figureBegin_;
    float scale_;
    D2D1_POINT_2F current_ = D2D1::Point2F(0.0F, 0.0F);
    D2D1_POINT_2F subpathStart_ = D2D1::Point2F(0.0F, 0.0F);
    bool open_ = false;
    bool hasBounds_ = false;
    D2D1_RECT_F bounds_ = D2D1::RectF(0.0F, 0.0F, 0.0F, 0.0F);
    float totalLength_ = 0.0F;
    float weightedCx_ = 0.0F;
    float weightedCy_ = 0.0F;
};

// ---------------------------------------------------------------- 路径数据
// 支持 M L H V C S Q T A Z（大小写 = 相对/绝对）。返回 false 表示数据有
// 语法错误 —— 交给调用方决定是否降级，而不是静默画出半个图标。
bool parsePathData(const wchar_t* data, PathWriter* writer) {
    const wchar_t* p = data;
    wchar_t command = L'\0';
    wchar_t previousCommand = L'\0';
    float currentX = 0.0F;
    float currentY = 0.0F;
    float startX = 0.0F;
    float startY = 0.0F;
    float lastCubicX = 0.0F;
    float lastCubicY = 0.0F;
    float lastQuadX = 0.0F;
    float lastQuadY = 0.0F;

    for (;;) {
        skipSeparators(&p);
        if (*p == L'\0') {
            break;
        }
        if (std::iswalpha(static_cast<wint_t>(*p)) != 0) {
            command = *p;
            ++p;
        } else if (command == L'\0') {
            return false;  // 第一个标记必须是命令
        }

        const wchar_t upper = static_cast<wchar_t>(
            std::towupper(static_cast<wint_t>(command)));
        const bool absolute = std::iswupper(static_cast<wint_t>(command)) != 0;

        switch (upper) {
            case L'M': {
                float x = 0.0F;
                float y = 0.0F;
                if (!readNumber(&p, &x) || !readNumber(&p, &y)) {
                    return false;
                }
                currentX = absolute ? x : currentX + x;
                currentY = absolute ? y : currentY + y;
                startX = currentX;
                startY = currentY;
                writer->moveTo(currentX, currentY);
                // 后续隐式坐标按「直线」处理（SVG 规范）
                command = absolute ? L'L' : L'l';
                break;
            }
            case L'L': {
                float x = 0.0F;
                float y = 0.0F;
                if (!readNumber(&p, &x) || !readNumber(&p, &y)) {
                    return false;
                }
                currentX = absolute ? x : currentX + x;
                currentY = absolute ? y : currentY + y;
                writer->lineTo(currentX, currentY);
                break;
            }
            case L'H': {
                float x = 0.0F;
                if (!readNumber(&p, &x)) {
                    return false;
                }
                currentX = absolute ? x : currentX + x;
                writer->lineTo(currentX, currentY);
                break;
            }
            case L'V': {
                float y = 0.0F;
                if (!readNumber(&p, &y)) {
                    return false;
                }
                currentY = absolute ? y : currentY + y;
                writer->lineTo(currentX, currentY);
                break;
            }
            case L'C': {
                float x1 = 0.0F;
                float y1 = 0.0F;
                float x2 = 0.0F;
                float y2 = 0.0F;
                float x = 0.0F;
                float y = 0.0F;
                if (!readNumber(&p, &x1) || !readNumber(&p, &y1) ||
                    !readNumber(&p, &x2) || !readNumber(&p, &y2) ||
                    !readNumber(&p, &x) || !readNumber(&p, &y)) {
                    return false;
                }
                if (!absolute) {
                    x1 += currentX; y1 += currentY;
                    x2 += currentX; y2 += currentY;
                    x += currentX; y += currentY;
                }
                writer->cubicTo(x1, y1, x2, y2, x, y);
                lastCubicX = x2;
                lastCubicY = y2;
                currentX = x;
                currentY = y;
                break;
            }
            case L'S': {
                float x2 = 0.0F;
                float y2 = 0.0F;
                float x = 0.0F;
                float y = 0.0F;
                if (!readNumber(&p, &x2) || !readNumber(&p, &y2) ||
                    !readNumber(&p, &x) || !readNumber(&p, &y)) {
                    return false;
                }
                if (!absolute) {
                    x2 += currentX; y2 += currentY;
                    x += currentX; y += currentY;
                }
                const wchar_t previousUpper = static_cast<wchar_t>(
                    std::towupper(static_cast<wint_t>(previousCommand)));
                const bool reflect = previousUpper == L'C' || previousUpper == L'S';
                const float x1 = reflect ? 2.0F * currentX - lastCubicX : currentX;
                const float y1 = reflect ? 2.0F * currentY - lastCubicY : currentY;
                writer->cubicTo(x1, y1, x2, y2, x, y);
                lastCubicX = x2;
                lastCubicY = y2;
                currentX = x;
                currentY = y;
                break;
            }
            case L'Q': {
                float x1 = 0.0F;
                float y1 = 0.0F;
                float x = 0.0F;
                float y = 0.0F;
                if (!readNumber(&p, &x1) || !readNumber(&p, &y1) ||
                    !readNumber(&p, &x) || !readNumber(&p, &y)) {
                    return false;
                }
                if (!absolute) {
                    x1 += currentX; y1 += currentY;
                    x += currentX; y += currentY;
                }
                writer->quadTo(x1, y1, x, y);
                lastQuadX = x1;
                lastQuadY = y1;
                currentX = x;
                currentY = y;
                break;
            }
            case L'T': {
                float x = 0.0F;
                float y = 0.0F;
                if (!readNumber(&p, &x) || !readNumber(&p, &y)) {
                    return false;
                }
                if (!absolute) {
                    x += currentX;
                    y += currentY;
                }
                const wchar_t previousUpper = static_cast<wchar_t>(
                    std::towupper(static_cast<wint_t>(previousCommand)));
                const bool reflect = previousUpper == L'Q' || previousUpper == L'T';
                const float x1 = reflect ? 2.0F * currentX - lastQuadX : currentX;
                const float y1 = reflect ? 2.0F * currentY - lastQuadY : currentY;
                writer->quadTo(x1, y1, x, y);
                lastQuadX = x1;
                lastQuadY = y1;
                currentX = x;
                currentY = y;
                break;
            }
            case L'A': {
                float rx = 0.0F;
                float ry = 0.0F;
                float rotation = 0.0F;
                float largeArc = 0.0F;
                float sweep = 0.0F;
                float x = 0.0F;
                float y = 0.0F;
                if (!readNumber(&p, &rx) || !readNumber(&p, &ry) ||
                    !readNumber(&p, &rotation) || !readNumber(&p, &largeArc) ||
                    !readNumber(&p, &sweep) || !readNumber(&p, &x) ||
                    !readNumber(&p, &y)) {
                    return false;
                }
                if (!absolute) {
                    x += currentX;
                    y += currentY;
                }
                writer->arcTo(rx, ry, rotation, largeArc != 0.0F, sweep != 0.0F,
                              x, y);
                currentX = x;
                currentY = y;
                break;
            }
            case L'Z': {
                writer->closePath();
                currentX = startX;
                currentY = startY;
                break;
            }
            default:
                return false;  // 未知命令：宁可整体失败，也不画出残缺图标
        }

        previousCommand = command;
    }
    return true;
}

// ---------------------------------------------------------------- 形状图元
void emitRect(PathWriter* writer, float x, float y, float width, float height,
              float rx, float ry) {
    if (width <= 0.0F || height <= 0.0F) {
        return;
    }
    if (rx <= 0.0F || ry <= 0.0F) {
        writer->moveTo(x, y);
        writer->lineTo(x + width, y);
        writer->lineTo(x + width, y + height);
        writer->lineTo(x, y + height);
        writer->closePath();
        return;
    }
    rx = (std::min)(rx, width * 0.5F);
    ry = (std::min)(ry, height * 0.5F);
    // 顺时针一圈，从顶边左侧圆角后开始
    writer->moveTo(x + rx, y);
    writer->lineTo(x + width - rx, y);
    writer->arcTo(rx, ry, 0.0F, false, true, x + width, y + ry);
    writer->lineTo(x + width, y + height - ry);
    writer->arcTo(rx, ry, 0.0F, false, true, x + width - rx, y + height);
    writer->lineTo(x + rx, y + height);
    writer->arcTo(rx, ry, 0.0F, false, true, x, y + height - ry);
    writer->lineTo(x, y + ry);
    writer->arcTo(rx, ry, 0.0F, false, true, x + rx, y);
    writer->closePath();
}

void emitEllipse(PathWriter* writer, const float cx, const float cy,
                 const float rx, const float ry) {
    if (rx <= 0.0F || ry <= 0.0F) {
        return;
    }
    writer->moveTo(cx - rx, cy);
    writer->arcTo(rx, ry, 0.0F, false, true, cx + rx, cy);
    writer->arcTo(rx, ry, 0.0F, false, true, cx - rx, cy);
    writer->closePath();
}

void emitPolyline(PathWriter* writer, const wchar_t* points, const bool close) {
    const wchar_t* p = points;
    float x = 0.0F;
    float y = 0.0F;
    if (!readNumber(&p, &x) || !readNumber(&p, &y)) {
        return;
    }
    writer->moveTo(x, y);
    while (readNumber(&p, &x) && readNumber(&p, &y)) {
        writer->lineTo(x, y);
    }
    if (close) {
        writer->closePath();
    }
}

// ---------------------------------------------------------------- 属性
using Attributes = std::vector<std::pair<std::wstring, std::wstring>>;

const wchar_t* findAttribute(const Attributes& attributes, const wchar_t* name) {
    for (const auto& entry : attributes) {
        if (entry.first == name) {
            return entry.second.c_str();
        }
    }
    return nullptr;
}

float numberAttribute(const Attributes& attributes, const wchar_t* name,
                      const float fallback) {
    const wchar_t* value = findAttribute(attributes, name);
    if (value == nullptr) {
        return fallback;
    }
    const wchar_t* p = value;
    float result = fallback;
    if (!readNumber(&p, &result)) {
        return fallback;
    }
    return result;
}

// 解析 stroke="#RRGGBB" / "#RGB" 形式的显式颜色。
// 返回 0 表示「没有显式颜色」（currentColor / none / 非法值），
// 此时沿用调用方的画笔颜色，保持原有单色图标行为不变。
std::uint32_t hexStrokeColor(const Attributes& attributes) {
    const wchar_t* value = findAttribute(attributes, L"stroke");
    if (value == nullptr || *value != L'#') {
        return 0;
    }
    ++value;
    std::uint32_t rgb = 0;
    int digits = 0;
    for (; *value != L'\0' && digits < 6; ++value, ++digits) {
        const wint_t c = static_cast<wint_t>(*value);
        std::uint32_t nibble = 0;
        if (c >= L'0' && c <= L'9') {
            nibble = static_cast<std::uint32_t>(c - L'0');
        } else if (c >= L'a' && c <= L'f') {
            nibble = 10U + static_cast<std::uint32_t>(c - L'a');
        } else if (c >= L'A' && c <= L'F') {
            nibble = 10U + static_cast<std::uint32_t>(c - L'A');
        } else {
            return 0;
        }
        rgb = (rgb << 4) | nibble;
    }
    if (digits == 3) {
        const std::uint32_t r = (rgb >> 8) & 0xFU;
        const std::uint32_t g = (rgb >> 4) & 0xFU;
        const std::uint32_t b = rgb & 0xFU;
        rgb = ((r * 17U) << 16) | ((g * 17U) << 8) | (b * 17U);
    } else if (digits != 6) {
        return 0;
    }
    return 0xFF000000U | rgb;
}

// 元素上的 transform="translate(...) scale(...)"（图标只用到这两种）
Affine parseTransform(const wchar_t* text) {
    Affine transform;
    if (text == nullptr) {
        return transform;
    }
    const wchar_t* p = text;
    while (*p != L'\0') {
        skipSeparators(&p);
        // 读函数名
        const wchar_t* nameBegin = p;
        while (*p != L'\0' && *p != L'(') {
            ++p;
        }
        if (*p != L'(') {
            break;
        }
        const std::wstring name(nameBegin, p);
        ++p;  // 跳过 '('
        float values[6] = {0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F};
        int count = 0;
        while (count < 6 && readNumber(&p, &values[count])) {
            ++count;
        }
        skipSeparators(&p);
        if (*p == L')') {
            ++p;
        }

        Affine step;
        if (name == L"translate" && count >= 1) {
            step.dx = values[0];
            step.dy = count >= 2 ? values[1] : 0.0F;
        } else if (name == L"scale" && count >= 1) {
            step.m11 = values[0];
            step.m22 = count >= 2 ? values[1] : values[0];
        } else {
            continue;  // 不认识的变换忽略（不猜）
        }
        // SVG 里多个变换是「后写的先作用」，即 transform="A B" 等价于 A∘B
        transform = compose(transform, step);
    }
    return transform;
}

}  // namespace

// ---------------------------------------------------------------- 对外接口

bool buildSvgGlyph(ID2D1Factory* factory, const wchar_t* markup,
                   const float scale, const float dx, const float dy,
                   SvgGlyph* out, const bool forceFilled) {
    if (factory == nullptr || markup == nullptr || out == nullptr) {
        return false;
    }
    out->strokeGeometry.reset();
    out->fillGeometry.reset();
    out->colorLayers.clear();

    ComPtr<ID2D1PathGeometry> strokeGeometry;
    ComPtr<ID2D1GeometrySink> strokeSink;
    ComPtr<ID2D1PathGeometry> fillGeometry;
    ComPtr<ID2D1GeometrySink> fillSink;
    if (FAILED(factory->CreatePathGeometry(strokeGeometry.put())) ||
        FAILED(strokeGeometry->Open(strokeSink.put())) ||
        FAILED(factory->CreatePathGeometry(fillGeometry.put())) ||
        FAILED(fillGeometry->Open(fillSink.put()))) {
        return false;
    }

    // 显式颜色的图元按颜色分组，各开一条几何（同一图标里颜色通常只有
    // 一两种，线性查找足够）。
    struct AccentSink {
        std::uint32_t color = 0;
        ComPtr<ID2D1PathGeometry> geometry;
        ComPtr<ID2D1GeometrySink> sink;
    };
    std::vector<AccentSink> accents;

    Affine base;
    base.m11 = scale;
    base.m22 = scale;
    base.dx = dx;
    base.dy = dy;

    // 累计所有 PathWriter 的包围盒，用于绘制时光学居中。
    D2D1_RECT_F combinedBounds_ = D2D1::RectF(0.0F, 0.0F, 0.0F, 0.0F);
    bool hasCombinedBounds = false;
    auto unionCombinedBounds = [&](const D2D1_RECT_F& g) {
        if (!hasCombinedBounds) {
            combinedBounds_ = g;
            hasCombinedBounds = true;
        } else {
            if (g.left < combinedBounds_.left) combinedBounds_.left = g.left;
            if (g.top < combinedBounds_.top) combinedBounds_.top = g.top;
            if (g.right > combinedBounds_.right) combinedBounds_.right = g.right;
            if (g.bottom > combinedBounds_.bottom) combinedBounds_.bottom = g.bottom;
        }
    };

    // 累计弧长加权质心：每个 PathWriter 自己算 Σ(中点 × 段长)，
    // 这里把所有 writer 的累加再除以总长，得到整图的光学中心。
    float combinedLength = 0.0F;
    float combinedWcx = 0.0F;
    float combinedWcy = 0.0F;

    const wchar_t* p = markup;
    bool ok = true;
    while (ok && *p != L'\0') {
        if (*p != L'<') {
            ++p;
            continue;
        }
        ++p;
        if (*p == L'/') {  // 结束标签：跳过
            while (*p != L'\0' && *p != L'>') {
                ++p;
            }
            if (*p == L'>') {
                ++p;
            }
            continue;
        }
        if (*p == L'!' || *p == L'?') {  // 注释 / 声明：跳过
            while (*p != L'\0' && *p != L'>') {
                ++p;
            }
            if (*p == L'>') {
                ++p;
            }
            continue;
        }

        // 读标签名
        std::wstring tagName;
        while (*p != L'\0' && std::iswalpha(static_cast<wint_t>(*p)) != 0) {
            tagName.push_back(*p);
            ++p;
        }

        // 读属性
        Attributes attributes;
        while (*p != L'\0' && *p != L'>') {
            while (*p != L'\0' && (isSeparator(*p) || *p == L'/')) {
                ++p;
            }
            if (*p == L'\0' || *p == L'>') {
                break;
            }
            std::wstring key;
            while (*p != L'\0' && isSeparator(*p) == false && *p != L'=' &&
                   *p != L'>' && *p != L'/') {
                key.push_back(*p);
                ++p;
            }
            while (*p != L'\0' && isSeparator(*p)) {
                ++p;
            }
            std::wstring value;
            if (*p == L'=') {
                ++p;
                while (*p != L'\0' && isSeparator(*p)) {
                    ++p;
                }
                if (*p == L'"' || *p == L'\'') {
                    const wchar_t quote = *p;
                    ++p;
                    while (*p != L'\0' && *p != quote) {
                        value.push_back(*p);
                        ++p;
                    }
                    if (*p == quote) {
                        ++p;
                    }
                } else {
                    while (*p != L'\0' && isSeparator(*p) == false && *p != L'>') {
                        value.push_back(*p);
                        ++p;
                    }
                }
            }
            if (!key.empty()) {
                attributes.emplace_back(key, value);
            }
        }
        if (*p == L'>') {
            ++p;
        }
        if (tagName.empty()) {
            continue;
        }

        // fill="currentColor" 的图元是实心块，其余按线框描边；
        // forceFilled 时全部按实心处理（供「描边 + 实心」双份几何用）
        const wchar_t* fillValue = findAttribute(attributes, L"fill");
        const bool filled = forceFilled ||
                            (fillValue != nullptr &&
                             std::wcscmp(fillValue, L"currentColor") == 0);

        const Affine transform =
            compose(base, parseTransform(findAttribute(attributes, L"transform")));

        ID2D1GeometrySink* sink = filled ? fillSink.get() : strokeSink.get();
        const std::uint32_t accentColor =
            filled ? 0U : hexStrokeColor(attributes);
        if (accentColor != 0) {
            AccentSink* matched = nullptr;
            for (AccentSink& accent : accents) {
                if (accent.color == accentColor) {
                    matched = &accent;
                    break;
                }
            }
            if (matched == nullptr) {
                AccentSink accent;
                accent.color = accentColor;
                if (FAILED(factory->CreatePathGeometry(accent.geometry.put())) ||
                    FAILED(accent.geometry->Open(accent.sink.put()))) {
                    ok = false;
                    break;
                }
                accents.push_back(accent);
                matched = &accents.back();
            }
            sink = matched->sink.get();
        }

        PathWriter writer(sink, transform,
                          filled ? D2D1_FIGURE_BEGIN_FILLED
                                 : D2D1_FIGURE_BEGIN_HOLLOW);

        if (tagName == L"path") {
            const wchar_t* data = findAttribute(attributes, L"d");
            if (data != nullptr) {
                ok = parsePathData(data, &writer);
            }
        } else if (tagName == L"rect") {
            float rx = numberAttribute(attributes, L"rx", 0.0F);
            float ry = numberAttribute(attributes, L"ry", 0.0F);
            if (rx > 0.0F && ry <= 0.0F) {
                ry = rx;
            } else if (ry > 0.0F && rx <= 0.0F) {
                rx = ry;
            }
            emitRect(&writer, numberAttribute(attributes, L"x", 0.0F),
                     numberAttribute(attributes, L"y", 0.0F),
                     numberAttribute(attributes, L"width", 0.0F),
                     numberAttribute(attributes, L"height", 0.0F), rx, ry);
        } else if (tagName == L"circle") {
            const float radius = numberAttribute(attributes, L"r", 0.0F);
            emitEllipse(&writer, numberAttribute(attributes, L"cx", 0.0F),
                        numberAttribute(attributes, L"cy", 0.0F), radius, radius);
        } else if (tagName == L"ellipse") {
            emitEllipse(&writer, numberAttribute(attributes, L"cx", 0.0F),
                        numberAttribute(attributes, L"cy", 0.0F),
                        numberAttribute(attributes, L"rx", 0.0F),
                        numberAttribute(attributes, L"ry", 0.0F));
        } else if (tagName == L"line") {
            writer.moveTo(numberAttribute(attributes, L"x1", 0.0F),
                          numberAttribute(attributes, L"y1", 0.0F));
            writer.lineTo(numberAttribute(attributes, L"x2", 0.0F),
                          numberAttribute(attributes, L"y2", 0.0F));
        } else if (tagName == L"polyline" || tagName == L"polygon") {
            const wchar_t* points = findAttribute(attributes, L"points");
            if (points != nullptr) {
                emitPolyline(&writer, points, tagName == L"polygon");
            }
        }
        // 其它标签（g/title/defs 等）静默跳过

        writer.finish();
        if (writer.hasBounds()) {
            unionCombinedBounds(writer.bounds());
        }
        if (writer.hasCentroid()) {
            const D2D1_POINT_2F c = writer.centroid();
            const float writerLen = writer.totalLength();
            combinedLength += writerLen;
            combinedWcx += c.x * writerLen;
            combinedWcy += c.y * writerLen;
        }
    }

    if (!ok) {
        return false;
    }
    if (FAILED(strokeSink->Close()) || FAILED(fillSink->Close())) {
        return false;
    }
    for (AccentSink& accent : accents) {
        if (FAILED(accent.sink->Close())) {
            return false;
        }
        SvgColorLayer layer;
        layer.geometry = accent.geometry;
        layer.color = accent.color;
        out->colorLayers.push_back(layer);
    }
    out->strokeGeometry = strokeGeometry;
    out->fillGeometry = fillGeometry;

    // 包围盒由 PathWriter 在写入时同步累计（见 PathWriter::expandBounds），
    // 不依赖 ID2D1Geometry::GetBounds——实测在某些 D2D 运行时下 GetBounds
    // 对空心图元返回 inf，导致居中失效。
    out->bounds = combinedBounds_;

    // 弧长加权质心：描边图元的光学中心。
    if (combinedLength > 0.0F) {
        out->centroid = D2D1::Point2F(combinedWcx / combinedLength,
                                      combinedWcy / combinedLength);
        out->hasCentroid = true;
    } else {
        out->hasCentroid = false;
    }
    return true;
}

ComPtr<ID2D1StrokeStyle> createRoundStrokeStyle(ID2D1Factory* factory) {
    ComPtr<ID2D1StrokeStyle> style;
    if (factory == nullptr) {
        return style;
    }
    const D2D1_STROKE_STYLE_PROPERTIES properties = D2D1::StrokeStyleProperties(
        D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
        D2D1_LINE_JOIN_ROUND, 10.0F, D2D1_DASH_STYLE_SOLID, 0.0F);
    factory->CreateStrokeStyle(properties, nullptr, 0, style.put());
    return style;
}

}  // namespace win32
}  // namespace platform
}  // namespace ktile
