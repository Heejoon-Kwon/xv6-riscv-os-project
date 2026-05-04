#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define FAIR_LOOP      250000000
#define LAGGER_LOOP    400000000
#define SLEEPER_LOOP   250000000

static void
busy_loop(int n)
{
  volatile int x = 0;
  for(int i = 0; i < n; i++)
    x += i;

  if(x == 123456789)
    printf("never\n");
}

static void
must_read_one(int fd)
{
  char c;
  if(read(fd, &c, 1) != 1){
    printf("read failed\n");
    exit(1);
  }
}

static void
must_write_one(int fd)
{
  char c = 'A';
  if(write(fd, &c, 1) != 1){
    printf("write failed\n");
    exit(1);
  }
}

static void
fairness_phase(void)
{
  int fds[3][2];
  int pids[3];

  printf("\n================ fairness phase ================\n");
  printf("Goal: 3 CPU-bound children run together.\n");
  printf("Check whether runtime follows priority.\n");
  printf("Expected: nice 5 > nice 20 > nice 35 in runtime.\n");

  for(int i = 0; i < 3; i++){
    if(pipe(fds[i]) < 0){
      printf("pipe failed\n");
      exit(1);
    }
  }

  for(int i = 0; i < 3; i++){
    int pid = fork();
    if(pid < 0){
      printf("fork failed\n");
      exit(1);
    }

    if(pid == 0){
      for(int j = 0; j < 3; j++){
        close(fds[j][1]);
        if(j != i)
          close(fds[j][0]);
      }

      if(i == 0)
        setnice(getpid(), 5);
      else if(i == 1)
        setnice(getpid(), 20);
      else
        setnice(getpid(), 35);

      printf("[fair child %d] pid=%d nice=%d waiting for start\n",
             i, getpid(), getnice(getpid()));

      must_read_one(fds[i][0]);
      close(fds[i][0]);

      busy_loop(FAIR_LOOP);

      printf("\n[fair child %d] final ps(%d)\n", i, getpid());
      ps(getpid());
      exit(0);
    }

    pids[i] = pid;
  }

  for(int i = 0; i < 3; i++)
    close(fds[i][0]);

  pause(20);

  printf("\nRole map\n");
  printf(" child0 pid=%d nice=5\n", pids[0]);
  printf(" child1 pid=%d nice=20\n", pids[1]);
  printf(" child2 pid=%d nice=35\n", pids[2]);

  printf("\nRelease all fairness children\n");
  for(int i = 0; i < 3; i++){
    must_write_one(fds[i][1]);
    close(fds[i][1]);
  }

  pause(80);

  printf("\n=== fairness snapshot: ps(0) ===\n");
  ps(0);

  wait(0);
  wait(0);
  wait(0);

  printf("================ fairness phase end ================\n");
}

static void
eligibility_phase(void)
{
  int s1[2], s2[2];
  int sleeper1, sleeper2, lagger;

  printf("\n================ eligibility phase ================\n");
  printf("Goal: multiple RUNNING/RUNNABLE processes coexist.\n");
  printf("Check whether is_eligible really differs across them.\n");

  if(pipe(s1) < 0 || pipe(s2) < 0){
    printf("pipe failed\n");
    exit(1);
  }

  sleeper1 = fork();
  if(sleeper1 < 0){
    printf("fork failed\n");
    exit(1);
  }
  if(sleeper1 == 0){
    close(s1[1]);
    close(s2[0]);
    close(s2[1]);

    setnice(getpid(), 20);
    printf("[sleeper1] pid=%d nice=%d blocking on read\n",
           getpid(), getnice(getpid()));

    must_read_one(s1[0]);
    close(s1[0]);

    printf("[sleeper1] woke up, now busy loop\n");
    busy_loop(SLEEPER_LOOP);

    printf("\n[sleeper1] final ps(%d)\n", getpid());
    ps(getpid());
    exit(0);
  }

  sleeper2 = fork();
  if(sleeper2 < 0){
    printf("fork failed\n");
    exit(1);
  }
  if(sleeper2 == 0){
    close(s2[1]);
    close(s1[0]);
    close(s1[1]);

    setnice(getpid(), 20);
    printf("[sleeper2] pid=%d nice=%d blocking on read\n",
           getpid(), getnice(getpid()));

    must_read_one(s2[0]);
    close(s2[0]);

    printf("[sleeper2] woke up, now busy loop\n");
    busy_loop(SLEEPER_LOOP);

    printf("\n[sleeper2] final ps(%d)\n", getpid());
    ps(getpid());
    exit(0);
  }

  lagger = fork();
  if(lagger < 0){
    printf("fork failed\n");
    exit(1);
  }
  if(lagger == 0){
    close(s1[0]); close(s1[1]);
    close(s2[0]); close(s2[1]);

    setnice(getpid(), 35);
    printf("[lagger] pid=%d nice=%d running alone first\n",
           getpid(), getnice(getpid()));

    busy_loop(LAGGER_LOOP);

    printf("\n[lagger] final ps(%d)\n", getpid());
    ps(getpid());
    exit(0);
  }

  close(s1[0]);
  close(s2[0]);

  pause(80);

  printf("\n=== snapshot before wakeup ===\n");
  ps(0);

  printf("\nWake both sleepers\n");
  must_write_one(s1[1]);
  must_write_one(s2[1]);
  close(s1[1]);
  close(s2[1]);

  pause(30);

  printf("\n=== snapshot after wakeup ===\n");
  ps(0);

  wait(0);
  wait(0);
  wait(0);

  printf("================ eligibility phase end ================\n");
}

int
main(void)
{
  printf("=== improved eevdf_test start ===\n");

  fairness_phase();
  eligibility_phase();

  printf("\n=== final snapshot ===\n");
  ps(0);

  printf("=== improved eevdf_test end ===\n");
  exit(0);
}
