#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <semaphore.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <time.h>

#define SEM_NAME "/medium_control_semaphore"
#define SEM_PERMS (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP)

int RECV_PROC_COUNT = 1;
int SENDER_PROC_COUNT = 2;
int INITIAL_SEM_VALUE = 1;
int ITERS = 5;
int M_TYPE = 1;
int WAIT_MAX = 5;
int WAIT_MIN = 1;

// structure for message queue
struct mesg_buffer
{
  long mesg_type;
  char mesg_text[200];
  unsigned long timestamp;
};

void child_sender(int id)
{
  // ----- CONNECT TO ACCESS CONTROL SEMAPHORE ----------------------
  printf("[Sender %d] Opening semaphore\n", id);
  sem_t *semaphore = sem_open(SEM_NAME, O_RDWR);
  if (semaphore == SEM_FAILED)
  {
    perror("[Sender] sem_open failed");
    exit(EXIT_FAILURE);
  }

  // ----- CONNECT TO MESSAGE QUEUE ---------------------------------
  printf("[Sender %d] Connecting to msg queue\n", id);
  key_t unique_key = ftok("progfile", 65);
  int msgid = msgget(unique_key, 0666 | IPC_CREAT);

  // ------ START SENDING -------------------------------------------
  for (int i = 0; i < ITERS; i++)
  {
    // Calculate a random timeout for the lock wait
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) == -1)
    {
      perror("clock_gettime in sender child");
      exit(EXIT_FAILURE);
    }
    srand(time(0));
    int rand_timeout = (rand() % (WAIT_MAX - WAIT_MIN + WAIT_MIN)) + WAIT_MIN; //max = 5, min = 1
    ts.tv_sec += rand_timeout;

    printf("[Sender %d] Attempting to acquire lock, will timeout after t = %d secs\n", id, rand_timeout);

    if (sem_timedwait(semaphore, &ts) < 0)
    {
      printf("[Sender %d] lock time out, will try again\n", id);
      i--;
      continue;
    }

    // If the process managed to acquire the lock, then it can send on the wire
    printf("[Sender %d] acquired lock, sending data [%d]\n", id, i + 1);
    struct mesg_buffer message;
    message.mesg_type = M_TYPE;
    sprintf(message.mesg_text, "random message from child %d", id);
    message.timestamp = (unsigned long)time(NULL);
    msgsnd(msgid, &message, sizeof(message), 0);

    sleep(1); //simulate work

    if (sem_post(semaphore) < 0)
    {
      perror("sem_post error on sender child");
    }
  }

  printf("[Sender %d] Closing semaphore\n", id);
  if (sem_close(semaphore) < 0)
  {
    perror("[Sender] sem_close failed");
    exit(EXIT_FAILURE);
  }

  printf("[Sender %d] Terminating\n", id);
}

void child_receiver()
{
  // ----- CONNECT TO MESSAGE QUEUE ---------------------------------
  printf("[Receiver] Connecting to msg queue\n");
  key_t unique_key = ftok("progfile", 65);
  int msgid = msgget(unique_key, 0666 | IPC_CREAT);

  // ------ START RECEIVING ------------------------------------------
  for (int i = 0; i < SENDER_PROC_COUNT * ITERS; i++)
  {
    // If the process managed to acquire the lock, then it can send on the wire
    struct mesg_buffer message;
    msgrcv(msgid, &message, sizeof(message), 1, 0);
    printf("[Receiver] received[%d] data = %s, timestamp = %lu\n", i + 1, message.mesg_text, message.timestamp);

    sleep(1);
  }

  printf("[Receiver] Terminating\n");
}

int main()
{
  sem_unlink(SEM_NAME);
  printf("[Parent] Process launched\n");

  // ----- CREATING ACCESS CONTROL SEMAPHORE ----------------------

  // Init a semaphore to be used by child processes as the medium access control method
  printf("[Parent] Creating Semaphore\n");
  sem_t *semaphore = sem_open(SEM_NAME, O_CREAT | O_EXCL, SEM_PERMS, INITIAL_SEM_VALUE);
  if (semaphore == SEM_FAILED)
  {
    perror("[Parent] sem_open error");
    exit(EXIT_FAILURE);
  }

  // Close the semaphore from the parent process as it wouldn't need to use it
  printf("[Parent] Closing semaphore\n");
  if (sem_close(semaphore) < 0)
  {
    perror("[Parent] sem_close failed");
    sem_unlink(SEM_NAME);
    exit(EXIT_FAILURE);
  }

  // ----- CREATING MESSAGE QUEUE --------------------------------
  printf("[Parent] Creating message queue\n");
  key_t unique_key;
  int msgid;

  // Generate a unique key
  unique_key = ftok("progfile", 65);

  // Create a message queue and return the identifier
  msgid = msgget(unique_key, 0666 | IPC_CREAT);

  // ----- FORKING PROCESSES -------------------------------------

  pid_t *child_pids = new pid_t[RECV_PROC_COUNT + SENDER_PROC_COUNT];

  for (int i = 0; i < RECV_PROC_COUNT; i++)
  {
    printf("[Parent] Forking recv child process\n");

    if ((child_pids[i] = fork()) < 0)
    {
      perror("[Parent] fork failed");
      exit(EXIT_FAILURE);
    }

    // Child process
    if (child_pids[i] == 0)
    {
      child_receiver();
      exit(0);
    }
  }

  for (int i = 1; i < RECV_PROC_COUNT + SENDER_PROC_COUNT; i++)
  {
    printf("[Parent] Forking sender child process %d\n", i);

    if ((child_pids[i] = fork()) < 0)
    {
      perror("[Parent] fork failed");
      exit(EXIT_FAILURE);
    }

    // Child process
    if (child_pids[i] == 0)
    {
      child_sender(i);
      exit(0);
    }
  }

  // Wait for child processes to finish
  printf("[Parent] Waiting for children to terminate\n");
  for (int i = 0; i < RECV_PROC_COUNT + SENDER_PROC_COUNT; i++)
  {
    if (waitpid(child_pids[i], NULL, 0) < 0)
    {
      perror("waitpid failed");
    }
  }

  sleep(1);

  printf("[Parent] Unlink semaphore\n");
  sem_unlink(SEM_NAME);

  printf("[Parent] Destroying message queue\n");
  msgctl(msgid, IPC_RMID, NULL);

  printf("[Parent] Process completed\n");
}
