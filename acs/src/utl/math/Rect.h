#pragma once
#include "Vector2.h"

namespace ACSU_Math
{
    struct Rect
    {
        float x, y, width, height;

        Rect() : x(0), y(0), width(0), height(0) {}
        Rect(float _x, float _y, float _w, float _h) : x(_x), y(_y), width(_w), height(_h) {}

        float xMin() const { return x; }
        float xMax() const { return x + width; }
        float yMin() const { return y; }
        float yMax() const { return y + height; }

        bool Contains(const Vector2& point) const {
            return point.x >= xMin() && point.x < xMax() && point.y >= yMin() && point.y < yMax();
        }
    };
}