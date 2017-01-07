#pragma once

#ifdef _OPENMP
#include <omp.h>
#endif

#include <tbb/parallel_for.h>
#include <tbb/parallel_reduce.h>
#include <tbb/blocked_range2d.h>
#include <tbb/partitioner.h>

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

extern tbb::affinity_partitioner ap;

/**
 * 1D parallel for
 */
template<typename T = int>
inline void parallel_for_(const T x_min, const T x_max,
                          std::function<void(T)> func,
                          const int grain_x = TBB_GRAIN_Y,
                          Parallel parr = Parallel::TBB)
{
  switch (parr) {
    case Parallel::TBB:
      tbb::parallel_for(tbb::blocked_range<T>(x_min, x_max, grain_x),
                        [&func](const tbb::blocked_range<T> &r) {
                          for (T x = r.begin(); x != r.end(); ++x)
                            func(x);
                        },ap);
      break;
#ifdef _OPENMP
    case Parallel::OpenMP:
  #pragma omp for schedule(dynamic,grain_x)
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
template<typename T = int>
inline void parallel_for_(const T outer_min, const T outer_max,
                          const T inner_min, const T inner_max,
                          std::function<void(T, T)> func,
                          const int grain_outer = TBB_GRAIN_Y, const int grain_inner = TBB_GRAIN_Y,
                          Parallel parr = Parallel::TBB)
{
  switch (parr) {
    case Parallel::TBB:
      tbb::parallel_for(
        tbb::blocked_range2d<T>(outer_min, outer_max, grain_outer, inner_min, inner_max, grain_inner),
        [&func](const tbb::blocked_range2d<T> &r) {
          for (T y = r.rows().begin(); y != r.rows().end(); ++y) {
            for (T x = r.cols().begin(); x != r.cols().end(); ++x) {
              func(x, y);
            }
          }
        },ap);
      break;
#ifdef _OPENMP
    case Parallel::OpenMP:
  #pragma omp for collapse(2) schedule(dynamic, grain_outer)
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
