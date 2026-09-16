/* First-fault publication is immutable while its process lifetime is held. */
#include "process.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <threads.h>
static process_t proc;
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
  fprintf(stderr, "FAULT RECORD FAIL line %d: %s\n", __LINE__, #x); exit(1); \
} } while (0)
static void *publish(void *unused) {
  (void)unused;
  process_record_fault(&proc, 3, 0x92000046u, USER_VA_BASE + 0x138, 0, 0x60000340u);
  return NULL;
}
int main(void) {
  process_fault_t value = {0};
  CHECK(!process_fault_snapshot(NULL, &value));
  CHECK(!process_fault_snapshot(&proc, NULL));
  CHECK(!process_fault_snapshot(&proc, &value));
  process_record_fault(NULL, 0, 0, 0, 0, 0);
  for (unsigned cycle = 0; cycle < 500; ++cycle) {
    proc = (process_t){0}; /* Only after the old publisher/readers are joined. */
    pthread_t writer;
    CHECK(!pthread_create(&writer, NULL, publish, NULL));
    while (!process_fault_snapshot(&proc, &value)) thrd_yield();
    CHECK(value.cpu == 3 && value.esr == 0x92000046u &&
          value.elr == USER_VA_BASE + 0x138 && value.far == 0 &&
          value.spsr == 0x60000340u);
    CHECK(!pthread_join(writer, NULL));
    process_record_fault(&proc, 1, 1, 1, 1, 1);
    CHECK(process_fault_snapshot(&proc, &value));
    CHECK(value.cpu == 3 && value.esr == 0x92000046u && value.far == 0);
  }
  puts("M11 FAULT RECORD: PASS (500 release/acquire publication and reuse cycles)");
  return 0;
}
