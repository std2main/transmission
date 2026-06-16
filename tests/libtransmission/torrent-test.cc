// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <vector>

#include <libtransmission/makemeta.h>
#include <libtransmission/torrent.h>

#include "test-fixtures.h"

using TorrentTest = libtransmission::test::SessionTest;

namespace
{
auto constexpr TorFilenames = std::array{
    "Android-x86 8.1 r6 iso.torrent"sv,
    "debian-11.2.0-amd64-DVD-1.iso.torrent"sv,
    "ubuntu-18.04.6-desktop-amd64.iso.torrent"sv,
    "ubuntu-20.04.4-desktop-amd64.iso.torrent"sv,
};

void overwrite_byte(std::string_view const filename, uint64_t const offset, char const value)
{
    auto error = tr_error{};
    auto const fd = tr_sys_file_open(std::string{ filename }.c_str(), TR_SYS_FILE_WRITE, 0600, &error);
    ASSERT_FALSE(error);
    ASSERT_NE(TR_BAD_SYS_FILE, fd);

    uint64_t n_written = 0;
    ASSERT_TRUE(tr_sys_file_write_at(fd, &value, 1U, offset, &n_written, &error));
    ASSERT_FALSE(error);
    ASSERT_EQ(1U, n_written);
    tr_sys_file_close(fd, &error);
    ASSERT_FALSE(error);
}

void set_quick_verify_settings(tr_session* const session, bool const enabled, bool const fallback_enabled)
{
    auto settings = tr_sessionGetSettings(session);
    auto* const map = settings.template get_if<tr_variant::Map>();
    ASSERT_NE(nullptr, map);
    map->insert_or_assign(TR_KEY_torrent_quick_verify_enabled, enabled);
    map->insert_or_assign(TR_KEY_torrent_quick_verify_fallback_enabled, fallback_enabled);
    tr_sessionSet(session, settings);
}
} // namespace

TEST_F(TorrentTest, queueMoveUp)
{
    static constexpr auto ExpectedQueuePosition = std::array{ 0, 1, 3, 2 };
    auto ctor = tr_ctor{ session_ };
    auto torrents = std::array<tr_torrent*, TorFilenames.size()>{};
    std::transform(
        TorFilenames.begin(),
        TorFilenames.end(),
        torrents.begin(),
        [this](auto const filename) { return torrentInitFromFile(filename); });
    auto const move_torrents = std::array{ torrents[0], torrents[1], torrents[3] };

    // Pre-test sanity checks
    for (size_t i = 0; i < torrents.size(); ++i)
    {
        ASSERT_EQ(i, torrents[i]->queue_position());
        ASSERT_EQ(i + 1U, torrents[i]->id());
    }

    tr_torrentsQueueMoveUp(move_torrents.data(), move_torrents.size());

    for (size_t i = 0; i < ExpectedQueuePosition.size(); ++i)
    {
        EXPECT_EQ(ExpectedQueuePosition[i], torrents[i]->queue_position()) << i;
    }
}

TEST_F(TorrentTest, quickVerifyWithoutFallbackDoesNotEscalateToFullVerify)
{
    set_quick_verify_settings(session_, true, false);

    auto* const tor = zeroTorrentInit(ZeroTorrentState::Complete);
    ASSERT_NE(nullptr, tor);
    ASSERT_TRUE(tor->has_all());

    auto const found = tor->find_file(0);
    ASSERT_TRUE(found.has_value());

    static constexpr auto PieceSize = uint64_t{ 32768U };
    overwrite_byte(found->filename(), 0U, '\1');
    overwrite_byte(found->filename(), 15U * PieceSize, '\1');

    blockingTorrentVerify(tor);

    EXPECT_FALSE(tor->has_piece(0U));
    EXPECT_FALSE(tor->has_piece(15U));
    EXPECT_FALSE(tor->has_all());
}

TEST_F(TorrentTest, quickVerifyWithFallbackEscalatesToFullVerify)
{
    set_quick_verify_settings(session_, true, true);

    auto* const tor = zeroTorrentInit(ZeroTorrentState::Complete);
    ASSERT_NE(nullptr, tor);
    ASSERT_TRUE(tor->has_all());

    auto const found = tor->find_file(0);
    ASSERT_TRUE(found.has_value());

    static constexpr auto PieceSize = uint64_t{ 32768U };
    overwrite_byte(found->filename(), 0U, '\1');
    overwrite_byte(found->filename(), 15U * PieceSize, '\1');

    blockingTorrentVerify(tor);

    EXPECT_FALSE(tor->has_piece(0U));
    EXPECT_FALSE(tor->has_piece(15U));
    EXPECT_FALSE(tor->has_all());
}

TEST_F(TorrentTest, seedExistingModeForcesQuickVerifyWithoutFallbackAndStopsOnMismatch)
{
    set_quick_verify_settings(session_, false, true);

    auto* const ctor = zeroTorrentCtor();
    createZeroTorrentFiles(ctor, ZeroTorrentState::Complete);
    ctor->set_seed_existing_mode(true);
    tr_ctorSetPaused(ctor, TR_FORCE, false);

    auto const* const metainfo = tr_ctorGetMetainfo(ctor);
    ASSERT_NE(nullptr, metainfo);
    auto const corrupt_file = tr_pathbuf{
        tr_sessionGetDownloadDir(session_),
        "/",
        std::string_view{ metainfo->file_subpath(0) },
    };

    static constexpr auto PieceSize = uint64_t{ 32768U };
    overwrite_byte(corrupt_file, 0U, '\1');
    overwrite_byte(corrupt_file, 15U * PieceSize, '\1');

    auto* const tor = createTorrentAndWaitForVerifyDone(ctor);
    tr_ctorFree(ctor);

    ASSERT_NE(nullptr, tor);
    EXPECT_FALSE(tor->has_piece(0U));
    EXPECT_FALSE(tor->has_piece(15U));
    EXPECT_FALSE(tor->has_all());
    EXPECT_FALSE(tor->is_running());
    EXPECT_EQ(TR_STAT_LOCAL_ERROR, tor->error().error_type());
}

TEST_F(TorrentTest, seedExistingModeStartsSeedingAfterSuccessfulQuickVerify)
{
    set_quick_verify_settings(session_, false, true);

    auto* const ctor = zeroTorrentCtor();
    createZeroTorrentFiles(ctor, ZeroTorrentState::Complete);
    ctor->set_seed_existing_mode(true);
    tr_ctorSetPaused(ctor, TR_FORCE, false);

    auto* const tor = createTorrentAndWaitForVerifyDone(ctor);
    tr_ctorFree(ctor);

    ASSERT_NE(nullptr, tor);
    EXPECT_TRUE(tor->has_all());
    EXPECT_TRUE(libtransmission::test::waitFor([tor]() { return tor->is_running(); }, 5000));
}

TEST_F(TorrentTest, quickVerifySamplesSmallTorrentsMoreEvenly)
{
    set_quick_verify_settings(session_, true, false);

    static constexpr auto PieceSize = uint32_t{ 16384U };
    auto const root = tr_pathbuf{ tr_sessionGetDownloadDir(session_), '/', "quick-verify-small-torrent"sv };
    auto const filename = tr_pathbuf{ root, '/', "single.bin"sv };
    createFileWithContents(filename, std::vector<std::byte>(PieceSize * 8U, std::byte{}).data(), PieceSize * 8U);

    auto builder = tr_metainfo_builder{ root };
    ASSERT_TRUE(builder.set_piece_size(PieceSize));
    auto const checksum_error = builder.make_checksums().get();
    ASSERT_FALSE(checksum_error) << checksum_error;

    auto const benc = builder.benc();
    auto* const ctor = tr_ctorNew(session_);
    auto error = tr_error{};
    ASSERT_TRUE(tr_ctorSetMetainfo(ctor, std::data(benc), std::size(benc), &error));
    ASSERT_FALSE(error) << error;
    tr_ctorSetPaused(ctor, TR_FORCE, true);

    auto* const tor = createTorrentAndWaitForVerifyDone(ctor);
    tr_ctorFree(ctor);

    ASSERT_NE(nullptr, tor);
    ASSERT_TRUE(tor->has_all());

    auto const found = tor->find_file(0);
    ASSERT_TRUE(found.has_value());

    overwrite_byte(found->filename(), uint64_t{ 4U } * PieceSize, '\1');

    blockingTorrentVerify(tor);

    EXPECT_FALSE(tor->has_piece(4U));
    EXPECT_FALSE(tor->has_all());
}

TEST_F(TorrentTest, quickVerifyDoesNotOversampleManySmallFiles)
{
    set_quick_verify_settings(session_, true, false);

    static constexpr auto PieceSize = uint32_t{ 65536U };
    auto const root = tr_pathbuf{ tr_sessionGetDownloadDir(session_), '/', "quick-verify-many-small-files"sv };
    auto const payload = std::vector<std::byte>(4096U, std::byte{});

    for (int i = 0; i < 512; ++i)
    {
        createFileWithContents(
            tr_pathbuf{ root, '/', fmt::format("file-{:02}.bin", i) },
            std::data(payload),
            std::size(payload));
    }

    auto builder = tr_metainfo_builder{ root };
    ASSERT_TRUE(builder.set_piece_size(PieceSize));
    auto const checksum_error = builder.make_checksums().get();
    ASSERT_FALSE(checksum_error) << checksum_error;

    auto const benc = builder.benc();
    auto* const ctor = tr_ctorNew(session_);
    auto error = tr_error{};
    ASSERT_TRUE(tr_ctorSetMetainfo(ctor, std::data(benc), std::size(benc), &error));
    ASSERT_FALSE(error) << error;
    tr_ctorSetPaused(ctor, TR_FORCE, true);

    auto* const tor = createTorrentAndWaitForVerifyDone(ctor);
    tr_ctorFree(ctor);

    ASSERT_NE(nullptr, tor);
    ASSERT_TRUE(tor->has_all());

    auto const found = tor->find_file(200U);
    ASSERT_TRUE(found.has_value());

    overwrite_byte(found->filename(), 0U, '\1');

    blockingTorrentVerify(tor);

    EXPECT_FALSE(tor->has_piece(12U));
    EXPECT_FALSE(tor->has_all());
}

TEST_F(TorrentTest, queueMoveDown)
{
    static constexpr auto ExpectedQueuePosition = std::array{ 1, 0, 2, 3 };
    auto ctor = tr_ctor{ session_ };
    auto torrents = std::array<tr_torrent*, TorFilenames.size()>{};
    std::transform(
        TorFilenames.begin(),
        TorFilenames.end(),
        torrents.begin(),
        [this](auto const filename) { return torrentInitFromFile(filename); });
    auto const move_torrents = std::array{ torrents[0], torrents[2], torrents[3] };

    // Pre-test sanity checks
    for (size_t i = 0; i < torrents.size(); ++i)
    {
        ASSERT_EQ(i, torrents[i]->queue_position());
        ASSERT_EQ(i + 1U, torrents[i]->id());
    }

    tr_torrentsQueueMoveDown(move_torrents.data(), move_torrents.size());

    for (size_t i = 0; i < ExpectedQueuePosition.size(); ++i)
    {
        EXPECT_EQ(ExpectedQueuePosition[i], torrents[i]->queue_position()) << i;
    }
}

TEST_F(TorrentTest, queueMoveTop)
{
    static constexpr auto ExpectedQueuePosition = std::array{ 0, 3, 1, 2 };
    auto ctor = tr_ctor{ session_ };
    auto torrents = std::array<tr_torrent*, TorFilenames.size()>{};
    std::transform(
        TorFilenames.begin(),
        TorFilenames.end(),
        torrents.begin(),
        [this](auto const filename) { return torrentInitFromFile(filename); });
    auto const move_torrents = std::array{ torrents[0], torrents[2], torrents[3] };

    // Pre-test sanity checks
    for (size_t i = 0; i < torrents.size(); ++i)
    {
        ASSERT_EQ(i, torrents[i]->queue_position());
        ASSERT_EQ(i + 1U, torrents[i]->id());
    }

    tr_torrentsQueueMoveTop(move_torrents.data(), move_torrents.size());

    for (size_t i = 0; i < ExpectedQueuePosition.size(); ++i)
    {
        EXPECT_EQ(ExpectedQueuePosition[i], torrents[i]->queue_position()) << i;
    }
}

TEST_F(TorrentTest, queueMoveBottom)
{
    static constexpr auto ExpectedQueuePosition = std::array{ 1, 2, 0, 3 };
    auto ctor = tr_ctor{ session_ };
    auto torrents = std::array<tr_torrent*, TorFilenames.size()>{};
    std::transform(
        TorFilenames.begin(),
        TorFilenames.end(),
        torrents.begin(),
        [this](auto const filename) { return torrentInitFromFile(filename); });
    auto const move_torrents = std::array{ torrents[0], torrents[1], torrents[3] };

    // Pre-test sanity checks
    for (size_t i = 0; i < torrents.size(); ++i)
    {
        ASSERT_EQ(i, torrents[i]->queue_position());
        ASSERT_EQ(i + 1U, torrents[i]->id());
    }

    tr_torrentsQueueMoveBottom(move_torrents.data(), move_torrents.size());

    for (size_t i = 0; i < ExpectedQueuePosition.size(); ++i)
    {
        EXPECT_EQ(ExpectedQueuePosition[i], torrents[i]->queue_position()) << i;
    }
}
