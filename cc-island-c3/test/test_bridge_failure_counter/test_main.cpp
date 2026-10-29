#include <unity.h>

#include "../../src/fetch_failure_counter.h"

void test_repeated_beacons_do_not_clear_fetch_failures() {
  FetchFailureCounter counter;
  counter.noteEndpointSeen(true);

  TEST_ASSERT_FALSE(counter.noteFailure());
  counter.noteEndpointSeen(false);
  TEST_ASSERT_EQUAL_UINT32(1, counter.count());
  TEST_ASSERT_FALSE(counter.noteFailure());
  counter.noteEndpointSeen(false);
  TEST_ASSERT_EQUAL_UINT32(2, counter.count());
  TEST_ASSERT_TRUE(counter.noteFailure());
  TEST_ASSERT_EQUAL_UINT32(3, counter.count());
}

void test_endpoint_change_and_success_reset_fetch_failures() {
  FetchFailureCounter counter;
  counter.noteFailure();
  counter.noteFailure();
  counter.noteEndpointSeen(true);
  TEST_ASSERT_EQUAL_UINT32(0, counter.count());

  counter.noteFailure();
  counter.reset();
  TEST_ASSERT_EQUAL_UINT32(0, counter.count());
  TEST_ASSERT_FALSE(counter.noteFailure());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_repeated_beacons_do_not_clear_fetch_failures);
  RUN_TEST(test_endpoint_change_and_success_reset_fetch_failures);
  return UNITY_END();
}
