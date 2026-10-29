#include <unity.h>
#include "../../src/status_order.h"

void test_delayed_status_does_not_replace_newer_status() {
  StatusOrder order;
  TEST_ASSERT_TRUE(order.accept("bridge-a", 3));
  TEST_ASSERT_FALSE(order.accept("bridge-a", 2));
  TEST_ASSERT_TRUE(order.accept("bridge-a", 3));
  TEST_ASSERT_TRUE(order.accept("bridge-a", 4));
}

void test_bridge_restart_rejects_delayed_old_stream() {
  StatusOrder order;
  TEST_ASSERT_TRUE(order.accept("bridge-a", 100));
  TEST_ASSERT_TRUE(order.accept("bridge-b", 1));
  TEST_ASSERT_FALSE(order.accept("bridge-a", 101));
  TEST_ASSERT_FALSE(order.accept("", 1));
  TEST_ASSERT_FALSE(order.accept("bridge-b", 0));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_delayed_status_does_not_replace_newer_status);
  RUN_TEST(test_bridge_restart_rejects_delayed_old_stream);
  return UNITY_END();
}
