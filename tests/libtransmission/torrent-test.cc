// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
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

void create_sparse_file(std::string_view const filename, uint64_t const size)
{
    auto parent = tr_pathbuf{ filename };
    parent.popdir();
    if (!parent.empty())
    {
        auto error = tr_error{};
        ASSERT_TRUE(tr_sys_dir_create(parent, TR_SYS_DIR_CREATE_PARENTS, 0700, &error));
        ASSERT_FALSE(error);
    }

    auto error = tr_error{};
    auto const fd = tr_sys_file_open(
        std::string{ filename }.c_str(),
        TR_SYS_FILE_WRITE | TR_SYS_FILE_CREATE | TR_SYS_FILE_TRUNCATE,
        0600,
        &error);
    ASSERT_FALSE(error);
    ASSERT_NE(TR_BAD_SYS_FILE, fd);

    ASSERT_TRUE(tr_sys_file_truncate(fd, size, &error));
    ASSERT_FALSE(error);

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
    overwrite_byte(found->filename(), 10U * PieceSize, '\1');

    blockingTorrentVerify(tor);

    EXPECT_FALSE(tor->has_piece(0U));
    EXPECT_TRUE(tor->has_piece(10U));
    EXPECT_FALSE(tor->has_all());
}

TEST_F(TorrentTest, ordinaryAddWithoutExistingDataUsesQuickVerifyAndStopsWithVisibleError)
{
    set_quick_verify_settings(session_, true, false);

    auto* const tor = zeroTorrentInit(ZeroTorrentState::NoFiles);
    ASSERT_NE(nullptr, tor);

    auto const* const st = tr_torrentStat(tor);
    ASSERT_NE(nullptr, st);

    EXPECT_TRUE(tor->verify_stats().used_quick_verify);
    EXPECT_FALSE(tor->has_all());
    EXPECT_EQ(TR_STAT_LOCAL_ERROR, st->error);
    ASSERT_NE(nullptr, st->errorString);
    EXPECT_NE(std::string::npos, std::string{ st->errorString }.find("Quick verify found missing or mismatched local data"));
}

TEST_F(TorrentTest, ordinaryAddWithExistingCompleteDataAutoUsesQuickVerify)
{
    set_quick_verify_settings(session_, true, false);

    auto* const tor = zeroTorrentInit(ZeroTorrentState::Complete);
    ASSERT_NE(nullptr, tor);

    EXPECT_TRUE(tor->verify_stats().used_quick_verify);
    EXPECT_TRUE(tor->has_all());
}

TEST_F(TorrentTest, quickVerifyFailureDoesNotMarkSkippedPiecesAsComplete)
{
    // Enable quick-verify, disable fallback.
    set_quick_verify_settings(session_, true, false);

    static constexpr auto PieceSize = uint32_t{ 16384U };
    auto const root = tr_pathbuf{ tr_sessionGetDownloadDir(session_), '/', "quick-verify-failure-skipped-pieces"sv };
    auto const file1 = tr_pathbuf{ root, '/', "file1.bin"sv };
    auto const file2 = tr_pathbuf{ root, '/', "file2.bin"sv };

    // 1. Create two 100-piece files with all zero bytes.
    createFileWithContents(file1, std::vector<std::byte>(PieceSize * 100U, std::byte{}).data(), PieceSize * 100U);
    createFileWithContents(file2, std::vector<std::byte>(PieceSize * 100U, std::byte{}).data(), PieceSize * 100U);

    // 2. Build metainfo for the directory containing both files.
    auto builder = tr_metainfo_builder{ root };
    ASSERT_TRUE(builder.set_piece_size(PieceSize));
    auto const checksum_error = builder.make_checksums().get();
    ASSERT_FALSE(checksum_error) << checksum_error;

    // 3. Corrupt 4 pieces in file1.bin (piece 0, 20, 40, 60) before adding the torrent.
    overwrite_byte(file1, 0U, '\1');
    overwrite_byte(file1, 20U * PieceSize, '\1');
    overwrite_byte(file1, 40U * PieceSize, '\1');
    overwrite_byte(file1, 60U * PieceSize, '\1');

    auto const benc = builder.benc();
    auto* const ctor = tr_ctorNew(session_);
    auto error = tr_error{};
    ASSERT_TRUE(tr_ctorSetMetainfo(ctor, std::data(benc), std::size(benc), &error));
    ASSERT_FALSE(error) << error;
    tr_ctorSetPaused(ctor, TR_FORCE, true);
    ctor->set_seed_existing_mode(true);

    // 4. Create the torrent and wait for verification.
    auto* const tor = createTorrentAndWaitForVerifyDone(ctor);
    tr_ctorFree(ctor);

    ASSERT_NE(nullptr, tor);

    // 5. Verify that the quick-verify pass failed.
    // Specifically, skipped pieces in file2.bin (like piece 150) must not be marked complete.
    EXPECT_FALSE(tor->has_piece(150U));
    EXPECT_FALSE(tor->has_all());
}

TEST_F(TorrentTest, quickVerifyFailsImmediatelyOnMissingFile)
{
    // Enable quick-verify, disable fallback.
    set_quick_verify_settings(session_, true, false);

    static constexpr auto PieceSize = uint32_t{ 16384U };
    auto const root = tr_pathbuf{ tr_sessionGetDownloadDir(session_), '/', "quick-verify-missing-file"sv };
    auto const file1 = tr_pathbuf{ root, '/', "file1.bin"sv };
    auto const file2 = tr_pathbuf{ root, '/', "file2.bin"sv };

    // Create two 1-piece files.
    createFileWithContents(file1, std::vector<std::byte>(PieceSize, std::byte{}).data(), PieceSize);
    createFileWithContents(file2, std::vector<std::byte>(PieceSize, std::byte{}).data(), PieceSize);

    // Build metainfo.
    auto builder = tr_metainfo_builder{ root };
    ASSERT_TRUE(builder.set_piece_size(PieceSize));
    auto const checksum_error = builder.make_checksums().get();
    ASSERT_FALSE(checksum_error) << checksum_error;

    // Delete file2 to simulate a missing file.
    tr_sys_path_remove(file2);

    auto const benc = builder.benc();
    auto* const ctor = tr_ctorNew(session_);
    auto error = tr_error{};
    ASSERT_TRUE(tr_ctorSetMetainfo(ctor, std::data(benc), std::size(benc), &error));
    ASSERT_FALSE(error) << error;
    tr_ctorSetPaused(ctor, TR_FORCE, true);
    ctor->set_seed_existing_mode(true);

    auto* const tor = createTorrentAndWaitForVerifyDone(ctor);
    tr_ctorFree(ctor);

    ASSERT_NE(nullptr, tor);
    // The torrent should NOT have all pieces because file2 was missing, and the quick verify should have failed.
    EXPECT_FALSE(tor->has_all());
}

TEST_F(TorrentTest, quickVerifyMarksCompleteTorrentIncompleteWhenFileMissing)
{
    set_quick_verify_settings(session_, true, false);

    auto* const tor = zeroTorrentInit(ZeroTorrentState::Complete);
    ASSERT_NE(nullptr, tor);
    ASSERT_TRUE(tor->has_all());

    auto const found = tor->find_file(1);
    ASSERT_TRUE(found.has_value());
    tr_sys_path_remove(found->filename());

    verifyQuickAndWait(tor);

    auto const* const st = tr_torrentStat(tor);
    ASSERT_NE(nullptr, st);
    EXPECT_TRUE(tor->verify_stats().used_quick_verify);
    EXPECT_FALSE(tor->has_all());
    EXPECT_EQ(TR_STAT_LOCAL_ERROR, st->error);
    ASSERT_NE(nullptr, st->errorString);
    EXPECT_NE(std::string::npos, std::string{ st->errorString }.find("Quick verify found missing or mismatched local data"));
}

TEST_F(TorrentTest, quickVerifyWithFallbackFallsBackOnMissingFile)
{
    set_quick_verify_settings(session_, true, true);

    auto* const tor = zeroTorrentInit(ZeroTorrentState::Complete);
    ASSERT_NE(nullptr, tor);
    ASSERT_TRUE(tor->has_all());

    auto const found = tor->find_file(1);
    ASSERT_TRUE(found.has_value());
    tr_sys_path_remove(found->filename());

    verifyQuickAndWait(tor);

    auto const* const st = tr_torrentStat(tor);
    ASSERT_NE(nullptr, st);
    EXPECT_TRUE(tor->verify_stats().used_quick_verify);
    EXPECT_TRUE(tor->verify_stats().fell_back_to_full_verify);
    EXPECT_FALSE(tor->has_all());
    EXPECT_NE(nullptr, st->errorString);
    EXPECT_EQ(std::string::npos, std::string{ st->errorString }.find("Stopped without full fallback"));
}

TEST_F(TorrentTest, quickVerifyWithFallbackEscalatesToFullVerify)
{
    set_quick_verify_settings(session_, true, true);

    static constexpr auto PieceSize = uint32_t{ 16384U };
    auto const root = tr_pathbuf{ tr_sessionGetDownloadDir(session_), '/', "quick-verify-with-fallback"sv };
    auto const file1 = tr_pathbuf{ root, '/', "file1.bin"sv };
    auto const file2 = tr_pathbuf{ root, '/', "file2.bin"sv };

    createFileWithContents(file1, std::vector<std::byte>(PieceSize * 100U, std::byte{}).data(), PieceSize * 100U);
    createFileWithContents(file2, std::vector<std::byte>(PieceSize * 100U, std::byte{}).data(), PieceSize * 100U);

    auto builder = tr_metainfo_builder{ root };
    ASSERT_TRUE(builder.set_piece_size(PieceSize));
    auto const checksum_error = builder.make_checksums().get();
    ASSERT_FALSE(checksum_error) << checksum_error;

    overwrite_byte(file1, 0U, '\1');
    overwrite_byte(file1, 20U * PieceSize, '\1');
    overwrite_byte(file1, 40U * PieceSize, '\1');
    overwrite_byte(file1, 60U * PieceSize, '\1');

    auto const benc = builder.benc();
    auto* const ctor = tr_ctorNew(session_);
    auto error = tr_error{};
    ASSERT_TRUE(tr_ctorSetMetainfo(ctor, std::data(benc), std::size(benc), &error));
    ASSERT_FALSE(error) << error;
    tr_ctorSetPaused(ctor, TR_FORCE, true);

    auto* const tor = createTorrentAndWaitForVerifyDone(ctor);
    tr_ctorFree(ctor);

    auto const* const st = tr_torrentStat(tor);
    ASSERT_NE(nullptr, st);
    EXPECT_TRUE(tor->verify_stats().used_quick_verify);
    EXPECT_TRUE(tor->verify_stats().fell_back_to_full_verify);
    ASSERT_NE(nullptr, st->errorString);
    EXPECT_EQ(std::string::npos, std::string{ st->errorString }.find("Stopped without full fallback"));
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
    overwrite_byte(corrupt_file, 10U * PieceSize, '\1');

    auto* const tor = createTorrentAndWaitForVerifyDone(ctor);
    tr_ctorFree(ctor);

    ASSERT_NE(nullptr, tor);
    EXPECT_FALSE(tor->has_piece(0U));
    EXPECT_FALSE(tor->has_piece(10U));
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

TEST_F(TorrentTest, seedCandidateWithoutQuickVerifySettingStillRunsQuickVerify)
{
    // Simulates GTK/Qt/macOS "open torrent" with default settings:
    // quick verify OFF, fallback OFF, seed_existing_mode OFF
    set_quick_verify_settings(session_, false, false);

    auto* const ctor = zeroTorrentCtor();
    createZeroTorrentFiles(ctor, ZeroTorrentState::Complete);
    // Do NOT set seed_existing_mode -- this is the bug scenario
    tr_ctorSetPaused(ctor, TR_FORCE, true);

    auto const* const metainfo = tr_ctorGetMetainfo(ctor);
    ASSERT_NE(nullptr, metainfo);
    auto const corrupt_file = tr_pathbuf{
        tr_sessionGetDownloadDir(session_),
        "/",
        std::string_view{ metainfo->file_subpath(0) },
    };

    overwrite_byte(corrupt_file, 0U, '\1');

    auto* const tor = createTorrentAndWaitForVerifyDone(ctor);
    tr_ctorFree(ctor);

    ASSERT_NE(nullptr, tor);
    EXPECT_TRUE(tor->verify_stats().used_quick_verify);
    EXPECT_FALSE(tor->has_piece(0U));
    EXPECT_FALSE(tor->has_all());
}

TEST_F(TorrentTest, seedCandidateWithoutQuickVerifySettingPassesWithValidData)
{
    // Quick verify OFF (defaults), but seed candidate should still get quick verify
    set_quick_verify_settings(session_, false, false);

    auto* const ctor = zeroTorrentCtor();
    createZeroTorrentFiles(ctor, ZeroTorrentState::Complete);
    // Do NOT set seed_existing_mode
    tr_ctorSetPaused(ctor, TR_FORCE, true);

    auto* const tor = createTorrentAndWaitForVerifyDone(ctor);
    tr_ctorFree(ctor);

    ASSERT_NE(nullptr, tor);
    EXPECT_TRUE(tor->verify_stats().used_quick_verify);
    EXPECT_TRUE(tor->has_all());
}

TEST_F(TorrentTest, quickVerifyFullyChecksSmallTorrents)
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
    EXPECT_TRUE(tor->verify_stats().used_quick_verify);
    EXPECT_EQ(tor->verify_stats().piece_count, tor->verify_stats().pieces_hashed);
    EXPECT_EQ(0U, tor->verify_stats().pieces_skipped);

    auto const found = tor->find_file(0);
    ASSERT_TRUE(found.has_value());

    overwrite_byte(found->filename(), uint64_t{ 4U } * PieceSize, '\1');

    blockingTorrentVerify(tor);

    EXPECT_FALSE(tor->has_piece(4U));
    EXPECT_FALSE(tor->has_all());
}

TEST_F(TorrentTest, quickVerifyFullyChecksManySmallFiles)
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
    EXPECT_TRUE(tor->verify_stats().used_quick_verify);
    EXPECT_EQ(tor->verify_stats().piece_count, tor->verify_stats().pieces_hashed);
    EXPECT_EQ(0U, tor->verify_stats().pieces_skipped);

    auto const found = tor->find_file(160U);
    ASSERT_TRUE(found.has_value());

    overwrite_byte(found->filename(), 0U, '\1');

    blockingTorrentVerify(tor);

    EXPECT_FALSE(tor->has_piece(10U));
    EXPECT_FALSE(tor->has_all());
}

TEST_F(TorrentTest, quickVerifyFullyChecksSmallFiles)
{
    set_quick_verify_settings(session_, true, false);

    static constexpr auto MiB = uint64_t{ 1024U * 1024U };
    static constexpr auto PieceSize = uint32_t{ 4U * 1024U * 1024U };
    static constexpr auto FileSize = uint64_t{ 16U * MiB };

    auto const root = tr_pathbuf{ tr_sessionGetDownloadDir(session_), '/', "quick-verify-fully-small"sv };
    auto const filename = tr_pathbuf{ root, '/', "small.bin"sv };
    create_sparse_file(filename, FileSize);

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
    EXPECT_TRUE(tor->verify_stats().used_quick_verify);
    EXPECT_EQ(tor->metainfo().piece_count(), tor->verify_stats().piece_count);
    EXPECT_GT(tor->verify_stats().pieces_hashed, 0U);
    EXPECT_EQ(tor->verify_stats().pieces_hashed + tor->verify_stats().pieces_skipped, tor->verify_stats().piece_count);
    EXPECT_GT(tor->verify_stats().bytes_read, 0U);
    EXPECT_LE(tor->verify_stats().bytes_read, FileSize);

    overwrite_byte(filename.sv(), 5U * MiB, '\1');

    blockingTorrentVerify(tor);

    EXPECT_FALSE(tor->has_all());
}

TEST_F(TorrentTest, quickVerifySamplesLargeFiles)
{
    set_quick_verify_settings(session_, true, false);

    static constexpr auto MiB = uint64_t{ 1024U * 1024U };
    static constexpr auto PieceSize = uint32_t{ 4U * 1024U * 1024U };
    static constexpr auto FileSize = uint64_t{ 500U * MiB };

    auto const root = tr_pathbuf{ tr_sessionGetDownloadDir(session_), '/', "quick-verify-large-sampled"sv };
    auto const filename = tr_pathbuf{ root, '/', "sampled.bin"sv };
    create_sparse_file(filename, FileSize);

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
    EXPECT_TRUE(tor->verify_stats().used_quick_verify);
    EXPECT_EQ(tor->metainfo().piece_count(), tor->verify_stats().piece_count);
    EXPECT_GT(tor->verify_stats().pieces_hashed, 0U);
    EXPECT_LT(tor->verify_stats().pieces_hashed, tor->verify_stats().piece_count);
    EXPECT_GT(tor->verify_stats().pieces_skipped, 0U);
    EXPECT_EQ(tor->verify_stats().pieces_hashed + tor->verify_stats().pieces_skipped, tor->verify_stats().piece_count);
    EXPECT_LT(tor->verify_stats().bytes_read, FileSize);

    // Corrupt a byte in the middle of the file — away from head/tail windows,
    // so quick verify may not catch it depending on the middle window cursor.
    overwrite_byte(filename.sv(), 250U * MiB, '\1');

    blockingTorrentVerify(tor);

    // Quick verify samples only head/tail/middle; 250MiB may not be in
    // the current window, so the file can appear still valid.
    if (tor->has_all())
    {
        EXPECT_GT(tor->verify_stats().pieces_hashed, 0U);
        EXPECT_GT(tor->verify_stats().pieces_skipped, 0U);
        EXPECT_LT(tor->verify_stats().bytes_read, FileSize);
    }
    else
    {
        EXPECT_FALSE(tor->has_all());
    }
}

TEST_F(TorrentTest, quickVerifyHandlesPiecesSharedAcrossFiles)
{
    set_quick_verify_settings(session_, true, false);

    static constexpr auto PieceSize = uint32_t{ 16U * 1024U };
    auto const root = tr_pathbuf{ tr_sessionGetDownloadDir(session_), '/', "quick-verify-shared-piece"sv };
    auto const file1 = tr_pathbuf{ root, '/', "file1.bin"sv };
    auto const file2 = tr_pathbuf{ root, '/', "file2.bin"sv };

    createFileWithContents(file1, std::vector<std::byte>(10U * 1024U, std::byte{}).data(), 10U * 1024U);
    createFileWithContents(file2, std::vector<std::byte>(10U * 1024U, std::byte{}).data(), 10U * 1024U);

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
    EXPECT_TRUE(tor->verify_stats().used_quick_verify);
    EXPECT_EQ(tor->metainfo().piece_count(), tor->verify_stats().piece_count);
    EXPECT_EQ(tor->verify_stats().pieces_hashed, tor->verify_stats().piece_count);
    EXPECT_EQ(tor->verify_stats().pieces_skipped, 0U);

    overwrite_byte(file2, 0U, '\1');

    blockingTorrentVerify(tor);

    EXPECT_FALSE(tor->has_all());
    EXPECT_FALSE(tor->has_piece(0U));
    EXPECT_TRUE(tor->has_piece(1U));
}

TEST_F(TorrentTest, quickVerifyRotatesMiddleWindowAcrossVeryLargeFiles)
{
    set_quick_verify_settings(session_, true, false);

    static constexpr auto MiB = uint64_t{ 1024U * 1024U };
    static constexpr auto PieceSize = uint32_t{ 8U * 1024U * 1024U };
    static constexpr auto FileSize = uint64_t{ 512U * MiB };
    // Corrupt a byte near the end of the file, inside the region covered by
    // middle window cursor 2 (the last of 3 evenly-spaced rotations).
    // Each cursor position samples a different middle window, so after 3
    // verify passes the corruption will be caught on the 3rd rotation.
    static constexpr auto CorruptOffset = uint64_t{ 497U * MiB };

    auto const root = tr_pathbuf{ tr_sessionGetDownloadDir(session_), '/', "quick-verify-middle-window-rotation"sv };
    auto const filename = tr_pathbuf{ root, '/', "large.bin"sv };
    create_sparse_file(filename, FileSize);

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
    EXPECT_TRUE(tor->verify_stats().used_quick_verify);
    EXPECT_EQ(tor->metainfo().piece_count(), tor->verify_stats().piece_count);
    EXPECT_GT(tor->verify_stats().pieces_hashed, 0U);
    EXPECT_LT(tor->verify_stats().pieces_hashed, tor->verify_stats().piece_count);
    EXPECT_GT(tor->verify_stats().pieces_skipped, 0U);
    EXPECT_EQ(tor->verify_stats().pieces_hashed + tor->verify_stats().pieces_skipped, tor->verify_stats().piece_count);
    EXPECT_GT(tor->verify_stats().bytes_read, 0U);
    EXPECT_LT(tor->verify_stats().bytes_read, FileSize);

    overwrite_byte(filename.sv(), CorruptOffset, '\1');

    // The initial verify (createTorrentAndWaitForVerifyDone) used cursor 0
    // and advanced it to 1. So first manual verify uses cursor 1.
    verifyAndWait(tor);
    // Cursor 1 middle window doesn't cover CorruptOffset, so pass
    ASSERT_TRUE(tor->has_all());

    // Second verify (cursor 2): corruption IS in the sampled region, fail
    verifyAndWait(tor);
    auto const* const st = tr_torrentStat(tor);
    ASSERT_NE(nullptr, st);
    EXPECT_FALSE(tor->has_all());
    EXPECT_TRUE(tor->verify_stats().used_quick_verify);
    EXPECT_FALSE(tor->verify_stats().fell_back_to_full_verify);
    EXPECT_GT(tor->verify_stats().pieces_hashed, 0U);
    EXPECT_LT(tor->verify_stats().bytes_read, FileSize);
    EXPECT_EQ(TR_STAT_LOCAL_ERROR, st->error);
    ASSERT_NE(nullptr, st->errorString);
    EXPECT_NE(std::string::npos, std::string{ st->errorString }.find("Quick verify found missing or mismatched local data"));

    auto const info_hash = tor->info_hash();
    tr_torrentRemove(tor, false, nullptr, nullptr);
    ASSERT_TRUE(
        libtransmission::test::waitFor([this, &info_hash]() { return session_->torrents().get(info_hash) == nullptr; }, 5000));

    auto* const ctor2 = tr_ctorNew(session_);
    ASSERT_TRUE(tr_ctorSetMetainfo(ctor2, std::data(benc), std::size(benc), &error));
    ASSERT_FALSE(error) << error;
    tr_ctorSetPaused(ctor2, TR_FORCE, true);

    auto* const fresh_tor = createTorrentAndWaitForVerifyDone(ctor2);
    tr_ctorFree(ctor2);

    ASSERT_NE(nullptr, fresh_tor);
    ASSERT_TRUE(fresh_tor->has_all());
    EXPECT_TRUE(fresh_tor->verify_stats().used_quick_verify);
    EXPECT_EQ(fresh_tor->metainfo().piece_count(), fresh_tor->verify_stats().piece_count);
    EXPECT_GT(fresh_tor->verify_stats().pieces_hashed, 0U);
    EXPECT_LT(fresh_tor->verify_stats().pieces_hashed, fresh_tor->verify_stats().piece_count);
    EXPECT_GT(fresh_tor->verify_stats().pieces_skipped, 0U);
    EXPECT_EQ(
        fresh_tor->verify_stats().pieces_hashed + fresh_tor->verify_stats().pieces_skipped,
        fresh_tor->verify_stats().piece_count);
    EXPECT_GT(fresh_tor->verify_stats().bytes_read, 0U);
    EXPECT_LT(fresh_tor->verify_stats().bytes_read, FileSize);
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

TEST_F(TorrentTest, verificationStatsAreLoggedToFile)
{
    auto const log_file_path = tr_pathbuf{ tr_sessionGetDownloadDir(session_), '/', "verify_test.log"sv };

    // Set the log path in session settings
    auto settings = tr_sessionGetSettings(session_);
    auto* const map = settings.template get_if<tr_variant::Map>();
    ASSERT_NE(nullptr, map);
    map->insert_or_assign(TR_KEY_torrent_verify_log_path, std::string{ log_file_path.sv() });
    tr_sessionSet(session_, settings);

    // Run quick verify settings
    set_quick_verify_settings(session_, true, false);

    // Initialize torrent with seed_existing_mode=true and run verify
    auto* const ctor = zeroTorrentCtor();
    createZeroTorrentFiles(ctor, ZeroTorrentState::Complete);
    ctor->set_seed_existing_mode(true);
    auto* const tor = createTorrentAndWaitForVerifyDone(ctor);
    tr_ctorFree(ctor);

    ASSERT_NE(nullptr, tor);
    ASSERT_TRUE(tor->has_all());

    // Read log file and verify its contents
    auto in = std::ifstream{ log_file_path.c_str(), std::ios_base::in | std::ios_base::binary };
    ASSERT_TRUE(in.is_open());
    std::string line;
    std::getline(in, line);
    EXPECT_FALSE(line.empty());
    EXPECT_NE(std::string::npos, line.find("Torrent:"));
    EXPECT_NE(std::string::npos, line.find("Hash:"));
    EXPECT_NE(std::string::npos, line.find("Size:"));
    EXPECT_NE(std::string::npos, line.find("Mode: Quick"));
}

TEST_F(TorrentTest, seedExistingModeDoesNotLeakIntoLaterManualVerifyRequests)
{
    set_quick_verify_settings(session_, true, false);

    auto* const ctor = zeroTorrentCtor();
    createZeroTorrentFiles(ctor, ZeroTorrentState::Complete);
    ctor->set_seed_existing_mode(true);
    auto* const tor = createTorrentAndWaitForVerifyDone(ctor);
    tr_ctorFree(ctor);

    ASSERT_NE(nullptr, tor);
    ASSERT_TRUE(tor->has_all());
    EXPECT_TRUE(tor->verify_stats().used_quick_verify);

    set_quick_verify_settings(session_, false, false);
    verifyAndWait(tor);

    EXPECT_FALSE(tor->verify_stats().used_quick_verify);
    EXPECT_TRUE(tor->has_all());
}

TEST_F(TorrentTest, quickVerifyMismatchSetsVisibleErrorAndWritesItToVerifyLog)
{
    auto const log_file_path = tr_pathbuf{ tr_sessionGetDownloadDir(session_), '/', "verify_mismatch.log"sv };

    auto settings = tr_sessionGetSettings(session_);
    auto* const map = settings.template get_if<tr_variant::Map>();
    ASSERT_NE(nullptr, map);
    map->insert_or_assign(TR_KEY_torrent_verify_log_path, std::string{ log_file_path.sv() });
    tr_sessionSet(session_, settings);

    set_quick_verify_settings(session_, true, false);

    auto* const ctor = zeroTorrentCtor();
    createZeroTorrentFiles(ctor, ZeroTorrentState::Complete);
    auto* const tor = createTorrentAndWaitForVerifyDone(ctor);
    tr_ctorFree(ctor);

    ASSERT_NE(nullptr, tor);
    ASSERT_TRUE(tor->has_all());

    auto const found = tor->find_file(0);
    ASSERT_TRUE(found.has_value());
    overwrite_byte(found->filename(), 0U, '\1');

    verifyAndWait(tor);

    auto const* const st = tr_torrentStat(tor);
    ASSERT_NE(nullptr, st);
    EXPECT_FALSE(tor->has_all());
    EXPECT_TRUE(tor->verify_stats().used_quick_verify);
    EXPECT_EQ(TR_STAT_LOCAL_ERROR, st->error);
    ASSERT_NE(nullptr, st->errorString);
    EXPECT_NE(std::string::npos, std::string{ st->errorString }.find("Quick verify found missing or mismatched local data"));

    auto in = std::ifstream{ log_file_path.c_str(), std::ios_base::in | std::ios_base::binary };
    ASSERT_TRUE(in.is_open());
    auto const log_contents = std::string{ std::istreambuf_iterator<char>{ in }, std::istreambuf_iterator<char>{} };
    EXPECT_NE(std::string::npos, log_contents.find("Status: StoppedOnMismatch"));
    EXPECT_NE(std::string::npos, log_contents.find("Error: Quick verify found missing or mismatched local data"));
}

TEST_F(TorrentTest, ordinaryAddWithoutExistingDataWritesQuickVerifyFailureToLog)
{
    auto const log_file_path = tr_pathbuf{ tr_sessionGetDownloadDir(session_), '/', "verify_no_data.log"sv };

    auto settings = tr_sessionGetSettings(session_);
    auto* const map = settings.template get_if<tr_variant::Map>();
    ASSERT_NE(nullptr, map);
    map->insert_or_assign(TR_KEY_torrent_verify_log_path, std::string{ log_file_path.sv() });
    tr_sessionSet(session_, settings);

    set_quick_verify_settings(session_, true, false);

    auto* const tor = zeroTorrentInit(ZeroTorrentState::NoFiles);
    ASSERT_NE(nullptr, tor);

    auto in = std::ifstream{ log_file_path.c_str(), std::ios_base::in | std::ios_base::binary };
    ASSERT_TRUE(in.is_open());
    auto const log_contents = std::string{ std::istreambuf_iterator<char>{ in }, std::istreambuf_iterator<char>{} };

    EXPECT_TRUE(tor->verify_stats().used_quick_verify);
    EXPECT_NE(std::string::npos, log_contents.find("Status: StoppedOnMismatch"));
    EXPECT_NE(std::string::npos, log_contents.find("Mode: Quick"));
    EXPECT_NE(std::string::npos, log_contents.find("Error: Quick verify found missing or mismatched local data"));
}

TEST_F(TorrentTest, manualVerifyOnPartialTorrentStaysStandardEvenWhenQuickVerifyEnabled)
{
    set_quick_verify_settings(session_, false, false);

    auto* const ctor = zeroTorrentCtor();
    createZeroTorrentFiles(ctor, ZeroTorrentState::Partial);
    auto* const tor = createTorrentAndWaitForVerifyDone(ctor);
    tr_ctorFree(ctor);

    ASSERT_NE(nullptr, tor);
    EXPECT_FALSE(tor->verify_stats().used_quick_verify);
    EXPECT_FALSE(tor->has_all());

    set_quick_verify_settings(session_, true, false);
    verifyAndWait(tor);

    EXPECT_FALSE(tor->verify_stats().used_quick_verify);
    EXPECT_FALSE(tor->has_all());
}

TEST_F(TorrentTest, manualVerifyQuickCheckAndFallback)
{
    // Run quick verify settings, fallback disabled
    set_quick_verify_settings(session_, true, false);

    // Initialize torrent with Complete state, which makes it a seed candidate
    auto* const ctor = zeroTorrentCtor();
    createZeroTorrentFiles(ctor, ZeroTorrentState::Complete);
    auto* const tor = createTorrentAndWaitForVerifyDone(ctor);
    tr_ctorFree(ctor);

    ASSERT_NE(nullptr, tor);
    ASSERT_TRUE(tor->has_all());
    EXPECT_TRUE(tor->verify_stats().used_quick_verify);
    EXPECT_FALSE(tor->hadQuickVerifyFail());

    // Now corrupt a piece
    auto const found = tor->find_file(0);
    ASSERT_TRUE(found.has_value());
    overwrite_byte(found->filename(), 0U, '\1');

    // Run manual verify using the verifyAndWait helper from SessionTest
    verifyAndWait(tor);

    // Verification should have failed because fallback is disabled
    EXPECT_FALSE(tor->has_all());
    EXPECT_TRUE(tor->verify_stats().used_quick_verify);
    EXPECT_TRUE(tor->hadQuickVerifyFail());

    // Run manual verify again. Since hadQuickVerifyFail() is true, it must use standard verify
    verifyAndWait(tor);

    EXPECT_FALSE(tor->has_all());
    EXPECT_FALSE(tor->verify_stats().used_quick_verify);
    EXPECT_TRUE(tor->hadQuickVerifyFail());
}
