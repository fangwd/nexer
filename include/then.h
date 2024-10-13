#ifndef NEXER_THEN_H_
#define NEXER_THEN_H_

#include <functional>

namespace nexer {

template <class T>
class Then {
  private:
    std::function<void(T)> fn_;

  public:
    Then(std::function<void(T)>&& fn) : fn_{std::forward<std::function<void(T)>>(fn)} {}
    void operator()(T result) const {
        fn_(result);
    }
};

}  // namespace nexer

#endif // NEXER_THEN_H_