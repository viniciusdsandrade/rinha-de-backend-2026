#pragma once

#include <array>
#include <string>
#include <utility>

#include "rinha/refs.hpp"
#include "rinha/types.hpp"

namespace rinha {

class Classifier {
public:
    explicit Classifier(ReferenceSet refs);

    bool classify(const Payload& payload, Classification& classification, std::string& error) const;
    static bool supports_avx2() noexcept;

private:
    using Top5 = std::array<std::pair<float, bool>, 5>;

    Top5 top5_scalar(const QueryVector& query) const noexcept;

#if defined(__x86_64__) || defined(_M_X64)
    Top5 top5_avx2(const QueryVector& query) const noexcept;
#endif

    static void insert_top5(Top5& top, float distance, bool is_fraud) noexcept;

    ReferenceSet refs_;
};

}  // namespace rinha
