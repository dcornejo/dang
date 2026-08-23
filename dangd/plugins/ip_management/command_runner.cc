// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/plugins/ip_management/command_runner.h"

#include <cerrno>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>

namespace dangd::ip_management {

bool RunCommand(const std::vector<std::string>& arguments, std::string* error) {
  if (arguments.empty() || arguments.front().empty()) return false;
  const pid_t child = fork();
  if (child == -1) {
    if (error) *error = std::string("fork failed: ") + std::strerror(errno);
    return false;
  }
  if (child == 0) {
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const std::string& argument : arguments)
      argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    execv(argv.front(), argv.data());
    _exit(127);
  }
  int status = 0;
  while (waitpid(child, &status, 0) == -1) {
    if (errno == EINTR) continue;
    if (error) *error = std::string("waitpid failed: ") + std::strerror(errno);
    return false;
  }
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return true;
  if (error) {
    *error = arguments.front() + " failed";
    if (WIFEXITED(status)) *error += " with exit status " + std::to_string(WEXITSTATUS(status));
    else if (WIFSIGNALED(status)) *error += " from signal " + std::to_string(WTERMSIG(status));
  }
  return false;
}

}  // namespace dangd::ip_management
