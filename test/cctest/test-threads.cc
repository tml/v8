// Copyright 2008 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "v8.h"

#include "platform.h"
#include "isolate.h"

#include "cctest.h"


TEST(Preemption) {
  v8::Locker locker;
  v8::V8::Initialize();
  v8::HandleScope scope;
  v8::Context::Scope context_scope(v8::Context::New());

  v8::Locker::StartPreemption(100);

  v8::Handle<v8::Script> script = v8::Script::Compile(
      v8::String::New("var count = 0; var obj = new Object(); count++;\n"));

  script->Run();

  v8::Locker::StopPreemption();
  v8::internal::OS::Sleep(500);  // Make sure the timer fires.

  script->Run();
}


enum Turn {
  FILL_CACHE,
  CLEAN_CACHE,
  SECOND_TIME_FILL_CACHE,
  DONE
};

static Turn turn = FILL_CACHE;


class ThreadA: public v8::internal::Thread {
 public:
  explicit ThreadA(i::Isolate* isolate) : Thread(isolate, "ThreadA") { }
  void Run() {
    v8::Locker locker;
    v8::HandleScope scope;
    v8::Context::Scope context_scope(v8::Context::New());

    CHECK_EQ(FILL_CACHE, turn);

    // Fill String.search cache.
    v8::Handle<v8::Script> script = v8::Script::Compile(
        v8::String::New(
          "for (var i = 0; i < 3; i++) {"
          "  var result = \"a\".search(\"a\");"
          "  if (result != 0) throw \"result: \" + result + \" @\" + i;"
          "};"
          "true"));
    CHECK(script->Run()->IsTrue());

    turn = CLEAN_CACHE;
    do {
      {
        v8::Unlocker unlocker;
        Thread::YieldCPU();
      }
    } while (turn != SECOND_TIME_FILL_CACHE);

    // Rerun the script.
    CHECK(script->Run()->IsTrue());

    turn = DONE;
  }
};


class ThreadB: public v8::internal::Thread {
 public:
  explicit ThreadB(i::Isolate* isolate) : Thread(isolate, "ThreadB") { }
  void Run() {
    do {
      {
        v8::Locker locker;
        if (turn == CLEAN_CACHE) {
          v8::HandleScope scope;
          v8::Context::Scope context_scope(v8::Context::New());

          // Clear the caches by forcing major GC.
          HEAP->CollectAllGarbage(false);
          turn = SECOND_TIME_FILL_CACHE;
          break;
        }
      }

      Thread::YieldCPU();
    } while (true);
  }
};


TEST(JSFunctionResultCachesInTwoThreads) {
  v8::V8::Initialize();

  ThreadA threadA(i::Isolate::Current());
  ThreadB threadB(i::Isolate::Current());

  threadA.Start();
  threadB.Start();

  threadA.Join();
  threadB.Join();

  CHECK_EQ(DONE, turn);
}

class ThreadIdValidationThread : public v8::internal::Thread {
 public:
  ThreadIdValidationThread(i::Thread* thread_to_start,
                           i::List<i::ThreadId>* refs,
                           unsigned int thread_no,
                           i::Semaphore* semaphore)
    : Thread(NULL, "ThreadRefValidationThread"),
      refs_(refs), thread_no_(thread_no), thread_to_start_(thread_to_start),
      semaphore_(semaphore) {
  }

  void Run() {
    i::ThreadId thread_id = i::ThreadId::Current();
    for (int i = 0; i < thread_no_; i++) {
      CHECK(!(*refs_)[i].Equals(thread_id));
    }
    CHECK(thread_id.IsValid());
    (*refs_)[thread_no_] = thread_id;
    if (thread_to_start_ != NULL) {
      thread_to_start_->Start();
    }
    semaphore_->Signal();
  }
 private:
  i::List<i::ThreadId>* refs_;
  int thread_no_;
  i::Thread* thread_to_start_;
  i::Semaphore* semaphore_;
};

TEST(ThreadIdValidation) {
  const int kNThreads = 100;
  i::List<ThreadIdValidationThread*> threads(kNThreads);
  i::List<i::ThreadId> refs(kNThreads);
  i::Semaphore* semaphore = i::OS::CreateSemaphore(0);
  ThreadIdValidationThread* prev = NULL;
  for (int i = kNThreads - 1; i >= 0; i--) {
    ThreadIdValidationThread* newThread =
        new ThreadIdValidationThread(prev, &refs, i, semaphore);
    threads.Add(newThread);
    prev = newThread;
    refs.Add(i::ThreadId::Invalid());
  }
  prev->Start();
  for (int i = 0; i < kNThreads; i++) {
    semaphore->Wait();
  }
  for (int i = 0; i < kNThreads; i++) {
    delete threads[i];
  }
}
