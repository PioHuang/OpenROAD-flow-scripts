#ifndef PDN_GEOMETRY_HPP
#define PDN_GEOMETRY_HPP

#include <algorithm>
#include <cmath>

namespace pdn {

struct Point {
  double x_um {};
  double y_um {};
};

struct Rect {
  double llx_um {};
  double lly_um {};
  double urx_um {};
  double ury_um {};

  double widthUm() const { return std::max(0.0, urx_um - llx_um); }
  double heightUm() const { return std::max(0.0, ury_um - lly_um); }
  double areaUm2() const { return widthUm() * heightUm(); }

  bool contains(const Point& p) const
  {
    return p.x_um >= llx_um && p.x_um <= urx_um && p.y_um >= lly_um && p.y_um <= ury_um;
  }

  Point center() const { return {(llx_um + urx_um) * 0.5, (lly_um + ury_um) * 0.5}; }
};

inline double manhattanDistanceUm(const Point& a, const Point& b)
{
  return std::abs(a.x_um - b.x_um) + std::abs(a.y_um - b.y_um);
}

}  // namespace pdn

#endif  // PDN_GEOMETRY_HPP
