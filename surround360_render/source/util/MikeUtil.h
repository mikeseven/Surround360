#pragma once

#ifdef _OPENMP
#include <omp.h>
#endif

#include <tbb/parallel_for.h>
#include <tbb/parallel_reduce.h>
#include <tbb/blocked_range2d.h>

namespace surround360 {
// TBB setup
static constexpr int TBB_GRAIN_Y = 64;

enum Parallel
{
  None,
  TBB,
#ifdef _OPENMP
  OpenMP
#endif
};

/**
 * 1D parallel for
 */
template<typename T>
inline void parallel_for_(T x_min, T x_max,
                          std::function<void(T)> func,
                          Parallel parr = Parallel::TBB)
{
  switch (parr) {
    case Parallel::TBB:
      tbb::parallel_for(tbb::blocked_range<T>(x_min, x_max, TBB_GRAIN_Y),
                        [&func](const tbb::blocked_range<T> &r) {
                          for (auto x = r.begin(); x != r.end(); ++x)
                            func(x);
                        });
      break;
#ifdef _OPENMP
    case Parallel::OpenMP:
  #pragma omp for schedule(dynamic,TBB_GRAIN_Y)
      for (T x = x_min; x<x_max; ++x) {
        func(x);
      }
      break;
#endif
    default:
      for (T x = x_min; x < x_max; ++x)
        func(x);
      break;
  }
}

/**
 * 2D parallel for
 */
template<typename T>
inline void parallel_for_(T outer_min, T outer_max,
                          T inner_min, T inner_max,
                          std::function<void(T, T)> func,
                          Parallel parr = Parallel::TBB)
{
  switch (parr) {
    case Parallel::TBB:
      tbb::parallel_for(
        tbb::blocked_range2d<T>(outer_min, outer_max, TBB_GRAIN_Y, inner_min, inner_max, TBB_GRAIN_Y * 2),
        [&func](const tbb::blocked_range2d<T> &r) {
          for (auto y = r.rows().begin(); y != r.rows().end(); ++y) {
            for (auto x = r.cols().begin(); x != r.cols().end(); ++x) {
              func(x, y);
            }
          }
        });
      break;
#ifdef _OPENMP
    case Parallel::OpenMP:
  #pragma omp for collapse(2) schedule(dynamic,TBB_GRAIN_Y)
      for (T y = outer_min; y <outer_max; ++y) {
        for (T x = inner_min; x <inner_max; ++x) {
          func(x, y);
        }
      }
      break;
#endif
    default:
      for (T y = outer_min; y < outer_max; ++y)
        for (T x = inner_min; x < inner_max; ++x)
          func(x, y);
      break;
  }
}

} // namespace surround360
