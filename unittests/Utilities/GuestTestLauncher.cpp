// SPDX-License-Identifier: MIT
#include "PythonCompat.h"
#include <tiny-json.h>

static bool DoesFEXSupportAVX(const std::string& mode) {
  // Check if FEX indicates support for AVX
  auto fex_interpreter_path = os::path::dirname(sys::argv[7]) + "/FEX";

  list args;
  args.push_back(fex_interpreter_path);
  args.push_back("/bin/cat");
  args.push_back("/proc/cpuinfo");

  auto output = subprocess::run(args);

  for (const auto& line : splitlines(output)) {
    if (line.find("flags") != std::string::npos) {
      auto flags = split(strip(split(line, ":")[1]), " ");
      return std::ranges::count(flags, "avx") && std::ranges::count(flags, "avx2");
    }
  }
  return false;
}

static bool TestRequiresAVXSupport() {
  // Check if the test itself requires AVX
  auto exe_path = sys::argv[sys::argv.size() - 1];
  auto json_path = os::path::dirname(os::path::dirname(exe_path)) + "/requirements/" + os::path::basename(exe_path) + ".json";

  std::ifstream json_file {json_path};
  if (json_file.is_open()) {
    std::string json_text {std::istreambuf_iterator<char> {json_file}, {}};
    json_t json_pool[64];
    const json_t* json_data = json_create(json_text.data(), json_pool, std::size(json_pool));
    if (json_data) {
      const json_t* host_features = json_getProperty(json_data, "HostFeatures");
      if (host_features && json_getType(host_features) == JSON_ARRAY) {
        for (const json_t* feature = json_getChild(host_features); feature != nullptr; feature = json_getSibling(feature)) {
          if (json_getType(feature) == JSON_TEXT && json_getValue(feature) == std::string_view {"AVX"}) {
            return true;
          }
        }
      }
    } else {
      print("JSON error:", json_path);
    }
  } else {
    // If we get here, then we don't have a corresponding JSON
    // file for the associated test, and can assume there's no
    // feature requirements for the test.
  }
  return false;
}

static dict LoadTestsFile(const std::string& File) {
  dict Dict;
  if (!os::path::exists(File)) {
    return Dict;
  }

  auto dtf = open(File);
  for (const auto& line : dtf) {
    auto test = strip(split(line, "#")[0]); // remove comments and empty spaces
    if (test.size() > 0) {
      Dict[test] = 1;
    }
  }

  return Dict;
}

static dict LoadTestsFileResults(const std::string& File) {
  dict Dict;
  if (!os::path::exists(File)) {
    return Dict;
  }

  auto dtf = open(File);
  for (const auto& line : dtf) {
    auto test = strip(split(line, "#")[0]); // remove comments and empty spaces
    if (test.size() > 0) {
      auto parts = split(line, " ");
      Dict[parts[0]] = std::stoi(parts[1]);
    }
  }

  return Dict;
}

int main(int argc, char** argv) {
  sys::argv.assign(argv, argv + argc);
  // Args: <Known Failures file> <ExpectedOutputsFile> <DisabledTestsFile> <FlakeTestsFile> <TestName> <Mode> <FexExecutable> <FexArgs>...

  // fexargs should also include the test executable

  if (sys::argv.size() < 8) {
    sys::exit();
  }

  auto known_failures_file = sys::argv[1];
  auto expected_output_file = sys::argv[2];
  auto disabled_tests_file = sys::argv[3];
  auto flake_tests_file = sys::argv[4];
  auto test_name = sys::argv[5];
  auto mode = sys::argv[6];
  auto fexecutable = sys::argv[7];
  int StartingFEXArgsOffset = 8;

  // If the test requires AVX and FEX doesn't support it, just pass the test and move on
  if (TestRequiresAVXSupport() && !DoesFEXSupportAVX(mode)) {
    sys::exit(0);
  }

  // Open test expected information files and load in to dictionaries.
  auto known_failures = LoadTestsFile(known_failures_file);
  auto expected_output = LoadTestsFileResults(expected_output_file);
  auto disabled_tests = LoadTestsFile(disabled_tests_file);
  auto flake_tests = LoadTestsFile(flake_tests_file);

  // run with timeout to avoid locking up
  list RunnerArgs;

  RunnerArgs.push_back(fexecutable);

  // Add the rest of the arguments
  for (int i = 0; i < std::ssize(sys::argv) - StartingFEXArgsOffset; ++i) {
    RunnerArgs.push_back(sys::argv[StartingFEXArgsOffset + i]);
  }

  // print(RunnerArgs)

  int ResultCode = 0;

  // Handle flakes
  int TryCount = 1;
  if (flake_tests.contains(test_name)) {
    TryCount = 5;
  }

  if (disabled_tests.contains(test_name)) {
    // This error code tells ctest that the test was skipped
    sys::exit(125);
  }

  // expect zero by default
  if (!expected_output.contains(test_name)) {
    expected_output[test_name] = 0;
  }

  if (ResultCode == 0) {
    for (int Try = 0; Try < TryCount; ++Try) {
      // Run the test and wait for it to end to get the result
      print(RunnerArgs);
      auto Process = subprocess::Popen(RunnerArgs);
      Process.wait();
      ResultCode = Process.returncode;

      // Break if the expected output is the result code
      if (expected_output[test_name] == ResultCode) {
        break;
      }
    }
  }

  if (expected_output[test_name] != ResultCode) {
    if (expected_output.contains(test_name)) {
      print("test failed, expected is", expected_output[test_name], "but got", ResultCode);
    } else {
      print("Test doesn't have expected output,", test_name);
    }

    if (known_failures.contains(test_name)) {
      print("Passing because it was expected to fail");
      // failed and expected to fail -- pass the test
      sys::exit(0);
    } else {
      // failed and unexpected to fail -- fail the test
      sys::exit(1);
    }
  } else {
    print("test passed with", ResultCode);
    if (known_failures.contains(test_name)) {
      print("Failing because it was expected to fail");
      // passed and expected to fail -- fail the test
      sys::exit(1);
    } else {
      // passed and expected to pass -- pass the test
      sys::exit(0);
    }
  }
}
