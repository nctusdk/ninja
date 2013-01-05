// Copyright 2012 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef NINJA_SUBPROCESS_H_
#define NINJA_SUBPROCESS_H_

#include <memory>
#include <string>
#include <vector>
#include <queue>
using namespace std;

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#endif

#include "exit_status.h"

/// Subprocess wraps a single async subprocess.  It is entirely
/// passive: it expects the caller to notify it when its fds are ready
/// for reading, as well as call Finish() to reap the child once done()
/// is true.
struct Subprocess {
  ~Subprocess();

  /// Returns ExitSuccess on successful process exit, ExitInterrupted if
  /// the process was interrupted, ExitFailure if it otherwise failed.
  ExitStatus Finish();

  bool Done() const;

  const string& GetOutput() const;

#ifndef _WIN32
  /// Get the output from the separate pipe for implicit dependencies.
  const string& GetDepsOutput() const;
#endif

 private:
  struct Pipe;

  Subprocess();
  bool Start(struct SubprocessSet* set, const string& command);
  void OnPipeReady(Pipe* pipe);

#ifdef _WIN32
  /// Set up pipe_ as the parent-side pipe of the subprocess; return the
  /// other end of the pipe, usable in the child process.
  /// TODO: hide some of this in a "Pipe" struct, like on non-Windows.
  HANDLE SetupPipe(HANDLE ioport);

  HANDLE child_;
  HANDLE pipe_;
  OVERLAPPED overlapped_;
  char overlapped_buf_[4 << 10];
  bool is_reading_;
  string buf_;
#else
  auto_ptr<Pipe> output_;
  auto_ptr<Pipe> deps_;
  pid_t pid_;
#endif

  friend struct SubprocessSet;
};

/// SubprocessSet runs a ppoll/pselect() loop around a set of Subprocesses.
/// DoWork() waits for any state change in subprocesses; finished_
/// is a queue of subprocesses as they finish.
struct SubprocessSet {
  SubprocessSet();
  ~SubprocessSet();

  Subprocess* Add(const string& command);
  bool DoWork();
  Subprocess* NextFinished();
  void Clear();

  vector<Subprocess*> running_;
  queue<Subprocess*> finished_;

#ifdef _WIN32
  static BOOL WINAPI NotifyInterrupted(DWORD dwCtrlType);
  static HANDLE ioport_;
#else
  static void SetInterruptedFlag(int signum);
  static bool interrupted_;

  struct sigaction old_act_;
  sigset_t old_mask_;
#endif
};

#endif // NINJA_SUBPROCESS_H_
