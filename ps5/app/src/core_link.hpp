// The link between the UI and stremio-core, kept off the drawing thread.
//
// Reading the core's state (which means serialising it to JSON and parsing that) and
// sending it actions both take long enough to make a frame late, and the core can hold
// its state locked while it works. So one thread does all of it: it carries out the
// commands the UI posts, watches the core's events, and leaves the freshest board, title
// details and account for the drawing thread to pick up between frames.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "account_data.hpp"
#include "board_data.hpp"
#include "details_data.hpp"
#include "pages_data.hpp"

void log_line(const char *format, ...) __attribute__((format(printf, 1, 2)));

namespace ps5
{
class CoreLink
{
  public:
    ~CoreLink();
    // Starts the thread. The core itself must already be running.
    void start();
    // Runs `command` (calls into the core) on the link's thread, in the order posted.
    void post(std::function<void()> command);

    // What the UI is looking at, which decides what the link keeps fresh.
    void set_focused_catalog(std::size_t catalog)
    {
        focused_catalog_.store(catalog, std::memory_order_relaxed);
    }
    void set_title_open(bool open)
    {
        title_open_.store(open, std::memory_order_relaxed);
    }
    // Which rows the screen wants: 0 the board, 1 a search (for `query`), 2 the library.
    void set_view(int view, const std::string &query);
    // Which other screen is on show and so kept fresh: 0 none, 1 the calendar, 2 the
    // add-ons, 3 Discover.
    void set_page(int page)
    {
        page_.store(page, std::memory_order_relaxed);
    }

    // Each returns true, and moves the value out, when something newer than the last one
    // taken has arrived.
    // `view` and `query` say which rows these are (see set_view).
    bool take_board(std::vector<ui::BoardRow> &rows, int &view, std::string &query);
    bool take_details(ui::Details &details);
    bool take_account(ui::Account &account);
    bool take_calendar(ui::CalendarMonth &month);
    bool take_addons(std::vector<ui::Addon> &addons);
    bool take_discover(ui::DiscoverData &discover);

  private:
    void run();

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> commands_;
    bool stopping_ = false;
    std::optional<std::vector<ui::BoardRow>> board_;
    int board_view_ = 0;       // the view and query `board_` was read for
    std::string board_query_;
    int view_ = 0;             // the view and query wanted
    std::string query_;
    std::optional<ui::Details> details_;
    std::optional<ui::Account> account_;
    std::optional<ui::CalendarMonth> calendar_;
    std::optional<std::vector<ui::Addon>> addons_;
    std::optional<ui::DiscoverData> discover_;
    std::atomic<int> page_{0};
    std::atomic<std::size_t> focused_catalog_{0};
    std::atomic<bool> title_open_{false};
};
} // namespace ps5
