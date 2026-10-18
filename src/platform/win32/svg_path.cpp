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
        sink_->BeginFigure(current_, figureBegin_);
        open_ = true;
    }

    void lineTo(const float x, const float y) {
        ensureOpen();
        current_ = mapPoint(transform_, x, y);
        sink_->AddLine(current_);
    }

    void cubicTo(const float x1, const float y1, const float x2, const float y2,
                 const float x, const float y) {
        ensureOpen();
        D2D1_BEZIER_SEGMENT segment;
        segment.point1 = mapPoint(transform_, x1, y1);
        segment.point2 = mapPoint(transform_, x2, y2);
        segment.point3 = mapPoint(transform_, x, y);
        sink_->AddBezier(&segment);
        current_ = segment.point3;
    }

    void quadTo(const float x1, const float y1, const float x, const float y) {
        ensureOpen();
        D2D1_QUADRATIC_BEZIER_SEGMENT segment;
        segment.point1 = mapPoint(transform_, x1, y1);
        segment.point2 = mapPoint(transform_, x, y);
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
        const D2D1_POINT_2F end = mapPoint(transform_, x, y);
        // 半径为 0 或起终点重合时，SVG 规定退化成直线
        if (rx <= 0.0F || ry <= 0.0F || (end.x == current_.x && end.y == current_.y)) {
            sink_->AddLine(end);
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

    ID2D1GeometrySink* sink_;
    Affine transform_;
    D2D1_FIGURE_BEGIN figureBegin_;
    float scale_;
    D2D1_POINT_2F current_ = D2D1::Point2F(0.0F, 0.0F);
    D2D1_POINT_2F subpathStart_ = D2D1::Point2F(0.0F, 0.0F);
    bool open_ = false;
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

    Affine base;
    base.m11 = scale;
    base.m22 = scale;
    base.dx = dx;
    base.dy = dy;

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
        PathWriter writer(filled ? fillSink.get() : strokeSink.get(), transform,
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
    }

    if (!ok) {
        return false;
    }
    if (FAILED(strokeSink->Close()) || FAILED(fillSink->Close())) {
        return false;
    }
    out->strokeGeometry = strokeGeometry;
    out->fillGeometry = fillGeometry;
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
