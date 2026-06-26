// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef> // std::byte
#include <cstdint> // uint64_t, uint32_t
#include <memory>
#include <mutex>
#include <thread>
#include <utility> // for std::move()
#include <vector>

#include "libtransmission/transmission.h"

#include "libtransmission/crypto-utils.h"
#include "libtransmission/file-piece-map.h"
#include "libtransmission/file.h"
#include "libtransmission/tr-macros.h"
#include "libtransmission/verify.h"

using namespace std::chrono_literals;
namespace
{
constexpr auto QuickVerifyWindowSize = uint64_t{ 8U * 1024U * 1024U };
constexpr auto QuickVerifyLargeFileThreshold = uint64_t{ 32U * 1024U * 1024U };
constexpr auto QuickVerifyMiddleFileThreshold = uint64_t{ 256U * 1024U * 1024U };
constexpr auto QuickVerifyReadBufferSize = uint64_t{ 256U * 1024U };

struct QuickVerifyFile
{
    tr_file_index_t index = {};
    uint64_t start = {};
    uint64_t size = {};
    std::string path;
};

[[nodiscard]] auto current_time_secs()
{
    return std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::steady_clock::now());
}

[[nodiscard]] constexpr auto evenly_spaced_index(uint64_t const nth, uint64_t const count, uint64_t const size) noexcept
{
    if (count <= 1U || size <= 1U)
    {
        return uint64_t{};
    }

    return (nth * (size - 1U)) / (count - 1U);
}

[[nodiscard]] auto collect_quick_verify_files(
    tr_verify_worker::Mediator const& verify_mediator,
    tr_torrent_metainfo const& metainfo,
    std::optional<tr_file_index_t>& failed_file_index) -> std::optional<std::vector<QuickVerifyFile>>
{
    failed_file_index.reset();

    auto files = std::vector<QuickVerifyFile>{};
    files.reserve(metainfo.file_count());

    uint64_t file_start = 0U;
    for (tr_file_index_t file_index = 0U, n_files = metainfo.file_count(); file_index < n_files; ++file_index)
    {
        auto const found = verify_mediator.find_file(file_index);
        if (!found)
        {
            failed_file_index = file_index;
            return {};
        }

        auto const info = tr_sys_path_get_info(*found);
        if (!info || !info->isFile() || info->size != metainfo.file_size(file_index))
        {
            failed_file_index = file_index;
            return {};
        }

        files.push_back(
            QuickVerifyFile{
                .index = file_index,
                .start = file_start,
                .size = metainfo.file_size(file_index),
                .path = std::string{ *found },
            });
        file_start += metainfo.file_size(file_index);
    }

    return files;
}

void append_piece_span(
    std::vector<tr_piece_index_t>& selected_pieces,
    std::vector<bool>& selected_piece_map,
    uint64_t const piece_size,
    uint64_t const begin_byte,
    uint64_t const end_byte_exclusive)
{
    if (begin_byte >= end_byte_exclusive)
    {
        return;
    }

    auto const first_piece = static_cast<tr_piece_index_t>(begin_byte / piece_size);
    auto const last_piece = static_cast<tr_piece_index_t>((end_byte_exclusive - 1U) / piece_size);

    for (tr_piece_index_t piece = first_piece; piece <= last_piece; ++piece)
    {
        if (!selected_piece_map[piece])
        {
            selected_piece_map[piece] = true;
            selected_pieces.push_back(piece);
        }
    }
}

[[nodiscard]] auto quick_verify_middle_window_start(uint64_t const file_size, uint32_t const cursor_index) noexcept
{
    auto const aligned_window_count = (file_size - (2U * QuickVerifyWindowSize)) / QuickVerifyWindowSize;
    auto const aligned_window_index = evenly_spaced_index(cursor_index, 3U, aligned_window_count);
    return QuickVerifyWindowSize * (aligned_window_index + 1U);
}

[[nodiscard]] auto build_quick_verify_piece_plan(
    tr_torrent_metainfo const& metainfo,
    std::vector<QuickVerifyFile> const& files,
    uint32_t const middle_window_cursor) -> std::vector<tr_piece_index_t>
{
    auto selected_piece_map = std::vector<bool>(metainfo.piece_count(), false);
    auto selected_pieces = std::vector<tr_piece_index_t>{};

    auto const piece_size = uint64_t{ metainfo.piece_size() };

    for (auto const& file : files)
    {
        if (file.size > QuickVerifyLargeFileThreshold)
        {
            append_piece_span(selected_pieces, selected_piece_map, piece_size, file.start, file.start + QuickVerifyWindowSize);
            append_piece_span(
                selected_pieces,
                selected_piece_map,
                piece_size,
                file.start + file.size - QuickVerifyWindowSize,
                file.start + file.size);
        }
    }

    auto const selected_middle_window_index = middle_window_cursor % 3U;
    for (auto const& file : files)
    {
        if (file.size > QuickVerifyMiddleFileThreshold)
        {
            auto const middle_window_start = file.start +
                quick_verify_middle_window_start(file.size, selected_middle_window_index);
            append_piece_span(
                selected_pieces,
                selected_piece_map,
                piece_size,
                middle_window_start,
                middle_window_start + QuickVerifyWindowSize);
        }
    }

    for (auto const& file : files)
    {
        if (file.size <= QuickVerifyLargeFileThreshold)
        {
            append_piece_span(selected_pieces, selected_piece_map, piece_size, file.start, file.start + file.size);
        }
    }

    return selected_pieces;
}
} // namespace

void tr_verify_worker::verify_torrent(
    Mediator& verify_mediator,
    std::atomic<bool> const& abort_flag,
    std::chrono::milliseconds const sleep_per_seconds_during_verify)
{
    auto stats = Statistics{};
    auto const& metainfo = verify_mediator.metainfo();
    stats.piece_count = metainfo.piece_count();
    stats.used_quick_verify = verify_mediator.should_use_quick_verify();
    stats.used_matching_seed_shortcut = false;
    verify_mediator.on_verify_started(stats);

    auto run_full_pass = [&]()
    {
        auto constexpr quick_verify_pass = false;
        tr_sys_file_t fd = TR_BAD_SYS_FILE;
        uint64_t file_pos = 0U;
        uint32_t piece_pos = 0U;
        tr_file_index_t file_index = 0U;
        tr_file_index_t prev_file_index = ~file_index;
        tr_piece_index_t piece = 0U;
        auto buffer = std::vector<std::byte>(1024U * 256U);
        auto sha = tr_sha1{};
        auto last_slept_at = current_time_secs();
        auto bad_result = false;
        auto sampled_pieces = std::vector<bool>{};
        auto skipped_pieces = std::vector<bool>{};

        while (!abort_flag && piece < metainfo.piece_count())
        {
            auto const file_length = metainfo.file_size(file_index);

            /* if we're starting a new file... */
            if (file_pos == 0U && fd == TR_BAD_SYS_FILE && file_index != prev_file_index)
            {
                auto const found = verify_mediator.find_file(file_index);
                fd = !found ? TR_BAD_SYS_FILE : tr_sys_file_open(found->c_str(), TR_SYS_FILE_READ | TR_SYS_FILE_SEQUENTIAL, 0);
                prev_file_index = file_index;
            }

            /* figure out how much we can read this pass */
            uint64_t left_in_piece = metainfo.piece_size(piece) - piece_pos;
            uint64_t left_in_file = file_length - file_pos;
            uint64_t bytes_this_pass = std::min(left_in_file, left_in_piece);
            bytes_this_pass = std::min(bytes_this_pass, uint64_t(std::size(buffer)));

            auto should_hash_piece = !quick_verify_pass || sampled_pieces[piece];
            if (fd == TR_BAD_SYS_FILE)
            {
                should_hash_piece = true;
                if (quick_verify_pass)
                {
                    bad_result = true;
                }
            }

            /* read a bit */
            if (fd != TR_BAD_SYS_FILE && should_hash_piece)
            {
                auto num_read = uint64_t{};
                if (tr_sys_file_read_at(fd, std::data(buffer), bytes_this_pass, file_pos, &num_read) && num_read > 0U)
                {
                    bytes_this_pass = num_read;
                    stats.bytes_read += bytes_this_pass;
                    if (quick_verify_pass)
                    {
                        stats.sampled_bytes_read += bytes_this_pass;
                    }
                    sha.add(std::data(buffer), bytes_this_pass);
                }
            }

            /* move our offsets */
            left_in_piece -= bytes_this_pass;
            left_in_file -= bytes_this_pass;
            piece_pos += bytes_this_pass;
            file_pos += bytes_this_pass;

            /* if we're finishing a piece... */
            if (left_in_piece == 0U)
            {
                if (should_hash_piece)
                {
                    auto const has_piece = sha.finish() == metainfo.piece_hash(piece);
                    ++stats.pieces_hashed;
                    if (quick_verify_pass)
                    {
                        ++stats.sampled_pieces_hashed;
                    }
                    if (!has_piece)
                    {
                        bad_result = true;
                    }

                    verify_mediator.on_piece_checked(piece, has_piece, should_hash_piece, stats);
                }
                else
                {
                    ++stats.pieces_skipped;
                    if (quick_verify_pass)
                    {
                        ++stats.sampled_pieces_skipped;
                        skipped_pieces[piece] = true;
                    }
                }

                if (sleep_per_seconds_during_verify > std::chrono::milliseconds::zero())
                {
                    /* sleeping even just a few msec per second goes a long
                     * way towards reducing IO load... */
                    if (auto const now = current_time_secs(); last_slept_at != now)
                    {
                        last_slept_at = now;
                        std::this_thread::sleep_for(sleep_per_seconds_during_verify);
                    }
                }

                sha.clear();
                ++piece;
                piece_pos = 0U;
            }

            /* if we're finishing a file... */
            if (left_in_file == 0U)
            {
                if (fd != TR_BAD_SYS_FILE)
                {
                    tr_sys_file_close(fd);
                    fd = TR_BAD_SYS_FILE;
                }

                ++file_index;
                file_pos = 0U;
            }
        }

        /* cleanup */
        if (fd != TR_BAD_SYS_FILE)
        {
            tr_sys_file_close(fd);
        }

        if (!abort_flag && !bad_result && quick_verify_pass)
        {
            for (tr_piece_index_t p = 0U; p < metainfo.piece_count(); ++p)
            {
                if (skipped_pieces[p])
                {
                    verify_mediator.on_piece_checked(p, true, false, stats);
                }
            }
        }

        return !bad_result;
    };

    auto run_quick_pass = [&]()
    {
        auto failed_file_index = std::optional<tr_file_index_t>{};
        auto const files = collect_quick_verify_files(verify_mediator, metainfo, failed_file_index);
        if (!files)
        {
            stats.metadata_validation_failed = true;
            stats.failed_file_index = failed_file_index;
            return false;
        }

        auto const selected_pieces = build_quick_verify_piece_plan(
            metainfo,
            *files,
            verify_mediator.quick_verify_middle_window_cursor());
        stats.quick_verify_progress_piece_count = metainfo.piece_count();
        stats.quick_verify_progress_files_total = static_cast<tr_file_index_t>(files->size());
        stats.quick_verify_progress_files_done = 0;

        auto last_file_index = static_cast<tr_file_index_t>(-1);

        auto selected_piece_map = std::vector<bool>(metainfo.piece_count(), false);
        for (auto const piece : selected_pieces)
        {
            selected_piece_map[piece] = true;
        }

        auto const file_map = tr_file_piece_map{ metainfo };
        auto buffer = std::vector<std::byte>(QuickVerifyReadBufferSize);
        auto last_slept_at = current_time_secs();
        auto bad_result = false;
        auto current_fd = TR_BAD_SYS_FILE;
        auto current_file_index = ~tr_file_index_t{};

        auto close_current_file = [&]()
        {
            if (current_fd != TR_BAD_SYS_FILE)
            {
                tr_sys_file_close(current_fd);
                current_fd = TR_BAD_SYS_FILE;
            }
        };

        auto open_file = [&](tr_file_index_t const file_index) -> bool
        {
            if (file_index == current_file_index)
            {
                return current_fd != TR_BAD_SYS_FILE;
            }

            close_current_file();
            auto const& path = (*files)[file_index].path;
            current_fd = tr_sys_file_open(path.c_str(), TR_SYS_FILE_READ | TR_SYS_FILE_SEQUENTIAL, 0);
            current_file_index = file_index;
            return current_fd != TR_BAD_SYS_FILE;
        };

        auto read_piece = [&](tr_piece_index_t const piece) -> bool
        {
            auto const piece_start = uint64_t{ piece } * metainfo.piece_size();
            auto const piece_size = metainfo.piece_size(piece);
            auto remaining = uint64_t{ piece_size };
            auto cursor = piece_start;
            auto sha = tr_sha1{};

            while (remaining > 0U)
            {
                if (abort_flag)
                {
                    return false;
                }

                auto const [file_index, file_offset] = file_map.file_offset(cursor);
                auto const file_remaining = metainfo.file_size(file_index) - file_offset;
                auto const bytes_this_pass = std::min({ remaining, file_remaining, uint64_t{ std::size(buffer) } });

                if (!open_file(file_index))
                {
                    bad_result = true;
                    return false;
                }

                auto num_read = uint64_t{};
                if (!tr_sys_file_read_at(current_fd, std::data(buffer), bytes_this_pass, file_offset, &num_read) ||
                    num_read != bytes_this_pass)
                {
                    bad_result = true;
                    return false;
                }

                if (file_index != last_file_index)
                {
                    last_file_index = file_index;
                    ++stats.quick_verify_progress_files_done;
                }

                stats.bytes_read += num_read;
                stats.sampled_bytes_read += num_read;
                sha.add(std::data(buffer), num_read);
                cursor += num_read;
                remaining -= num_read;
            }

            auto const has_piece = sha.finish() == metainfo.piece_hash(piece);
            ++stats.pieces_hashed;
            ++stats.sampled_pieces_hashed;
            verify_mediator.on_piece_checked(piece, has_piece, true, stats);
            if (!has_piece)
            {
                bad_result = true;
                return false;
            }

            return true;
        };

        auto const piece_count = metainfo.piece_count();
        for (auto const piece : selected_pieces)
        {
            if (abort_flag)
            {
                break;
            }

            if (!read_piece(piece))
            {
                break;
            }

            if (sleep_per_seconds_during_verify > std::chrono::milliseconds::zero())
            {
                if (auto const now = current_time_secs(); last_slept_at != now)
                {
                    last_slept_at = now;
                    std::this_thread::sleep_for(sleep_per_seconds_during_verify);
                }
            }
        }

        close_current_file();

        if (!abort_flag && !bad_result)
        {
            for (tr_piece_index_t piece = 0U; piece < piece_count; ++piece)
            {
                if (!selected_piece_map[piece])
                {
                    ++stats.pieces_skipped;
                    ++stats.sampled_pieces_skipped;
                    verify_mediator.on_piece_checked(piece, true, false, stats);
                }
            }
        }

        return !bad_result;
    };

    if (stats.used_quick_verify)
    {
        auto const quick_pass_ok = run_quick_pass();

        if (!abort_flag)
        {
            verify_mediator.advance_quick_verify_middle_window_cursor();
        }

        if (!quick_pass_ok && verify_mediator.should_fallback_on_quick_verify_failure())
        {
            stats.fell_back_to_full_verify = true;
            stats.used_matching_seed_shortcut = false;
            stats.pieces_hashed = 0;
            stats.pieces_skipped = 0;
            stats.quick_verify_progress_piece_count = 0;
            stats.quick_verify_progress_files_done = 0;
            stats.quick_verify_progress_files_total = 0;
            stats.bytes_read = 0;
            if (!abort_flag)
            {
                run_full_pass();
            }
        }
    }
    else
    {
        run_full_pass();
    }

    verify_mediator.on_verify_done(abort_flag, stats);
}

void tr_verify_worker::verify_thread_func()
{
    for (;;)
    {
        {
            auto const lock = std::scoped_lock{ verify_mutex_ };

            if (stop_current_)
            {
                stop_current_ = false;
                stop_current_cv_.notify_one();
            }

            if (std::empty(todo_))
            {
                current_node_.reset();
                verify_thread_id_.reset();
                return;
            }

            current_node_ = std::move(todo_.extract(std::begin(todo_)).value());
        }

        verify_torrent(*current_node_->mediator_, stop_current_, sleep_per_seconds_during_verify_);
    }
}

void tr_verify_worker::add(std::unique_ptr<Mediator> mediator, tr_priority_t priority)
{
    auto const lock = std::scoped_lock{ verify_mutex_ };

    mediator->on_verify_queued();
    todo_.emplace(std::move(mediator), priority);

    if (!verify_thread_id_)
    {
        auto thread = std::thread(&tr_verify_worker::verify_thread_func, this);
        verify_thread_id_ = thread.get_id();
        thread.detach();
    }
}

void tr_verify_worker::remove(tr_sha1_digest_t const& info_hash)
{
    auto lock = std::unique_lock(verify_mutex_);

    if (current_node_ && current_node_->matches(info_hash))
    {
        stop_current_ = true;
        stop_current_cv_.wait(lock, [this]() { return !stop_current_; });
    }
    else if (auto const iter = std::find_if(
                 std::begin(todo_),
                 std::end(todo_),
                 [&info_hash](auto const& node) { return node.matches(info_hash); });
             iter != std::end(todo_))
    {
        auto stats = Statistics{};
        stats.piece_count = iter->mediator_->metainfo().piece_count();
        iter->mediator_->on_verify_done(true /*aborted*/, stats);
        todo_.erase(iter);
    }
}

tr_verify_worker::~tr_verify_worker()
{
    {
        auto const lock = std::scoped_lock{ verify_mutex_ };
        stop_current_ = true;
        todo_.clear();
    }

    while (verify_thread_id_.has_value())
    {
        std::this_thread::sleep_for(20ms);
    }
}

void tr_verify_worker::set_sleep_per_seconds_during_verify(std::chrono::milliseconds const sleep_per_seconds_during_verify)
{
    sleep_per_seconds_during_verify_ = sleep_per_seconds_during_verify;
}

int tr_verify_worker::Node::compare(Node const& that) const noexcept
{
    // prefer higher-priority torrents
    if (priority_ != that.priority_)
    {
        return priority_ > that.priority_ ? -1 : 1;
    }

    // prefer smaller torrents, since they will verify faster
    auto const& metainfo = mediator_->metainfo();
    auto const& that_metainfo = that.mediator_->metainfo();
    if (metainfo.total_size() != that_metainfo.total_size())
    {
        return metainfo.total_size() < that_metainfo.total_size() ? -1 : 1;
    }

    // uniqueness check
    auto const& this_hash = metainfo.info_hash();
    auto const& that_hash = that_metainfo.info_hash();
    if (this_hash != that_hash)
    {
        return this_hash < that_hash ? -1 : 1;
    }

    return 0;
}
