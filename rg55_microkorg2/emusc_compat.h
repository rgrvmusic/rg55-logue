// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gerard Braad (Regroove | RGRV)

#pragma once

#include <algorithm>

#if !defined(__cpp_lib_clamp)
namespace std {
template <class T>
constexpr const T& clamp(const T& value, const T& low, const T& high) {
    return value < low ? low : high < value ? high : value;
}
}
#endif
