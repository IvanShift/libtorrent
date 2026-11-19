#include "helpers/test_fixture.h"

class test_torrent : public test_fixture {
  CPPUNIT_TEST_SUITE(test_torrent);
  CPPUNIT_TEST(test_cleanup_removes_downloads);
  CPPUNIT_TEST(test_download_add_zero_info_hash);
  CPPUNIT_TEST(test_download_find_unknown_hash);
  CPPUNIT_TEST(test_download_find_wrong_size);
  CPPUNIT_TEST(test_resume_extra_tracker);
  CPPUNIT_TEST(test_resume_extra_tracker_unavailable);
  CPPUNIT_TEST(test_single_file_name_utf8);
  CPPUNIT_TEST(test_start_keep_files);
  CPPUNIT_TEST(test_start_reopens_files);
  CPPUNIT_TEST(test_start_keep_files_with_advise);
  CPPUNIT_TEST(test_resume_progress_unchanged);
  CPPUNIT_TEST(test_resume_progress_bitfield_changed);
  CPPUNIT_TEST(test_resume_progress_files_changed);
  CPPUNIT_TEST(test_resume_progress_file_entry_changed);
  CPPUNIT_TEST(test_resume_progress_file_not_ready);
  CPPUNIT_TEST(test_open_evicts_files_of_earlier_downloads);
  CPPUNIT_TEST_SUITE_END();

public:
  void setUp() override;
  void tearDown() override;

  void test_cleanup_removes_downloads();
  void test_download_add_zero_info_hash();
  void test_download_find_unknown_hash();
  void test_download_find_wrong_size();
  void test_resume_extra_tracker();
  void test_resume_extra_tracker_unavailable();
  void test_single_file_name_utf8();
  void test_start_keep_files();
  void test_start_reopens_files();
  void test_start_keep_files_with_advise();
  void test_resume_progress_unchanged();
  void test_resume_progress_bitfield_changed();
  void test_resume_progress_files_changed();
  void test_resume_progress_file_entry_changed();
  void test_resume_progress_file_not_ready();
  void test_open_evicts_files_of_earlier_downloads();
};
