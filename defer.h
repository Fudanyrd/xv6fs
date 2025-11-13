#ifndef _DEFER_H_
#define _DEFER_H_ 1

#define _MY_CONCAT(a, b) a##b
#define CONCAT(a, b) _MY_CONCAT(a, b)

#define defer(statement)                                                       \
  auto CONCAT(__defer_wrapper_, __LINE__) = [&](void) { statement; };          \
  class CONCAT(__defer, __LINE__) {                                            \
  public:                                                                      \
    typedef typeof(CONCAT(__defer_wrapper_, __LINE__)) _Tp;                    \
    CONCAT(__defer, __LINE__)(_Tp & fn) { this->fn_ = &fn; }                   \
    ~CONCAT(__defer, __LINE__)(void) { (*fn_)(); }                             \
                                                                               \
  private:                                                                     \
    _Tp *fn_;                                                                  \
  };                                                                           \
  CONCAT(__defer, __LINE__)                                                    \
  CONCAT(__defer_instance, __LINE__)(CONCAT(__defer_wrapper_, __LINE__))

#if __cplusplus >= 201700L

#undef defer

template <typename _Tp> class __defer_call {
public:
  __defer_call(_Tp &callable) : _M_fn_(&callable) {}

  ~__defer_call() { (*_M_fn_)(); }

  __defer_call(const __defer_call &other) = delete;
  __defer_call &operator=(const __defer_call &other) = delete;

private:
  _Tp *_M_fn_;
};

#define defer(statement)                                                       \
  auto CONCAT(__defer_wrapper_, __LINE__) = [&](void) { statement; };          \
  auto CONCAT(__defer_instance, __LINE__) =                                    \
      __defer_call(CONCAT(__defer_wrapper_, __LINE__))

#endif

#endif /* _DEFER_H_ 1 */
