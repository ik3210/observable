// Observable Library
// Copyright (c) 2016 David Capello
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#include "obs/signal.h"
#include "test.h"

#include <vector>

using namespace obs;

class A {
  int m_code;

public:
  A(int code) : m_code(code) {
    EXPECT_TRUE(m_code >= 0);
  }

  ~A() {
    EXPECT_TRUE(m_code >= 0);
    m_code = -1;
  }

  void on_signal(int value) {
    EXPECT_TRUE(m_code >= 0);
  }
};

int main() {
  const int N = 1;
  signal<void(int)> signal;
  std::vector<std::thread> threads;

  A b(1);
  signal.connect(&A::on_signal, &b);

  for (int i=0; i<500*N; ++i) {
    // Emit the signal
    if ((i%2) == 0) {
      threads.push_back(
        std::thread(
          [&](){
            for (int c=100*N; c>0; --c) {
              signal(c);
            }
          }));
    }
    // Observe the signal
    else {
      threads.push_back(
        std::thread(
          [&](){
            A a(i);
            scoped_connection conn = signal.connect(&A::on_signal, &a);
            for (int c=10*N; c>0; --c) {
              signal(i);
            }
          }));
    }

    signal(1000+i);
  }

  for (auto& thread : threads)
    thread.join();
}
