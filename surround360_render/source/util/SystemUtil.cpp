/**
* Copyright (c) 2016-present, Facebook, Inc.
* All rights reserved.
*
* This source code is licensed under the BSD-style license found in the
* LICENSE_render file in the root directory of this subproject. An additional grant
* of patent rights can be found in the PATENTS file in the same directory.
*/

#include "SystemUtil.h"

#ifdef _WINDOWS
#define backtrace(x,y) 0
#define backtrace_symbols(x,y) NULL
#else
#include <execinfo.h>
#endif
#include <signal.h>

#include <exception>
#include <stdexcept>

#include <gflags/gflags.h>
#define GLOG_NO_ABBREVIATED_SEVERITIES
#include <glog/logging.h>

namespace fLB {
	bool FLAGS_help;
	bool FLAGS_helpshort;
}

namespace surround360 {
namespace util {

using namespace std;

void printStacktrace() {
  const size_t maxStackDepth = 128;
  void* stack[maxStackDepth];
  size_t stackDepth = backtrace(stack, maxStackDepth);
  char** stackStrings = backtrace_symbols(stack, stackDepth);
  for (size_t i = 0; i < stackDepth; ++i) {
    LOG(ERROR) << stackStrings[i];
  }
  free(stackStrings);
}

void terminateHandler() {
  exception_ptr exptr = current_exception();
  if (exptr != 0) {
    try {
      rethrow_exception(exptr);
    } catch (VrCamException &ex) {
      LOG(ERROR) << "Terminated with VrCamException: " << ex.what();
    } catch (exception &ex) {
      LOG(ERROR) << "Terminated with exception: " << ex.what();
      printStacktrace();
    } catch (...) {
      LOG(ERROR) << "Terminated with unknown exception";
      printStacktrace();
    }
  } else {
    LOG(ERROR) << "Terminated due to unknown reason";
    printStacktrace();
  }
  abort();
}

void sigHandler(int signal) {
  #ifndef _WINDOWS
  LOG(ERROR) << strsignal(signal);
  #endif
  printStacktrace();
  abort();
}

void initSurround360(int argc, char** argv) {
  // Initialize Google's logging library
  google::InitGoogleLogging(argv[0]);

  // GFlags
  google::ParseCommandLineNonHelpFlags(&argc, &argv, true);
  fLB::FLAGS_helpshort = fLB::FLAGS_help;
  fLB::FLAGS_help = false;
  google::HandleCommandLineHelpFlags();

  // setup signal and termination handlers
  set_terminate(terminateHandler);

  // terminate process: terminal line hangup
#ifndef _WINDOWS
  signal(SIGHUP, sigHandler);
#endif

  // terminate process: interrupt program
  signal(SIGINT, sigHandler);

  // create core image: quit program
#ifndef _WINDOWS
  signal(SIGQUIT, sigHandler);
#endif

  // create core image: illegal instruction
  signal(SIGILL, sigHandler);

  // create core image: trace trap
#ifndef _WINDOWS
  signal(SIGTRAP, sigHandler);
#endif

  // create core image: floating-point exception
  signal(SIGFPE, sigHandler);

  // terminate process: kill program
#ifndef _WINDOWS
  signal(SIGKILL, sigHandler);
#endif

  // create core image: bus error
#ifndef _WINDOWS
  signal(SIGBUS, sigHandler);
#endif

  // create core image: segmentation violation
  signal(SIGSEGV, sigHandler);

  // create core image: non-existent system call invoked
#ifndef _WINDOWS
  signal(SIGSYS, sigHandler);
#endif

  // terminate process: write on a pipe with no reader
#ifndef _WINDOWS
  signal(SIGPIPE, sigHandler);
#endif

  // terminate process: software termination signal
  signal(SIGTERM, sigHandler);
}

} // namespace util
} // namespace surround360
