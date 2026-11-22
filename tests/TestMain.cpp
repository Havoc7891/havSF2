#include "Test.hpp"

#include <exception>

// Runs every registered test case or only those whose name contains the
// first command-line argument. Exit code 0 means everything passed.
int main(int argc, char **argv)
{
  const std::string_view filter = argc > 1 ? std::string_view(argv[1]) : std::string_view();
  int executed = 0;
  int failedCases = 0;

  for (const test::Case &testCase : test::Registry())
  {
    if (testCase.name.find(filter) == std::string_view::npos)
    {
      continue;
    }

    const int failuresBefore = test::FailureCount();
    ++executed;

    try
    {
      testCase.function();
    }
    catch (const test::RequireFailed &)
    {
      // Already reported by the failing REQUIRE
    }
    catch (const std::exception &error)
    {
      test::ReportFailure(testCase.name, 0, std::string("unexpected exception: ") + error.what());
    }

    if (test::FailureCount() != failuresBefore)
    {
      ++failedCases;
      std::cerr << "  in test case " << testCase.name << '\n';
    }
  }

  std::cout << executed - failedCases << " of " << executed << " test cases passed\n";

  if (executed == 0)
  {
    std::cerr << "no test cases matched\n";
    return 1;
  }

  return failedCases == 0 ? 0 : 1;
}
