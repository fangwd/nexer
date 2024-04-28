/* Copyright (c) Weidong Fang. All rights reserved. */

#ifndef NEXER_FUNCTION_LIST_H_
#define NEXER_FUNCTION_LIST_H_

#include <functional>
#include <list>

namespace nexer {

template <typename T, typename... U>
class FunctionList {
    std::list<std::function<T(U...)>> fn_list_;

  public:
    typedef std::function<void()> Remove;

    Remove Add(std::function<T(U...)> fn) {
        fn_list_.push_back(fn);
        auto it = std::prev(fn_list_.end());
        return [this, it] { fn_list_.erase(it); };
    }

    void Invoke(U... args) {
        for (auto& fn : fn_list_) {
            fn(args...);
        }
    }
};

}  // namespace nexer

#endif  // NEXER_FUNCTION_LIST_H_
