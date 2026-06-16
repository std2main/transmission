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
#include "libtransmission/file.h"
#include "libtransmission/tr-macros.h"
#include "libtransmission/verify.h"

using namespace std::chrono_literals;
namespace
{
constexpr auto QuickVerifyInterval = tr_piece_index_t{ 256U };
constexpr auto MatchingSeedQuickVerifyInterval = tr_piece_index_t{ 512U };
constexpr auto QuickVerifyMinimumSamples = tr_piece_index_t{ 4U };
constexpr auto MatchingSeedQuickVerifyMinimumSamples = tr_piece_index_t{ 3U };
constexpr auto QuickVerifyFullyCoveredFilePieceThreshold = uint64_t{ 2U };

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

void mark_evenly_spaced_pieces(
    std::vector<bool>& sampled_pieces,
    tr_piece_index_t const piece_count,
    tr_piece_index_t const target_sample_count)
{
    auto const sample_count = std::min(piece_count, target_sample_count);

    if (sample_count == 0U)
    {
        return;
    }

    for (tr_piece_index_t nth = 0U; nth < sample_count; ++nth)
    {
        auto const piece = static_cast<tr_piece_index_t>(evenly_spaced_index(nth, sample_count, piece_count));
        sampled_pieces[piece] = true;
    }
}

void mark_file_coverage_pieces(std::vector<bool>& sampled_pieces, tr_torrent_metainfo const& metainfo)
{
    auto const default_piece_size = uint64_t(metainfo.piece_size());
    auto const full_coverage_cutoff = default_piece_size * QuickVerifyFullyCoveredFilePieceThreshold;

    auto file_offset = uint64_t{};
    for (tr_file_index_t file_index = 0U, n_files = metainfo.file_count(); file_index < n_files; ++file_index)
    {
        auto const file_size = metainfo.file_size(file_index);
        if (file_size == 0U)
        {
            file_offset += file_size;
            continue;
        }

        auto const first_piece = metainfo.byte_loc(file_offset).piece;
        auto const last_piece = metainfo.byte_loc(file_offset + file_size - 1U).piece;

        if (file_size <= full_coverage_cutoff)
        {
            for (auto piece = first_piece; piece <= last_piece; ++piece)
            {
                sampled_pieces[piece] = true;
            }
        }
        else
        {
            sampled_pieces[first_piece] = true;
            sampled_pieces[last_piece] = true;
        }

        file_offset += file_size;
    }
}

[[nodiscard]] auto make_quick_verify_sample_map(tr_torrent_metainfo const& metainfo, bool const matching_seed_shortcut)
{
    auto const piece_count = metainfo.piece_count();
    auto sampled_pieces = std::vector<bool>(piece_count, false);

    auto const interval = matching_seed_shortcut ? MatchingSeedQuickVerifyInterval : QuickVerifyInterval;
    auto const minimum_samples = matching_seed_shortcut ? MatchingSeedQuickVerifyMinimumSamples : QuickVerifyMinimumSamples;
    auto const interval_sample_count = piece_count == 0U ? tr_piece_index_t{} :
                                                           tr_piece_index_t{ 2U + (piece_count - 1U) / interval };
    auto const base_sample_count = std::max(minimum_samples, interval_sample_count);

    mark_evenly_spaced_pieces(sampled_pieces, piece_count, base_sample_count);

    if (!matching_seed_shortcut)
    {
        mark_file_coverage_pieces(sampled_pieces, metainfo);
    }

    return sampled_pieces;
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
    stats.used_matching_seed_shortcut = stats.used_quick_verify && verify_mediator.has_matching_seed();
    verify_mediator.on_verify_started(stats);

    auto run_pass = [&](bool const quick_verify_pass)
    {
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
        auto dynamic_error_count = int{ 0 };
        auto sampled_pieces = quick_verify_pass ? make_quick_verify_sample_map(metainfo, stats.used_matching_seed_shortcut) :
                                                  std::vector<bool>{};

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
                auto const has_piece = should_hash_piece ? sha.finish() == metainfo.piece_hash(piece) : true;

                if (should_hash_piece)
                {
                    ++stats.pieces_hashed;
                    if (quick_verify_pass)
                    {
                        ++stats.sampled_pieces_hashed;
                    }
                    if (!has_piece)
                    {
                        if (quick_verify_pass && dynamic_error_count < 3)
                        {
                            ++dynamic_error_count;

                            auto const piece_count = metainfo.piece_count();
                            auto const window_size = std::max(
                                tr_piece_index_t{ 10U },
                                static_cast<tr_piece_index_t>(piece_count / 100U));
                            auto file_offset = uint64_t{};

                            for (tr_file_index_t f = 0U, n_files = metainfo.file_count(); f < n_files; ++f)
                            {
                                auto const file_size = metainfo.file_size(f);
                                if (file_size > 0U)
                                {
                                    auto const first_piece = metainfo.byte_loc(file_offset).piece;
                                    auto const last_piece = metainfo.byte_loc(file_offset + file_size - 1U).piece;

                                    if (piece >= first_piece && piece <= last_piece)
                                    {
                                        for (auto p = first_piece; p <= last_piece; ++p)
                                        {
                                            sampled_pieces[p] = true;
                                        }

                                        auto const start_window = (piece >= window_size) ? (piece - window_size) : 0U;
                                        auto const end_window = std::min(piece_count - 1U, piece + window_size);
                                        for (auto p = start_window; p <= end_window; ++p)
                                        {
                                            sampled_pieces[p] = true;
                                        }
                                    }
                                }
                                file_offset += file_size;
                            }
                        }
                        else
                        {
                            bad_result = true;
                        }
                    }
                }
                else
                {
                    ++stats.pieces_skipped;
                    if (quick_verify_pass)
                    {
                        ++stats.sampled_pieces_skipped;
                    }
                }

                verify_mediator.on_piece_checked(piece, has_piece, should_hash_piece);

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

        return !bad_result;
    };

    if (stats.used_quick_verify && !run_pass(true))
    {
        if (verify_mediator.should_fallback_on_quick_verify_failure())
        {
            stats.fell_back_to_full_verify = true;
            stats.used_matching_seed_shortcut = false;
            stats.pieces_hashed = 0;
            stats.pieces_skipped = 0;
            stats.bytes_read = 0;
            if (!abort_flag)
            {
                run_pass(false);
            }
        }
    }
    else if (!stats.used_quick_verify)
    {
        run_pass(false);
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
