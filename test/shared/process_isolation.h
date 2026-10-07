// Copyright 2026 Stephan Friedl. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#pragma once

#include <sys/wait.h>
#include <unistd.h>

namespace MINIMAL_STD_NAMESPACE
{
    namespace pmr
    {
        namespace test
        {
            //  Runs body in a forked child with a watchdog.  True only if the child exits normally with
            //      status 0; a crash (e.g. SIGFPE) or a hang (SIGALRM) returns false without killing the runner.

            template <typename F>
            bool runs_to_completion(F body, unsigned timeout_seconds = 5)
            {
                pid_t pid = fork();

                if (pid == 0)
                {
                    alarm(timeout_seconds);
                    body();
                    _exit(0);
                }

                int status = 0;
                waitpid(pid, &status, 0);

                return WIFEXITED(status) && (WEXITSTATUS(status) == 0);
            }
        }
    }
}
