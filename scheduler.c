#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/wait.h>
#include <sys/time.h>

#define MAX_PROCS 64
#define AUX_PROCS 4
#define PROC_NAME 20

int shmid, semid, msqid;

char procsList[MAX_PROCS][PROC_NAME];
int procs = 0, auxProcs = AUX_PROCS;
int* nextExecList;

int mode, status, parent;
int sigRcvd = 0, exitSig = 0;

union semun{
    int val;
    struct semid_ds* buf;
    ushort* array;
} sem_union;

struct message{
    long pid;
} msgbuf;

void shuffle(int*, int);
void createIPC(void);
void Psem(void);
void Vsem(void);
void notifyTerm(void);
void sigHandler(int);
void removeIPC(void);

int main(int argc, char* argv[]){
    if(argc < 2){
        printf("Incorret number of arguments\n");
        exit(1);
    }

    if(!strcmp(argv[1], "-N") || !strcmp(argv[1], "-n"))
        mode = 0;
    else if(!strcmp(argv[1], "-R") || !strcmp(argv[1], "-r"))
        mode = 1;
    else{
        printf("Unknown command\n");
        exit(1);
    }

    FILE* file = fopen(argv[2], "r");
    if(!file){
        printf("Unable to open %s\n", argv[1]);
        exit(1);
    }
    char line[PROC_NAME];
    while(fgets(line, sizeof(line), file)){
        line[strcspn(line, "\r\n")] = 0;
        strcpy(procsList[procs], line);
        procs++;
    }
    fclose(file);

    createIPC();
    parent = getpid();

    int stripId, pid;
    struct timeval start, end;
    gettimeofday(&start, NULL);
    for(int i = 0; i < AUX_PROCS; i++){
        if((pid = fork()) < 0){
            printf("Error in creating child process: %s\n", strerror(errno));
            exit(1);
        }
        else if(pid == 0){
            stripId = i;
            srand(time(NULL) + getpid());
            break;
        }
    }
    if(pid == 0){
        signal(SIGUSR1, sigHandler);
        signal(SIGUSR2, sigHandler);

        nextExecList = (int*) shmat(shmid, (char*) 0, 0);
        if(nextExecList == (int*) -1){
            printf("Error in attaching shared memory: %s\n", strerror(errno));
            exit(1);
        }

        int execProc;
        Psem();
            execProc = nextExecList[stripId];
            nextExecList[stripId] += AUX_PROCS;
        Vsem();

        while(execProc < procs){
            int pid;
            if((pid = fork()) < 0){
                printf("Error in creating child process: %s\n", strerror(errno));
                exit(1);
            }
            else if(pid == 0){
                if(execl(procsList[execProc], procsList[execProc], (char*) 0) < 0){
                    printf("Error in executing child process in strip %d: %s\n", stripId, strerror(errno));
                    exit(1);
                }
            }
            else{
                if(wait(&status) < 0){
                    printf("Error in waiting child to terminate: %s\n", strerror(errno));
                    exit(1);
                }
                else if(!WIFEXITED(status) || WEXITSTATUS(status) != 0)
                    exit(1);
            }
            Psem();
                execProc = nextExecList[stripId];
                nextExecList[stripId] += AUX_PROCS;
            Vsem();
        }

        notifyTerm();
        if(exitSig){
            gettimeofday(&end, NULL);
            double time = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
            printf("Process Aux %d time = %.2fs\n", stripId, time);
            exit(0);
        }

        int strips[AUX_PROCS - 1];
        while(!exitSig){
            shuffle(strips, stripId);
            for(int i = 0; i < AUX_PROCS - 1; i++){
                Psem();
                    execProc = nextExecList[strips[i]];
                    nextExecList[strips[i]] += AUX_PROCS;
                Vsem();
                if(execProc < procs)
                    break;
            }
            if(execProc >= procs)
                exitSig = 1;
            else{
                int pid;
                if((pid = fork()) < 0){
                    printf("Error in creating child process: %s\n", strerror(errno));
                    exit(1);
                }
                else if(pid == 0){
                    if(execl(procsList[execProc], procsList[execProc], (char*) 0) < 0){
                        printf("Error in executing child process in strip %d: %s\n", stripId, strerror(errno));
                        exit(1);
                    }
                }
                else{
                    if(wait(&status) < 0){
                        printf("Error in waiting child to terminate: %s\n", strerror(errno));
                        exit(1);
                    }
                    else if(!WIFEXITED(status) || WEXITSTATUS(status) != 0)
                        exit(1);
                }
                notifyTerm();
            }
        }
        gettimeofday(&end, NULL);
        double time = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
        printf("Process Aux %d time = %.2fs\n", stripId, time);
        exit(0);
    }
    else{
        while(auxProcs){
            if(msgrcv(msqid, &msgbuf, sizeof(msgbuf), 0, IPC_NOWAIT) < 0){
                if(errno != ENOMSG){
                    printf("Error in receiving message: %s\n", strerror(errno));
                    exit(1);
                }
            }
            else if(mode && auxProcs > 1)
                kill(msgbuf.pid, SIGUSR1);
            else
                kill(msgbuf.pid, SIGUSR2);

            int waitRet;
            if((waitRet = waitpid(-1, &status, WNOHANG)) < 0){
                printf("Error in waiting child to terminate: %s\n", strerror(errno));
                exit(1);
            }
            else if(waitRet > 0){
                if(!WIFEXITED(status) || WEXITSTATUS(status) != 0)
                    exit(1);
                auxProcs--;
            }
        }
        gettimeofday(&end, NULL);
        double time = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
        printf("Scheduler Makespan = %.2fs\n", time);
        removeIPC();
    }
    return 0;
}

void shuffle(int* arr, int idx){
    int cont = 0, n = AUX_PROCS - 1;
    for(int i = 0; i < n + 1; i++){
        if(i == idx)
            continue;
        arr[cont] = i;
        cont++;
    }
    for(int i = 0; i < n - 1; i++){
        int j = i + rand() / (RAND_MAX / (n - i) + 1);
        int temp = arr[j];
        arr[j] = arr[i];
        arr[i] = temp;
    }
}

void createIPC(){
    if((shmid = shmget(0x3126, sizeof(int) * AUX_PROCS, IPC_CREAT | 0777)) < 0){
        printf("Error in creating shared memory: %s\n", strerror(errno));
        exit(1);
    }
    nextExecList = (int*) shmat(shmid, (char*) 0, 0);
    if(nextExecList == (int*) -1){
        printf("Error in attaching shared memory: %s\n", strerror(errno));
        exit(1);
    }
    for(int i = 0; i < AUX_PROCS; i++)
        nextExecList[i] = i;
    if(shmdt(nextExecList) < 0){
        printf("Error in detaching shared memory: %s\n", strerror(errno));
        exit(1);
    }

    if((semid = semget(0x3126, 1, IPC_CREAT | 0777)) < 0){
        printf("Error in creating semaphore: %s\n", strerror(errno));
        exit(1);
    }
    sem_union.val = 1;
    if(semctl(semid, 0, SETVAL, sem_union) < 0){
        printf("Error in setting semaphore value: %s\n", strerror(errno));
        exit(1);
    }

    if((msqid = msgget(0x3126, IPC_CREAT | 0777)) < 0){
        printf("Error in creating message queue: %s\n", strerror(errno));
        exit(1);
    }
}

void Psem(){
    struct sembuf op;
    op.sem_num = 0;
    op.sem_op = -1;
    op.sem_flg = 0;
    if(semop(semid, &op, 1) < 0){
        printf("Error in operation P: %s\n", strerror(errno));
        exit(1);
    }
}

void Vsem(){
    struct sembuf op;
    op.sem_num = 0;
    op.sem_op = 1;
    op.sem_flg = 0;
    if(semop(semid, &op, 1) < 0){
        printf("Error in operation V: %s\n", strerror(errno));
        exit(1);
    }
}

void notifyTerm(){
    msgbuf.pid = getpid();
    if(msgsnd(msqid, &msgbuf, sizeof(msgbuf), 0) < 0){
        printf("Error in sending message: %s\n", strerror(errno));
        exit(1);
    }
    while(!sigRcvd){
        if(kill(parent, 0) < 0){
            printf("Parent process doesn't exist anymore\n");
            exit(1);
        }
    }
    sigRcvd = 0;
}

void sigHandler(int sig){
    sigRcvd = 1;
    if(sig == SIGUSR2)
        exitSig = 1;
}

void removeIPC(){
    if(shmctl(shmid, IPC_RMID, (struct shmid_ds*) NULL) < 0){
        printf("Error in removing shared memory: %s\n", strerror(errno));
        exit(1);
    }
    if(semctl(semid, 0, IPC_RMID, sem_union) < 0){
        printf("Error in removing semaphore: %s\n", strerror(errno));
        exit(1);
    }
    if(msgctl(msqid, IPC_RMID, (struct msqid_ds*) NULL) < 0){
        printf("Error in removing message queue: %s\n", strerror(errno));
        exit(1);
    }
}