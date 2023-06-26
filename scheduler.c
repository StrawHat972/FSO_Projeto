/*
    Universidade de Brasília
    Alunos:
        Eduardo Xavier Dantas           - 190086530
        João Víctor Siqueira de Araujo  - 190031026
        Léo Akira Abe Barros            - 180137531
        Thiago Masera Tokarski          - 190096063
    Semestre: 2023/1
    Disciplina: Fundamentos de Sistemas Operacionais - CIC0205
    Professora: Alba Cristina Magalhães Alves de Melo

    Para compilar o projeto utilize o seguinte comando:
        gcc -Wall -o scheduler scheduler.c

    Comandos para executar o Escalonador:
        ./scheduler -n <arquivo_entrada> (executa no Modo Normal)
        ./scheduler -r <arquivo_entrada> (executa no Modo Roubo De Trabalho)
    
    Observação: Para executar esse programa precisa passar um arquivo (<arquivo_entrada>) contendo o nome 
    dos programas executáveis, um em cada linha. Os programas rapido.c, medio.c e demorado.c devem ser
    compilados antes da execução do Escalonador e os nomes de seus executáveis devem estar contidos no
    <arquivo_entrada>. O projeto foi testado no Ubuntu (20.04.6) usando gcc 9.4.0.

    Ideia geral do Escalonador:
        Existe um vetor em memória compartilhada que guarda o índice do próximo processo que será executado pelo
    processo auxiliar correspondente, ou seja, o próximo processo que o processo auxiliar i irá executar é o
    processo com índice vetor[i]. Todos os processos lidos do <arquivo_entrada> são guardados em um vetor no qual
    cada posição contém o nome de um arquivo executável. Se o próximo processo que um auxiliar irá executar for um
    com índice maior ou igual ao número total de nomes lidos do <arquivo_entrada>, isso quer dizer que o índice
    desse processo é inválido e o auxiliar já executou todos processos atribuídos (fila está vazia).

        Quando isso acontece o auxiliar envia ao processo pai uma mensagem através da fila de mensagens avisando que
    terminou de executar os processos atribuídos. Se o Escalonador foi executado em Modo Normal, ele envia um sinal
    SIGUSR2 para o processo auxiliar, esse sinal indica que o processo pode encerrar sua execução. Caso o Escalonador
    tenha sido executado em Modo Roubo De Trabalho, ele verifica se o processo que terminou é o último, caso não
    seja, o processo pai envia um sinal SIGUSR1 para o processo auxiliar, indicando que ele deve continuar executando
    e deve entrar em modo roubo.

        O processo auxiliar, então, escolhe aleatoriamente um dos outros auxiliares e pega o índice contido no vetor
    em memória compartilhada da posição do processo escolhido. Se for um índice válido, executa o processo. Caso 
    contrário, ele escolhe outro processo auxiliar e fica fazendo isso até encontrar um índice válido ou acabar
    os processos auxiliares a serem escolhidos, nesse último caso o processo se encerra. Ao terminar de executar o
    processo roubado, o auxiliar avisa o processo pai que terminou a execução por meio de uma mensagem e, então,
    todo o procedimento explicado anteriormente se repete até que todos os processos auxiliares se encerrem.
*/
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

#define MAX_PROCS 64 //Número máximo de processos que podem ser passados para o Escalonador
#define AUX_PROCS 4  //Número de processos auxiliares (No caso do projeto, são 4)
#define PROC_NAME 20 //Tamanho máximo dos nomes dos executáveis (No caso, 'demorado' é o que tem maior nome)

//Identificadores dos mecanismos IPC
int shmid, semid, msqid;

//Vetor que guarda o nome dos executáveis lidos do <arquivo_entrada>
char procsList[MAX_PROCS][PROC_NAME];

//procs é a quantidade de processos lidos de <arquivo_entrada> e auxProcs é a de auxiliares ainda em execução
int procs = 0, auxProcs = AUX_PROCS;

//Serve como ponteiro para o vetor em memória compartilhada
int* nextExecList;

//mode indica o modo do Escalonador, status é o valor passado pelo wait e parent é o ppid original dos auxiliares
int mode, status, parent;

//Ambos são flags para o processo auxiliar. sigRcvd indica que recebeu um sinal e exitSig que o sinal foi de término
int sigRcvd = 0, exitSig = 0;

//Union usada pela primitiva semctl
union semun{
    int val;
    struct semid_ds* buf;
    ushort* array;
} sem_union;

//Estrutura das mensagens enviadas ao processo pai
struct message{
    long pid; //No caso, o auxiliar envia apenas o seu pid para o pai quando avisa que terminou o seu trabalho
} msgbuf;

//Função que gera um vetor embaralhado com os índices dos processos auxiliares diferentes de idx
void shuffle(int* arr, int idx){
    int cont = 0, n = AUX_PROCS - 1;
    //Se idx = 2, o vetor gerado será [0, 1, 3] no caso do projeto
    for(int i = 0; i < n + 1; i++){
        if(i == idx)
            continue;
        arr[cont] = i;
        cont++;
    }
    //Aqui é feito o embaralhamento dos elementos do vetor
    for(int i = 0; i < n - 1; i++){
        int j = i + rand() / (RAND_MAX / (n - i) + 1);
        int temp = arr[j];
        arr[j] = arr[i];
        arr[i] = temp;
    }
}

//Função que cria os mecanismos IPC e inicializa o vetor em memória compartilhada
void createIPC(){
    //Cria um segmento de memória compartilhada
    if((shmid = shmget(0x3126, sizeof(int) * AUX_PROCS, IPC_CREAT | 0777)) < 0){
        printf("Error in creating shared memory: %s\n", strerror(errno));
        exit(1);
    }
    //Mapea esse vetor e passa o ponteiro para nextExecList
    nextExecList = (int*) shmat(shmid, (char*) 0, 0);
    if(nextExecList == (int*) -1){
        printf("Error in attaching shared memory: %s\n", strerror(errno));
        exit(1);
    }
    //Inicializa o vetor com os índices dos processos que cada auxiliar irá executar primeiro
    for(int i = 0; i < AUX_PROCS; i++)
        nextExecList[i] = i;
    //Desmapea o vetor, pois o pai não acessa mais esse segmento de memória compartilhada
    if(shmdt(nextExecList) < 0){
        printf("Error in detaching shared memory: %s\n", strerror(errno));
        exit(1);
    }

    //Cria um conjunto de um semáforo
    if((semid = semget(0x3126, 1, IPC_CREAT | 0777)) < 0){
        printf("Error in creating semaphore: %s\n", strerror(errno));
        exit(1);
    }
    //Seta o valor inicial do semáforo para 1 para ele ficar igual ao semáforo de Dijsktra
    sem_union.val = 1;
    if(semctl(semid, 0, SETVAL, sem_union) < 0){
        printf("Error in setting semaphore value: %s\n", strerror(errno));
        exit(1);
    }

    //Cria a fila de mensagens na qual os auxiliares irão se comunicar com o pai
    if((msqid = msgget(0x3126, IPC_CREAT | 0777)) < 0){
        printf("Error in creating message queue: %s\n", strerror(errno));
        exit(1);
    }
}

//Função que realiza a operação de aquisição (P) do semáforo
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

//Função que realiza a operação de liberação (V) do semáforo
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

//Função que irá tratar o recebimento de sinais SIGUSR1 e SIGUSR2
void sigHandler(int sig){
    //Se um sinal desse tipo for recebido, seta a flag de sinal recebido
    sigRcvd = 1;
    /* SIGUSR2 é um sinal para o auxiliar encerrar sua execução.
    Só é recebido se está no Modo Normal ou quando é o último processo auxiliar no Modo Roubo De Trabalho */
    if(sig == SIGUSR2)
        exitSig = 1;
}

/* Função que notifica o processo pai que o auxiliar terminou de fazer seu "trabalho".
Trabalho é a execução dos processos atribuídos ou a execução do processo roubado */
void notifyTerm(){
    msgbuf.pid = getpid();
    //Envia ao pai uma mensagem que contém apenas seu pid para que o pai possa enviar um sinal de volta
    if(msgsnd(msqid, &msgbuf, sizeof(msgbuf), 0) < 0){ //A mensagem enviada, por si só, indica o término do trabalho
        printf("Error in sending message: %s\n", strerror(errno));
        exit(1);
    }
    //O processo auxiliar fica em Busy Waiting enquanto o pai não responder enviando o sinal
    while(!sigRcvd){
        //O auxiliar fica verificando se o pai ainda existe, se não existe ele dá exit (tratamento de erros)
        if(kill(parent, 0) < 0){
            printf("Parent process doesn't exist anymore\n");
            exit(1);
        }
    }
    //Reseta a flag de sinal recebido quando sair do Busy Waiting
    sigRcvd = 0;
}

//Função que remove os mecanismos IPC
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

//Programa do Escalonador
int main(int argc, char* argv[]){
    //Precisa receber no mínimo 2 argumentos (Modo de execução e <arquivo_entrada>)
    if(argc < 2){
        printf("Incorret number of arguments\n");
        exit(1);
    }

    //O modo de execução é indicado por '-n' (Modo Normal) ou '-r' (Modo Roubo De Trabalho)
    if(!strcmp(argv[1], "-N") || !strcmp(argv[1], "-n"))
        mode = 0;
    else if(!strcmp(argv[1], "-R") || !strcmp(argv[1], "-r"))
        mode = 1;
    else{
        printf("Unknown command: %s\n", argv[1]);
        exit(1);
    }

    //Lê o <arquivo_entrada> para obter os nomes dos arquivos executáveis
    FILE* file = fopen(argv[2], "r");
    if(!file){
        printf("Unable to open file %s\n", argv[2]);
        exit(1);
    }
    char line[PROC_NAME];
    //Recupera os nomes dos executáveis em cada linha do arquivo
    while(fgets(line, sizeof(line), file)){
        line[strcspn(line, "\r\n")] = 0; //Desconsidera line feed e new line caso exista 
        strcpy(procsList[procs], line);
        procs++; //procs conterá a quantidade total de processos a serem executados
    }
    fclose(file);

    createIPC();
    parent = getpid(); //Parent vai sempre conter o pid do processo inicial (Pai)

    //stripId indica o índice do strip do auxiliar. Se stripId = 0, o auxiliar executa 0, 4, 8, ...
    int stripId, pid;

    //Estruturas usadas para contar o tempo de execução dos processos
    struct timeval start, end;
    gettimeofday(&start, NULL); //Inicia o contador
    
    //Loop para criar os processos auxiliares
    for(int i = 0; i < AUX_PROCS; i++){
        if((pid = fork()) < 0){
            printf("Error in creating child process: %s\n", strerror(errno));
            exit(1);
        }
        else if(pid == 0){
            stripId = i; //Seta o stripId de cada processo auxiliar
            srand(time(NULL) + getpid()); //Seta uma seed randômica diferente para cada processo auxiliar
            break;
        }
    }
    if(pid == 0){
        /* Código dos processos auxiliares (Processos Filhos) */

        //Muda o tratamento dos sinais SIGUSR1 e SIGUSR2 para ser a função sigHandler
        signal(SIGUSR1, sigHandler);
        signal(SIGUSR2, sigHandler);

        //Mapea o vetor em memória compartilhada
        nextExecList = (int*) shmat(shmid, (char*) 0, 0);
        if(nextExecList == (int*) -1){
            printf("Error in attaching shared memory: %s\n", strerror(errno));
            exit(1);
        }

        int execProc; //Indica qual é o processo (índice) que o auxiliar está executando
        //Para acessar a memória compartilhada é preciso usar o semáforo por causa da condição de corrida
        Psem();
            execProc = nextExecList[stripId];   //Recupera o índice do processo a ser executado
            nextExecList[stripId] += AUX_PROCS; //Seta o índice do próximo processo a ser executado
        Vsem();

        //Fica nesse loop enquanto execProc for um índice válido, ou seja, existe processos a serem executados
        while(execProc < procs){
            int pid;
            //Cria um proceso filho
            if((pid = fork()) < 0){
                printf("Error in creating child process: %s\n", strerror(errno));
                exit(1);
            }
            else if(pid == 0){
                //O processo filho vai executar o processo de índice execProc
                if(execl(procsList[execProc], procsList[execProc], (char*) 0) < 0){
                    printf("Error in executing child process in strip %d: %s\n", stripId, strerror(errno));
                    exit(1);
                }
            }
            else{
                //Enquanto o filho executa o processo, o pai fica em wait
                if(wait(&status) < 0){
                    printf("Error in waiting child to terminate: %s\n", strerror(errno));
                    exit(1);
                }
                //Se o processo filho terminou por causa de erro, o auxiliar encerra com erro (exit 1)
                else if(!WIFEXITED(status) || WEXITSTATUS(status) != 0)
                    exit(1);
            }
            //Depois de executar o processo, recupera o próximo processo da "fila"
            Psem();
                execProc = nextExecList[stripId];
                nextExecList[stripId] += AUX_PROCS;
            Vsem();
        }

        //Ao terminar de executar todos os processos atribuídos, informa o pai sobre o término
        notifyTerm();
        //Se recebeu um sinal SIGUSR2, então a flag exitSig foi setada e o processo se encerra
        if(exitSig){
            //Encerra o contador, converte o resultado para segundos e encerra a execução
            gettimeofday(&end, NULL);
            double time = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
            printf("Process Aux %d time = %.2fs\n", stripId, time);
            exit(0);
        }

        //Se não recebeu um sinal SIGUSR2, então ele entra em modo roubo
        int strips[AUX_PROCS - 1]; //Vetor que conterá os stripIds embaralhados dos outros processos auxiliares
        //Enquanto não receber um sinal SIGUSR2 ou ainda conseguir roubar processos, o processo rouba e executa
        while(!exitSig){
            shuffle(strips, stripId); //Obtém um vetor embaralhado de stripIds dos outros auxiliares
            //Tenta roubar um processo de outro auxiliar usando o vetor de strips
            for(int i = 0; i < AUX_PROCS - 1; i++){
                //Vai na "fila" de outro auxiliar e pega o índice do próximo processo que seria executado
                Psem();
                    execProc = nextExecList[strips[i]];
                    nextExecList[strips[i]] += AUX_PROCS;
                Vsem();
                //Se o índice é válido, a "fila" não tava vazia e o auxiliar roubou um processo com sucesso
                if(execProc < procs)
                    break;
            }
            //Se saiu do loop e o índice recuperado é inválido, então todas as "filas" estão vazias e encerra
            if(execProc >= procs)
                exitSig = 1; //Ele próprio seta a flag de saída
            //Se saiu do loop com índice válido, executa o processo roubado
            else{
                int pid;
                //Cria o processo filho que executará o processo roubado
                if((pid = fork()) < 0){
                    printf("Error in creating child process: %s\n", strerror(errno));
                    exit(1);
                }
                else if(pid == 0){
                    //O processo filho vai executar o processo de índice execProc
                    if(execl(procsList[execProc], procsList[execProc], (char*) 0) < 0){
                        printf("Error in executing child process in strip %d: %s\n", stripId, strerror(errno));
                        exit(1);
                    }
                }
                else{
                    //Enquanto o filho executa o processo, o pai fica em wait
                    if(wait(&status) < 0){
                        printf("Error in waiting child to terminate: %s\n", strerror(errno));
                        exit(1);
                    }
                    //Se o processo filho terminou por causa de erro, o auxiliar encerra com erro (exit 1)
                    else if(!WIFEXITED(status) || WEXITSTATUS(status) != 0)
                        exit(1);
                }
                //Depois de executar o processo roubado, o auxiliar avisa ao pai que terminou o trabalho
                notifyTerm();
            }
        }
        //Se saiu do loop, quer dizer que encerrou sua execução
        //Encerra o contador, converte o resultado para segundos e encerra a execução
        gettimeofday(&end, NULL);
        double time = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
        printf("Process Aux %d time = %.2fs\n", stripId, time);
        exit(0);
    }
    else{
        /* Código do processo pai */

        //O processo pai fica em Busy Waiting enquanto houver auxiliares executando
        while(auxProcs){
            //Dentro Busy Waiting, o pai fica verificando se recebeu alguma mensagem dos filhos (Não Bloqueante)
            if(msgrcv(msqid, &msgbuf, sizeof(msgbuf), 0, IPC_NOWAIT) < 0){
                //Se deu erro por causa de ENOMSG, então não havia mensagens na fila (esse caso de erro é ignorado)
                if(errno != ENOMSG){
                    printf("Error in receiving message: %s\n", strerror(errno));
                    exit(1);
                }
            }
            //Se recebeu uma mensagem, envia ao filho que enviou a mensagem um sinal através do pid recebido
            else if(mode && auxProcs > 1)
                kill(msgbuf.pid, SIGUSR1); //Sinal para continuar executando e entrar em modo roubo
            else
                kill(msgbuf.pid, SIGUSR2); //Sinal para encerrar a execução

            int waitRet; //Guarda o retorno da função waitpid
            //Ainda dentro do Busy Waiting, o pai fica fazendo wait para evitar zumbis (Não Bloqueante)
            if((waitRet = waitpid(-1, &status, WNOHANG)) < 0){
                printf("Error in waiting child to terminate: %s\n", strerror(errno));
                exit(1);
            }
            //Se o retorno foi positivo, então um processo filho terminou a execução
            else if(waitRet > 0){
                //Se o processo filho terminou por causa de erro, o pai encerra com erro (exit 1)
                if(!WIFEXITED(status) || WEXITSTATUS(status) != 0)
                    exit(1);
                //Caso contrário, o filho encerrou corretamente, então decrementa auxProcs
                auxProcs--;
            }
        }
        //Se saiu do Busy Waiting, então todos os processos auxiliares acabaram
        //Encerra o contador, converte o resultado para segundos, remove os mecanismos IPC e encerra a execução
        gettimeofday(&end, NULL);
        double time = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
        printf("Scheduler Makespan = %.2fs\n", time);
        removeIPC();
    }
    return 0;
}