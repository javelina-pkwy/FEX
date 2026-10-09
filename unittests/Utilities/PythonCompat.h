// SPDX-License-Identifier: MIT
// Stand-ins for the python that the test runner scripts used, so their native ports can follow them line for line.
#pragma once
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <spawn.h>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

using dict = std::unordered_map<std::string, int>;
using list = std::vector<std::string>;

namespace sys {
inline list argv;

[[noreturn]] inline void exit(int Code = 0) {
  std::exit(Code);
}

// Fails the same way an uncaught python exception does
[[noreturn]] inline void exit(const std::string& Message) {
  std::cerr << Message << std::endl;
  std::exit(1);
}
} // namespace sys

inline std::string strip(std::string_view Str) {
  const auto Begin = Str.find_first_not_of(" \t\n\r\v\f");
  if (Begin == std::string_view::npos) {
    return {};
  }
  return std::string {Str.substr(Begin, Str.find_last_not_of(" \t\n\r\v\f") - Begin + 1)};
}

inline list split(std::string_view Str, std::string_view Separator) {
  list Parts;
  size_t Start = 0;
  for (size_t End; (End = Str.find(Separator, Start)) != std::string_view::npos; Start = End + Separator.size()) {
    Parts.emplace_back(Str.substr(Start, End - Start));
  }
  Parts.emplace_back(Str.substr(Start));
  return Parts;
}

inline list readlines(std::istream&& Stream) {
  list Lines;
  for (std::string Line; std::getline(Stream, Line);) {
    Lines.push_back(Line);
  }
  return Lines;
}

inline list splitlines(const std::string& Str) {
  return readlines(std::istringstream {Str});
}

inline list open(const std::string& File) {
  std::ifstream Stream {File};
  if (!Stream.is_open()) {
    sys::exit("FileNotFoundError: '" + File + "'");
  }
  return readlines(std::move(Stream));
}

template<typename... Args>
inline void print(const Args&... Values) {
  const char* Separator = "";
  ((std::cout << std::exchange(Separator, " ") << Values), ...);
  std::cout << std::endl;
}

// Lists are printed like python's repr of them
inline void print(const list& List) {
  std::cout << "[";
  for (size_t i = 0; i < List.size(); ++i) {
    std::cout << (i ? ", '" : "'") << List[i] << "'";
  }
  std::cout << "]" << std::endl;
}

namespace os::path {
inline bool exists(const std::string& Path) {
  return access(Path.c_str(), F_OK) == 0;
}

inline std::string dirname(const std::string& Path) {
  return std::filesystem::path {Path}.parent_path();
}

inline std::string basename(const std::string& Path) {
  return std::filesystem::path {Path}.filename();
}
} // namespace os::path

namespace shutil {
inline std::optional<std::string> which(const std::string& cmd) {
  const char* Path = getenv("PATH");
  for (const auto& Dir : split(Path ? Path : "", ":")) {
    if (auto Candidate = (Dir.empty() ? "." : Dir) + "/" + cmd; access(Candidate.c_str(), X_OK) == 0) {
      return Candidate;
    }
  }
  return std::nullopt;
}
} // namespace shutil

namespace subprocess {
class Popen {
public:
  explicit Popen(const list& args, const posix_spawn_file_actions_t* Actions = nullptr) {
    std::vector<char*> Argv;
    for (const auto& Arg : args) {
      Argv.push_back(const_cast<char*>(Arg.c_str()));
    }
    Argv.push_back(nullptr);
    if (int Error = posix_spawnp(&pid, Argv[0], Actions, nullptr, Argv.data(), environ); Error != 0) {
      sys::exit(std::string {"OSError: "} + strerror(Error) + ": '" + args[0] + "'");
    }
  }

  void wait() {
    int Status {};
    waitpid(pid, &Status, 0);
    // Killed by a signal is the negative signal number
    returncode = WIFSIGNALED(Status) ? -WTERMSIG(Status) : WEXITSTATUS(Status);
  }

  int returncode = 0;

private:
  pid_t pid;
};

// subprocess.run(args, capture_output=True, text=True).stdout
inline std::string run(const list& args) {
  int Pipe[2];
  if (pipe2(Pipe, O_CLOEXEC) != 0) {
    sys::exit("OSError: pipe2");
  }
  posix_spawn_file_actions_t Actions;
  posix_spawn_file_actions_init(&Actions);
  posix_spawn_file_actions_adddup2(&Actions, Pipe[1], STDOUT_FILENO);
  posix_spawn_file_actions_addopen(&Actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
  Popen Process {args, &Actions};
  posix_spawn_file_actions_destroy(&Actions);
  close(Pipe[1]);

  std::string Output;
  char Buffer[4096];
  for (ssize_t Size; (Size = read(Pipe[0], Buffer, sizeof(Buffer))) > 0;) {
    Output.append(Buffer, Size);
  }
  close(Pipe[0]);
  Process.wait();
  return Output;
}
} // namespace subprocess
