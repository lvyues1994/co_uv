#pragma once
// The only bridge to co2's unstable internals. CMake capability-checks this
// surface for both installed and fetched co2; update this file when co2 changes.
#include <co2/coroutine.hpp>
#include <co2/coroutine_handle.hpp>
#include <co2/detail/awaitable.hpp>
#include <co2/detail/late_init.hpp>
#include <co2/detail/result_storage.hpp>
#include <co2/env.hpp>
#include <co2/task.hpp>
namespace uvco { namespace detail { namespace co2compat {
using co2::detail::AwaiterOf;
using co2::detail::ForwardStop;
using co2::detail::FrameHeader;
using co2::detail::HandleAccess;
using co2::detail::LateInit;
using co2::detail::ResultStorage;
using co2::detail::TaskAccess;
using co2::detail::noopDestroy;
using co2::detail::stopTokenOf;
}}}
