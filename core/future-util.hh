/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */


/** @file */

#ifndef CORE_FUTURE_UTIL_HH_
#define CORE_FUTURE_UTIL_HH_

#include "task.hh"
#include "future.hh"
#include "shared_ptr.hh"
#include "do_with.hh"
#include <tuple>
#include <iterator>
#include <vector>
#include <experimental/optional>

/// \cond internal
extern __thread size_t task_quota;
/// \endcond


/// \addtogroup future-util
/// @{

/// \cond internal

struct parallel_for_each_state {
    // use optional<> to avoid out-of-line constructor
    std::experimental::optional<std::exception_ptr> ex;
    size_t waiting = 0;
    promise<> pr;
    void complete() {
        if (--waiting == 0) {
            if (ex) {
                pr.set_exception(std::move(*ex));
            } else {
                pr.set_value();
            }
        }
    }
};

/// \endcond

/// Run tasks in parallel (iterator version).
///
/// Given a range [\c begin, \c end) of objects, run \c func on each \c *i in
/// the range, and return a future<> that resolves when all the functions
/// complete.  \c func should return a future<> that indicates when it is
/// complete.  All invocations are performed in parallel.
///
/// \param begin an \c InputIterator designating the beginning of the range
/// \param end an \c InputIterator designating the end of the range
/// \param func Function to apply to each element in the range (returning
///             a \c future<>)
/// \return a \c future<> that resolves when all the function invocations
///         complete.  If one or more return an exception, the return value
///         contains one of the exceptions.
template <typename Iterator, typename Func>
inline
future<>
parallel_for_each(Iterator begin, Iterator end, Func&& func) {
    return do_with(parallel_for_each_state(), [&] (parallel_for_each_state& state) -> future<> {
        // increase ref count to ensure all functions run
        ++state.waiting;
        while (begin != end) {
            ++state.waiting;
            func(*begin++).then_wrapped([&] (future<> f) {
                try {
                    f.get();
                } catch (...) {
                    // We can only store one exception.  For more, use when_all().
                    if (!state.ex) {
                        state.ex = std::move(std::current_exception());
                    }
                }
                state.complete();
            });
        }
        // match increment on top
        state.complete();
        return state.pr.get_future();
    });
}

/// Run tasks in parallel (range version).
///
/// Given a \c range of objects, apply \c func to each object
/// in the range, and return a future<> that resolves when all
/// the functions complete.  \c func should return a future<> that indicates
/// when it is complete.  All invocations are performed in parallel.
///
/// \param range A range of objects to iterate run \c func on
/// \param func  A callable, accepting reference to the range's
///              \c value_type, and returning a \c future<>.
/// \return a \c future<> that becomes ready when the entire range
///         was processed.  If one or more of the invocations of
///         \c func returned an exceptional future, then the return
///         value will contain one of those exceptions.
template <typename Range, typename Func>
inline
future<>
parallel_for_each(Range&& range, Func&& func) {
    return parallel_for_each(std::begin(range), std::end(range),
            std::forward<Func>(func));
}

// The AsyncAction concept represents an action which can complete later than
// the actual function invocation. It is represented by a function which
// returns a future which resolves when the action is done.

/// \cond internal
template<typename AsyncAction, typename StopCondition>
static inline
void do_until_continued(StopCondition&& stop_cond, AsyncAction&& action, promise<> p) {
    while (!stop_cond()) {
        try {
            auto&& f = action();
            if (!f.available()) {
                f.then_wrapped([action = std::forward<AsyncAction>(action),
                    stop_cond = std::forward<StopCondition>(stop_cond), p = std::move(p)](std::result_of_t<AsyncAction()> fut) mutable {
                    try {
                        fut.get();
                        do_until_continued(stop_cond, std::forward<AsyncAction>(action), std::move(p));
                    } catch(...) {
                        p.set_exception(std::current_exception());
                    }
                });
                return;
            }

            if (f.failed()) {
                f.forward_to(std::move(p));
                return;
            }
        } catch (...) {
            p.set_exception(std::current_exception());
            return;
        }
    }

    p.set_value();
}
/// \endcond

enum class stop_iteration { no, yes };

/// Invokes given action until it fails or the function requests iteration to stop by returning
/// \c stop_iteration::yes.
///
/// \param action a callable taking no arguments, returning a future<stop_iteration>.  Will
///               be called again as soon as the future resolves, unless the
///               future fails, action throws, or it resolves with \c stop_iteration::yes.
///               If \c action is an r-value it can be moved in the middle of iteration.
/// \return a ready future if we stopped successfully, or a failed future if
///         a call to to \c action failed.
template<typename AsyncAction>
static inline
future<> repeat(AsyncAction&& action) {
    using futurator = futurize<std::result_of_t<AsyncAction()>>;
    static_assert(std::is_same<future<stop_iteration>, typename futurator::type>::value, "bad AsyncAction signature");

    while (task_quota--) {
        try {
            auto f = futurator::apply(action);

            if (!f.available()) {
                return f.then([action = std::forward<AsyncAction>(action)] (stop_iteration stop) mutable {
                    if (stop == stop_iteration::yes) {
                        return make_ready_future<>();
                    } else {
                        return repeat(std::forward<AsyncAction>(action));
                    }
                });
            }

            if (f.get0() == stop_iteration::yes) {
                return make_ready_future<>();
            }
        } catch (...) {
            return make_exception_future<>(std::current_exception());
        }
    }

    promise<> p;
    auto f = p.get_future();
    schedule(make_task([action = std::forward<AsyncAction>(action), p = std::move(p)] () mutable {
        repeat(std::forward<AsyncAction>(action)).forward_to(std::move(p));
    }));
    return f;
}

/// Invokes given action until it fails or given condition evaluates to true.
///
/// \param stop_cond a callable taking no arguments, returning a boolean that
///                  evalutes to true when you don't want to call \c action
///                  any longer
/// \param action a callable taking no arguments, returning a future<>.  Will
///               be called again as soon as the future resolves, unless the
///               future fails, or \c stop_cond returns \c true.
/// \return a ready future if we stopped successfully, or a failed future if
///         a call to to \c action failed.
template<typename AsyncAction, typename StopCondition>
static inline
future<> do_until(StopCondition&& stop_cond, AsyncAction&& action) {
    promise<> p;
    auto f = p.get_future();
    do_until_continued(std::forward<StopCondition>(stop_cond),
        std::forward<AsyncAction>(action), std::move(p));
    return f;
}

/// Invoke given action until it fails.
///
/// Calls \c action repeatedly until it returns a failed future.
///
/// \param action a callable taking no arguments, returning a \c future<>
///        that becomes ready when you wish it to be called again.
/// \return a future<> that will resolve to the first failure of \c action
template<typename AsyncAction>
static inline
future<> keep_doing(AsyncAction&& action) {
    return repeat([action = std::forward<AsyncAction>(action)] () mutable {
        return action().then([] {
            return stop_iteration::no;
        });
    });
}

/// Call a function for each item in a range, sequentially (iterator version).
///
/// For each item in a range, call a function, waiting for the previous
/// invocation to complete before calling the next one.
///
/// \param begin an \c InputIterator designating the beginning of the range
/// \param end an \c InputIterator designating the endof the range
/// \param action a callable, taking a reference to objects from the range
///               as a parameter, and returning a \c future<> that resolves
///               when it is acceptable to process the next item.
/// \return a ready future on success, or the first failed future if
///         \c action failed.
template<typename Iterator, typename AsyncAction>
static inline
future<> do_for_each(Iterator begin, Iterator end, AsyncAction&& action) {
    if (begin == end) {
        return make_ready_future<>();
    }
    while (true) {
        auto f = action(*begin++);
        if (begin == end) {
            return f;
        }
        if (!f.available()) {
            return std::move(f).then([action = std::forward<AsyncAction>(action),
                    begin = std::move(begin), end = std::move(end)] () mutable {
                return do_for_each(std::move(begin), std::move(end), std::forward<AsyncAction>(action));
            });
        }
        if (f.failed()) {
            return std::move(f);
        }
    }
}

/// Call a function for each item in a range, sequentially (range version).
///
/// For each item in a range, call a function, waiting for the previous
/// invocation to complete before calling the next one.
///
/// \param range an \c Range object designating input values
/// \param action a callable, taking a reference to objects from the range
///               as a parameter, and returning a \c future<> that resolves
///               when it is acceptable to process the next item.
/// \return a ready future on success, or the first failed future if
///         \c action failed.
template<typename Container, typename AsyncAction>
static inline
future<> do_for_each(Container& c, AsyncAction&& action) {
    return do_for_each(std::begin(c), std::end(c), std::forward<AsyncAction>(action));
}

/// \cond internal
inline
future<std::tuple<>>
when_all() {
    return make_ready_future<std::tuple<>>();
}

// gcc can't capture a parameter pack, so we need to capture
// a tuple and use apply.  But apply cannot accept an overloaded
// function pointer as its first parameter, so provide this instead.
struct do_when_all {
    template <typename... Future>
    future<std::tuple<Future...>> operator()(Future&&... fut) const {
        return when_all(std::move(fut)...);
    }
};
/// \endcond

/// Wait for many futures to complete, capturing possible errors (variadic version).
///
/// Given a variable number of futures as input, wait for all of them
/// to resolve (either successfully or with an exception), and return
/// them as a tuple so individual values or exceptions can be examined.
///
/// \param fut the first future to wait for
/// \param rest more futures to wait for
/// \return an \c std::tuple<> of all the futures in the input; when
///         ready, all contained futures will be ready as well.
template <typename... FutureArgs, typename... Rest>
inline
future<std::tuple<future<FutureArgs...>, Rest...>>
when_all(future<FutureArgs...>&& fut, Rest&&... rest) {
    using Future = future<FutureArgs...>;
    return fut.then_wrapped(
            [rest = std::make_tuple(std::move(rest)...)] (Future&& fut) mutable {
        return apply(do_when_all(), std::move(rest)).then_wrapped(
                [fut = std::move(fut)] (future<std::tuple<Rest...>>&& rest) mutable {
            return make_ready_future<std::tuple<Future, Rest...>>(
                    std::tuple_cat(std::make_tuple(std::move(fut)), std::get<0>(rest.get())));
        });
    });
}

/// \cond internal
template <typename Iterator, typename IteratorCategory>
inline
size_t
when_all_estimate_vector_capacity(Iterator begin, Iterator end, IteratorCategory category) {
    // For InputIterators we can't estimate needed capacity
    return 0;
}

template <typename Iterator>
inline
size_t
when_all_estimate_vector_capacity(Iterator begin, Iterator end, std::forward_iterator_tag category) {
    // May be linear time below random_access_iterator_tag, but still better than reallocation
    return std::distance(begin, end);
}

// Internal function for when_all().
template <typename Future>
inline
future<std::vector<Future>>
complete_when_all(std::vector<Future>&& futures, typename std::vector<Future>::iterator pos) {
    // If any futures are already ready, skip them.
    while (pos != futures.end() && pos->available()) {
        ++pos;
    }
    // Done?
    if (pos == futures.end()) {
        return make_ready_future<std::vector<Future>>(std::move(futures));
    }
    // Wait for unready future, store, and continue.
    return pos->then_wrapped([futures = std::move(futures), pos] (auto fut) mutable {
        *pos++ = std::move(fut);
        return complete_when_all(std::move(futures), pos);
    });
}
/// \endcond

/// Wait for many futures to complete, capturing possible errors (iterator version).
///
/// Given a range of futures as input, wait for all of them
/// to resolve (either successfully or with an exception), and return
/// them as a \c std::vector so individual values or exceptions can be examined.
///
/// \param begin an \c InputIterator designating the beginning of the range of futures
/// \param end an \c InputIterator designating the end of the range of futures
/// \return an \c std::vector<> of all the futures in the input; when
///         ready, all contained futures will be ready as well.
template <typename FutureIterator>
inline
future<std::vector<typename std::iterator_traits<FutureIterator>::value_type>>
when_all(FutureIterator begin, FutureIterator end) {
    using itraits = std::iterator_traits<FutureIterator>;
    std::vector<typename itraits::value_type> ret;
    ret.reserve(when_all_estimate_vector_capacity(begin, end, typename itraits::iterator_category()));
    // Important to invoke the *begin here, in case it's a function iterator,
    // so we launch all computation in parallel.
    std::move(begin, end, std::back_inserter(ret));
    return complete_when_all(std::move(ret), ret.begin());
}

template <typename T>
struct reducer_with_get_traits {
    using result_type = decltype(std::declval<T>().get());
    using future_type = future<result_type>;
    static future_type maybe_call_get(future<> f, lw_shared_ptr<T> r) {
        return f.then([r = std::move(r)] () mutable {
            return make_ready_future<result_type>(std::move(*r).get());
        });
    }
};

template <typename T, typename V = void>
struct reducer_traits {
    using future_type = future<>;
    static future_type maybe_call_get(future<> f, lw_shared_ptr<T> r) {
        return f.then([r = std::move(r)] {});
    }
};

template <typename T>
struct reducer_traits<T, decltype(std::declval<T>().get(), void())> : public reducer_with_get_traits<T> {};

// @Mapper is a callable which transforms values from the iterator range
// into a future<T>. @Reducer is an object which can be called with T as
// parameter and yields a future<>. It may have a get() method which returns
// a value of type U which holds the result of reduction. This value is wrapped
// in a future and returned by this function. If the reducer has no get() method
// then this function returns future<>.
//
// TODO: specialize for non-deferring reducer
template <typename Iterator, typename Mapper, typename Reducer>
inline
auto
map_reduce(Iterator begin, Iterator end, Mapper&& mapper, Reducer&& r)
    -> typename reducer_traits<Reducer>::future_type
{
    auto r_ptr = make_lw_shared(std::forward<Reducer>(r));
    future<> ret = make_ready_future<>();
    while (begin != end) {
        ret = mapper(*begin++).then([ret = std::move(ret), r_ptr] (auto value) mutable {
            return ret.then([value = std::move(value), r_ptr] () mutable {
                return (*r_ptr)(std::move(value));
            });
        });
    }
    return reducer_traits<Reducer>::maybe_call_get(std::move(ret), r_ptr);
}

/// Asynchronous map/reduce transformation.
///
/// Given a range of objects, an asynchronous unary function
/// operating on these objects, an initial value, and a
/// binary function for reducing, map_reduce() will
/// transform each object in the range, then apply
/// the the reducing function to the result.
///
/// Example:
///
/// Calculate the total size of several files:
///
/// \code
///  map_reduce(files.begin(), files.end(),
///             std::mem_fn(file::size),
///             size_t(0),
///             std::plus<size_t>())
/// \endcode
///
/// Requirements:
///    - Iterator: an InputIterator.
///    - Mapper: unary function taking Iterator::value_type and producing a future<...>.
///    - Initial: any value type
///    - Reduce: a binary function taking two Initial values and returning an Initial
///
/// Return type:
///    - future<Initial>
///
/// \param begin beginning of object range to operate on
/// \param end end of object range to operate on
/// \param mapper map function to call on each object, returning a future
/// \param initial initial input value to reduce function
/// \param reduce binary function for merging two result values from \c mapper
///
/// \return equivalent to \c reduce(reduce(initial, mapper(obj0)), mapper(obj1)) ...
template <typename Iterator, typename Mapper, typename Initial, typename Reduce>
inline
future<Initial>
map_reduce(Iterator begin, Iterator end, Mapper&& mapper, Initial initial, Reduce reduce) {
    struct state {
        Initial result;
        Reduce reduce;
    };
    auto s = make_lw_shared(state{std::move(initial), std::move(reduce)});
    future<> ret = make_ready_future<>();
    while (begin != end) {
        ret = mapper(*begin++).then([s = s.get(), ret = std::move(ret)] (auto&& value) mutable {
            s->result = s->reduce(std::move(s->result), std::move(value));
            return std::move(ret);
        });
    }
    return ret.then([s] {
        return make_ready_future<Initial>(std::move(s->result));
    });
}

/// Asynchronous map/reduce transformation (range version).
///
/// Given a range of objects, an asynchronous unary function
/// operating on these objects, an initial value, and a
/// binary function for reducing, map_reduce() will
/// transform each object in the range, then apply
/// the the reducing function to the result.
///
/// Example:
///
/// Calculate the total size of several files:
///
/// \code
///  std::vector<file> files = ...;
///  map_reduce(files,
///             std::mem_fn(file::size),
///             size_t(0),
///             std::plus<size_t>())
/// \endcode
///
/// Requirements:
///    - Iterator: an InputIterator.
///    - Mapper: unary function taking Iterator::value_type and producing a future<...>.
///    - Initial: any value type
///    - Reduce: a binary function taking two Initial values and returning an Initial
///
/// Return type:
///    - future<Initial>
///
/// \param range object range to operate on
/// \param mapper map function to call on each object, returning a future
/// \param initial initial input value to reduce function
/// \param reduce binary function for merging two result values from \c mapper
///
/// \return equivalent to \c reduce(reduce(initial, mapper(obj0)), mapper(obj1)) ...
template <typename Range, typename Mapper, typename Initial, typename Reduce>
inline
future<Initial>
map_reduce(Range&& range, Mapper&& mapper, Initial initial, Reduce reduce) {
    return map_reduce(std::begin(range), std::end(range), std::forward<Mapper>(mapper),
            std::move(initial), std::move(reduce));
}

// Implements @Reducer concept. Calculates the result by
// adding elements to the accumulator.
template <typename Result, typename Addend = Result>
class adder {
private:
    Result _result;
public:
    future<> operator()(const Addend& value) {
        _result += value;
        return make_ready_future<>();
    }
    Result get() && {
        return std::move(_result);
    }
};

static inline
future<> now() {
    return make_ready_future<>();
}

/// @}

#endif /* CORE_FUTURE_UTIL_HH_ */
