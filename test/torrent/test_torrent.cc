#include "config.h"

#include "test_torrent.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <tuple>
#include <utility>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include "torrent/data/file.h"
#include "torrent/data/file_list.h"
#include "torrent/data/file_manager.h"
#include "torrent/download.h"
#include "torrent/download_info.h"
#include "torrent/exceptions.h"
#include "torrent/object.h"
#include "torrent/path.h"
#include "torrent/runtime/client_config.h"
#include "torrent/system/scheduler.h"
#include "torrent/torrent.h"
#include "torrent/tracker/tracker.h"
#include "torrent/utils/functional.h"
#include "torrent/utils/resume.h"

CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(test_torrent, "torrent");

namespace {

torrent::Object*
create_metafile(const std::string& name, int64_t length) {
  auto object = new torrent::Object(torrent::Object::create_map());
  auto& info = object->insert_key("info", torrent::Object::create_map());
  info.insert_key("name", name);
  info.insert_key("length", length);
  info.insert_key("piece length", int64_t{16384});
  info.insert_key("pieces", std::string(20, '\x01'));
  return object;
}

torrent::Object*
create_magnet(const std::string& uri) {
  auto object = new torrent::Object(torrent::Object::create_map());
  object->insert_key("magnet-uri", uri);
  return object;
}

// Removes the download when the scope ends, so a failed assertion does not leave it registered.
struct scoped_download {
  ~scoped_download() {
    if (download.is_valid())
      torrent::download_remove(download);
  }

  torrent::Download download;
};

struct scheduler_access : torrent::system::Scheduler {
  using torrent::system::Scheduler::perform;
};

struct checked_download {
  explicit checked_download(std::initializer_list<int64_t> lengths, const uint8_t* bitfield = nullptr) {
    CPPUNIT_ASSERT(::mkdtemp(base) != nullptr);
    CPPUNIT_ASSERT(::mkdir(root().c_str(), 0700) == 0);
    CPPUNIT_ASSERT(::mkdir((root() + "/a").c_str(), 0700) == 0);
    CPPUNIT_ASSERT(::mkdir((std::string(base) + "/elsewhere").c_str(), 0700) == 0);
    CPPUNIT_ASSERT(::symlink((std::string(base) + "/elsewhere").c_str(), (root() + "/s").c_str()) == 0);

    auto object = new torrent::Object(torrent::Object::create_map());
    auto& info  = object->insert_key("info", torrent::Object::create_map());
    auto& files = info.insert_key("files", torrent::Object::create_list()).as_list();
    int64_t total = 0;

    for (auto length : lengths) {
      auto& path = files.emplace_back(torrent::Object::create_map()).insert_key("path", torrent::Object::create_list()).as_list();
      path.emplace_back(std::string(files.size() % 2 == 1 ? "a" : "s"));
      path.emplace_back("f" + std::to_string(files.size() - 1));
      files.back().insert_key("length", length);

      std::ofstream(root() + "/" + path.front().as_string() + "/" + path.back().as_string()) << std::string(length, 'x');
      total += length;
    }

    info.insert_key("name", std::string(base + 29));
    info.insert_key("piece length", int64_t{16384});
    info.insert_key("pieces", std::string((total + 16383) / 16384 * 20, '\x01'));

    download = torrent::download_add(object, 0);
    download.file_list()->set_root_dir(root());
    download.open();

    if (bitfield == nullptr)
      download.set_bitfield(true);
    else
      download.set_bitfield(bitfield, bitfield + download.file_list()->bitfield()->size_bytes());

    CPPUNIT_ASSERT(download.hash_check(true));
    (torrent::this_thread::scheduler()->*&scheduler_access::perform)(torrent::this_thread::cached_time());
    CPPUNIT_ASSERT(download.is_hash_checked());
  }

  ~checked_download() {
    torrent::download_remove(download);
    std::filesystem::remove_all(base);
  }

  std::string root() const { return std::string(base) + "/data"; }

  char              base[40] = "/tmp/libtorrent_test_torrent_XXXXXX";
  torrent::Download download;
};

torrent::Object
saved_progress(torrent::Download download) {
  for (const auto& file : *download.file_list())
    file->unset_flags(torrent::File::flag_create_queued | torrent::File::flag_resize_queued);

  auto resume = torrent::Object::create_map();
  torrent::resume_save_progress(download, resume);
  return resume;
}

} // namespace

void
test_torrent::setUp() {
  test_fixture::setUp();
  mock_redirect_defaults();

  // torrent::initialize() needs an open file limit of at least 512.
  rlimit limit{};
  if (getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_cur < 1024 && limit.rlim_max >= 1024) {
    limit.rlim_cur = 1024;
    setrlimit(RLIMIT_NOFILE, &limit);
  }

  torrent::initialize_main_thread();
  torrent::initialize();
}

void
test_torrent::tearDown() {
  torrent::cleanup();

  test_fixture::tearDown();
}

void
test_torrent::test_cleanup_removes_downloads() {
  char root_dir[] = "/tmp/libtorrent_test_torrent_XXXXXX";
  CPPUNIT_ASSERT(::mkdtemp(root_dir) != nullptr);

  const std::string data_path = std::string(root_dir) + "/data";
  std::ofstream(data_path) << "data";

  torrent::download_add(create_magnet("magnet:?xt=urn:btih:" + std::string(40, '1')), 0);
  torrent::download_add(create_metafile("closed", 1), 0);

  // An open download whose chunk is queued for hashing on the disk thread.
  auto download = torrent::download_add(create_metafile("data", 4), 0);
  download.file_list()->set_root_dir(root_dir);
  download.open();
  download.hash_check(false);
  CPPUNIT_ASSERT(torrent::main_thread::hash_queue_size() == 1);

  // The downloads are still registered, so cleanup() has to remove them itself.
  CPPUNIT_ASSERT_NO_THROW(torrent::cleanup());

  std::remove(data_path.c_str());
  ::rmdir(root_dir);

  // Bring the library up again for tearDown().
  torrent::initialize_main_thread();
  torrent::initialize();
}

void
test_torrent::test_download_add_zero_info_hash() {
  const std::string zero_magnets[] = {
    "magnet:?xt=urn:btih:" + std::string(40, '0'),
    "magnet:?xt=urn:btih:" + std::string(32, 'A'),
    // A tag runs to the next '=', so "&xt" is not an exact topic and the zero hash is kept.
    "magnet:?xt=urn:btih:" + std::string(40, '0') + "&&xt=urn:btih:" + std::string(40, '1'),
  };

  for (const auto& uri : zero_magnets) {
    std::unique_ptr<torrent::Object> object(create_magnet(uri));
    CPPUNIT_ASSERT_THROW_MESSAGE(uri, torrent::download_add(object.get(), 0), torrent::input_error);
  }

  std::unique_ptr<torrent::Object> meta_download(new torrent::Object(torrent::Object::create_map()));
  auto& info = meta_download->insert_key("info", torrent::Object::create_map());
  info.insert_key("meta_download", int64_t{1});
  info.insert_key("name", std::string("zero.meta"));
  info.insert_key("pieces", std::string(20, '\0'));

  CPPUNIT_ASSERT_THROW(torrent::download_add(meta_download.get(), 0), torrent::input_error);

  // Only an all-zero hash is refused.
  const std::pair<std::string, std::string> valid_magnets[] = {
    { "magnet:?xt=urn:btih:" + std::string(38, '0') + "01", std::string(19, '\0') + '\x01' },
    { "magnet:?xt=urn:btih:" + std::string(40, '1'), std::string(20, '\x11') },
  };

  for (const auto& [uri, hash] : valid_magnets) {
    torrent::Download download;

    CPPUNIT_ASSERT_NO_THROW_MESSAGE(uri, download = torrent::download_add(create_magnet(uri), 0));
    CPPUNIT_ASSERT_MESSAGE(uri, download.info()->hash().str() == hash);
    torrent::download_remove(download);
  }
}

void
test_torrent::test_download_find_unknown_hash() {
  auto hash = std::string(20, '\x33');

  scoped_download added{torrent::download_add(create_magnet("magnet:?xt=urn:btih:" + std::string(40, '3')), 0)};
  CPPUNIT_ASSERT(torrent::download_find(hash).ptr() == added.download.ptr());

  torrent::download_remove(added.download);
  added.download = torrent::Download();

  CPPUNIT_ASSERT(!torrent::download_find(hash).is_valid());
  CPPUNIT_ASSERT(!torrent::download_find(std::string(20, '\x44')).is_valid());
}

void
test_torrent::test_download_find_wrong_size() {
  scoped_download added{torrent::download_add(create_magnet("magnet:?xt=urn:btih:" + std::string(38, '3') + "00"), 0)};

  // The terminating null of a 19 byte string would complete this hash.
  CPPUNIT_ASSERT(!torrent::download_find(std::string(19, '\x33')).is_valid());
  CPPUNIT_ASSERT(!torrent::download_find(std::string(38, '3') + "00").is_valid());
  CPPUNIT_ASSERT(!torrent::download_find("").is_valid());
  // A longer string that starts with the registered hash.
  CPPUNIT_ASSERT(!torrent::download_find(std::string(19, '\x33') + std::string(1, '\0') + "x").is_valid());
}

void
test_torrent::test_resume_extra_tracker() {
  const std::string url = "http://example.com/announce";

  auto              first_save  = torrent::Object::create_map();
  auto              second_save = torrent::Object::create_map();

  {
    scoped_download added{torrent::download_add(create_metafile("resume", 1), 0)};
    added.download.tracker_controller().add_extra_tracker(1, url);
    torrent::resume_save_tracker_settings(added.download, first_save);
  }

  {
    scoped_download added{torrent::download_add(create_metafile("resume", 1), 0)};
    torrent::resume_load_tracker_settings(added.download, first_save);
    torrent::resume_save_tracker_settings(added.download, second_save);
  }

  scoped_download added{torrent::download_add(create_metafile("resume", 1), 0)};
  torrent::resume_load_tracker_settings(added.download, second_save);

  auto tracker = added.download.tracker_controller().c_find_if([&url](const auto& t) { return t.url() == url; });

  CPPUNIT_ASSERT(tracker.is_valid());
  CPPUNIT_ASSERT(tracker.is_extra_tracker());
  CPPUNIT_ASSERT(tracker.group() == 1);
}

void
test_torrent::test_resume_extra_tracker_unavailable() {
  auto  resume = torrent::Object::create_map();
  auto& entry  = resume.insert_key("trackers", torrent::Object::create_map()).insert_key("dht://", torrent::Object::create_map());

  entry.insert_key("extra_tracker", int64_t{1});
  entry.insert_key("group", int64_t{0});

  scoped_download added{torrent::download_add(create_metafile("resume", 1), 0)};

  CPPUNIT_ASSERT_THROW(added.download.tracker_controller().add_extra_tracker(0, "dht://"), torrent::input_error);
  CPPUNIT_ASSERT_NO_THROW(torrent::resume_load_tracker_settings(added.download, resume));
  CPPUNIT_ASSERT(added.download.tracker_controller().size() == 0);
}

void
test_torrent::test_single_file_name_utf8() {
  auto config = torrent::runtime::client_config();
  const bool previous_utf8 = config->file_name_allow_legacy_utf8();
  const bool previous_sanitized = config->torrent_name_use_sanitized();
  const auto previous_replace = config->file_name_replace_slash();
  auto restore = torrent::utils::scope_guard([=]() {
      config->set_file_name_allow_legacy_utf8(previous_utf8);
      config->set_torrent_name_use_sanitized(previous_sanitized);
      config->set_file_name_replace_slash(previous_replace);
    });

  const std::string legacy = "episode-\xF1\xE5\xF0\xE8\xFF.mkv";
  const std::string utf8 = "episode-\xD1\x81\xD0\xB5\xD1\x80\xD0\xB8\xD1\x8F.mkv";
  struct name_case {
    std::string name;
    std::string alias;
    std::string raw_path;
    std::string alias_path;
  };
  const name_case cases[] = {
    { legacy, utf8, legacy, utf8 },
    { "raw/episode.mkv", "utf8/episode.mkv", "raw_episode.mkv", "utf8_episode.mkv" },
    { "raw/episode.mkv", "", "raw_episode.mkv", "raw_episode.mkv" },
  };

  config->set_file_name_replace_slash("_");
  for (bool allow_utf8 : {false, true}) {
    config->set_file_name_allow_legacy_utf8(allow_utf8);
    for (bool use_sanitized : {false, true}) {
      config->set_torrent_name_use_sanitized(use_sanitized);
      for (const auto& entry : cases) {
        std::unique_ptr<torrent::Object> object(create_metafile(entry.name, 1));
        if (!entry.alias.empty())
          object->get_key("info").insert_key("name.utf-8", entry.alias);
        scoped_download added{torrent::download_add(object.get(), 0)};
        object.release();

        const auto path = added.download.file_list()->front()->path();
        const auto& expected = allow_utf8 && !entry.alias.empty() ? entry.alias_path : entry.raw_path;
        CPPUNIT_ASSERT(path->size() == 1);
        CPPUNIT_ASSERT_EQUAL(expected, path->front().str());
      }
    }
  }
}

void
test_torrent::test_start_keep_files() {
  checked_download checked({4, 4});
  auto             opened = torrent::file_manager()->files_opened_counter();

  checked.download.start(torrent::Download::start_keep_files);

  CPPUNIT_ASSERT_EQUAL(opened, torrent::file_manager()->files_opened_counter());

  for (const auto& file : *checked.download.file_list()) {
    CPPUNIT_ASSERT(file->is_open());
    CPPUNIT_ASSERT(!file->is_create_queued());
  }

  auto links = checked.download.file_list()->indirect_links();
  CPPUNIT_ASSERT(std::find(links->begin(), links->end(), checked.root() + "/s") != links->end());
}

void
test_torrent::test_start_reopens_files() {
  checked_download checked({4, 4});
  auto             opened = torrent::file_manager()->files_opened_counter();

  checked.download.start(0);

  CPPUNIT_ASSERT_EQUAL(opened + 2, torrent::file_manager()->files_opened_counter());
}

void
test_torrent::test_start_keep_files_with_advise() {
  const std::tuple<bool, bool, uint64_t> cases[] = {
    { false, false, 0 },
    { true, true, 0 },
    { true, false, 2 },
    { false, true, 2 },
  };

  for (const auto& [random, random_hashing, reopened] : cases) {
    torrent::file_manager()->set_advise_random(random);
    torrent::file_manager()->set_advise_random_hashing(random_hashing);

    checked_download checked({4, 4});
    auto             opened = torrent::file_manager()->files_opened_counter();

    checked.download.start(torrent::Download::start_keep_files);

    CPPUNIT_ASSERT_EQUAL(opened + reopened, torrent::file_manager()->files_opened_counter());
  }
}

void
test_torrent::test_resume_progress_unchanged() {
  const uint8_t    partial[] = { 0x80 };
  const uint8_t    none[]    = { 0x00 };
  checked_download full({100, 200});
  checked_download half({16384, 16384}, partial);
  checked_download empty({16384, 16384}, none);
  auto             full_resume  = saved_progress(full.download);
  auto             half_resume  = saved_progress(half.download);
  auto             empty_resume = saved_progress(empty.download);

  CPPUNIT_ASSERT_EQUAL(int64_t{1}, full_resume.get_key_value("bitfield"));
  CPPUNIT_ASSERT(torrent::resume_progress_unchanged(full.download, full_resume));
  CPPUNIT_ASSERT(half_resume.has_key_string("bitfield"));
  CPPUNIT_ASSERT(torrent::resume_progress_unchanged(half.download, half_resume));
  CPPUNIT_ASSERT_EQUAL(int64_t{0}, empty_resume.get_key_value("bitfield"));
  CPPUNIT_ASSERT(torrent::resume_progress_unchanged(empty.download, empty_resume));
}

void
test_torrent::test_resume_progress_bitfield_changed() {
  const uint8_t    partial[] = { 0x80 };
  checked_download full({100, 200});
  checked_download half({16384, 16384}, partial);

  for (const torrent::Object& bitfield : { torrent::Object(int64_t{0}), torrent::Object(std::string(1, '\x80')) }) {
    auto resume = saved_progress(full.download);
    resume.insert_key("bitfield", bitfield);
    CPPUNIT_ASSERT(!torrent::resume_progress_unchanged(full.download, resume));
  }

  for (const torrent::Object& bitfield : { torrent::Object(int64_t{1}), torrent::Object(std::string(1, '\x40')) }) {
    auto resume = saved_progress(half.download);
    resume.insert_key("bitfield", bitfield);
    CPPUNIT_ASSERT(!torrent::resume_progress_unchanged(half.download, resume));
  }

  auto missing = saved_progress(full.download);
  missing.erase_key("bitfield");
  CPPUNIT_ASSERT(!torrent::resume_progress_unchanged(full.download, missing));
}

void
test_torrent::test_resume_progress_files_changed() {
  checked_download checked({100, 200});

  auto shorter = saved_progress(checked.download);
  shorter.get_key_list("files").pop_back();
  CPPUNIT_ASSERT(!torrent::resume_progress_unchanged(checked.download, shorter));

  auto longer = saved_progress(checked.download);
  longer.get_key_list("files").push_back(longer.get_key_list("files").back());
  CPPUNIT_ASSERT(!torrent::resume_progress_unchanged(checked.download, longer));

  auto missing = saved_progress(checked.download);
  missing.erase_key("files");
  CPPUNIT_ASSERT(!torrent::resume_progress_unchanged(checked.download, missing));

  auto value = saved_progress(checked.download);
  value.get_key_list("files").front() = int64_t{0};
  CPPUNIT_ASSERT(!torrent::resume_progress_unchanged(checked.download, value));
}

void
test_torrent::test_resume_progress_file_entry_changed() {
  checked_download checked({100, 200});

  for (int64_t mtime : { ~int64_t{0}, ~int64_t{1}, ~int64_t{2}, ~int64_t{3} }) {
    auto resume = saved_progress(checked.download);
    resume.get_key_list("files").front().insert_key("mtime", mtime);
    CPPUNIT_ASSERT(!torrent::resume_progress_unchanged(checked.download, resume));
  }

  auto completed = saved_progress(checked.download);
  completed.get_key_list("files").front().insert_key("completed", int64_t{0});
  CPPUNIT_ASSERT(!torrent::resume_progress_unchanged(checked.download, completed));

  for (const char* key : { "mtime", "completed" }) {
    auto resume = saved_progress(checked.download);
    resume.get_key_list("files").front().erase_key(key);
    CPPUNIT_ASSERT(!torrent::resume_progress_unchanged(checked.download, resume));
  }
}

void
test_torrent::test_resume_progress_file_not_ready() {
  checked_download empty({100, 0});
  CPPUNIT_ASSERT(!torrent::resume_progress_unchanged(empty.download, saved_progress(empty.download)));

  checked_download resized({100, 200});
  auto             resume = saved_progress(resized.download);
  resized.download.file_list()->at(1)->set_flags(torrent::File::flag_resize_queued);
  CPPUNIT_ASSERT(!torrent::resume_progress_unchanged(resized.download, resume));
}

void
test_torrent::test_open_evicts_files_of_earlier_downloads() {
  torrent::file_manager()->set_max_open_files(4);

  checked_download earlier({4, 4});
  earlier.download.start(torrent::Download::start_keep_files);
  checked_download later({4, 4});
  later.download.start(torrent::Download::start_keep_files);

  checked_download latest({4, 4, 4});
  auto             opened = torrent::file_manager()->files_opened_counter();

  latest.download.start(torrent::Download::start_keep_files);

  CPPUNIT_ASSERT_EQUAL(opened, torrent::file_manager()->files_opened_counter());

  for (const auto& file : *earlier.download.file_list())
    CPPUNIT_ASSERT(!file->is_open());
}
