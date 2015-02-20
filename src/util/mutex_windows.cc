//
// The MIT License (MIT)
//
// Copyright 2013-2014 The MilkCat Project Developers
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// mutex_windows.cc --- Created at 2015-02-18
//

#include "util/mutex.h"

#include <windows.h>
#include "util/util.h"

namespace milkcat {

class Mutex::MutexImpl {
 public:
  MutexImpl() {
    mutex_ = CreateMutex(NULL, FALSE, NULL);
  }

  ~MutexImpl() {
    CloseHandle(mutex_);
  }

  void Lock() {
    DWORD wait_result = WaitForSingleObject(mutex_, INFINITE);
    MC_ASSERT(wait_result == WAIT_OBJECT_0, "wait for mutex error");
  }

  void Unlock() {
    MC_ASSERT(ReleaseMutex(mutex_), "release mutex error");
  }

 private:
  HANDLE mutex_;
};

Mutex::Mutex(): impl_(new MutexImpl()) {}
Mutex::~Mutex() {
  delete impl_;
  impl_ = NULL;
}

void Mutex::Lock() {
  impl_->Lock();
}

void Mutex::Unlock() {
  impl_->Unlock();
}

}  // namespace milkcat
