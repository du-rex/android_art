/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "thread.h"

#include <dynamic_annotations.h>
#include <pthread.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/time.h>

#include <algorithm>
#include <bitset>
#include <cerrno>
#include <iostream>
#include <list>

#include "class_linker.h"
#include "class_loader.h"
#include "debugger.h"
#include "heap.h"
#include "jni_internal.h"
#include "monitor.h"
#include "oat/runtime/context.h"
#include "object.h"
#include "object_utils.h"
#include "reflection.h"
#include "runtime.h"
#include "runtime_support.h"
#include "scoped_thread_state_change.h"
#include "ScopedLocalRef.h"
#include "space.h"
#include "stack.h"
#include "stack_indirect_reference_table.h"
#include "thread_list.h"
#include "utils.h"
#include "verifier/gc_map.h"
#include "well_known_classes.h"

namespace art {

pthread_key_t Thread::pthread_key_self_;
ConditionVariable* Thread::resume_cond_;

static const char* kThreadNameDuringStartup = "<native thread without managed peer>";

void Thread::InitCardTable() {
  card_table_ = Runtime::Current()->GetHeap()->GetCardTable()->GetBiasedBegin();
}

#if !defined(__APPLE__)
static void UnimplementedEntryPoint() {
  UNIMPLEMENTED(FATAL);
}
#endif

void Thread::InitFunctionPointers() {
#if !defined(__APPLE__) // The Mac GCC is too old to accept this code.
  // Insert a placeholder so we can easily tell if we call an unimplemented entry point.
  uintptr_t* begin = reinterpret_cast<uintptr_t*>(&entrypoints_);
  uintptr_t* end = reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(begin) + sizeof(entrypoints_));
  for (uintptr_t* it = begin; it != end; ++it) {
    *it = reinterpret_cast<uintptr_t>(UnimplementedEntryPoint);
  }
#endif
  InitEntryPoints(&entrypoints_);
}

void Thread::SetDebuggerUpdatesEnabled(bool enabled) {
  LOG(INFO) << "Turning debugger updates " << (enabled ? "on" : "off") << " for " << *this;
#if !defined(ART_USE_LLVM_COMPILER)
  ChangeDebuggerEntryPoint(&entrypoints_, enabled);
#else
  UNIMPLEMENTED(FATAL);
#endif
}

void Thread::InitTid() {
  tid_ = ::art::GetTid();
}

void Thread::InitAfterFork() {
  // One thread (us) survived the fork, but we have a new tid so we need to
  // update the value stashed in this Thread*.
  InitTid();
}

void* Thread::CreateCallback(void* arg) {
  Thread* self = reinterpret_cast<Thread*>(arg);
  self->Init();

  {
    ScopedObjectAccess soa(self);
    {
      SirtRef<String> thread_name(self->GetThreadName(soa));
      self->SetThreadName(thread_name->ToModifiedUtf8().c_str());
    }

    Dbg::PostThreadStart(self);

    // Invoke the 'run' method of our java.lang.Thread.
    CHECK(self->peer_ != NULL);
    Object* receiver = self->peer_;
    jmethodID mid = WellKnownClasses::java_lang_Thread_run;
    Method* m = receiver->GetClass()->FindVirtualMethodForVirtualOrInterface(soa.DecodeMethod(mid));
    m->Invoke(self, receiver, NULL, NULL);
  }

  // Detach and delete self.
  Runtime::Current()->GetThreadList()->Unregister(self);

  return NULL;
}

static void SetVmData(const ScopedObjectAccess& soa, Object* managed_thread,
                      Thread* native_thread)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Field* f = soa.DecodeField(WellKnownClasses::java_lang_Thread_vmData);
  f->SetInt(managed_thread, reinterpret_cast<uintptr_t>(native_thread));
}

Thread* Thread::FromManagedThread(const ScopedObjectAccessUnchecked& soa, Object* thread_peer) {
  Field* f = soa.DecodeField(WellKnownClasses::java_lang_Thread_vmData);
  Thread* result = reinterpret_cast<Thread*>(static_cast<uintptr_t>(f->GetInt(thread_peer)));
  // Sanity check that if we have a result it is either suspended or we hold the thread_list_lock_
  // to stop it from going away.
  MutexLock mu(*Locks::thread_suspend_count_lock_);
  if (result != NULL && !result->IsSuspended()) {
    Locks::thread_list_lock_->AssertHeld();
  }
  return result;
}

Thread* Thread::FromManagedThread(const ScopedObjectAccessUnchecked& soa, jobject java_thread) {
  return FromManagedThread(soa, soa.Decode<Object*>(java_thread));
}

static size_t FixStackSize(size_t stack_size) {
  // A stack size of zero means "use the default".
  if (stack_size == 0) {
    stack_size = Runtime::Current()->GetDefaultStackSize();
  }

  // Dalvik used the bionic pthread default stack size for native threads,
  // so include that here to support apps that expect large native stacks.
  stack_size += 1 * MB;

  // It's not possible to request a stack smaller than the system-defined PTHREAD_STACK_MIN.
  if (stack_size < PTHREAD_STACK_MIN) {
    stack_size = PTHREAD_STACK_MIN;
  }

  // It's likely that callers are trying to ensure they have at least a certain amount of
  // stack space, so we should add our reserved space on top of what they requested, rather
  // than implicitly take it away from them.
  stack_size += Thread::kStackOverflowReservedBytes;

  // Some systems require the stack size to be a multiple of the system page size, so round up.
  stack_size = RoundUp(stack_size, kPageSize);

  return stack_size;
}

static void SigAltStack(stack_t* new_stack, stack_t* old_stack) {
  if (sigaltstack(new_stack, old_stack) == -1) {
    PLOG(FATAL) << "sigaltstack failed";
  }
}

static void SetUpAlternateSignalStack() {
  // Create and set an alternate signal stack.
  stack_t ss;
  ss.ss_sp = new uint8_t[SIGSTKSZ];
  ss.ss_size = SIGSTKSZ;
  ss.ss_flags = 0;
  CHECK(ss.ss_sp != NULL);
  SigAltStack(&ss, NULL);

  // Double-check that it worked.
  ss.ss_sp = NULL;
  SigAltStack(NULL, &ss);
  VLOG(threads) << "Alternate signal stack is " << PrettySize(ss.ss_size) << " at " << ss.ss_sp;
}

static void TearDownAlternateSignalStack() {
  // Get the pointer so we can free the memory.
  stack_t ss;
  SigAltStack(NULL, &ss);
  uint8_t* allocated_signal_stack = reinterpret_cast<uint8_t*>(ss.ss_sp);

  // Tell the kernel to stop using it.
  ss.ss_sp = NULL;
  ss.ss_flags = SS_DISABLE;
  ss.ss_size = SIGSTKSZ; // Avoid ENOMEM failure with Mac OS' buggy libc.
  SigAltStack(&ss, NULL);

  // Free it.
  delete[] allocated_signal_stack;
}

void Thread::CreateNativeThread(JNIEnv* env, jobject java_peer, size_t stack_size, bool daemon) {
  Thread* native_thread = new Thread(daemon);
  {
    ScopedObjectAccess soa(env);
    Object* peer = soa.Decode<Object*>(java_peer);
    CHECK(peer != NULL);
    native_thread->peer_ = peer;

    stack_size = FixStackSize(stack_size);

    // Thread.start is synchronized, so we know that vmData is 0, and know that we're not racing to
    // assign it.
    SetVmData(soa, peer, native_thread);
  }

  pthread_t new_pthread;
  pthread_attr_t attr;
  CHECK_PTHREAD_CALL(pthread_attr_init, (&attr), "new thread");
  CHECK_PTHREAD_CALL(pthread_attr_setdetachstate, (&attr, PTHREAD_CREATE_DETACHED), "PTHREAD_CREATE_DETACHED");
  CHECK_PTHREAD_CALL(pthread_attr_setstacksize, (&attr, stack_size), stack_size);
  int pthread_create_result = pthread_create(&new_pthread, &attr, Thread::CreateCallback, native_thread);
  CHECK_PTHREAD_CALL(pthread_attr_destroy, (&attr), "new thread");

  if (UNLIKELY(pthread_create_result != 0)) {
    // pthread_create(3) failed, so clean up.
    {
      ScopedObjectAccess soa(env);
      Object* peer = soa.Decode<Object*>(java_peer);
      SetVmData(soa, peer, 0);

      std::string msg(StringPrintf("pthread_create (%s stack) failed: %s",
                                   PrettySize(stack_size).c_str(), strerror(pthread_create_result)));
      Thread::Current()->ThrowOutOfMemoryError(msg.c_str());
    }
    delete native_thread;
    return;
  }
}

void Thread::Init() {
  // This function does all the initialization that must be run by the native thread it applies to.
  // (When we create a new thread from managed code, we allocate the Thread* in Thread::Create so
  // we can handshake with the corresponding native thread when it's ready.) Check this native
  // thread hasn't been through here already...
  CHECK(Thread::Current() == NULL);

  SetUpAlternateSignalStack();
  InitCpu();
  InitFunctionPointers();
#ifdef ART_USE_GREENLAND_COMPILER
  InitRuntimeEntryPoints(&runtime_entry_points_);
#endif
  InitCardTable();

  Runtime* runtime = Runtime::Current();
  CHECK(runtime != NULL);
  if (runtime->IsShuttingDown()) {
    UNIMPLEMENTED(WARNING) << "Thread attaching whilst runtime is shutting down";
  }
  thin_lock_id_ = runtime->GetThreadList()->AllocThreadId();
  pthread_self_ = pthread_self();

  InitTid();
  InitStackHwm();

  CHECK_PTHREAD_CALL(pthread_setspecific, (Thread::pthread_key_self_, this), "attach self");

  jni_env_ = new JNIEnvExt(this, runtime->GetJavaVM());

  runtime->GetThreadList()->Register(this);
}

Thread* Thread::Attach(const char* thread_name, bool as_daemon, jobject thread_group) {
  Thread* self = new Thread(as_daemon);
  self->Init();

  {
    MutexLock mu(*Locks::thread_suspend_count_lock_);
    CHECK_NE(self->GetState(), kRunnable);
    self->SetState(kNative);
  }

  // If we're the main thread, ClassLinker won't be created until after we're attached,
  // so that thread needs a two-stage attach. Regular threads don't need this hack.
  // In the compiler, all threads need this hack, because no-one's going to be getting
  // a native peer!
  if (self->thin_lock_id_ != ThreadList::kMainId && !Runtime::Current()->IsCompiler()) {
    self->CreatePeer(thread_name, as_daemon, thread_group);
  } else {
    // These aren't necessary, but they improve diagnostics for unit tests & command-line tools.
    if (thread_name != NULL) {
      self->name_->assign(thread_name);
      ::art::SetThreadName(thread_name);
    }
  }

  self->GetJniEnv()->locals.AssertEmpty();
  return self;
}

void Thread::CreatePeer(const char* name, bool as_daemon, jobject thread_group) {
  Runtime* runtime = Runtime::Current();
  CHECK(runtime->IsStarted());
  JNIEnv* env = jni_env_;

  if (thread_group == NULL) {
    thread_group = runtime->GetMainThreadGroup();
  }
  ScopedLocalRef<jobject> thread_name(env, env->NewStringUTF(name));
  jint thread_priority = GetNativePriority();
  jboolean thread_is_daemon = as_daemon;

  ScopedLocalRef<jobject> peer(env, env->AllocObject(WellKnownClasses::java_lang_Thread));
  {
    ScopedObjectAccess soa(env);
    peer_ = DecodeJObject(peer.get());
    if (peer_ == NULL) {
      CHECK(IsExceptionPending());
      return;
    }
  }
  env->CallNonvirtualVoidMethod(peer.get(),
                                WellKnownClasses::java_lang_Thread,
                                WellKnownClasses::java_lang_Thread_init,
                                thread_group, thread_name.get(), thread_priority, thread_is_daemon);
  AssertNoPendingException();

  ScopedObjectAccess soa(this);
  SetVmData(soa, peer_, Thread::Current());
  SirtRef<String> peer_thread_name(GetThreadName(soa));
  if (peer_thread_name.get() == NULL) {
    // The Thread constructor should have set the Thread.name to a
    // non-null value. However, because we can run without code
    // available (in the compiler, in tests), we manually assign the
    // fields the constructor should have set.
    soa.DecodeField(WellKnownClasses::java_lang_Thread_daemon)->SetBoolean(peer_, thread_is_daemon);
    soa.DecodeField(WellKnownClasses::java_lang_Thread_group)->SetObject(peer_, soa.Decode<Object*>(thread_group));
    soa.DecodeField(WellKnownClasses::java_lang_Thread_name)->SetObject(peer_, soa.Decode<Object*>(thread_name.get()));
    soa.DecodeField(WellKnownClasses::java_lang_Thread_priority)->SetInt(peer_, thread_priority);
    peer_thread_name.reset(GetThreadName(soa));
  }
  // 'thread_name' may have been null, so don't trust 'peer_thread_name' to be non-null.
  if (peer_thread_name.get() != NULL) {
    SetThreadName(peer_thread_name->ToModifiedUtf8().c_str());
  }
}

void Thread::SetThreadName(const char* name) {
  name_->assign(name);
  ::art::SetThreadName(name);
  Dbg::DdmSendThreadNotification(this, CHUNK_TYPE("THNM"));
}

void Thread::InitStackHwm() {
  void* stack_base;
  size_t stack_size;
  GetThreadStack(stack_base, stack_size);

  // TODO: include this in the thread dumps; potentially useful in SIGQUIT output?
  VLOG(threads) << StringPrintf("Native stack is at %p (%s)", stack_base, PrettySize(stack_size).c_str());

  stack_begin_ = reinterpret_cast<byte*>(stack_base);
  stack_size_ = stack_size;

  if (stack_size_ <= kStackOverflowReservedBytes) {
    LOG(FATAL) << "Attempt to attach a thread with a too-small stack (" << stack_size_ << " bytes)";
  }

  // TODO: move this into the Linux GetThreadStack implementation.
#if !defined(__APPLE__)
  // If we're the main thread, check whether we were run with an unlimited stack. In that case,
  // glibc will have reported a 2GB stack for our 32-bit process, and our stack overflow detection
  // will be broken because we'll die long before we get close to 2GB.
  if (thin_lock_id_ == 1) {
    rlimit stack_limit;
    if (getrlimit(RLIMIT_STACK, &stack_limit) == -1) {
      PLOG(FATAL) << "getrlimit(RLIMIT_STACK) failed";
    }
    if (stack_limit.rlim_cur == RLIM_INFINITY) {
      // Find the default stack size for new threads...
      pthread_attr_t default_attributes;
      size_t default_stack_size;
      CHECK_PTHREAD_CALL(pthread_attr_init, (&default_attributes), "default stack size query");
      CHECK_PTHREAD_CALL(pthread_attr_getstacksize, (&default_attributes, &default_stack_size),
                         "default stack size query");
      CHECK_PTHREAD_CALL(pthread_attr_destroy, (&default_attributes), "default stack size query");

      // ...and use that as our limit.
      size_t old_stack_size = stack_size_;
      stack_size_ = default_stack_size;
      stack_begin_ += (old_stack_size - stack_size_);
      VLOG(threads) << "Limiting unlimited stack (reported as " << PrettySize(old_stack_size) << ")"
                    << " to " << PrettySize(stack_size_)
                    << " with base " << reinterpret_cast<void*>(stack_begin_);
    }
  }
#endif

  // Set stack_end_ to the bottom of the stack saving space of stack overflows
  ResetDefaultStackEnd();

  // Sanity check.
  int stack_variable;
  CHECK_GT(&stack_variable, reinterpret_cast<void*>(stack_end_));
}

void Thread::ShortDump(std::ostream& os) const {
  os << "Thread[";
  if (GetThinLockId() != 0) {
    // If we're in kStarting, we won't have a thin lock id or tid yet.
    os << GetThinLockId()
             << ",tid=" << GetTid() << ',';
  }
  os << GetStateUnsafe()
           << ",Thread*=" << this
           << ",peer=" << peer_
           << ",\"" << *name_ << "\""
           << "]";
}

void Thread::Dump(std::ostream& os) const {
  DumpState(os);
  DumpStack(os);
}

String* Thread::GetThreadName(const ScopedObjectAccessUnchecked& soa) const {
  Field* f = soa.DecodeField(WellKnownClasses::java_lang_Thread_name);
  return (peer_ != NULL) ? reinterpret_cast<String*>(f->GetObject(peer_)) : NULL;
}

void Thread::GetThreadName(std::string& name) const {
  name.assign(*name_);
}

// Attempt to rectify locks so that we dump thread list with required locks before exiting.
static void UnsafeLogFatalForSuspendCount(Thread* self) NO_THREAD_SAFETY_ANALYSIS {
  Locks::thread_suspend_count_lock_->Unlock();
  Locks::mutator_lock_->SharedTryLock();
  if (!Locks::mutator_lock_->IsSharedHeld()) {
    LOG(WARNING) << "Dumping thread list without holding mutator_lock_";
  }
  Locks::thread_list_lock_->TryLock();
  if (!Locks::thread_list_lock_->IsExclusiveHeld()) {
    LOG(WARNING) << "Dumping thread list without holding thread_list_lock_";
  }
  std::ostringstream ss;
  Runtime::Current()->GetThreadList()->DumpLocked(ss);
  LOG(FATAL) << self << " suspend count already zero.\n" << ss.str();
}

void Thread::ModifySuspendCount(int delta, bool for_debugger) {
  DCHECK(delta == -1 || delta == +1 || delta == -debug_suspend_count_)
      << delta << " " << debug_suspend_count_ << " " << this;
  DCHECK_GE(suspend_count_, debug_suspend_count_) << this;
  Locks::thread_suspend_count_lock_->AssertHeld();

  if (delta == -1 && suspend_count_ <= 0) {
    // This is expected if you attach a thread during a GC.
    if (UNLIKELY(!IsStillStarting())) {
      UnsafeLogFatalForSuspendCount(this);
    }
    return;
  }
  suspend_count_ += delta;
  if (for_debugger) {
    debug_suspend_count_ += delta;
  }
}

void Thread::FullSuspendCheck() {
  VLOG(threads) << this << " self-suspending";
  // Make thread appear suspended to other threads, release mutator_lock_.
  TransitionFromRunnableToSuspended(kSuspended);
  // Transition back to runnable noting requests to suspend, re-acquire share on mutator_lock_.
  TransitionFromSuspendedToRunnable();
  VLOG(threads) << this << " self-reviving";
}

void Thread::TransitionFromRunnableToSuspended(ThreadState new_state) {
  AssertThreadSuspensionIsAllowable();
  CHECK_NE(new_state, kRunnable);
  CHECK_EQ(this, Thread::Current());
  // Change to non-runnable state, thereby appearing suspended to the system.
  ThreadState old_state = SetStateUnsafe(new_state);
  CHECK_EQ(old_state, kRunnable);
  // Release share on mutator_lock_.
  Locks::mutator_lock_->SharedUnlock();
}

ThreadState Thread::TransitionFromSuspendedToRunnable() {
  bool done = false;
  ThreadState old_state = GetStateUnsafe();
  DCHECK_NE(old_state, kRunnable);
  do {
    // Do a racy unsafe check of the suspend count to see if a wait is necessary. Any race that
    // may occur is covered by the second check after we acquire a share of the mutator_lock_.
    if (GetSuspendCountUnsafe() > 0) {
      // Wait while our suspend count is non-zero.
      MutexLock mu(*Locks::thread_suspend_count_lock_);
      Locks::mutator_lock_->AssertNotHeld();  // Otherwise we starve GC..
      while (GetSuspendCount() != 0) {
        // Re-check when Thread::resume_cond_ is notified.
        Thread::resume_cond_->Wait(*Locks::thread_suspend_count_lock_);
      }
    }
    // Re-acquire shared mutator_lock_ access.
    Locks::mutator_lock_->SharedLock();
    // Holding the mutator_lock_, synchronize with any thread trying to raise the suspend count
    // and change state to Runnable if no suspend is pending.
    MutexLock mu(*Locks::thread_suspend_count_lock_);
    if (GetSuspendCount() == 0) {
      SetState(kRunnable);
      done = true;
    } else {
      // Release shared mutator_lock_ access and try again.
      Locks::mutator_lock_->SharedUnlock();
    }
  } while (!done);
  return old_state;
}

Thread* Thread::SuspendForDebugger(jobject peer, bool request_suspension, bool* timeout) {
  static const useconds_t kTimeoutUs = 30 * 1000000; // 30s.
  useconds_t total_delay_us = 0;
  useconds_t delay_us = 0;
  bool did_suspend_request = false;
  *timeout = false;
  while (true) {
    Thread* thread;
    {
      ScopedObjectAccess soa(Thread::Current());
      MutexLock mu(*Locks::thread_list_lock_);
      thread = Thread::FromManagedThread(soa, peer);
      if (thread == NULL) {
        LOG(WARNING) << "No such thread for suspend: " << peer;
        return NULL;
      }
      {
        MutexLock mu(*Locks::thread_suspend_count_lock_);
        if (request_suspension) {
          thread->ModifySuspendCount(+1, true /* for_debugger */);
          request_suspension = false;
          did_suspend_request = true;
        }
        // IsSuspended on the current thread will fail as the current thread is changed into
        // Runnable above. As the suspend count is now raised if this is the current thread
        // it will self suspend on transition to Runnable, making it hard to work with. Its simpler
        // to just explicitly handle the current thread in the callers to this code.
        CHECK_NE(thread, soa.Self()) << "Attempt to suspend for debugger the current thread";
        // If thread is suspended (perhaps it was already not Runnable but didn't have a suspend
        // count, or else we've waited and it has self suspended) or is the current thread, we're
        // done.
        if (thread->IsSuspended()) {
          return thread;
        }
        if (total_delay_us >= kTimeoutUs) {
          LOG(ERROR) << "Thread suspension timed out: " << peer;
          if (did_suspend_request) {
            thread->ModifySuspendCount(-1, true /* for_debugger */);
          }
          *timeout = true;
          return NULL;
        }
      }
      // Release locks and come out of runnable state.
    }
    for (int i = kMaxMutexLevel; i >= 0; --i) {
      BaseMutex* held_mutex = Thread::Current()->GetHeldMutex(static_cast<MutexLevel>(i));
      if (held_mutex != NULL) {
        LOG(FATAL) << "Holding " << held_mutex->GetName()
            << " while sleeping for thread suspension";
      }
    }
    {
      useconds_t new_delay_us = delay_us * 2;
      CHECK_GE(new_delay_us, delay_us);
      if (new_delay_us < 500000) {  // Don't allow sleeping to be more than 0.5s.
        delay_us = new_delay_us;
      }
    }
    if (delay_us == 0) {
      sched_yield();
      // Default to 1 milliseconds (note that this gets multiplied by 2 before the first sleep).
      delay_us = 500;
    } else {
      usleep(delay_us);
      total_delay_us += delay_us;
    }
  }
}

void Thread::DumpState(std::ostream& os, const Thread* thread, pid_t tid) {
  std::string group_name;
  int priority;
  bool is_daemon = false;

  if (thread != NULL && thread->peer_ != NULL) {
    ScopedObjectAccess soa(Thread::Current());
    priority = soa.DecodeField(WellKnownClasses::java_lang_Thread_priority)->GetInt(thread->peer_);
    is_daemon = soa.DecodeField(WellKnownClasses::java_lang_Thread_daemon)->GetBoolean(thread->peer_);

    Object* thread_group = thread->GetThreadGroup(soa);
    if (thread_group != NULL) {
      Field* group_name_field = soa.DecodeField(WellKnownClasses::java_lang_ThreadGroup_name);
      String* group_name_string = reinterpret_cast<String*>(group_name_field->GetObject(thread_group));
      group_name = (group_name_string != NULL) ? group_name_string->ToModifiedUtf8() : "<null>";
    }
  } else {
    priority = GetNativePriority();
  }

  std::string scheduler_group_name(GetSchedulerGroupName(tid));
  if (scheduler_group_name.empty()) {
    scheduler_group_name = "default";
  }

  if (thread != NULL) {
    os << '"' << *thread->name_ << '"';
    if (is_daemon) {
      os << " daemon";
    }
    MutexLock mu(*Locks::thread_suspend_count_lock_);
    os << " prio=" << priority
       << " tid=" << thread->GetThinLockId()
       << " " << thread->GetState() << "\n";
  } else {
    os << '"' << ::art::GetThreadName(tid) << '"'
       << " prio=" << priority
       << " (not attached)\n";
  }

  if (thread != NULL) {
    MutexLock mu(*Locks::thread_suspend_count_lock_);
    os << "  | group=\"" << group_name << "\""
       << " sCount=" << thread->suspend_count_
       << " dsCount=" << thread->debug_suspend_count_
       << " obj=" << reinterpret_cast<void*>(thread->peer_)
       << " self=" << reinterpret_cast<const void*>(thread) << "\n";
  }

  os << "  | sysTid=" << tid
     << " nice=" << getpriority(PRIO_PROCESS, tid)
     << " cgrp=" << scheduler_group_name;
  if (thread != NULL) {
    int policy;
    sched_param sp;
    CHECK_PTHREAD_CALL(pthread_getschedparam, (thread->pthread_self_, &policy, &sp), __FUNCTION__);
    os << " sched=" << policy << "/" << sp.sched_priority
       << " handle=" << reinterpret_cast<void*>(thread->pthread_self_);
  }
  os << "\n";

  // Grab the scheduler stats for this thread.
  std::string scheduler_stats;
  if (ReadFileToString(StringPrintf("/proc/self/task/%d/schedstat", tid), &scheduler_stats)) {
    scheduler_stats.resize(scheduler_stats.size() - 1); // Lose the trailing '\n'.
  } else {
    scheduler_stats = "0 0 0";
  }

  int utime = 0;
  int stime = 0;
  int task_cpu = 0;
  GetTaskStats(tid, utime, stime, task_cpu);

  os << "  | schedstat=( " << scheduler_stats << " )"
     << " utm=" << utime
     << " stm=" << stime
     << " core=" << task_cpu
     << " HZ=" << sysconf(_SC_CLK_TCK) << "\n";
  if (thread != NULL) {
    os << "  | stack=" << reinterpret_cast<void*>(thread->stack_begin_) << "-" << reinterpret_cast<void*>(thread->stack_end_)
       << " stackSize=" << PrettySize(thread->stack_size_) << "\n";
  }
}

void Thread::DumpState(std::ostream& os) const {
  Thread::DumpState(os, this, GetTid());
}

struct StackDumpVisitor : public StackVisitor {
  StackDumpVisitor(std::ostream& os, const Thread* thread, Context* context, bool can_allocate)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : StackVisitor(thread->GetManagedStack(), thread->GetTraceStack(), context),
        os(os), thread(thread), can_allocate(can_allocate),
        last_method(NULL), last_line_number(0), repetition_count(0), frame_count(0) {
  }

  virtual ~StackDumpVisitor() {
    if (frame_count == 0) {
      os << "  (no managed stack frames)\n";
    }
  }

  bool VisitFrame() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    Method* m = GetMethod();
    if (m->IsRuntimeMethod()) {
      return true;
    }
    const int kMaxRepetition = 3;
    Class* c = m->GetDeclaringClass();
    ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
    const DexCache* dex_cache = c->GetDexCache();
    int line_number = -1;
    if (dex_cache != NULL) {  // be tolerant of bad input
      const DexFile& dex_file = class_linker->FindDexFile(dex_cache);
      line_number = dex_file.GetLineNumFromPC(m, GetDexPc());
    }
    if (line_number == last_line_number && last_method == m) {
      repetition_count++;
    } else {
      if (repetition_count >= kMaxRepetition) {
        os << "  ... repeated " << (repetition_count - kMaxRepetition) << " times\n";
      }
      repetition_count = 0;
      last_line_number = line_number;
      last_method = m;
    }
    if (repetition_count < kMaxRepetition) {
      os << "  at " << PrettyMethod(m, false);
      if (m->IsNative()) {
        os << "(Native method)";
      } else {
        mh.ChangeMethod(m);
        const char* source_file(mh.GetDeclaringClassSourceFile());
        os << "(" << (source_file != NULL ? source_file : "unavailable")
           << ":" << line_number << ")";
      }
      os << "\n";
      if (frame_count == 0) {
        Monitor::DescribeWait(os, thread);
      }
      if (can_allocate) {
        Monitor::DescribeLocks(os, this);
      }
    }

    ++frame_count;
    return true;
  }
  std::ostream& os;
  const Thread* thread;
  bool can_allocate;
  MethodHelper mh;
  Method* last_method;
  int last_line_number;
  int repetition_count;
  int frame_count;
};

void Thread::DumpStack(std::ostream& os) const {
  // If we're currently in native code, dump that stack before dumping the managed stack.
  ThreadState state;
  {
    MutexLock mu(*Locks::thread_suspend_count_lock_);
    state = GetState();
  }
  if (state == kNative) {
    DumpKernelStack(os, GetTid(), "  kernel: ", false);
    DumpNativeStack(os, GetTid(), "  native: ", false);
  }
  UniquePtr<Context> context(Context::Create());
  StackDumpVisitor dumper(os, this, context.get(), !throwing_OutOfMemoryError_);
  dumper.WalkStack();
}

void Thread::ThreadExitCallback(void* arg) {
  Thread* self = reinterpret_cast<Thread*>(arg);
  if (self->thread_exit_check_count_ == 0) {
    LOG(WARNING) << "Native thread exiting without having called DetachCurrentThread (maybe it's going to use a pthread_key_create destructor?): " << *self;
    CHECK_PTHREAD_CALL(pthread_setspecific, (Thread::pthread_key_self_, self), "reattach self");
    self->thread_exit_check_count_ = 1;
  } else {
    LOG(FATAL) << "Native thread exited without calling DetachCurrentThread: " << *self;
  }
}

void Thread::Startup() {
  {
    MutexLock mu(*Locks::thread_suspend_count_lock_);  // Keep GCC happy.
    resume_cond_ = new ConditionVariable("Thread resumption condition variable");
  }

  // Allocate a TLS slot.
  CHECK_PTHREAD_CALL(pthread_key_create, (&Thread::pthread_key_self_, Thread::ThreadExitCallback), "self key");

  // Double-check the TLS slot allocation.
  if (pthread_getspecific(pthread_key_self_) != NULL) {
    LOG(FATAL) << "Newly-created pthread TLS slot is not NULL";
  }
}

void Thread::FinishStartup() {
  Runtime* runtime = Runtime::Current();
  CHECK(runtime->IsStarted());

  // Finish attaching the main thread.
  ScopedObjectAccess soa(Thread::Current());
  Thread::Current()->CreatePeer("main", false, runtime->GetMainThreadGroup());

  Runtime::Current()->GetClassLinker()->RunRootClinits();
}

void Thread::Shutdown() {
  CHECK_PTHREAD_CALL(pthread_key_delete, (Thread::pthread_key_self_), "self key");
}

Thread::Thread(bool daemon)
    : suspend_count_(0),
      card_table_(NULL),
      exception_(NULL),
      stack_end_(NULL),
      managed_stack_(),
      jni_env_(NULL),
      self_(NULL),
      state_(kNative),
      peer_(NULL),
      stack_begin_(NULL),
      stack_size_(0),
      thin_lock_id_(0),
      tid_(0),
      wait_mutex_(new Mutex("a thread wait mutex")),
      wait_cond_(new ConditionVariable("a thread wait condition variable")),
      wait_monitor_(NULL),
      interrupted_(false),
      wait_next_(NULL),
      monitor_enter_object_(NULL),
      top_sirt_(NULL),
      runtime_(NULL),
      class_loader_override_(NULL),
      long_jump_context_(NULL),
      throwing_OutOfMemoryError_(false),
      debug_suspend_count_(0),
      debug_invoke_req_(new DebugInvokeReq),
      trace_stack_(new std::vector<TraceStackFrame>),
      name_(new std::string(kThreadNameDuringStartup)),
      daemon_(daemon),
      pthread_self_(0),
      no_thread_suspension_(0),
      last_no_thread_suspension_cause_(NULL),
      thread_exit_check_count_(0) {
  CHECK_EQ((sizeof(Thread) % 4), 0U) << sizeof(Thread);
  memset(&held_mutexes_[0], 0, sizeof(held_mutexes_));
}

bool Thread::IsStillStarting() const {
  // You might think you can check whether the state is kStarting, but for much of thread startup,
  // the thread might also be in kVmWait.
  // You might think you can check whether the peer is NULL, but the peer is actually created and
  // assigned fairly early on, and needs to be.
  // It turns out that the last thing to change is the thread name; that's a good proxy for "has
  // this thread _ever_ entered kRunnable".
  return (*name_ == kThreadNameDuringStartup);
}

void Thread::AssertNoPendingException() const {
  if (UNLIKELY(IsExceptionPending())) {
    ScopedObjectAccess soa(Thread::Current());
    Throwable* exception = GetException();
    LOG(FATAL) << "No pending exception expected: " << exception->Dump();
  }
}

static void MonitorExitVisitor(const Object* object, void* arg) NO_THREAD_SAFETY_ANALYSIS {
  Thread* self = reinterpret_cast<Thread*>(arg);
  Object* entered_monitor = const_cast<Object*>(object);
  if (self->HoldsLock(entered_monitor)) {
    LOG(WARNING) << "Calling MonitorExit on object "
                 << object << " (" << PrettyTypeOf(object) << ")"
                 << " left locked by native thread "
                 << *Thread::Current() << " which is detaching";
    entered_monitor->MonitorExit(self);
  }
}

void Thread::Destroy() {
  // On thread detach, all monitors entered with JNI MonitorEnter are automatically exited.
  if (jni_env_ != NULL) {
    jni_env_->monitors.VisitRoots(MonitorExitVisitor, Thread::Current());
  }

  if (peer_ != NULL) {
    Thread* self = this;

    // We may need to call user-supplied managed code.
    ScopedObjectAccess soa(this);

    HandleUncaughtExceptions(soa);
    RemoveFromThreadGroup(soa);

    // this.vmData = 0;
    SetVmData(soa, peer_, NULL);

    Dbg::PostThreadDeath(self);

    // Thread.join() is implemented as an Object.wait() on the Thread.lock
    // object. Signal anyone who is waiting.
    Object* lock = soa.DecodeField(WellKnownClasses::java_lang_Thread_lock)->GetObject(peer_);
    // (This conditional is only needed for tests, where Thread.lock won't have been set.)
    if (lock != NULL) {
      lock->MonitorEnter(self);
      lock->NotifyAll();
      lock->MonitorExit(self);
    }
  }
}

Thread::~Thread() {
  delete jni_env_;
  jni_env_ = NULL;

  {
    MutexLock mu(*Locks::thread_suspend_count_lock_);
    CHECK_NE(GetState(), kRunnable);
    SetState(kTerminated);
  }

  delete wait_cond_;
  delete wait_mutex_;

#if !defined(ART_USE_LLVM_COMPILER)
  delete long_jump_context_;
#endif

  delete debug_invoke_req_;
  delete trace_stack_;
  delete name_;

  TearDownAlternateSignalStack();
}

void Thread::HandleUncaughtExceptions(const ScopedObjectAccess& soa) {
  if (!IsExceptionPending()) {
    return;
  }
  // Get and clear the exception.
  Object* exception = GetException();
  ClearException();

  // If the thread has its own handler, use that.
  Object* handler =
      soa.DecodeField(WellKnownClasses::java_lang_Thread_uncaughtHandler)->GetObject(peer_);
  if (handler == NULL) {
    // Otherwise use the thread group's default handler.
    handler = GetThreadGroup(soa);
  }

  // Call the handler.
  jmethodID mid = WellKnownClasses::java_lang_Thread$UncaughtExceptionHandler_uncaughtException;
  Method* m = handler->GetClass()->FindVirtualMethodForVirtualOrInterface(soa.DecodeMethod(mid));
  JValue args[2];
  args[0].SetL(peer_);
  args[1].SetL(exception);
  m->Invoke(this, handler, args, NULL);

  // If the handler threw, clear that exception too.
  ClearException();
}

Object* Thread::GetThreadGroup(const ScopedObjectAccessUnchecked& soa) const {
  return soa.DecodeField(WellKnownClasses::java_lang_Thread_group)->GetObject(peer_);
}

void Thread::RemoveFromThreadGroup(const ScopedObjectAccess& soa) {
  // this.group.removeThread(this);
  // group can be null if we're in the compiler or a test.
  Object* group = GetThreadGroup(soa);
  if (group != NULL) {
    jmethodID mid = WellKnownClasses::java_lang_ThreadGroup_removeThread;
    Method* m = group->GetClass()->FindVirtualMethodForVirtualOrInterface(soa.DecodeMethod(mid));
    JValue args[1];
    args[0].SetL(peer_);
    m->Invoke(this, group, args, NULL);
  }
}

size_t Thread::NumSirtReferences() {
  size_t count = 0;
  for (StackIndirectReferenceTable* cur = top_sirt_; cur; cur = cur->GetLink()) {
    count += cur->NumberOfReferences();
  }
  return count;
}

bool Thread::SirtContains(jobject obj) {
  Object** sirt_entry = reinterpret_cast<Object**>(obj);
  for (StackIndirectReferenceTable* cur = top_sirt_; cur; cur = cur->GetLink()) {
    if (cur->Contains(sirt_entry)) {
      return true;
    }
  }
  // JNI code invoked from portable code uses shadow frames rather than the SIRT.
  return managed_stack_.ShadowFramesContain(sirt_entry);
}

void Thread::SirtVisitRoots(Heap::RootVisitor* visitor, void* arg) {
  for (StackIndirectReferenceTable* cur = top_sirt_; cur; cur = cur->GetLink()) {
    size_t num_refs = cur->NumberOfReferences();
    for (size_t j = 0; j < num_refs; j++) {
      Object* object = cur->GetReference(j);
      if (object != NULL) {
        visitor(object, arg);
      }
    }
  }
}

Object* Thread::DecodeJObject(jobject obj) {
  DCHECK(CanAccessDirectReferences());
  if (obj == NULL) {
    return NULL;
  }
  IndirectRef ref = reinterpret_cast<IndirectRef>(obj);
  IndirectRefKind kind = GetIndirectRefKind(ref);
  Object* result;
  switch (kind) {
  case kLocal:
    {
      IndirectReferenceTable& locals = jni_env_->locals;
      result = const_cast<Object*>(locals.Get(ref));
      break;
    }
  case kGlobal:
    {
      JavaVMExt* vm = Runtime::Current()->GetJavaVM();
      IndirectReferenceTable& globals = vm->globals;
      MutexLock mu(vm->globals_lock);
      result = const_cast<Object*>(globals.Get(ref));
      break;
    }
  case kWeakGlobal:
    {
      JavaVMExt* vm = Runtime::Current()->GetJavaVM();
      IndirectReferenceTable& weak_globals = vm->weak_globals;
      MutexLock mu(vm->weak_globals_lock);
      result = const_cast<Object*>(weak_globals.Get(ref));
      if (result == kClearedJniWeakGlobal) {
        // This is a special case where it's okay to return NULL.
        return NULL;
      }
      break;
    }
  case kSirtOrInvalid:
  default:
    // TODO: make stack indirect reference table lookup more efficient
    // Check if this is a local reference in the SIRT
    if (SirtContains(obj)) {
      result = *reinterpret_cast<Object**>(obj);  // Read from SIRT
    } else if (Runtime::Current()->GetJavaVM()->work_around_app_jni_bugs) {
      // Assume an invalid local reference is actually a direct pointer.
      result = reinterpret_cast<Object*>(obj);
    } else {
      result = kInvalidIndirectRefObject;
    }
  }

  if (result == NULL) {
    JniAbortF(NULL, "use of deleted %s %p", ToStr<IndirectRefKind>(kind).c_str(), obj);
  } else {
    if (result != kInvalidIndirectRefObject) {
      Runtime::Current()->GetHeap()->VerifyObject(result);
    }
  }
  return result;
}

class CountStackDepthVisitor : public StackVisitor {
 public:
  CountStackDepthVisitor(const ManagedStack* stack,
                         const std::vector<TraceStackFrame>* trace_stack)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : StackVisitor(stack, trace_stack, NULL),
        depth_(0), skip_depth_(0), skipping_(true) {}

  bool VisitFrame() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    // We want to skip frames up to and including the exception's constructor.
    // Note we also skip the frame if it doesn't have a method (namely the callee
    // save frame)
    Method* m = GetMethod();
    if (skipping_ && !m->IsRuntimeMethod() &&
        !Throwable::GetJavaLangThrowable()->IsAssignableFrom(m->GetDeclaringClass())) {
      skipping_ = false;
    }
    if (!skipping_) {
      if (!m->IsRuntimeMethod()) {  // Ignore runtime frames (in particular callee save).
        ++depth_;
      }
    } else {
      ++skip_depth_;
    }
    return true;
  }

  int GetDepth() const {
    return depth_;
  }

  int GetSkipDepth() const {
    return skip_depth_;
  }

 private:
  uint32_t depth_;
  uint32_t skip_depth_;
  bool skipping_;
};

class BuildInternalStackTraceVisitor : public StackVisitor {
 public:
  explicit BuildInternalStackTraceVisitor(const ManagedStack* stack,
                                          const std::vector<TraceStackFrame>* trace_stack,
                                          int skip_depth)
      : StackVisitor(stack, trace_stack, NULL),
        skip_depth_(skip_depth), count_(0), dex_pc_trace_(NULL), method_trace_(NULL) {}

  bool Init(int depth, const ScopedObjectAccess& soa)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    // Allocate method trace with an extra slot that will hold the PC trace
    SirtRef<ObjectArray<Object> >
      method_trace(Runtime::Current()->GetClassLinker()->AllocObjectArray<Object>(depth + 1));
    if (method_trace.get() == NULL) {
      return false;
    }
    IntArray* dex_pc_trace = IntArray::Alloc(depth);
    if (dex_pc_trace == NULL) {
      return false;
    }
    // Save PC trace in last element of method trace, also places it into the
    // object graph.
    method_trace->Set(depth, dex_pc_trace);
    // Set the Object*s and assert that no thread suspension is now possible.
    const char* last_no_suspend_cause =
        soa.Self()->StartAssertNoThreadSuspension("Building internal stack trace");
    CHECK(last_no_suspend_cause == NULL) << last_no_suspend_cause;
    method_trace_ = method_trace.get();
    dex_pc_trace_ = dex_pc_trace;
    return true;
  }

  virtual ~BuildInternalStackTraceVisitor() {
    if (method_trace_ != NULL) {
      Thread::Current()->EndAssertNoThreadSuspension(NULL);
    }
  }

  bool VisitFrame() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    if (method_trace_ == NULL || dex_pc_trace_ == NULL) {
      return true; // We're probably trying to fillInStackTrace for an OutOfMemoryError.
    }
    if (skip_depth_ > 0) {
      skip_depth_--;
      return true;
    }
    Method* m = GetMethod();
    if (m->IsRuntimeMethod()) {
      return true;  // Ignore runtime frames (in particular callee save).
    }
    method_trace_->Set(count_, m);
    dex_pc_trace_->Set(count_, GetDexPc());
    ++count_;
    return true;
  }

  ObjectArray<Object>* GetInternalStackTrace() const {
    return method_trace_;
  }

 private:
  // How many more frames to skip.
  int32_t skip_depth_;
  // Current position down stack trace.
  uint32_t count_;
  // Array of dex PC values.
  IntArray* dex_pc_trace_;
  // An array of the methods on the stack, the last entry is a reference to the PC trace.
  ObjectArray<Object>* method_trace_;
};

void Thread::PushSirt(StackIndirectReferenceTable* sirt) {
  sirt->SetLink(top_sirt_);
  top_sirt_ = sirt;
}

StackIndirectReferenceTable* Thread::PopSirt() {
  CHECK(top_sirt_ != NULL);
  StackIndirectReferenceTable* sirt = top_sirt_;
  top_sirt_ = top_sirt_->GetLink();
  return sirt;
}

jobject Thread::CreateInternalStackTrace(const ScopedObjectAccess& soa) const {
  // Compute depth of stack
  CountStackDepthVisitor count_visitor(GetManagedStack(), GetTraceStack());
  count_visitor.WalkStack();
  int32_t depth = count_visitor.GetDepth();
  int32_t skip_depth = count_visitor.GetSkipDepth();

  // Build internal stack trace
  BuildInternalStackTraceVisitor build_trace_visitor(GetManagedStack(), GetTraceStack(),
                                                     skip_depth);
  if (!build_trace_visitor.Init(depth, soa)) {
    return NULL;  // Allocation failed
  }
  build_trace_visitor.WalkStack();
  return soa.AddLocalReference<jobjectArray>(build_trace_visitor.GetInternalStackTrace());
}

jobjectArray Thread::InternalStackTraceToStackTraceElementArray(JNIEnv* env, jobject internal,
    jobjectArray output_array, int* stack_depth) {
  // Transition into runnable state to work on Object*/Array*
  ScopedObjectAccess soa(env);
  // Decode the internal stack trace into the depth, method trace and PC trace
  ObjectArray<Object>* method_trace = soa.Decode<ObjectArray<Object>*>(internal);
  int32_t depth = method_trace->GetLength() - 1;
  IntArray* pc_trace = down_cast<IntArray*>(method_trace->Get(depth));

  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();

  jobjectArray result;
  ObjectArray<StackTraceElement>* java_traces;
  if (output_array != NULL) {
    // Reuse the array we were given.
    result = output_array;
    java_traces = soa.Decode<ObjectArray<StackTraceElement>*>(output_array);
    // ...adjusting the number of frames we'll write to not exceed the array length.
    depth = std::min(depth, java_traces->GetLength());
  } else {
    // Create java_trace array and place in local reference table
    java_traces = class_linker->AllocStackTraceElementArray(depth);
    if (java_traces == NULL) {
      return NULL;
    }
    result = soa.AddLocalReference<jobjectArray>(java_traces);
  }

  if (stack_depth != NULL) {
    *stack_depth = depth;
  }

  MethodHelper mh;
  for (int32_t i = 0; i < depth; ++i) {
    // Prepare parameters for StackTraceElement(String cls, String method, String file, int line)
    Method* method = down_cast<Method*>(method_trace->Get(i));
    mh.ChangeMethod(method);
    uint32_t dex_pc = pc_trace->Get(i);
    int32_t line_number = mh.GetLineNumFromDexPC(dex_pc);
    // Allocate element, potentially triggering GC
    // TODO: reuse class_name_object via Class::name_?
    const char* descriptor = mh.GetDeclaringClassDescriptor();
    CHECK(descriptor != NULL);
    std::string class_name(PrettyDescriptor(descriptor));
    SirtRef<String> class_name_object(String::AllocFromModifiedUtf8(class_name.c_str()));
    if (class_name_object.get() == NULL) {
      return NULL;
    }
    const char* method_name = mh.GetName();
    CHECK(method_name != NULL);
    SirtRef<String> method_name_object(String::AllocFromModifiedUtf8(method_name));
    if (method_name_object.get() == NULL) {
      return NULL;
    }
    const char* source_file = mh.GetDeclaringClassSourceFile();
    SirtRef<String> source_name_object(String::AllocFromModifiedUtf8(source_file));
    StackTraceElement* obj = StackTraceElement::Alloc(class_name_object.get(),
                                                      method_name_object.get(),
                                                      source_name_object.get(),
                                                      line_number);
    if (obj == NULL) {
      return NULL;
    }
#ifdef MOVING_GARBAGE_COLLECTOR
    // Re-read after potential GC
    java_traces = Decode<ObjectArray<Object>*>(soa.Env(), result);
    method_trace = down_cast<ObjectArray<Object>*>(Decode<Object*>(soa.Env(), internal));
    pc_trace = down_cast<IntArray*>(method_trace->Get(depth));
#endif
    java_traces->Set(i, obj);
  }
  return result;
}

void Thread::ThrowNewExceptionF(const char* exception_class_descriptor, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  ThrowNewExceptionV(exception_class_descriptor, fmt, args);
  va_end(args);
}

void Thread::ThrowNewExceptionV(const char* exception_class_descriptor, const char* fmt, va_list ap) {
  std::string msg;
  StringAppendV(&msg, fmt, ap);
  ThrowNewException(exception_class_descriptor, msg.c_str());
}

void Thread::ThrowNewException(const char* exception_class_descriptor, const char* msg) {
  AssertNoPendingException(); // Callers should either clear or call ThrowNewWrappedException.
  ThrowNewWrappedException(exception_class_descriptor, msg);
}

void Thread::ThrowNewWrappedException(const char* exception_class_descriptor, const char* msg) {
  // Convert "Ljava/lang/Exception;" into JNI-style "java/lang/Exception".
  CHECK_EQ('L', exception_class_descriptor[0]);
  std::string descriptor(exception_class_descriptor + 1);
  CHECK_EQ(';', descriptor[descriptor.length() - 1]);
  descriptor.erase(descriptor.length() - 1);

  JNIEnv* env = GetJniEnv();
  jobject cause = env->ExceptionOccurred();
  env->ExceptionClear();

  ScopedLocalRef<jclass> exception_class(env, env->FindClass(descriptor.c_str()));
  if (exception_class.get() == NULL) {
    LOG(ERROR) << "Couldn't throw new " << descriptor << " because JNI FindClass failed: "
               << PrettyTypeOf(GetException());
    CHECK(IsExceptionPending());
    return;
  }
  if (!Runtime::Current()->IsStarted()) {
    // Something is trying to throw an exception without a started
    // runtime, which is the common case in the compiler. We won't be
    // able to invoke the constructor of the exception, so use
    // AllocObject which will not invoke a constructor.
    ScopedLocalRef<jthrowable> exception(
        env, reinterpret_cast<jthrowable>(env->AllocObject(exception_class.get())));
    if (exception.get() != NULL) {
      ScopedObjectAccessUnchecked soa(env);
      Throwable* t = reinterpret_cast<Throwable*>(soa.Self()->DecodeJObject(exception.get()));
      t->SetDetailMessage(String::AllocFromModifiedUtf8(msg));
      soa.Self()->SetException(t);
    } else {
      LOG(ERROR) << "Couldn't throw new " << descriptor << " because JNI AllocObject failed: "
                 << PrettyTypeOf(GetException());
      CHECK(IsExceptionPending());
    }
    return;
  }
  int rc = ::art::ThrowNewException(env, exception_class.get(), msg, cause);
  if (rc != JNI_OK) {
    LOG(ERROR) << "Couldn't throw new " << descriptor << " because JNI ThrowNew failed: "
               << PrettyTypeOf(GetException());
    CHECK(IsExceptionPending());
  }
}

void Thread::ThrowOutOfMemoryError(const char* msg) {
  LOG(ERROR) << StringPrintf("Throwing OutOfMemoryError \"%s\"%s",
      msg, (throwing_OutOfMemoryError_ ? " (recursive case)" : ""));
  if (!throwing_OutOfMemoryError_) {
    throwing_OutOfMemoryError_ = true;
    ThrowNewException("Ljava/lang/OutOfMemoryError;", msg);
  } else {
    Dump(LOG(ERROR)); // The pre-allocated OOME has no stack, so help out and log one.
    SetException(Runtime::Current()->GetPreAllocatedOutOfMemoryError());
  }
  throwing_OutOfMemoryError_ = false;
}

Thread* Thread::CurrentFromGdb() {
  return Thread::Current();
}

void Thread::DumpFromGdb() const {
  std::ostringstream ss;
  Dump(ss);
  std::string str(ss.str());
  // log to stderr for debugging command line processes
  std::cerr << str;
#ifdef HAVE_ANDROID_OS
  // log to logcat for debugging frameworks processes
  LOG(INFO) << str;
#endif
}

struct EntryPointInfo {
  uint32_t offset;
  const char* name;
};
#define ENTRY_POINT_INFO(x) { ENTRYPOINT_OFFSET(x), #x }
static const EntryPointInfo gThreadEntryPointInfo[] = {
  ENTRY_POINT_INFO(pAllocArrayFromCode),
  ENTRY_POINT_INFO(pAllocArrayFromCodeWithAccessCheck),
  ENTRY_POINT_INFO(pAllocObjectFromCode),
  ENTRY_POINT_INFO(pAllocObjectFromCodeWithAccessCheck),
  ENTRY_POINT_INFO(pCheckAndAllocArrayFromCode),
  ENTRY_POINT_INFO(pCheckAndAllocArrayFromCodeWithAccessCheck),
  ENTRY_POINT_INFO(pInstanceofNonTrivialFromCode),
  ENTRY_POINT_INFO(pCanPutArrayElementFromCode),
  ENTRY_POINT_INFO(pCheckCastFromCode),
  ENTRY_POINT_INFO(pDebugMe),
  ENTRY_POINT_INFO(pUpdateDebuggerFromCode),
  ENTRY_POINT_INFO(pInitializeStaticStorage),
  ENTRY_POINT_INFO(pInitializeTypeAndVerifyAccessFromCode),
  ENTRY_POINT_INFO(pInitializeTypeFromCode),
  ENTRY_POINT_INFO(pResolveStringFromCode),
  ENTRY_POINT_INFO(pSet32Instance),
  ENTRY_POINT_INFO(pSet32Static),
  ENTRY_POINT_INFO(pSet64Instance),
  ENTRY_POINT_INFO(pSet64Static),
  ENTRY_POINT_INFO(pSetObjInstance),
  ENTRY_POINT_INFO(pSetObjStatic),
  ENTRY_POINT_INFO(pGet32Instance),
  ENTRY_POINT_INFO(pGet32Static),
  ENTRY_POINT_INFO(pGet64Instance),
  ENTRY_POINT_INFO(pGet64Static),
  ENTRY_POINT_INFO(pGetObjInstance),
  ENTRY_POINT_INFO(pGetObjStatic),
  ENTRY_POINT_INFO(pHandleFillArrayDataFromCode),
  ENTRY_POINT_INFO(pFindNativeMethod),
  ENTRY_POINT_INFO(pJniMethodStart),
  ENTRY_POINT_INFO(pJniMethodStartSynchronized),
  ENTRY_POINT_INFO(pJniMethodEnd),
  ENTRY_POINT_INFO(pJniMethodEndSynchronized),
  ENTRY_POINT_INFO(pJniMethodEndWithReference),
  ENTRY_POINT_INFO(pJniMethodEndWithReferenceSynchronized),
  ENTRY_POINT_INFO(pLockObjectFromCode),
  ENTRY_POINT_INFO(pUnlockObjectFromCode),
  ENTRY_POINT_INFO(pCmpgDouble),
  ENTRY_POINT_INFO(pCmpgFloat),
  ENTRY_POINT_INFO(pCmplDouble),
  ENTRY_POINT_INFO(pCmplFloat),
  ENTRY_POINT_INFO(pDadd),
  ENTRY_POINT_INFO(pDdiv),
  ENTRY_POINT_INFO(pDmul),
  ENTRY_POINT_INFO(pDsub),
  ENTRY_POINT_INFO(pF2d),
  ENTRY_POINT_INFO(pFmod),
  ENTRY_POINT_INFO(pI2d),
  ENTRY_POINT_INFO(pL2d),
  ENTRY_POINT_INFO(pD2f),
  ENTRY_POINT_INFO(pFadd),
  ENTRY_POINT_INFO(pFdiv),
  ENTRY_POINT_INFO(pFmodf),
  ENTRY_POINT_INFO(pFmul),
  ENTRY_POINT_INFO(pFsub),
  ENTRY_POINT_INFO(pI2f),
  ENTRY_POINT_INFO(pL2f),
  ENTRY_POINT_INFO(pD2iz),
  ENTRY_POINT_INFO(pF2iz),
  ENTRY_POINT_INFO(pIdivmod),
  ENTRY_POINT_INFO(pD2l),
  ENTRY_POINT_INFO(pF2l),
  ENTRY_POINT_INFO(pLdiv),
  ENTRY_POINT_INFO(pLdivmod),
  ENTRY_POINT_INFO(pLmul),
  ENTRY_POINT_INFO(pShlLong),
  ENTRY_POINT_INFO(pShrLong),
  ENTRY_POINT_INFO(pUshrLong),
  ENTRY_POINT_INFO(pIndexOf),
  ENTRY_POINT_INFO(pMemcmp16),
  ENTRY_POINT_INFO(pStringCompareTo),
  ENTRY_POINT_INFO(pMemcpy),
  ENTRY_POINT_INFO(pUnresolvedDirectMethodTrampolineFromCode),
  ENTRY_POINT_INFO(pInvokeDirectTrampolineWithAccessCheck),
  ENTRY_POINT_INFO(pInvokeInterfaceTrampoline),
  ENTRY_POINT_INFO(pInvokeInterfaceTrampolineWithAccessCheck),
  ENTRY_POINT_INFO(pInvokeStaticTrampolineWithAccessCheck),
  ENTRY_POINT_INFO(pInvokeSuperTrampolineWithAccessCheck),
  ENTRY_POINT_INFO(pInvokeVirtualTrampolineWithAccessCheck),
  ENTRY_POINT_INFO(pCheckSuspendFromCode),
  ENTRY_POINT_INFO(pTestSuspendFromCode),
  ENTRY_POINT_INFO(pDeliverException),
  ENTRY_POINT_INFO(pThrowAbstractMethodErrorFromCode),
  ENTRY_POINT_INFO(pThrowArrayBoundsFromCode),
  ENTRY_POINT_INFO(pThrowDivZeroFromCode),
  ENTRY_POINT_INFO(pThrowNoSuchMethodFromCode),
  ENTRY_POINT_INFO(pThrowNullPointerFromCode),
  ENTRY_POINT_INFO(pThrowStackOverflowFromCode),
};
#undef ENTRY_POINT_INFO

void Thread::DumpThreadOffset(std::ostream& os, uint32_t offset, size_t size_of_pointers) {
  CHECK_EQ(size_of_pointers, 4U); // TODO: support 64-bit targets.

#define DO_THREAD_OFFSET(x) if (offset == static_cast<uint32_t>(OFFSETOF_VOLATILE_MEMBER(Thread, x))) { os << # x; return; }
  DO_THREAD_OFFSET(card_table_);
  DO_THREAD_OFFSET(exception_);
  DO_THREAD_OFFSET(jni_env_);
  DO_THREAD_OFFSET(self_);
  DO_THREAD_OFFSET(stack_end_);
  DO_THREAD_OFFSET(state_);
  DO_THREAD_OFFSET(suspend_count_);
  DO_THREAD_OFFSET(thin_lock_id_);
  //DO_THREAD_OFFSET(top_of_managed_stack_);
  //DO_THREAD_OFFSET(top_of_managed_stack_pc_);
  DO_THREAD_OFFSET(top_sirt_);
#undef DO_THREAD_OFFSET

  size_t entry_point_count = arraysize(gThreadEntryPointInfo);
  CHECK_EQ(entry_point_count * size_of_pointers, sizeof(EntryPoints));
  uint32_t expected_offset = OFFSETOF_MEMBER(Thread, entrypoints_);
  for (size_t i = 0; i < entry_point_count; ++i) {
    CHECK_EQ(gThreadEntryPointInfo[i].offset, expected_offset);
    expected_offset += size_of_pointers;
    if (gThreadEntryPointInfo[i].offset == offset) {
      os << gThreadEntryPointInfo[i].name;
      return;
    }
  }
  os << offset;
}

static const bool kDebugExceptionDelivery = false;
class CatchBlockStackVisitor : public StackVisitor {
 public:
  CatchBlockStackVisitor(Thread* self, Throwable* exception)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : StackVisitor(self->GetManagedStack(), self->GetTraceStack(), self->GetLongJumpContext()),
        self_(self), exception_(exception), to_find_(exception->GetClass()), throw_method_(NULL),
        throw_frame_id_(0), throw_dex_pc_(0), handler_quick_frame_(NULL),
        handler_quick_frame_pc_(0), handler_dex_pc_(0), native_method_count_(0),
        method_tracing_active_(Runtime::Current()->IsMethodTracingActive()) {
    // Exception not in root sets, can't allow GC.
    last_no_assert_suspension_cause_ = self->StartAssertNoThreadSuspension("Finding catch block");
  }

  ~CatchBlockStackVisitor() {
    LOG(FATAL) << "UNREACHABLE";  // Expected to take long jump.
  }

  bool VisitFrame() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    Method* method = GetMethod();
    if (method == NULL) {
      // This is the upcall, we remember the frame and last pc so that we may long jump to them.
      handler_quick_frame_pc_ = GetCurrentQuickFramePc();
      handler_quick_frame_ = GetCurrentQuickFrame();
      return false;  // End stack walk.
    }
    uint32_t dex_pc = DexFile::kDexNoIndex;
    if (method->IsRuntimeMethod()) {
      // ignore callee save method
      DCHECK(method->IsCalleeSaveMethod());
    } else {
      if (throw_method_ == NULL) {
        throw_method_ = method;
        throw_frame_id_ = GetFrameId();
        throw_dex_pc_ = GetDexPc();
      }
      if (method->IsNative()) {
        native_method_count_++;
      } else {
        // Unwind stack when an exception occurs during method tracing
        if (UNLIKELY(method_tracing_active_ && IsTraceExitPc(GetCurrentQuickFramePc()))) {
          uintptr_t pc = TraceMethodUnwindFromCode(Thread::Current());
          dex_pc = method->ToDexPC(pc);
        } else {
          dex_pc = GetDexPc();
        }
      }
    }
    if (dex_pc != DexFile::kDexNoIndex) {
      uint32_t found_dex_pc = method->FindCatchBlock(to_find_, dex_pc);
      if (found_dex_pc != DexFile::kDexNoIndex) {
        handler_dex_pc_ = found_dex_pc;
        handler_quick_frame_pc_ = method->ToNativePC(found_dex_pc);
        handler_quick_frame_ = GetCurrentQuickFrame();
        return false;  // End stack walk.
      }
    }
    return true;  // Continue stack walk.
  }

  void DoLongJump() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    Method* catch_method = *handler_quick_frame_;
    Dbg::PostException(self_, throw_frame_id_, throw_method_, throw_dex_pc_,
                       catch_method, handler_dex_pc_, exception_);
    if (kDebugExceptionDelivery) {
      if (catch_method == NULL) {
        LOG(INFO) << "Handler is upcall";
      } else {
        ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
        const DexFile& dex_file =
            class_linker->FindDexFile(catch_method->GetDeclaringClass()->GetDexCache());
        int line_number = dex_file.GetLineNumFromPC(catch_method, handler_dex_pc_);
        LOG(INFO) << "Handler: " << PrettyMethod(catch_method) << " (line: " << line_number << ")";
      }
    }
    self_->SetException(exception_);  // Exception back in root set.
    self_->EndAssertNoThreadSuspension(last_no_assert_suspension_cause_);
    // Place context back on thread so it will be available when we continue.
    self_->ReleaseLongJumpContext(context_);
    context_->SetSP(reinterpret_cast<uintptr_t>(handler_quick_frame_));
    CHECK_NE(handler_quick_frame_pc_, 0u);
    context_->SetPC(handler_quick_frame_pc_);
    context_->SmashCallerSaves();
    context_->DoLongJump();
  }

 private:
  Thread* self_;
  Throwable* exception_;
  // The type of the exception catch block to find.
  Class* to_find_;
  Method* throw_method_;
  JDWP::FrameId throw_frame_id_;
  uint32_t throw_dex_pc_;
  // Quick frame with found handler or last frame if no handler found.
  Method** handler_quick_frame_;
  // PC to branch to for the handler.
  uintptr_t handler_quick_frame_pc_;
  // Associated dex PC.
  uint32_t handler_dex_pc_;
  // Number of native methods passed in crawl (equates to number of SIRTs to pop)
  uint32_t native_method_count_;
  // Is method tracing active?
  const bool method_tracing_active_;
  // Support for nesting no thread suspension checks.
  const char* last_no_assert_suspension_cause_;
};

void Thread::DeliverException() {
  Throwable* exception = GetException();  // Get exception from thread
  CHECK(exception != NULL);
  // Don't leave exception visible while we try to find the handler, which may cause class
  // resolution.
  ClearException();
  if (kDebugExceptionDelivery) {
    String* msg = exception->GetDetailMessage();
    std::string str_msg(msg != NULL ? msg->ToModifiedUtf8() : "");
    DumpStack(LOG(INFO) << "Delivering exception: " << PrettyTypeOf(exception)
                        << ": " << str_msg << "\n");
  }
  CatchBlockStackVisitor catch_finder(this, exception);
  catch_finder.WalkStack(true);
  catch_finder.DoLongJump();
  LOG(FATAL) << "UNREACHABLE";
}

Context* Thread::GetLongJumpContext() {
  Context* result = long_jump_context_;
  if (result == NULL) {
    result = Context::Create();
  } else {
    long_jump_context_ = NULL;  // Avoid context being shared.
  }
  return result;
}

Method* Thread::GetCurrentMethod(uint32_t* dex_pc, size_t* frame_id) const {
  struct CurrentMethodVisitor : public StackVisitor {
    CurrentMethodVisitor(const ManagedStack* stack,
                         const std::vector<TraceStackFrame>* trace_stack)
        SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
        : StackVisitor(stack, trace_stack, NULL), method_(NULL), dex_pc_(0), frame_id_(0) {}

    virtual bool VisitFrame() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
      Method* m = GetMethod();
      if (m->IsRuntimeMethod()) {
        // Continue if this is a runtime method.
        return true;
      }
      method_ = m;
      dex_pc_ = GetDexPc();
      frame_id_ = GetFrameId();
      return false;
    }
    Method* method_;
    uint32_t dex_pc_;
    size_t frame_id_;
  };

  CurrentMethodVisitor visitor(GetManagedStack(), GetTraceStack());
  visitor.WalkStack(false);
  if (dex_pc != NULL) {
    *dex_pc = visitor.dex_pc_;
  }
  if (frame_id != NULL) {
    *frame_id = visitor.frame_id_;
  }
  return visitor.method_;
}

bool Thread::HoldsLock(Object* object) {
  if (object == NULL) {
    return false;
  }
  return object->GetThinLockId() == thin_lock_id_;
}

class ReferenceMapVisitor : public StackVisitor {
 public:
  ReferenceMapVisitor(const ManagedStack* stack, const std::vector<TraceStackFrame>* trace_stack,
                      Context* context, Heap::RootVisitor* root_visitor, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : StackVisitor(stack, trace_stack, context), root_visitor_(root_visitor), arg_(arg) {}

  bool VisitFrame() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    if (false) {
      LOG(INFO) << "Visiting stack roots in " << PrettyMethod(GetMethod())
          << StringPrintf("@ PC:%04x", GetDexPc());
    }
    ShadowFrame* shadow_frame = GetCurrentShadowFrame();
    if (shadow_frame != NULL) {
      shadow_frame->VisitRoots(root_visitor_, arg_);
    } else {
      Method* m = GetMethod();
      // Process register map (which native and runtime methods don't have)
      if (!m->IsNative() && !m->IsRuntimeMethod() && !m->IsProxyMethod()) {
        const uint8_t* gc_map = m->GetGcMap();
        CHECK(gc_map != NULL) << PrettyMethod(m);
        uint32_t gc_map_length = m->GetGcMapLength();
        CHECK_NE(0U, gc_map_length) << PrettyMethod(m);
        verifier::PcToReferenceMap map(gc_map, gc_map_length);
        const uint8_t* reg_bitmap = map.FindBitMap(GetDexPc());
        CHECK(reg_bitmap != NULL);
        const VmapTable vmap_table(m->GetVmapTableRaw());
        const DexFile::CodeItem* code_item = MethodHelper(m).GetCodeItem();
        DCHECK(code_item != NULL) << PrettyMethod(m); // Can't be NULL or how would we compile its instructions?
        uint32_t core_spills = m->GetCoreSpillMask();
        uint32_t fp_spills = m->GetFpSpillMask();
        size_t frame_size = m->GetFrameSizeInBytes();
        // For all dex registers in the bitmap
        size_t num_regs = std::min(map.RegWidth() * 8,
                                   static_cast<size_t>(code_item->registers_size_));
        Method** cur_quick_frame = GetCurrentQuickFrame();
        DCHECK(cur_quick_frame != NULL);
        for (size_t reg = 0; reg < num_regs; ++reg) {
          // Does this register hold a reference?
          if (TestBitmap(reg, reg_bitmap)) {
            uint32_t vmap_offset;
            Object* ref;
            if (vmap_table.IsInContext(reg, vmap_offset)) {
              // Compute the register we need to load from the context
              uint32_t spill_mask = core_spills;
              CHECK_LT(vmap_offset, static_cast<uint32_t>(__builtin_popcount(spill_mask)));
              uint32_t matches = 0;
              uint32_t spill_shifts = 0;
              while (matches != (vmap_offset + 1)) {
                DCHECK_NE(spill_mask, 0u);
                matches += spill_mask & 1;  // Add 1 if the low bit is set
                spill_mask >>= 1;
                spill_shifts++;
              }
              spill_shifts--;  // wind back one as we want the last match
              ref = reinterpret_cast<Object*>(GetGPR(spill_shifts));
            } else {
              ref = reinterpret_cast<Object*>(GetVReg(cur_quick_frame, code_item, core_spills,
                                                      fp_spills, frame_size, reg));
            }
            if (ref != NULL) {
              root_visitor_(ref, arg_);
            }
          }
        }
      }
    }
    return true;
  }

 private:
  bool TestBitmap(int reg, const uint8_t* reg_vector) {
    return ((reg_vector[reg / 8] >> (reg % 8)) & 0x01) != 0;
  }

  // Call-back when we visit a root
  Heap::RootVisitor* root_visitor_;
  // Argument to call-back
  void* arg_;
};

void Thread::VisitRoots(Heap::RootVisitor* visitor, void* arg) {
  if (exception_ != NULL) {
    visitor(exception_, arg);
  }
  if (peer_ != NULL) {
    visitor(peer_, arg);
  }
  if (class_loader_override_ != NULL) {
    visitor(class_loader_override_, arg);
  }
  jni_env_->locals.VisitRoots(visitor, arg);
  jni_env_->monitors.VisitRoots(visitor, arg);

  SirtVisitRoots(visitor, arg);

  // Visit roots on this thread's stack
  Context* context = GetLongJumpContext();
  ReferenceMapVisitor mapper(GetManagedStack(), GetTraceStack(), context, visitor, arg);
  mapper.WalkStack();
  ReleaseLongJumpContext(context);
}

#if VERIFY_OBJECT_ENABLED
static void VerifyObject(const Object* obj, void* arg) {
  Heap* heap = reinterpret_cast<Heap*>(arg);
  heap->VerifyObject(obj);
}

void Thread::VerifyStack() {
  UniquePtr<Context> context(Context::Create());
  ReferenceMapVisitor mapper(GetManagedStack(), GetTraceStack(), context.get(), VerifyObject,
                             Runtime::Current()->GetHeap());
  mapper.WalkStack();
}
#endif

// Set the stack end to that to be used during a stack overflow
void Thread::SetStackEndForStackOverflow() {
  // During stack overflow we allow use of the full stack
  if (stack_end_ == stack_begin_) {
    DumpStack(std::cerr);
    LOG(FATAL) << "Need to increase kStackOverflowReservedBytes (currently "
               << kStackOverflowReservedBytes << ")";
  }

  stack_end_ = stack_begin_;
}

std::ostream& operator<<(std::ostream& os, const Thread& thread) {
  thread.ShortDump(os);
  return os;
}

#ifndef NDEBUG
void Thread::AssertThreadSuspensionIsAllowable(bool check_locks) const {
  CHECK_EQ(0u, no_thread_suspension_) << last_no_thread_suspension_cause_;
  if (check_locks) {
    bool bad_mutexes_held = false;
    for (int i = kMaxMutexLevel; i >= 0; --i) {
      // We expect no locks except the mutator_lock_.
      if (i != kMutatorLock) {
        BaseMutex* held_mutex = GetHeldMutex(static_cast<MutexLevel>(i));
        if (held_mutex != NULL) {
          LOG(ERROR) << "holding \"" << held_mutex->GetName()
                  << "\" at point where thread suspension is expected";
          bad_mutexes_held = true;
        }
      }
    }
    CHECK(!bad_mutexes_held);
  }
}
#endif

}  // namespace art
