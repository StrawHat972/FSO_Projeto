Para executar o Escalonador é preciso antes compilar os arquivos demorado.c, medio.c e rapido.c. Uma vez que os executáveis de tais programas tenham sido gerados pode-se executar o Escalonador com o seguinte comando:
```sh
./scheduler -n process.txt
```

Onde "n" é a flag para execução normal do Escalonador e "process.txt" é o arquivo de entrada que contém os processos a serem executados. Pode também executar com o seguinte comando:
```sh
./scheduler -r process.txt
```

Onde "r" é a flag para roubou de trabalho (work stealing).

Ao final da execução do Escalonador é mostrado o Makespan da Aplicação e à medida que os processos auxiliares terminam é mostrado o tempo de execução que cada processo auxiliar necessitou.