#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(void)
{
  int pid;
  int ret;
  uint64 before_mem, after_mem;

  printf("=== mytest: start ===\n");

  before_mem = meminfo();
  printf("free memory before fork: %d\n", (int)before_mem);

  pid = getpid();
  printf("current pid = %d\n", pid);

  ret = getnice(pid);
  printf("getnice(%d) = %d\n", pid, ret);

  ret = setnice(pid, 10);
  printf("setnice(%d, 10) = %d\n", pid, ret);

  ret = getnice(pid);
  printf("getnice(%d) after setnice = %d\n", pid, ret);

  printf("\n=== parent ps(%d) ===\n", pid);
  ps(pid);

  int child = fork();

  if(child < 0){
    printf("fork failed\n");
    exit(1);
  }

  if(child == 0){
    int cpid = getpid();

    printf("\n=== child start ===\n");
    printf("child pid = %d\n", cpid);
    printf("getnice(%d) = %d\n", cpid, getnice(cpid));

    printf("setnice(%d, 5) = %d\n", cpid, setnice(cpid, 5));
    printf("getnice(%d) after setnice = %d\n", cpid, getnice(cpid));

    printf("\n=== child ps(%d) ===\n", cpid);
    ps(cpid);

    printf("child exiting\n");
    exit(0);
  }

  ret = waitpid(child);
  printf("\nparent: waitpid(%d) returned %d\n", child, ret);

  after_mem = meminfo();
  printf("free memory after child exit: %d\n", (int)after_mem);

  printf("\n=== ps(0): all processes after waitpid ===\n");
  ps(0);

  printf("=== mytest: end ===\n");
  exit(0);
}
