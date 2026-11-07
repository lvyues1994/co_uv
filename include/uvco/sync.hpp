#pragma once

// uvco sync：loop 内单线程同步原语与组合子（docs/architecture.md §4.5）。

#include "uvco/sync/channel.hpp"
#include "uvco/sync/event.hpp"
#include "uvco/sync/mutex.hpp"
#include "uvco/sync/timeout.hpp"
#include "uvco/sync/wait_queue.hpp"
#include "uvco/sync/when_any.hpp"

#include "uvco/sync/when_all_results.hpp"
