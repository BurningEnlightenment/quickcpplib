/* Signal guard support
(C) 2018 Niall Douglas <http://www.nedproductions.biz/> (4 commits)
File Created: June 2018


Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License in the accompanying file
Licence.txt or at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.


Distributed under the Boost Software License, Version 1.0.
    (See accompanying file Licence.txt or copy at
          http://www.boost.org/LICENSE_1_0.txt)
*/

#include "../include/signal_guard.hpp"

#include "../include/spinlock.hpp"

#include <csignal>

QUICKCPPLIB_NAMESPACE_BEGIN

namespace signal_guard
{
  namespace detail
  {
    SIGNALGUARD_FUNC_DECL const char *signalc_to_string(signalc code) noexcept
    {
      static constexpr const char *strings[] = {"Signal abort process", "Signal undefined memory access", "Signal illegal instruction", "Signal interrupt", "Signal broken pipe", "Signal segmentation fault", nullptr, nullptr, "Signal floating point error"};
      if(code == signalc::none)
        return "none";
      for(size_t n = 0; n < sizeof(strings) / sizeof(strings[0]); n++)
      {
        if((static_cast<unsigned>(code) & (1 << n)) != 0)
          return strings[n];
      }
      if((static_cast<unsigned>(code) & static_cast<unsigned>(signalc::out_of_memory)) != 0)
        return "C++ out of memory";
      if((static_cast<unsigned>(code) & static_cast<unsigned>(signalc::termination)) != 0)
        return "C++ termination";
      return "unknown";
    }

    SIGNALGUARD_FUNC_DECL signal_handler_info_base *&current_signal_handler() noexcept
    {
      static QUICKCPPLIB_THREAD_LOCAL signal_handler_info_base *v;
      return v;
    }

    static configurable_spinlock::spinlock<bool> lock;
    static unsigned new_handler_count, terminate_handler_count;
    static std::new_handler new_handler_old;
    static std::terminate_handler terminate_handler_old;

    inline void new_handler()
    {
      auto *shi = current_signal_handler();
      if(shi != nullptr)
      {
        if(shi->set_siginfo(signalc::out_of_memory, nullptr, nullptr))
        {
          if(!shi->call_continuer())
          {
            longjmp(shi->buf, 1);
          }
        }
      }
      if(new_handler_old != nullptr)
      {
        new_handler_old();
      }
      else
      {
        throw std::bad_alloc();
      }
    }
    inline void terminate_handler()
    {
      auto *shi = current_signal_handler();
      if(shi != nullptr)
      {
        if(shi->set_siginfo(signalc::termination, nullptr, nullptr))
        {
          if(!shi->call_continuer())
          {
            longjmp(shi->buf, 1);
          }
        }
      }
      if(terminate_handler_old != nullptr)
      {
        terminate_handler_old();
      }
      else
      {
        std::abort();
      }
    }

#ifdef _WIN32
    inline signalc signalc_from_signo(intptr_t c)
    {
      switch(c)
      {
      case EXCEPTION_NONCONTINUABLE_EXCEPTION:
        return signalc::abort_process;
      case EXCEPTION_IN_PAGE_ERROR:
        return signalc::undefined_memory_access;
      case EXCEPTION_ILLEGAL_INSTRUCTION:
        return signalc::illegal_instruction;
      // case SIGINT:
      //  return signalc::interrupt;
      // case SIGPIPE:
      //  return signalc::broken_pipe;
      case EXCEPTION_ACCESS_VIOLATION:
        return signalc::segmentation_fault;
      case EXCEPTION_FLT_DENORMAL_OPERAND:
      case EXCEPTION_FLT_DIVIDE_BY_ZERO:
      case EXCEPTION_FLT_INEXACT_RESULT:
      case EXCEPTION_FLT_INVALID_OPERATION:
      case EXCEPTION_FLT_OVERFLOW:
      case EXCEPTION_FLT_STACK_CHECK:
      case EXCEPTION_FLT_UNDERFLOW:
        return signalc::floating_point_error;
      case EXCEPTION_STACK_OVERFLOW:
        return signalc::out_of_memory;
      default:
        return signalc::none;
      }
    }
    SIGNALGUARD_FUNC_DECL unsigned long win32_exception_filter_function(unsigned long code, _EXCEPTION_POINTERS *ptrs) noexcept
    {
      auto *shi = current_signal_handler();
      if(shi != nullptr)
      {
        if(shi->set_siginfo(code, ptrs->ExceptionRecord, ptrs->ContextRecord))
        {
          if(!shi->call_continuer())
          {
            // invoke longjmp
            return EXCEPTION_EXECUTE_HANDLER;
          }
        }
      }
      return EXCEPTION_CONTINUE_SEARCH;
    }
#else
    inline int signo_from_signalc(unsigned c)
    {
      switch(c)
      {
      case signalc::abort_process:
        return SIGABRT;
      case signalc::undefined_memory_access:
        return SIGBUS;
      case signalc::illegal_instruction:
        return SIGILL;
      case signalc::interrupt:
        return SIGINT;
      case signalc::broken_pipe:
        return SIGPIPE;
      case signalc::segmentation_fault:
        return SIGSEGV;
      case signalc::floating_point_error:
        return SIGFPE;
      }
      return -1;
    }
    inline signalc signalc_from_signo(intptr_t c)
    {
      switch(c)
      {
      case SIGABRT:
        return signalc::abort_process;
      case SIGBUS:
        return signalc::undefined_memory_access;
      case SIGILL:
        return signalc::illegal_instruction;
      case SIGINT:
        return signalc::interrupt;
      case SIGPIPE:
        return signalc::broken_pipe;
      case SIGSEGV:
        return signalc::segmentation_fault;
      case SIGFPE:
        return signalc::floating_point_error;
      }
      return signalc::none;
    }
    struct installed_signal_handler
    {
      int signo;
      unsigned count;
      struct sigaction former;
    };
    static installed_signal_handler handler_counts[32];

    inline void raw_signal_handler(int signo, siginfo_t *info, void *context)
    {
      auto *shi = current_signal_handler();
      if(shi != nullptr)
      {
        if(shi->set_siginfo(signo, info, context))
        {
          if(!shi->call_continuer())
          {
            longjmp(shi->buf, 1);
          }
        }
      }
      // Otherwise, call the previous signal handler
      for(const auto &i : handler_counts)
      {
        if(i.signo == signo)
        {
          void (*h1)(int signo, siginfo_t *info, void *context) = nullptr;
          void (*h2)(int signo) = nullptr;
          lock.lock();
          if(i.former.sa_flags & SA_SIGINFO)
          {
            h1 = i.former.sa_sigaction;
          }
          else
            h2 = i.former.sa_handler;
          lock.unlock();
          if(h1)
            h1(signo, info, context);
          else if(h2)
          {
            if(h2 != SIG_DFL && h2 != SIG_IGN)
              h2(signo);
          }
          return;
        }
      }
    }

#endif
  }

  SIGNALGUARD_MEMFUNC_DECL signal_guard_install::signal_guard_install(signalc guarded)
      : _guarded(guarded)
  {
#ifndef _WIN32
    for(size_t n = 0; n < 16; n++)
    {
      if((static_cast<unsigned>(guarded) & (1 << n)) != 0)
      {
        int signo = detail::signo_from_signalc(1 << n);
        if(signo != -1)
        {
          int ret = 0;
          detail::lock.lock();
          detail::handler_counts[n].signo = signo;
          if(!detail::handler_counts[n].count++)
          {
            struct sigaction sa;
            memset(&sa, 0, sizeof(sa));
            sa.sa_sigaction = detail::raw_signal_handler;
            sa.sa_flags = SA_SIGINFO;
            ret = sigaction(signo, &sa, &detail::handler_counts[n].former);
          }
          detail::lock.unlock();
          if(ret == -1)
          {
            throw std::system_error(errno, std::system_category());
          }
        }
      }
    }
#endif
    if((guarded & signalc::out_of_memory) || (guarded & signalc::termination))
    {
      detail::lock.lock();
      if(guarded & signalc::out_of_memory)
      {
        if(!detail::new_handler_count++)
        {
          detail::new_handler_old = std::set_new_handler(detail::new_handler);
        }
      }
      if(guarded & signalc::termination)
      {
        if(!detail::terminate_handler_count++)
        {
          detail::terminate_handler_old = std::set_terminate(detail::terminate_handler);
        }
      }
      detail::lock.unlock();
    }
  }
  SIGNALGUARD_MEMFUNC_DECL signal_guard_install::~signal_guard_install()
  {
    if((_guarded & signalc::out_of_memory) || (_guarded & signalc::termination))
    {
      detail::lock.lock();
      if(_guarded & signalc::out_of_memory)
      {
        if(!--detail::new_handler_count)
        {
          std::set_new_handler(detail::new_handler_old);
        }
      }
      if(_guarded & signalc::termination)
      {
        if(!--detail::terminate_handler_count)
        {
          std::set_terminate(detail::terminate_handler_old);
        }
      }
      detail::lock.unlock();
    }
#ifndef _WIN32
    for(size_t n = 0; n < 16; n++)
    {
      if((static_cast<unsigned>(_guarded) & (1 << n)) != 0)
      {
        int signo = detail::signo_from_signalc(1 << n);
        if(signo != -1)
        {
          int ret = 0;
          detail::lock.lock();
          if(!--detail::handler_counts[n].count)
          {
            ret = sigaction(signo, &detail::handler_counts[n].former, nullptr);
          }
          detail::lock.unlock();
          if(ret == -1)
          {
            abort();
          }
        }
      }
    }
#endif
  }

  namespace detail
  {
    struct signal_handler_info
    {
      signal_handler_info_base *next{nullptr};  // any previously installed info on this thread
      signalc guarded{signalc::none};           // handlers used

      signalc signal{signalc::none};  // the signal which occurred
#ifdef _WIN32
      _EXCEPTION_RECORD info;  // what the signal handler was called with
      CONTEXT context;
#else
      sigset_t former;  // former sigmask
      siginfo_t info;   // what the signal handler was called with
      ucontext_t context;
#endif
    };
    static_assert(sizeof(signal_handler_info) <= 1408, "signal_handler_info is too big for erased storage");

    SIGNALGUARD_MEMFUNC_DECL erased_signal_handler_info::erased_signal_handler_info()
    {
      auto *p = reinterpret_cast<signal_handler_info *>(_erased);
      new(p) signal_handler_info;
    }

    SIGNALGUARD_MEMFUNC_DECL signalc erased_signal_handler_info::signal() const
    {
      auto *p = reinterpret_cast<const signal_handler_info *>(_erased);
      return p->signal;
    }
    SIGNALGUARD_MEMFUNC_DECL const void *erased_signal_handler_info::info() const
    {
      auto *p = reinterpret_cast<const signal_handler_info *>(_erased);
      return static_cast<const void *>(&p->info);
    }
    SIGNALGUARD_MEMFUNC_DECL const void *erased_signal_handler_info::context() const
    {
      auto *p = reinterpret_cast<const signal_handler_info *>(_erased);
      return static_cast<const void *>(&p->context);
    }

    SIGNALGUARD_MEMFUNC_DECL void erased_signal_handler_info::acquire(signalc guarded)
    {
      auto *p = reinterpret_cast<signal_handler_info *>(_erased);
      // Set me to current handler
      p->next = current_signal_handler();
      p->guarded = guarded;
      current_signal_handler() = this;
#ifndef _WIN32
      // Enable my guarded signals for this thread
      sigset_t set;
      sigemptyset(&set);
#if 1
      for(size_t n = 0; n < 16; n++)
      {
        if((static_cast<unsigned>(guarded) & (1 << n)) != 0)
        {
          int signo = detail::signo_from_signalc(1 << n);
          if(signo != -1)
          {
            sigaddset(&set, signo);
          }
        }
      }
#else
      unsigned g = static_cast<unsigned>(guarded);
      for(size_t n = __builtin_ffs(g); n != 0; g ^= 1 << (n - 1), n = __builtin_ffs(g))
      {
        int signo = detail::signo_from_signalc(1 << (n - 1));
        if(signo != -1)
        {
          sigaddset(&set, signo);
        }
      }
#endif
      pthread_sigmask(SIG_UNBLOCK, &set, &p->former);
#endif
    }
    SIGNALGUARD_MEMFUNC_DECL void erased_signal_handler_info::release(signalc /* unused */)
    {
      auto *p = reinterpret_cast<signal_handler_info *>(_erased);
      current_signal_handler() = p->next;
// On POSIX our signal mask needs to be restored
#ifndef _WIN32
      pthread_sigmask(SIG_SETMASK, &p->former, nullptr);
#endif
      p->~signal_handler_info();
    }
    SIGNALGUARD_MEMFUNC_DECL bool erased_signal_handler_info::set_siginfo(intptr_t signo, void *info, void *context)
    {
      auto *p = reinterpret_cast<signal_handler_info *>(_erased);
      if(info == nullptr && context == nullptr)
      {
        // This is a C++ failure handler
        p->signal = static_cast<signalc>(signo);
        if((p->guarded & p->signal) == 0)
          return false;  // I am not guarding this signal
        return true;
      }
      // Otherwise this is either a Win32 exception code or POSIX signal number
      p->signal = signalc_from_signo(signo);
      if(p->signal == signalc::none)
        return false;  // I don't support this signal
      if((p->guarded & p->signal) == 0)
        return false;  // I am not guarding this signal
      memcpy(&p->info, info, sizeof(p->info));
      memcpy(&p->context, context, sizeof(p->context));
      return true;
    }
  }
}

QUICKCPPLIB_NAMESPACE_END