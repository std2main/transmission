// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility> // std::move

#include "libtransmission/transmission.h"

#include "libtransmission/torrent-metainfo.h"
#include "libtransmission/tr-macros.h"

class tr_verify_worker
{
public:
    struct Statistics
    {
        bool used_quick_verify = false;
        bool used_matching_seed_shortcut = false;
        bool fell_back_to_full_verify = false;
        tr_piece_index_t piece_count = 0;
        tr_piece_index_t pieces_hashed = 0;
        tr_piece_index_t pieces_skipped = 0;
        tr_piece_index_t sampled_pieces_hashed = 0;
        tr_piece_index_t sampled_pieces_skipped = 0;
        tr_piece_index_t quick_verify_progress_piece_count = 0;
        tr_file_index_t quick_verify_progress_files_done = 0;
        tr_file_index_t quick_verify_progress_files_total = 0;
        uint64_t bytes_read = 0;
        uint64_t sampled_bytes_read = 0;
        bool metadata_validation_failed = false;
        std::optional<tr_file_index_t> failed_file_index;
    };

    class Mediator
    {
    public:
        virtual ~Mediator() = default;

        [[nodiscard]] virtual tr_torrent_metainfo const& metainfo() const = 0;
        [[nodiscard]] virtual std::optional<std::string> find_file(tr_file_index_t file_index) const = 0;
        [[nodiscard]] virtual bool should_use_quick_verify() const = 0;
        [[nodiscard]] virtual bool should_fallback_on_quick_verify_failure() const = 0;
        [[nodiscard]] virtual bool has_matching_seed() const = 0;
        [[nodiscard]] virtual uint32_t quick_verify_middle_window_cursor() const = 0;
        virtual void advance_quick_verify_middle_window_cursor() = 0;

        virtual void on_verify_queued() = 0;
        virtual void on_verify_started(Statistics const& stats) = 0;
        virtual void on_piece_checked(tr_piece_index_t piece, bool has_piece, bool was_hashed, Statistics const& stats) = 0;
        virtual void on_verify_done(bool aborted, Statistics const& stats) = 0;
    };

    tr_verify_worker() = default;
    ~tr_verify_worker();

    tr_verify_worker(tr_verify_worker const&) = delete;
    tr_verify_worker(tr_verify_worker&&) = delete;
    tr_verify_worker& operator=(tr_verify_worker const&) = delete;
    tr_verify_worker& operator=(tr_verify_worker&&) = delete;

    void add(std::unique_ptr<Mediator> mediator, tr_priority_t priority);

    void remove(tr_sha1_digest_t const& info_hash);

    void set_sleep_per_seconds_during_verify(std::chrono::milliseconds sleep_per_seconds_during_verify);

    [[nodiscard]] constexpr auto sleep_per_seconds_during_verify() const noexcept
    {
        return sleep_per_seconds_during_verify_;
    }

private:
    struct Node
    {
        Node(std::unique_ptr<Mediator> mediator, tr_priority_t priority) noexcept
            : mediator_{ std::move(mediator) }
            , priority_{ priority }
        {
        }

        [[nodiscard]] int compare(Node const& that) const noexcept; // <=>

        [[nodiscard]] auto operator<(Node const& that) const noexcept
        {
            return compare(that) < 0;
        }

        [[nodiscard]] bool matches(tr_sha1_digest_t const& info_hash) const noexcept
        {
            return mediator_->metainfo().info_hash() == info_hash;
        }

        std::unique_ptr<Mediator> mediator_;
        tr_priority_t priority_;
    };

    static void verify_torrent(
        Mediator& verify_mediator,
        std::atomic<bool> const& abort_flag,
        std::chrono::milliseconds sleep_per_seconds_during_verify);

    void verify_thread_func();

    std::mutex verify_mutex_;

    std::set<Node> todo_;
    std::optional<Node> current_node_;

    std::optional<std::thread::id> verify_thread_id_;

    std::atomic<bool> stop_current_ = false;
    std::condition_variable stop_current_cv_;

    std::chrono::milliseconds sleep_per_seconds_during_verify_ = {};
};
