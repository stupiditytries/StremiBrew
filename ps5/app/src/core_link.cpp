#include "core_link.hpp"

#include <chrono>
#include <cstring>
#include <ctime>
#include <string_view>

extern "C"
{
    std::int32_t stremio_core_load_board(std::uint32_t rows);
    void stremio_core_board_load_range(std::uint32_t start, std::uint32_t end);
    std::size_t stremio_core_poll_event(char *out, std::size_t capacity);
    std::size_t stremio_core_board_rows(std::uint32_t items_per_row, char *out,
                                        std::size_t capacity);
    void stremio_core_account_advance(void);
    std::size_t stremio_core_account(char *out, std::size_t capacity);
    std::size_t stremio_core_details(char *out, std::size_t capacity);
}

namespace ps5
{
namespace
{
// How many board rows are requested at a time, and how far ahead of the focus.
constexpr std::uint32_t kRowBatch = 8;
constexpr std::uint32_t kRowsAhead = 4;
// The board is re-read at most this often while changes keep arriving (rows load one by
// one, each announcing itself), and the account is looked at this often at least.
constexpr double kBoardInterval = 0.2;
constexpr double kAccountInterval = 2.0;

double seconds_now()
{
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<double>(now.tv_sec) + static_cast<double>(now.tv_nsec) / 1e9;
}
} // namespace

CoreLink::~CoreLink()
{
    {
        const std::lock_guard<std::mutex> lock{mutex_};
        stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable())
        thread_.join();
}

void CoreLink::start()
{
    thread_ = std::thread{[this] { run(); }};
}

void CoreLink::post(std::function<void()> command)
{
    {
        const std::lock_guard<std::mutex> lock{mutex_};
        commands_.push_back(std::move(command));
    }
    wake_.notify_one();
}

bool CoreLink::take_board(std::vector<ui::BoardRow> &rows)
{
    const std::lock_guard<std::mutex> lock{mutex_};
    if (!board_)
        return false;
    rows = std::move(*board_);
    board_.reset();
    return true;
}

bool CoreLink::take_details(ui::Details &details)
{
    const std::lock_guard<std::mutex> lock{mutex_};
    if (!details_)
        return false;
    details = std::move(*details_);
    details_.reset();
    return true;
}

bool CoreLink::take_account(ui::Account &account)
{
    const std::lock_guard<std::mutex> lock{mutex_};
    if (!account_)
        return false;
    account = std::move(*account_);
    account_.reset();
    return true;
}

void CoreLink::run()
{
    std::vector<char> text(8 << 20);
    stremio_core_load_board(kRowBatch);
    std::uint32_t rows_requested = kRowBatch;
    bool board_stale = true, details_stale = false;
    double board_read = 0, account_read = 0;
    ui::Account known;
    bool account_known = false;
    bool title_was_open = false;

    for (;;)
    {
        // Commands first, so what follows reads the state they leave.
        std::deque<std::function<void()>> commands;
        {
            std::unique_lock<std::mutex> lock{mutex_};
            wake_.wait_for(lock, std::chrono::milliseconds(15),
                           [this] { return stopping_ || !commands_.empty(); });
            if (stopping_)
                return;
            commands.swap(commands_);
        }
        const bool commanded = !commands.empty();
        for (auto &command : commands)
            command();

        // The core announces every change as an event; what changed decides what is re-read.
        bool changed = false;
        std::size_t length = 0;
        while ((length = stremio_core_poll_event(text.data(), text.size())) != 0)
        {
            changed = true;
            const std::string_view event{text.data(), length < text.size() ? length : 0};
            // An event too large to read is taken to have changed everything.
            if (event.empty() || event.find("\"board\"") != std::string_view::npos ||
                event.find("\"ctx\"") != std::string_view::npos)
                board_stale = true;
            if (event.empty() || event.find("\"meta_details\"") != std::string_view::npos)
                details_stale = true;
        }

        const double now = seconds_now();
        const bool title_open = title_open_.load(std::memory_order_relaxed);
        if (title_open && (!title_was_open || commanded))
            details_stale = true;
        title_was_open = title_open;

        // The account: after any change, and every so often regardless, which is also when
        // a sign-in in progress is moved along (see the bridge's account.rs).
        if (changed || now - account_read > kAccountInterval)
        {
            account_read = now;
            stremio_core_account_advance();
            length = stremio_core_account(text.data(), text.size());
            ui::Account latest;
            if (length != 0 && length < text.size() &&
                ui::parse_account(std::string_view{text.data(), length}, latest))
            {
                // Signing in or out, or the account's add-ons arriving, changes which
                // catalogs the board has: it is loaded afresh.
                if (account_known &&
                    (latest.signed_in != known.signed_in || latest.addons != known.addons))
                {
                    log_line("account changed: signed %s, %d add-ons",
                             latest.signed_in ? "in" : "out", latest.addons);
                    stremio_core_load_board(kRowBatch);
                    rows_requested = kRowBatch;
                    board_stale = true;
                }
                known = latest;
                account_known = true;
                const std::lock_guard<std::mutex> lock{mutex_};
                account_ = std::move(latest);
            }
        }

        if (board_stale && now - board_read > kBoardInterval)
        {
            board_read = now;
            board_stale = false;
            length = stremio_core_board_rows(20, text.data(), text.size());
            std::vector<ui::BoardRow> rows;
            if (length != 0 && length < text.size() &&
                ui::parse_board(std::string_view{text.data(), length}, rows))
            {
                const std::lock_guard<std::mutex> lock{mutex_};
                board_ = std::move(rows);
            }
        }

        if (details_stale && title_open)
        {
            details_stale = false;
            length = stremio_core_details(text.data(), text.size());
            ui::Details details;
            if (length != 0 && length < text.size() &&
                ui::parse_details(std::string_view{text.data(), length}, details))
            {
                const std::lock_guard<std::mutex> lock{mutex_};
                details_ = std::move(details);
            }
        }

        // More rows as the focus nears the last of those asked for.
        if (focused_catalog_.load(std::memory_order_relaxed) + kRowsAhead >= rows_requested)
        {
            stremio_core_board_load_range(rows_requested, rows_requested + kRowBatch);
            rows_requested += kRowBatch;
        }
    }
}
} // namespace ps5
