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

#include "subprocess.h"

#include <algorithm>
#include <map>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>

// Older versions of glibc (like 2.4) won't find this in <poll.h>.  glibc
// 2.4 keeps it in <asm-generic/poll.h>, though attempting to include that
// will redefine the pollfd structure.
#ifndef POLLRDHUP
#define POLLRDHUP 0x2000
#endif

#include "util.h"

struct Subprocess::Pipe {
  Pipe() {
    pipe_[0] = -1;
    pipe_[1] = -1;
  }
  ~Pipe() {
    CloseRead();
    CloseWrite();
  }
  bool Setup();

  int read_fd() const { return pipe_[0]; }
  int write_fd() const { return pipe_[1]; }
  const string& buf() const { return buf_; }

  bool Read();

  bool CloseRead() {
    return Close(&pipe_[0]);
  }
  bool CloseWrite() {
    return Close(&pipe_[1]);
  }

 private:
  bool Close(int* fd);

  int pipe_[2];
  string buf_;
};

bool Subprocess::Pipe::Setup() {
  if (pipe(pipe_) < 0)
    Fatal("pipe: %s", strerror(errno));

#if !defined(linux)
  // On Linux we use ppoll; elsewhere we use pselect and so must avoid
  // overly-large FDs.
  if (pipe_[0] >= static_cast<int>(FD_SETSIZE)) {
    Fatal("pipe: %s", strerror(EMFILE));
  }
#endif  // !linux

  SetCloseOnExec(pipe_[0]);

  return true;
}

bool Subprocess::Pipe::Read() {
  char buf[4 << 10];
  ssize_t len = read(read_fd(), buf, sizeof(buf));
  if (len > 0) {
    buf_.append(buf, len);
  } else {
    if (len < 0)
      Fatal("read: %s", strerror(errno));
    CloseRead();
  }
  return true;
}

bool Subprocess::Pipe::Close(int* fd) {
  if (*fd == -1)
    return true;
  bool success = close(*fd) == 0;
  *fd = -1;
  return success;
}

Subprocess::Subprocess() : pid_(-1) {
}

Subprocess::~Subprocess() {
  // Reap child if forgotten.
  if (pid_ != -1)
    Finish();
}

bool Subprocess::Start(SubprocessSet* set, const string& command) {
  output_.reset(new Pipe);
  if (!output_->Setup())
    return false;

  deps_.reset(new Pipe);
  if (!deps_->Setup())
    return false;

  pid_ = fork();
  if (pid_ < 0)
    Fatal("fork: %s", strerror(errno));

  if (pid_ == 0) {
    output_->CloseRead();
    deps_->CloseRead();

    // Track which fd we use to report errors on.
    int output_pipe = output_->write_fd();
    do {
      if (setpgid(0, 0) < 0)
        break;

      if (sigaction(SIGINT, &set->old_act_, 0) < 0)
        break;
      if (sigprocmask(SIG_SETMASK, &set->old_mask_, 0) < 0)
        break;

      // Open /dev/null over stdin.
      int devnull = open("/dev/null", O_RDONLY);
      if (devnull < 0)
        break;
      if (dup2(devnull, 0) < 0)
        break;
      close(devnull);

      // Open the output pipe over stdout/stderr.
      if (dup2(output_pipe, 1) < 0 ||
          dup2(output_pipe, 2) < 0) {
        break;
      }

      // Now can use stderr for errors.
      output_pipe = 2;
      output_->CloseWrite();

      // Export the deps pipe fd as an environment variable.
      char buf[16];
      sprintf(buf, "%d", deps_->write_fd());
      if (setenv("NINJA_DEPS", buf, /* overwrite */ 1) < 0)
        break;

      execl("/bin/sh", "/bin/sh", "-c", command.c_str(), (char *) NULL);
    } while (false);

    // If we get here, something went wrong; the execl should have
    // replaced us.
    char* err = strerror(errno);
    if (write(output_pipe, err, strlen(err)) < 0) {
      // If the write fails, there's nothing we can do.
      // But this block seems necessary to silence the warning.
    }
    _exit(1);
  }

  output_->CloseWrite();
  deps_->CloseWrite();

  return true;
}

void Subprocess::OnPipeReady(Pipe* pipe) {
  pipe->Read();
}

ExitStatus Subprocess::Finish() {
  assert(pid_ != -1);
  int status;
  if (waitpid(pid_, &status, 0) < 0)
    Fatal("waitpid(%d): %s", pid_, strerror(errno));
  pid_ = -1;

  if (WIFEXITED(status)) {
    int exit = WEXITSTATUS(status);
    if (exit == 0)
      return ExitSuccess;
  } else if (WIFSIGNALED(status)) {
    if (WTERMSIG(status) == SIGINT)
      return ExitInterrupted;
  }
  return ExitFailure;
}

bool Subprocess::Done() const {
  return output_->read_fd() == -1;
}

const string& Subprocess::GetOutput() const {
  return output_->buf();
}

const string& Subprocess::GetDepsOutput() const {
  return deps_->buf();
}

bool SubprocessSet::interrupted_;

void SubprocessSet::SetInterruptedFlag(int signum) {
  (void) signum;
  interrupted_ = true;
}

SubprocessSet::SubprocessSet() {
  interrupted_ = false;

  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  if (sigprocmask(SIG_BLOCK, &set, &old_mask_) < 0)
    Fatal("sigprocmask: %s", strerror(errno));

  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_handler = SetInterruptedFlag;
  if (sigaction(SIGINT, &act, &old_act_) < 0)
    Fatal("sigaction: %s", strerror(errno));
}

SubprocessSet::~SubprocessSet() {
  Clear();

  if (sigaction(SIGINT, &old_act_, 0) < 0)
    Fatal("sigaction: %s", strerror(errno));
  if (sigprocmask(SIG_SETMASK, &old_mask_, 0) < 0)
    Fatal("sigprocmask: %s", strerror(errno));
}

Subprocess *SubprocessSet::Add(const string& command) {
  Subprocess *subprocess = new Subprocess;
  if (!subprocess->Start(this, command)) {
    delete subprocess;
    return 0;
  }
  running_.push_back(subprocess);
  return subprocess;
}

#ifdef linux
bool SubprocessSet::DoWork() {
  vector<pollfd> fds;
  nfds_t nfds = 0;

  for (vector<Subprocess*>::iterator i = running_.begin();
       i != running_.end(); ++i) {
    for (int j = 0; j < 2; ++j) {
      Subprocess::Pipe* pipe =
          j == 0 ? (*i)->output_.get() : (*i)->deps_.get();
      if (pipe->read_fd() < 0)
        continue;
      pollfd pfd = { pipe->read_fd(), POLLIN | POLLPRI | POLLRDHUP, 0 };
      fds.push_back(pfd);
      ++nfds;
    }
  }

  int ret = ppoll(&fds.front(), nfds, NULL, &old_mask_);
  if (ret == -1) {
    if (errno != EINTR) {
      perror("ninja: ppoll");
      return false;
    }
    bool interrupted = interrupted_;
    interrupted_ = false;
    return interrupted;
  }

  nfds_t cur_nfd = 0;
  for (vector<Subprocess*>::iterator i = running_.begin();
       i != running_.end(); ) {
    for (int j = 0; j < 2; ++j) {
      Subprocess::Pipe* pipe =
          j == 0 ? (*i)->output_.get() : (*i)->deps_.get();
      if (pipe->read_fd() < 0)
        continue;
      assert(pipe->read_fd() == fds[cur_nfd].fd);
      if (fds[cur_nfd++].revents)
        (*i)->OnPipeReady(pipe);
    }
    if ((*i)->Done()) {
      finished_.push(*i);
      i = running_.erase(i);
      continue;
    }
    ++i;
  }

  return false;
}

#else  // linux
bool SubprocessSet::DoWork() {
  fd_set set;
  int nfds = 0;
  FD_ZERO(&set);

  for (vector<Subprocess*>::iterator i = running_.begin();
       i != running_.end(); ++i) {
    for (int j = 0; j < 2; ++j) {
      Subprocess::Pipe* pipe =
          j == 0 ? (*i)->output_.get() : (*i)->deps_.get();
      int fd = pipe->read_fd();
      if (fd >= 0) {
        FD_SET(fd, &set);
        if (nfds < fd+1)
          nfds = fd+1;
      }
    }
  }

  int ret = pselect(nfds, &set, 0, 0, 0, &old_mask_);
  if (ret == -1) {
    if (errno != EINTR) {
      perror("ninja: pselect");
      return false;
    }
    bool interrupted = interrupted_;
    interrupted_ = false;
    return interrupted;
  }

  for (vector<Subprocess*>::iterator i = running_.begin();
       i != running_.end(); ) {
    for (int j = 0; j < 2; ++j) {
      Subprocess::Pipe* pipe =
          j == 0 ? (*i)->output_.get() : (*i)->deps_.get();
      int fd = pipe->read_fd();
      if (fd >= 0 && FD_ISSET(fd, &set))
        (*i)->OnPipeReady(pipe);
    }
    if ((*i)->Done()) {
      finished_.push(*i);
      i = running_.erase(i);
      continue;
    }
    ++i;
  }

  return false;
}
#endif  // linux

Subprocess* SubprocessSet::NextFinished() {
  if (finished_.empty())
    return NULL;
  Subprocess* subproc = finished_.front();
  finished_.pop();
  return subproc;
}

void SubprocessSet::Clear() {
  for (vector<Subprocess*>::iterator i = running_.begin();
       i != running_.end(); ++i)
    kill(-(*i)->pid_, SIGINT);
  for (vector<Subprocess*>::iterator i = running_.begin();
       i != running_.end(); ++i)
    delete *i;
  running_.clear();
}
