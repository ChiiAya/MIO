#pragma once
// ============================================================================
// 向量数学工具（header-only，供记忆系统与 Router 话题匹配共用）
//
// dot()：编译期按目标架构分派 SIMD 路径（AVX-512 / AVX2+FMA / SSE2 / NEON），
// x86-64 基线必有 SSE2，ARMv8 基线必有 NEON —— 拿不到更高指令集时自然回退，
// 标量兜底保证任何平台可编译可运行。发布构建加 -march=native 可自动吃满本机。
//
// 约定：调用方先把向量 L2 归一化（normalizeInPlace），之后 dot 即余弦相似度。
// 归一化放在本层而不是 Embedding 适配器：适配器只忠实转发端点返回值，
// "点积当相似度用"是检索侧的语义决定。
// ============================================================================

#include <cmath>
#include <cstddef>
#include <vector>

#if defined(__AVX512F__)
#include <immintrin.h>
#elif defined(__AVX2__) && defined(__FMA__)
#include <immintrin.h>
#elif defined(__SSE2__)
#include <emmintrin.h>
#elif defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace mio {
namespace vecmath {

// SIMD 点积：n 为维度（BGE-M3 = 1024，恰好是各 SIMD 宽度的整数倍）
inline float dot(const float* a, const float* b, std::size_t n) {
#if defined(__AVX512F__)
    __m512 acc = _mm512_setzero_ps();
    std::size_t i = 0;
    for (; i + 16 <= n; i += 16)
        acc = _mm512_fmadd_ps(_mm512_loadu_ps(a + i), _mm512_loadu_ps(b + i), acc);
    float sum = _mm512_reduce_add_ps(acc);
#elif defined(__AVX2__) && defined(__FMA__)
    __m256 acc = _mm256_setzero_ps();
    std::size_t i = 0;
    for (; i + 8 <= n; i += 8)
        acc = _mm256_fmadd_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i), acc);
    __m128 lo = _mm256_castps256_ps128(acc);
    __m128 hi = _mm256_extractf128_ps(acc, 1);
    lo = _mm_add_ps(lo, hi);
    lo = _mm_hadd_ps(lo, lo);
    lo = _mm_hadd_ps(lo, lo);
    float sum = _mm_cvtss_f32(lo);
#elif defined(__SSE2__)
    __m128 acc = _mm_setzero_ps();
    std::size_t i = 0;
    for (; i + 4 <= n; i += 4)
        acc = _mm_add_ps(acc, _mm_mul_ps(_mm_loadu_ps(a + i), _mm_loadu_ps(b + i)));
    // 纯 SSE2 水平求和（hadd 是 SSE3）：两轮 shuffle 归约到首车道
    __m128 shuf = _mm_shuffle_ps(acc, acc, _MM_SHUFFLE(2, 3, 0, 1));
    acc = _mm_add_ps(acc, shuf);
    shuf = _mm_shuffle_ps(acc, acc, _MM_SHUFFLE(1, 0, 3, 2));
    acc = _mm_add_ps(acc, shuf);
    float sum = _mm_cvtss_f32(acc);
#elif defined(__ARM_NEON) || defined(__aarch64__)
    float32x4_t acc = vdupq_n_f32(0.0f);
    std::size_t i = 0;
    for (; i + 4 <= n; i += 4)
        acc = vfmaq_f32(acc, vld1q_f32(a + i), vld1q_f32(b + i));
    float sum = vaddvq_f32(acc);
#else
    float sum = 0.0f;
    std::size_t i = 0;
#endif
    // 标量收尾（各路径共用的尾部；n 非 SIMD 宽度整数倍时补齐）
    for (; i < n; ++i) sum += a[i] * b[i];
    return sum;
}

// L2 范数（与 dot 同一分派结构；零向量返回 0，调用方据此跳过归一化）
inline float l2norm(const float* v, std::size_t n) {
    return std::sqrt(dot(v, v, n));
}

// 原地 L2 归一化；零向量 / 非有限值保持原样并返回 false（调用方应丢弃）
inline bool normalizeInPlace(float* v, std::size_t n) {
    float sq = 0.0f;
    for (std::size_t i = 0; i < n; ++i) {
        if (!std::isfinite(v[i])) return false;
        sq += v[i] * v[i];
    }
    const float norm = std::sqrt(sq);
    if (!(norm > 0.0f) || !std::isfinite(norm)) return false;
    const float inv = 1.0f / norm;
    for (std::size_t i = 0; i < n; ++i) v[i] *= inv;
    return true;
}

inline bool normalizeInPlace(std::vector<float>& v) {
    return normalizeInPlace(v.data(), v.size());
}

} // namespace vecmath
} // namespace mio
