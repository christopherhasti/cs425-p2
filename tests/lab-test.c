#include <stdlib.h>
#include <stdio.h>
#include "harness/unity.h"
#include "../src/lab.h"


void setUp(void) {
  printf("Setting up tests...\n");
}

void tearDown(void) {
  printf("Tearing down tests...\n");
}

void test_get_greeting(void) {
  char *greeting = get_greeting("Alice");
  TEST_ASSERT_NOT_NULL(greeting);
  TEST_ASSERT_EQUAL_STRING("Hello, Alice!", greeting);
  free(greeting); // Free the allocated memory for the greeting

  greeting = get_greeting(NULL);
  TEST_ASSERT_NULL(greeting);

  greeting = get_greeting("");
  TEST_ASSERT_NOT_NULL(greeting);
  TEST_ASSERT_EQUAL_STRING("Hello, !", greeting);
  free(greeting);
}

void test_multiply_positive(void) {
  TEST_ASSERT_EQUAL_INT(12, multiply(3, 4));
  TEST_ASSERT_EQUAL_INT(1, multiply(1, 1));
}

void test_multiply_zero(void) {
  TEST_ASSERT_EQUAL_INT(0, multiply(0, 42));
  TEST_ASSERT_EQUAL_INT(0, multiply(42, 0));
}

void test_multiply_negative(void) {
  TEST_ASSERT_EQUAL_INT(-20, multiply(-4, 5));
  TEST_ASSERT_EQUAL_INT(20, multiply(-4, -5));
}

void test_bad_add_is_off_by_one(void) {
  // bad_add is documented to return the wrong answer (sum + 1),
  // so the test asserts the wrong answer on purpose.
  TEST_ASSERT_EQUAL_INT(6, bad_add(2, 3));
  TEST_ASSERT_EQUAL_INT(1, bad_add(0, 0));
  TEST_ASSERT_EQUAL_INT(-4, bad_add(-2, -3));
}

void test_bad_add_does_not_match_real_sum(void) {
  int a = 7;
  int b = 8;
  TEST_ASSERT_NOT_EQUAL_INT(a + b, bad_add(a, b));
  TEST_ASSERT_EQUAL_INT(a + b + 1, bad_add(a, b));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_get_greeting);
  RUN_TEST(test_multiply_positive);
  RUN_TEST(test_multiply_zero);
  RUN_TEST(test_multiply_negative);
  RUN_TEST(test_bad_add_is_off_by_one);
  RUN_TEST(test_bad_add_does_not_match_real_sum);
  return UNITY_END();
}