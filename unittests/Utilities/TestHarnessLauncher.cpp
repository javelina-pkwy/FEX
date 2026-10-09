// SPDX-License-Identifier: MIT
#include "PythonCompat.h"
namespace path = os::path;
using shutil::which;

int main(int argc, char** argv) {
  sys::argv.assign(argv, argv + argc);
  // Args: <Known Failures file> <Known Failures Type File> <DisabledTestsFile> <DisabledTestsTypeFile> <DisabledTestsRunnerFile> <TestName> <FullTestName> <Test Harness Executable> <Args>...

  if (sys::argv.size() < 9) {
    sys::exit();
  }

  dict known_failures;
  dict disabled_tests;
  auto known_failures_file = sys::argv[1];
  auto known_failures_type_file = sys::argv[2];
  auto disabled_tests_file = sys::argv[3];
  auto disabled_tests_type_file = sys::argv[4];
  auto disabled_tests_runner_file = sys::argv[5];

  auto current_test = sys::argv[6];
  auto full_test_name = sys::argv[7];
  auto runner = sys::argv[8];
  int args_start_index = 9;

  // Open the known failures file and add it to a dictionary
  auto kff = open(known_failures_file);
  for (const auto& line : kff) {
    known_failures[strip(line)] = 1;
  }

  if (path::exists(known_failures_type_file)) {
    auto dtf = open(known_failures_type_file);
    for (const auto& line : dtf) {
      known_failures[strip(line)] = 1;
    }
  }

  auto dtf = open(disabled_tests_file);
  for (const auto& line : dtf) {
    disabled_tests[strip(line)] = 1;
  }

  if (path::exists(disabled_tests_type_file)) {
    auto dtf = open(disabled_tests_type_file);
    for (const auto& line : dtf) {
      disabled_tests[strip(line)] = 1;
    }
  }

  if (path::exists(disabled_tests_runner_file)) {
    auto dtf = open(disabled_tests_runner_file);
    for (const auto& line : dtf) {
      disabled_tests[strip(line)] = 1;
    }
  }

  list RunnerArgs = {"catchsegv", runner};

  if (which("catchsegv") == std::nullopt) {
    RunnerArgs.erase(RunnerArgs.begin());
  }
  // Add the rest of the arguments
  for (int i = 0; i < std::ssize(sys::argv) - args_start_index; ++i) {
    RunnerArgs.push_back(sys::argv[args_start_index + i]);
  }

  if (disabled_tests.contains(current_test)) {
    // This error code tells ctest that the test was skipped
    sys::exit(125);
  }

  // Run the test and wait for it to end to get the result
  auto Process = subprocess::Popen(RunnerArgs);
  Process.wait();
  int ResultCode = Process.returncode;

  // Check for known failures - try full test name first, then partial test name
  bool is_known_failure = known_failures.contains(full_test_name) || known_failures.contains(current_test);

  if (is_known_failure) {
    // If the test is on the known failures list
    if (ResultCode) {
      // If we errored but are on the known failures list then "pass" the test
      sys::exit(0);
    } else {
      // If we didn't error but are in the known failure list then we need to fail the test
      sys::exit(1);
    }
  } else {
    // Just return the result code if we don't have this test as a known failure
    sys::exit(ResultCode);
  }
}
