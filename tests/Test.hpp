#ifndef HAVSF2_TESTS_TEST_HPP
#define HAVSF2_TESTS_TEST_HPP

// A minimal test harness: TEST_CASE registers a function, CHECK* records a
// failure and continues, REQUIRE* records a failure and aborts the case.

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace test
{
  struct Case
  {
    std::string_view name;
    void (*function)();
  };

  inline std::vector<Case> &Registry()
  {
    static std::vector<Case> cases;
    return cases;
  }

  struct Registrar
  {
    Registrar(std::string_view name, void (*function)())
    {
      Registry().push_back(Case{name, function});
    }
  };

  // Thrown by REQUIRE* to abandon the current test case
  struct RequireFailed
  {
  };

  inline int &FailureCount()
  {
    static int count = 0;
    return count;
  }

  inline void ReportFailure(std::string_view file, int line, std::string_view message)
  {
    ++FailureCount();
    std::cerr << file << '(' << line << "): FAILED: " << message << '\n';
  }

  template <typename T>
  concept CmpInteger = std::integral<T> && !std::same_as<T, bool> && !std::same_as<T, char> &&
                       !std::same_as<T, wchar_t> && !std::same_as<T, char8_t> &&
                       !std::same_as<T, char16_t> && !std::same_as<T, char32_t>;

  template <typename T> std::string Describe(const T &value)
  {
    if constexpr (std::is_enum_v<T>)
    {
      return std::format("{}", static_cast<long long>(std::to_underlying(value)));
    }
    else if constexpr (std::is_arithmetic_v<T>)
    {
      return std::format("{}", value);
    }
    else if constexpr (std::convertible_to<const T &, std::string_view>)
    {
      return std::format("\"{}\"", std::string_view(value));
    }
    else
    {
      return "<value>";
    }
  }

  template <typename Actual, typename Expected>
  bool CheckEqual(std::string_view file,
                  int line,
                  std::string_view actualText,
                  std::string_view expectedText,
                  const Actual &actual,
                  const Expected &expected)
  {
    bool equal = false;

    if constexpr (CmpInteger<Actual> && CmpInteger<Expected>)
    {
      equal = std::cmp_equal(actual, expected);
    }
    else
    {
      equal = actual == expected;
    }

    if (!equal)
    {
      ReportFailure(file, line,
                    std::format("{} == {} (actual {}, expected {})", actualText, expectedText,
                                Describe(actual), Describe(expected)));
    }

    return equal;
  }

  // Byte-wise comparison that reports the first differing offset
  inline bool CheckBytesEqual(std::string_view file,
                              int line,
                              std::span<const std::uint8_t> actual,
                              std::span<const std::uint8_t> expected)
  {
    std::size_t offset = 0;

    while (offset < actual.size() && offset < expected.size() && actual[offset] == expected[offset])
    {
      ++offset;
    }

    if (offset == actual.size() && offset == expected.size())
    {
      return true;
    }

    std::string message = std::format("byte sequences differ: actual {} bytes, expected {} bytes",
                                      actual.size(), expected.size());

    if (offset < actual.size() && offset < expected.size())
    {
      message += std::format("; first difference at offset {} (actual 0x{:02X}, expected 0x{:02X})",
                             offset, actual[offset], expected[offset]);
    }
    else
    {
      message += std::format("; identical up to offset {}", offset);
    }

    ReportFailure(file, line, message);
    return false;
  }
} // namespace test

#define TEST_CASE(name)                                                                            \
  static void name();                                                                              \
  static const ::test::Registrar name##Registrar{#name, &name};                                    \
  static void name()

#define CHECK(condition)                                                                           \
  do                                                                                               \
  {                                                                                                \
    if (!(condition))                                                                              \
    {                                                                                              \
      ::test::ReportFailure(__FILE__, __LINE__, #condition);                                       \
    }                                                                                              \
  } while (false)

#define REQUIRE(condition)                                                                         \
  do                                                                                               \
  {                                                                                                \
    if (!(condition))                                                                              \
    {                                                                                              \
      ::test::ReportFailure(__FILE__, __LINE__, #condition);                                       \
      throw ::test::RequireFailed{};                                                               \
    }                                                                                              \
  } while (false)

#define CHECK_EQ(actual, expected)                                                                 \
  (void)::test::CheckEqual(__FILE__, __LINE__, #actual, #expected, (actual), (expected))

#define REQUIRE_EQ(actual, expected)                                                               \
  do                                                                                               \
  {                                                                                                \
    if (!::test::CheckEqual(__FILE__, __LINE__, #actual, #expected, (actual), (expected)))         \
    {                                                                                              \
      throw ::test::RequireFailed{};                                                               \
    }                                                                                              \
  } while (false)

#define CHECK_BYTES_EQ(actual, expected)                                                           \
  (void)::test::CheckBytesEqual(__FILE__, __LINE__, (actual), (expected))

#endif
