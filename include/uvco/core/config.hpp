#pragma once

#include <co2/config.hpp>
#include <co2/contract.hpp>

// [[nodiscard]] 是 C++17 属性。C++14 下 GCC 作为扩展接受且不告警；Clang 用自己的
// warn_unused_result 拼写以避免 -Wc++17-attribute-extensions。
#if CO2_CPLUSPLUS >= 201703L
#define UVCO_NODISCARD [[nodiscard]]
#elif defined(__clang__)
#define UVCO_NODISCARD [[clang::warn_unused_result]]
#elif defined(__GNUC__)
#define UVCO_NODISCARD [[nodiscard]]
#else
#define UVCO_NODISCARD
#endif

// uvco 的契约检查沿用 co2 的进程级契约处理器（co2::setContractViolationHandler）。
#define UVCO_CONTRACT_CHECK(condition) CO2_CONTRACT_CHECK(condition)
#define UVCO_CONTRACT_FAIL(message) CO2_CONTRACT_FAIL(message)
