#pragma once
#include "uvco/detail/watcher/event_port.hpp"
namespace uvco { namespace fs { namespace detail {
// Compatibility spellings for existing fs tests/detail clients. All delivery and
// waiting logic lives in the core; native path/stat projection remains in fs.
template<class T> using WatchRecord = ::uvco::detail::watcher::DeliveryRecord<T>;
template<class T> using NoWatchMerge = ::uvco::detail::watcher::NoMerge<T>;
template<class T, class Merge = NoWatchMerge<T>>
using WatchQueue = ::uvco::detail::watcher::BoundedFifo<T, 32, Merge>;
template<class T, class Merge = NoWatchMerge<T>>
using WatchBlock = ::uvco::detail::watcher::EventPort<T, WatchQueue<T, Merge>>;
template<class Block> using WatchAwaiter = ::uvco::detail::watcher::NextAwaiter<Block>;
} } } // namespace uvco::fs::detail
