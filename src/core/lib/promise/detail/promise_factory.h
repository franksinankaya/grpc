// Copyright 2021 gRPC authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef GRPC_CORE_LIB_PROMISE_DETAIL_PROMISE_FACTORY_H
#define GRPC_CORE_LIB_PROMISE_DETAIL_PROMISE_FACTORY_H

#include <grpc/impl/codegen/port_platform.h>

#include "src/core/lib/promise/detail/promise_like.h"
#include "src/core/lib/promise/poll.h"

#include "absl/meta/type_traits.h"

// PromiseFactory is an adaptor class.
//
// Where a Promise is a thing that's polled periodically, a PromiseFactory
// creates a Promise. Within this Promise/Activity framework, PromiseFactory's
// then provide the edges for computation -- invoked at state transition
// boundaries to provide the new steady state.
//
// A PromiseFactory formally is f(A) -> Promise<T> for some types A & T.
// This get a bit awkward and inapproprate to write however, and so the type
// contained herein can adapt various kinds of callable into the correct form.
// Of course a callable of a single argument returning a Promise will see an
// identity translation. One taking no arguments and returning a Promise
// similarly.
//
// A Promise passed to a PromiseFactory will yield a PromiseFactory that
// returns just that Promise.
//
// Generalizing slightly, a callable taking a single argument A and returning a
// Poll<T> will yield a PromiseFactory that captures it's argument A and
// returns a Poll<T>.
//
// Since various consumers of PromiseFactory run either repeatedly through an
// overarching Promises lifetime, or just once, and we can optimize just once
// by moving the contents of the PromiseFactory, two factory methods are
// provided: Once, that can be called just once, and Repeated, that can (wait
// for it) be called Repeatedly.

namespace grpc_core {
namespace promise_detail {

// Helper trait: given a T, and T x, is calling x() legal?
template <typename T, typename Ignored = void>
struct IsVoidCallableT {
  static constexpr bool value = false;
};
template <typename F>
struct IsVoidCallableT<F, absl::void_t<decltype(std::declval<F>()())>> {
  static constexpr bool value = true;
};

// Wrap that trait in some nice syntax.
template <typename T>
constexpr bool IsVoidCallable() {
  return IsVoidCallableT<T>::value;
}

// T -> T, const T& -> T
template <typename T>
using RemoveCVRef = absl::remove_cv_t<absl::remove_reference_t<T>>;

// Given F(A,B,C,...), what's the return type?
template <typename T, typename Ignored = void>
struct ResultOfTInner;
template <typename F, typename... Args>
struct ResultOfTInner<F(Args...), absl::void_t<decltype(std::declval<F>()(
                                      std::declval<Args>()...))>> {
  using T = decltype(std::declval<F>()(std::declval<Args>()...));
};

template <typename T>
struct ResultOfT;
template <typename F, typename... Args>
struct ResultOfT<F(Args...)> {
  using FP = RemoveCVRef<F>;
  using T = typename ResultOfTInner<FP(Args...)>::T;
};

template <typename T>
using ResultOf = typename ResultOfT<T>::T;

// Captures the promise functor and the argument passed.
// Provides the interface of a promise.
template <typename F, typename Arg>
class Curried {
 public:
  Curried(F&& f, Arg&& arg)
      : f_(std::forward<F>(f)), arg_(std::forward<Arg>(arg)) {}
  using Result = decltype(std::declval<F>()(std::declval<Arg>()));
  Result operator()() { return f_(arg_); }

 private:
  GPR_NO_UNIQUE_ADDRESS F f_;
  GPR_NO_UNIQUE_ADDRESS Arg arg_;
};

// Promote a callable(A) -> T | Poll<T> to a PromiseFactory(A) -> Promise<T> by
// capturing A.
template <typename A, typename F>
absl::enable_if_t<!IsVoidCallable<ResultOf<F(A)>>(), PromiseLike<Curried<A, F>>>
PromiseFactoryImpl(F&& f, A&& arg) {
  return Curried<A, F>(std::forward<F>(f), std::forward<A>(arg));
}

// Promote a callable() -> T|Poll<T> to a PromiseFactory(A) -> Promise<T>
// by dropping the argument passed to the factory.
template <typename A, typename F>
absl::enable_if_t<!IsVoidCallable<ResultOf<F()>>(), PromiseLike<RemoveCVRef<F>>>
PromiseFactoryImpl(F f, A&&) {
  return PromiseLike<F>(std::move(f));
}

// Promote a callable() -> T|Poll<T> to a PromiseFactory() -> Promise<T>
template <typename F>
absl::enable_if_t<!IsVoidCallable<ResultOf<F()>>(), PromiseLike<RemoveCVRef<F>>>
PromiseFactoryImpl(F f) {
  return PromiseLike<F>(std::move(f));
}

// Given a callable(A) -> Promise<T>, name it a PromiseFactory and use it.
template <typename A, typename F>
absl::enable_if_t<IsVoidCallable<ResultOf<F(A)>>(),
                  PromiseLike<decltype(std::declval<F>()(std::declval<A>()))>>
PromiseFactoryImpl(F&& f, A&& arg) {
  return f(std::forward<A>(arg));
}

// Given a callable() -> Promise<T>, promote it to a
// PromiseFactory(A) -> Promise<T> by dropping the first argument.
template <typename A, typename F>
absl::enable_if_t<IsVoidCallable<ResultOf<F()>>(),
                  PromiseLike<decltype(std::declval<F>()())>>
PromiseFactoryImpl(F&& f, A&&) {
  return f();
}

// Given a callable() -> Promise<T>, name it a PromiseFactory and use it.
template <typename F>
absl::enable_if_t<IsVoidCallable<ResultOf<F()>>(),
                  PromiseLike<decltype(std::declval<F>()())>>
PromiseFactoryImpl(F&& f) {
  return f();
};

template <typename A, typename F>
class PromiseFactory {
 private:
  GPR_NO_UNIQUE_ADDRESS F f_;

 public:
  using Arg = A;
  using Promise =
      decltype(PromiseFactoryImpl(std::move(f_), std::declval<A>()));

  explicit PromiseFactory(F f) : f_(std::move(f)) {}

  Promise Once(Arg&& a) {
    return PromiseFactoryImpl(std::move(f_), std::forward<Arg>(a));
  }

  Promise Repeated(Arg&& a) const {
    return PromiseFactoryImpl(f_, std::forward<Arg>(a));
  }
};

template <typename F>
class PromiseFactory<void, F> {
 private:
  GPR_NO_UNIQUE_ADDRESS F f_;

 public:
  using Arg = void;
  using Promise = decltype(PromiseFactoryImpl(std::move(f_)));

  explicit PromiseFactory(F f) : f_(std::move(f)) {}

  Promise Once() { return PromiseFactoryImpl(std::move(f_)); }

  Promise Repeated() const { return PromiseFactoryImpl(f_); }
};

}  // namespace promise_detail
}  // namespace grpc_core

#endif  // GRPC_CORE_LIB_PROMISE_DETAIL_PROMISE_FACTORY_H
