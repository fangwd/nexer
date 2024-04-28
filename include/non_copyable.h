/* Copyright (c) Weidong Fang. All rights reserved. */

#ifndef NEXER_NON_COPYABLE_H_
#define NEXER_NON_COPYABLE_H_

class NonCopyable {
  private:
    NonCopyable(const NonCopyable& other);
    NonCopyable& operator=(const NonCopyable& other);
  public:
    NonCopyable()=default;

};

#endif  // NEXER_NON_COPYABLE_H_
